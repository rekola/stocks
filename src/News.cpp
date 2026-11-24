#include "News.h"

#include "AttributeView.h"
#include "model/Dates.h"

namespace {

std::string twoDigits(int n) { return (n < 10 ? "0" : "") + std::to_string(n); }

}  // namespace

NewsTable::NewsTable(std::shared_ptr<const Snapshot> snapshot, std::function<bool(const NewsItem &)> keep, size_t limit)
    : keep_(std::move(keep)), limit_(limit) {
  setSnapshot(std::move(snapshot));
}

void NewsTable::setSnapshot(std::shared_ptr<const Snapshot> snapshot) {
  snapshot_ = std::move(snapshot);
  NewsQuery query;
  query.keep = keep_;
  query.limit = limit_;
  items_ = snapshot_->news(query);
}

std::string NewsTable::keyOf(const NewsItem & item) { return datasetCode(item.dataset) + "|" + item.id; }

std::string NewsTable::timeText(Timestamp t) {
  const int64_t day = dayNumberOfInstant(t);
  const int64_t second = t - (day - kUnixEpochDayNumber) * 86400;
  return dateText(day) + " " + twoDigits(static_cast<int>(second / 3600)) + ":" + twoDigits(static_cast<int>(second % 3600 / 60));
}

std::string NewsTable::aboutText(const Snapshot & snapshot, const NewsItem & item) {
  std::string out;
  for (const NodeRef & n : item.about) {
    std::string name = snapshot.name(n);
    if (name.empty() && n.type == NodeType::Instrument) name = snapshot.symbol(InstrumentId{n.id});
    if (name.empty()) name = ::text(NodeKey{n.id});  // a country with no name said: its code
    if (!name.empty()) out += (out.empty() ? "" : ", ") + name;
  }
  return out;
}

std::string NewsTable::text(int r, int c) const {
  const NewsItem & i = item(r);
  switch (c) {
    case TIME: return timeText(i.time);
    case KIND: return newsText(i, predicate::kind);
    case HEADLINE: return i.headline;
    case ABOUT: return aboutText(*snapshot_, i);
    case SOURCE: return snapshot_->name(NodeRef(i.dataset));
  }
  return {};
}

std::optional<double> NewsTable::number(int r, int c) const {
  if (c == TIME) return static_cast<double>(item(r).time);  // sorts by the instant, not the text
  return std::nullopt;
}
