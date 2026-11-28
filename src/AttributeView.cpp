#include "AttributeView.h"

#include "News.h"

#include "Quotes.h"
#include "model/Dates.h"

#include <cellui/DialogPanel.h>
#include <cellui/Input.h>
#include <cellui/Scrollbar.h>
#include <cellui/Utf8.h>

#include <algorithm>
#include <cctype>
#include <cmath>

using namespace cellui;

namespace {

constexpr int kMaxWidth = 100;
constexpr int kMaxPredicateWidth = 26;
constexpr const char * kHint = "Esc: close  Up/Down, PgUp/PgDn, Home/End: scroll";

std::string twoDigits(int n) { return (n < 10 ? "0" : "") + std::to_string(n); }

// One overload for each alternative of Value and no template to catch the rest: a new alternative does not compile here.
struct ValueText {
  const Snapshot & snapshot;
  const std::string & predicate;
  ShownValue operator()(std::monostate) const { return {}; }
  ShownValue operator()(const std::string & s) const { return {s, std::nullopt}; }
  ShownValue operator()(int64_t n) const { return {isDayPredicate(predicate) ? dateText(n) : std::to_string(n), std::nullopt}; }
  ShownValue operator()(double d) const { return {realText(d), std::nullopt}; }
  ShownValue operator()(bool b) const { return {b ? "yes" : "no", std::nullopt}; }
  ShownValue operator()(NodeKey key) const {
    const NodeRef node = nodeOf(key);
    std::string label = snapshot.name(node);
    if (label.empty() && node.type == NodeType::Instrument) label = snapshot.symbol(InstrumentId{key.value});
    if (label.empty()) label = describe(key);
    return {std::move(label), node};
  }
};

// The attributes of a node: `first` in that order, then the rest alphabetically.
std::vector<InfoRow> attributeRows(const Snapshot & snapshot, NodeRef node, const std::vector<std::string> & first) {
  std::vector<Attribute> all = snapshot.attributes(node);
  std::stable_sort(all.begin(), all.end(), [&](const Attribute & a, const Attribute & b) {
    auto rank = [&](const std::string & p) { return static_cast<size_t>(std::find(first.begin(), first.end(), p) - first.begin()); };
    const size_t ra = rank(a.predicate), rb = rank(b.predicate);
    if (ra != rb) return ra < rb;
    return a.predicate < b.predicate;
  });
  std::vector<InfoRow> rows;
  for (const Attribute & a : all) rows.push_back({a.predicate, showValue(snapshot, a.predicate, a.value)});
  return rows;
}

// A bar's time as a date, and for an intraday bar its minute (UTC).
std::string barTime(Resolution r, int64_t time) {
  if (isDateResolution(r)) return dateText(time);
  const Timestamp t = instantOfSlot(r, time);
  const int64_t secondOfDay = t - (dayNumberOfInstant(t) - kUnixEpochDayNumber) * 86400;
  return dateText(dayNumberOfInstant(t)) + " " + twoDigits(static_cast<int>(secondOfDay / 3600)) + ":" + twoDigits(static_cast<int>(secondOfDay % 3600 / 60));
}

}  // namespace

std::vector<std::string> wrapText(const std::string & text, double cols, const TextWidth & width) {
  auto measure = [&](const std::string & s) { return width ? width(s) : static_cast<double>(utf8::displayWidth(s)); };
  if (cols < 1) return {text};
  std::vector<std::string> lines;
  std::string line;
  auto flush = [&] {
    lines.push_back(std::move(line));
    line.clear();
  };
  size_t at = 0;
  while (at < text.size()) {
    while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at]))) at++;
    const size_t start = at;
    while (at < text.size() && !std::isspace(static_cast<unsigned char>(text[at]))) at++;
    if (start == at) break;
    const std::string word = text.substr(start, at - start);
    if (!line.empty() && measure(line + ' ' + word) <= cols) {
      line += ' ' + word;
    } else if (measure(word) <= cols) {
      if (!line.empty()) flush();
      line = word;
    } else {
      // Wider than a line: starts on a fresh one and is cut between characters.
      if (!line.empty()) flush();
      for (const std::string & cluster : utf8::clusters(word)) {
        if (!line.empty() && measure(line + cluster) > cols) flush();
        line += cluster;
      }
    }
  }
  if (!line.empty() || lines.empty()) flush();
  return lines;
}

std::string realText(double v) {
  if (!isDefined(v)) return "-";
  std::string s = fixed(v, 4);  // never printf: its point is the locale's
  if (s.find('.') != std::string::npos) {
    while (s.back() == '0') s.pop_back();
    if (s.back() == '.') s.pop_back();
  }
  return s;
}

std::string dateText(int64_t number) {
  const Date d = dateOfDayNumber(number);
  return std::to_string(d.year) + "-" + twoDigits(static_cast<int>(d.month)) + "-" + twoDigits(static_cast<int>(d.day));
}

bool isDayPredicate(const std::string & predicate) { return predicate.size() > 4 && predicate.compare(predicate.size() - 4, 4, "-day") == 0; }

ShownValue showValue(const Snapshot & snapshot, const std::string & predicate, const Value & value) {
  return std::visit(ValueText{snapshot, predicate}, value);
}

namespace {

// "2026-11-27 12:00 UTC" of a time in seconds.
std::string instantText(Timestamp t) {
  const int64_t second = t - (dayNumberOfInstant(t) - kUnixEpochDayNumber) * 86400;
  return dateText(dayNumberOfInstant(t)) + " " + twoDigits(static_cast<int>(second / 3600)) + ":" + twoDigits(static_cast<int>(second % 3600 / 60)) + " UTC";
}

// What one edge says of itself: its listing, the series it has, and its summary. `suffix` is added to the titles where the row has several.
void edgeSections(const Snapshot & snapshot, const EdgeKey & edge, const std::string & suffix, bool withDataset, std::vector<InfoSection> & out) {
  InfoSection listing{"Listing" + suffix, {}};
  auto add = [&](const char * predicate, std::string text) { listing.rows.push_back({predicate, {std::move(text), std::nullopt}}); };
  const std::optional<Listing> l = snapshot.listing(edge);
  add("symbol", l && !l->symbol.empty() ? l->symbol : snapshot.symbol(edge));
  if (l) {
    add("identifier", l->identifier);
    add("order book", l->orderBook);
    add("type", l->type);
    add("name", l->name);
  }
  if (withDataset) add("dataset", snapshot.name(edge.dataset));
  add("market", snapshot.name(edge.market));
  if (snapshot.known(NodeRef(edge.quote))) add("quote currency", snapshot.symbol(edge.quote));
  out.push_back(std::move(listing));
}

void seriesSections(const Snapshot & snapshot, const EdgeKey & edge, const std::string & suffix, std::vector<InfoSection> & out) {
  InfoSection series{"Series" + suffix, {}};
  for (Resolution r : snapshot.resolutions(edge)) {
    const CandleSeries bars = snapshot.candles(edge, r);
    if (bars.candles.empty()) continue;
    const size_t n = bars.candles.size();
    series.rows.push_back({name(r), {std::to_string(n) + (n == 1 ? " bar, " : " bars, ") + barTime(r, bars.candles.front().time) + " to " + barTime(r, bars.candles.back().time), std::nullopt}});
  }
  out.push_back(std::move(series));

  // The summary of the edge, which is not the data (the series above): what the table shows and what is kept between runs.
  InfoSection summary{"Summary" + suffix, {}};
  if (const auto q = snapshot.quote(edge)) {
    if (isDefined(q->last)) summary.rows.push_back({"last", {realText(q->last), std::nullopt}});
    if (isDefined(q->bid)) summary.rows.push_back({"bid", {realText(q->bid), std::nullopt}});
    if (isDefined(q->ask)) summary.rows.push_back({"ask", {realText(q->ask), std::nullopt}});
    if (q->time != 0) summary.rows.push_back({"quote time", {instantText(q->time), std::nullopt}});
  }
  for (Resolution r : snapshot.queues(edge)) {
    const auto c = snapshot.coverage(edge, r);
    if (!c || c->count == 0) continue;
    std::string text = std::to_string(c->count) + (c->count == 1 ? " bar, " : " bars, ") + barTime(r, c->first_key) + " to " + barTime(r, c->last_key) +
                       (snapshot.queueResident(edge, r) ? ", in memory" : ", on the disk");
    if (c->fetched > 0) {
      const int64_t seconds = c->fetched / 1000, second = seconds - (dayNumberOfInstant(seconds) - kUnixEpochDayNumber) * 86400;
      text += ", fetched " + dateText(dayNumberOfInstant(seconds)) + " " + twoDigits(static_cast<int>(second / 3600)) + ":" + twoDigits(static_cast<int>(second % 3600 / 60));
    }
    summary.rows.push_back({std::string("queue ") + name(r), {std::move(text), std::nullopt}});
  }
  if (!summary.rows.empty()) out.push_back(std::move(summary));
}

// A member is named by where and who says it: "Nasdaq Helsinki · KAR".
std::string memberName(const Snapshot & snapshot, const EdgeKey & edge) {
  const std::string market = snapshot.name(NodeRef(edge.market));
  return (market.empty() ? marketCode(edge.market) : market) + " · " + snapshot.name(edge.dataset);
}

// What the members say together, and what each says and whether it counts.
InfoSection sourcesSection(const Snapshot & snapshot, const Mix & mix) {
  InfoSection sources{"Sources", {}};
  auto row = [&](std::string predicate, std::string text) { sources.rows.push_back({std::move(predicate), {std::move(text), std::nullopt}}); };
  if (isDefined(mix.price)) {
    const char * rule = mix.used >= 3 ? "median of " : mix.used == 2 ? "mean of " : "";
    row("combined", realText(mix.price) + (mix.used > 1 ? std::string(" (") + rule + std::to_string(mix.used) + " datasets)" : " (one dataset)"));
  }
  if (isDefined(mix.disagreement)) row("disagreement", fixed(mix.disagreement * 100, 2) + "% (the typical distance of a dataset from the combined price)");
  for (const Member & m : mix.members) {
    std::string text = isDefined(m.price) ? realText(m.price) : std::string("no price");
    if (isDefined(m.offset) && m.state == MemberState::Used && mix.used > 1) text += "  " + fixed(m.offset * 100, 2, true) + "%";
    if (m.time != 0) text += "  " + instantText(m.time);
    switch (m.state) {
      case MemberState::Used: break;
      case MemberState::Stale: text += "  left out: older than the others"; break;
      case MemberState::Behind: text += "  left out: a faster dataset says the price"; break;
      case MemberState::Off: text += "  left out: switched off"; break;
      case MemberState::NoPrice: text += "  left out"; break;
    }
    row(memberName(snapshot, m.edge), std::move(text));
  }
  return sources;
}

}  // namespace

std::vector<InfoSection> buildInfo(const Snapshot & snapshot, const EdgeKey & edge, const Mix * mix) {
  std::vector<InfoSection> out;
  const bool grouped = mix && !mix->members.empty();
  if (grouped) out.push_back(sourcesSection(snapshot, *mix));
  else edgeSections(snapshot, edge, "", true, out);

  out.push_back({"Instrument", attributeRows(snapshot, NodeRef(edge.base), {predicate::isin, predicate::name, predicate::kind})});
  // The markets of the members, each once (the one edge's where the row is not a group).
  std::vector<MarketId> markets = {edge.market};
  if (grouped) {
    markets.clear();
    for (const Member & m : mix->members)
      if (std::find(markets.begin(), markets.end(), m.edge.market) == markets.end()) markets.push_back(m.edge.market);
  }
  for (const MarketId market : markets)
    out.push_back({markets.size() > 1 ? "Market · " + snapshot.name(NodeRef(market)) : std::string("Market"),
                   attributeRows(snapshot, NodeRef(market), {predicate::name, predicate::mic, predicate::country, predicate::timezone})});
  if (auto rows = attributeRows(snapshot, NodeRef(edge.quote), {predicate::isin, predicate::name, predicate::kind}); !rows.empty())
    out.push_back({"Quote currency", std::move(rows)});

  if (grouped) {
    for (const Member & m : mix->members) {
      const std::string suffix = " · " + memberName(snapshot, m.edge);
      edgeSections(snapshot, m.edge, suffix, false, out);
      seriesSections(snapshot, m.edge, suffix, out);
    }
  } else {
    seriesSections(snapshot, edge, "", out);
  }

  // The latest news of the instrument, of its market and of the market's country: a few headlines, newest first.
  {
    InfoSection latest{"Latest news", {}};
    std::vector<NodeRef> nodes = {NodeRef(edge.base)};
    for (const MarketId market : markets) {
      nodes.push_back(NodeRef(market));
      if (const std::string code = snapshot.said(NodeRef(market), predicate::country); !code.empty())
        if (const auto key = countryKey(code))
          if (const NodeRef country = nodeOf(*key); std::find(nodes.begin(), nodes.end(), country) == nodes.end()) nodes.push_back(country);
    }
    std::vector<NewsItem> items;
    for (const NodeRef & node : nodes) {
      NewsQuery q;
      q.about = node;
      q.limit = 5;
      for (NewsItem & n : snapshot.news(q)) items.push_back(std::move(n));
    }
    std::sort(items.begin(), items.end(), [](const NewsItem & a, const NewsItem & b) { return a.time > b.time; });
    for (const NewsItem & n : items) {
      if (latest.rows.size() >= 8) break;
      latest.rows.push_back({NewsTable::timeText(n.time), {n.headline, std::nullopt}});
    }
    if (!latest.rows.empty()) out.push_back(std::move(latest));
  }

  // A section with nothing to say is not shown.
  out.erase(std::remove_if(out.begin(), out.end(), [](const InfoSection & s) { return s.rows.empty(); }), out.end());
  return out;
}

std::vector<InfoSection> buildNewsInfo(const Snapshot & snapshot, const NewsItem & item) {
  std::vector<InfoSection> out;
  InfoSection about{"News", {}};
  auto add = [&](const char * predicate, std::string text) {
    if (!text.empty()) about.rows.push_back({predicate, {std::move(text), std::nullopt}});
  };
  add("headline", item.headline);
  add("time", NewsTable::timeText(item.time) + " UTC");
  add("dataset", snapshot.name(NodeRef(item.dataset)));
  add("id", item.id);
  add("url", item.url);
  out.push_back(std::move(about));
  InfoSection attributes{"Attributes", {}};
  for (const Attribute & a : item.attributes) attributes.rows.push_back({a.predicate, showValue(snapshot, a.predicate, a.value)});
  out.push_back(std::move(attributes));
  InfoSection whom{"About", {}};
  for (const NodeRef & n : item.about) {
    std::string name = snapshot.name(n);
    if (name.empty()) name = text(NodeKey{n.id});
    const char * kind = n.type == NodeType::Instrument ? "instrument" : n.type == NodeType::Market ? "market" : n.type == NodeType::Country ? "country" : "node";
    whom.rows.push_back({kind, {std::move(name), n}});
  }
  out.push_back(std::move(whom));
  if (!item.body.empty()) out.push_back({"Text", {{"intro", {item.body, std::nullopt}}}});
  out.erase(std::remove_if(out.begin(), out.end(), [](const InfoSection & s) { return s.rows.empty(); }), out.end());
  return out;
}

AttributeModal::AttributeModal(Widget & root, Plane & screen, EventLoop & loop, std::string title, std::shared_ptr<const Snapshot> snapshot,
                               std::vector<InfoSection> sections)
    : ModalDialog(root, screen, loop), title_(std::move(title)), snapshot_(std::move(snapshot)) {
  for (const InfoSection & section : sections) {
    if (!rows_.empty()) rows_.push_back({});
    rows_.push_back({Line::HEADING, section.title, "", false});
    for (const InfoRow & row : section.rows) {
      rows_.push_back({Line::ROW, row.predicate, row.value.text, row.value.link.has_value()});
      predicate_width_ = std::max(predicate_width_, utf8::displayWidth(row.predicate));
    }
  }
  predicate_width_ = std::min(predicate_width_, kMaxPredicateWidth);
  wrapLines(kMaxWidth);
  value_cols_ = -1;  // for lines() until the first layout, which wraps for the dialog's own width
}

std::vector<std::string> AttributeModal::lines() const {
  std::vector<std::string> out;
  for (const Line & line : lines_) {
    if (line.kind != Line::ROW) out.push_back(line.predicate);
    else if (line.continued) out.push_back(std::string(static_cast<size_t>(predicate_width_ + 2), ' ') + line.text);
    else out.push_back(line.predicate + "  " + line.text);
  }
  return out;
}

void AttributeModal::wrapLines(int value_cols, const TextWidth & width) {
  if (value_cols == value_cols_) return;
  value_cols_ = value_cols;
  lines_.clear();
  for (const Line & row : rows_) {
    if (row.kind != Line::ROW) {
      lines_.push_back(row);
      continue;
    }
    bool first = true;
    for (std::string & part : wrapText(row.text, value_cols, width)) {
      lines_.push_back({Line::ROW, first ? row.predicate : "", std::move(part), row.link, !first});
      first = false;
    }
  }
}

void AttributeModal::layout(Plane & screen, int rows, int cols) {
  const int width = std::min(kMaxWidth, std::max(20, cols - 4));
  const int valueX = 1 + predicate_width_ + 2, valueCols = std::max(1, width - 1 - valueX);
  // By what the display makes of the text: a window sets it in a proportional face, narrower than the cells it is
  // given, so counting cells would break the lines far short of the dialog's edge.
  wrapLines(valueCols, [&screen](const std::string & s) { return screen.textWidth(s, kPlainFace); });
  // Padding, title, a gap, the lines, a gap, the hint, padding.
  constexpr int kChrome = 6;
  const int height = std::min(static_cast<int>(lines_.size()) + kChrome, std::max(kChrome + 1, rows - 2));
  body_rows_ = std::max(1, height - kChrome);
  body_.setCount(static_cast<int>(lines_.size()));

  if (!plane_) {
    plane_ = cellui::dialog::create(screen, screen.getStyles().theme.elevation.modal);
  }
  plane_->resize(height, width);
  cellui::dialog::style(*plane_);
  const StyleProvider & styles = plane_->getStyles();
  const Color bg = styles.picker_bg_color, fg = styles.window_fg_color, strong = styles.window_accent_fg_color, quiet = styles.theme.text_secondary;
  // The box is glass where the display blurs pixels, like every dialog's.
  auto paint = [&](const Color & foreground) { cellui::dialog::ink(*plane_, foreground); };
  paint(fg);
  for (int y = 0; y < height; y++) plane_->putstr(y, 0, std::string(static_cast<size_t>(width), ' '));
  const int inner = width - 2;
  paint(strong);
  plane_->putstr(1, 1, utf8::truncateToWidth(title_, inner), kBoldFace);
  paint(quiet);
  plane_->putstr(height - 2, 1, utf8::truncateToWidth(kHint, inner));

  const int top = (rows - height) / 2, left = (cols - width) / 2;
  plane_->move(top, left);
  body_.layout(screen, *plane_, top, left, 3, body_rows_, width - 1, bg, fg, quiet, strong);
  Plane & body = body_.plane();
  const TextStyle plain{TextFont::PROPORTIONAL, TextOverflow::ELLIPSIS, kPlainFace, TextAlign::LEFT};
  for (int i = 0; i < body_.drawnRows(); i++) {
    const size_t at = static_cast<size_t>(body_.firstLine() + i);
    if (at >= lines_.size()) break;
    const Line & line = lines_[at];
    if (line.kind == Line::HEADING) {
      body.setFgColor(strong);
      body.putText(i, 1, inner, line.predicate, TextStyle{TextFont::PROPORTIONAL, TextOverflow::ELLIPSIS, kBoldFace, TextAlign::LEFT});
    } else if (line.kind == Line::ROW) {
      body.setFgColor(quiet);
      if (!line.continued) body.putText(i, 1, predicate_width_, line.predicate, plain);
      body.setFgColor(line.link ? strong : fg);  // where a value refers to a node it will be a link
      body.putText(i, valueX, valueCols, line.text, plain);
    }
  }
  plane_->moveToTop();
  body.moveToTop();
}

void AttributeModal::scrollBy(int delta) {
  body_.scrollBy(delta);
  relayout();
}

void AttributeModal::keyDown(const KeyDown & e) {
  const int page = std::max(1, bodyRows() - 1);
  if (e.key == Key::Escape || e.key == 'q' || (e.mods.ctrl && e.key == 'g')) done(0);
  else if (e.key == Key::Down || (e.mods.ctrl && e.key == 'n')) scrollBy(1);
  else if (e.key == Key::Up || (e.mods.ctrl && e.key == 'p')) scrollBy(-1);
  else if (e.key == Key::PageDown || (e.mods.ctrl && e.key == 'v')) scrollBy(page);
  else if (e.key == Key::PageUp || (e.mods.alt && e.key == 'v')) scrollBy(-page);
  else if (e.key == Key::Home) body_.home(), relayout();
  else if (e.key == Key::End) body_.end(), relayout();
}

void AttributeModal::wheel(const Wheel & e) {
  if (e.dy != 0) scrollBy(-e.dy * 3);
}

void AttributeModal::mouseDown(const MouseDown & e) {
  if (e.button == 1 && body_.press(e.row, e.col)) relayout();
}

void AttributeModal::mouseMove(const MouseMove & e) {
  if ((e.buttons & 1u) && body_.drag(e.row, e.col)) relayout();
}

void AttributeModal::mouseUp(const MouseUp & e) {
  if (e.button == 1 && body_.release(e.row, e.col)) relayout();
}
