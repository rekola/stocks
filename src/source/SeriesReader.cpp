#include "source/SeriesReader.h"

#include "model/Dates.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <set>

using json = nlohmann::ordered_json;

namespace series {

namespace {

enum class Role { None, Time, End, Open, High, Low, Close, Value, Volume, Bid, Ask };

std::string normal(const std::string & s) {
  std::string out;
  for (char c : s)
    if (std::isalnum(static_cast<unsigned char>(c))) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

Role roleByName(const std::string & key) {
  static const std::map<std::string, Role> names = {
      {"time", Role::Time},        {"t", Role::Time},          {"ts", Role::Time},          {"timestamp", Role::Time},   {"date", Role::Time},
      {"datetime", Role::Time},    {"starttime", Role::Time},  {"startdatetime", Role::Time}, {"start", Role::Time},    {"barstart", Role::Time},
      {"tradetime", Role::Time},   {"x", Role::Time},          {"day", Role::Time},         {"asof", Role::Time},
      {"endtime", Role::End},      {"enddatetime", Role::End}, {"end", Role::End},          {"barend", Role::End},
      {"open", Role::Open},        {"o", Role::Open},          {"openprice", Role::Open},
      {"high", Role::High},        {"h", Role::High},          {"highprice", Role::High},
      {"low", Role::Low},          {"l", Role::Low},           {"lowprice", Role::Low},
      {"close", Role::Close},      {"c", Role::Close},         {"closeprice", Role::Close}, {"last", Role::Close},       {"lastprice", Role::Close},
      {"value", Role::Value},      {"price", Role::Value},     {"rate", Role::Value},       {"nav", Role::Value},        {"y", Role::Value},
      {"mid", Role::Value},        {"midprice", Role::Value},
      {"volume", Role::Volume},    {"vol", Role::Volume},      {"v", Role::Volume},         {"qty", Role::Volume},       {"quantity", Role::Volume},
      {"bid", Role::Bid},          {"bidprice", Role::Bid},
      {"ask", Role::Ask},          {"askprice", Role::Ask}};
  const auto it = names.find(normal(key));
  return it == names.end() ? Role::None : it->second;
}

Role roleByHintName(const std::string & name) {
  static const std::map<std::string, Role> names = {{"time", Role::Time}, {"end", Role::End},   {"open", Role::Open}, {"high", Role::High},
                                                    {"low", Role::Low},   {"close", Role::Close}, {"value", Role::Value}, {"volume", Role::Volume},
                                                    {"bid", Role::Bid},   {"ask", Role::Ask}};
  const auto it = names.find(name);
  return it == names.end() ? Role::None : it->second;
}

// A number, or a text that is wholly one; none for anything else (null included).
bool number(const json & v, double & out) {
  if (v.is_number()) {
    out = v.get<double>();
    return std::isfinite(out);
  }
  if (v.is_string()) {
    const std::string s = v.get<std::string>();
    if (s.empty()) return false;
    char * end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end == s.c_str() + s.size() && std::isfinite(out);
  }
  return false;
}

double numberOrUndefined(const json & v) {
  double d = 0;
  return number(v, d) ? d : kUndefined;
}

Timestamp ofNumber(double d) { return static_cast<Timestamp>(d >= 1e11 ? d / 1000.0 : d); }

bool timeOf(const json & v, Timestamp & out) {
  if (v.is_number()) {
    const double d = v.get<double>();
    if (!std::isfinite(d) || d <= 0) return false;
    out = ofNumber(d);
    return true;
  }
  return v.is_string() && parseTime(v.get<std::string>(), out);
}

// Whether the value is, by itself, a time: an ISO text or a number as large as the seconds of 1973.
bool looksLikeTime(const json & v) {
  Timestamp t = 0;
  if (v.is_number()) return v.get<double>() >= 1e8 && timeOf(v, t);
  if (!v.is_string()) return false;
  const std::string s = v.get<std::string>();
  return s.size() >= 10 && s[4] == '-' && s[7] == '-' && timeOf(v, t);
}

bool isRowLike(const json & v) { return v.is_array() ? !v.empty() && (v[0].is_object() || v[0].is_array()) : false; }

bool columnar(const json & o);
bool named(const json & o);

bool seriesLike(const json & v) { return isRowLike(v) || (v.is_object() && (columnar(v) || named(v))); }

// An object with an array or an object in it: something that may hold the series (one of scalars, such as a `meta`, does not).
bool holdsSomething(const json & v) {
  if (!v.is_object()) return false;
  for (const auto & item : v.items())
    if (item.value().is_array() || item.value().is_object()) return true;
  return false;
}

// Struct of arrays: more than one member, all arrays of one length of values that are not themselves rows, one of them with a time role.
bool columnar(const json & o) {
  if (!o.is_object() || o.size() < 2) return false;
  size_t length = 0;
  bool time = false;
  for (const auto & [key, value] : o.items()) {
    if (!value.is_array() || value.empty() || value[0].is_object() || value[0].is_array()) return false;
    if (length == 0) length = value.size();
    if (value.size() != length) return false;
    if (roleByName(key) == Role::Time) time = true;
  }
  return time;
}

bool named(const json & o) {
  if (!o.is_object() || o.size() < 2) return false;
  for (const auto & item : o.items())
    if (!seriesLike(item.value())) return false;
  return true;
}

bool preferredName(const std::string & key) {
  static const std::set<std::string> names = {"data", "result", "results", "bars", "candles", "prices", "series", "items", "values", "history", "rows", "chart", "quotes"};
  return names.count(normal(key)) > 0;
}

// What an answer says of its own failure, or empty.
std::string failure(const json & o) {
  const auto text = [](const json & v) -> std::string {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_object())
      for (const char * k : {"description", "message", "detail", "type", "code"})
        if (const auto it = v.find(k); it != v.end() && it->is_string()) return it->get<std::string>();
    return "the answer says it is an error";
  };
  if (const auto e = o.find("error"); e != o.end() && !e->is_null() && !(e->is_boolean() && !e->get<bool>()) && !(e->is_string() && e->get<std::string>().empty()) &&
                                      !(e->is_array() && e->empty()))
    return text(*e);
  for (const char * k : {"statusCode", "status", "httpStatus"})
    if (const auto s = o.find(k); s != o.end() && s->is_number() && s->get<double>() >= 400) {
      for (const char * m : {"message", "description", "detail"})
        if (const auto it = o.find(m); it != o.end() && it->is_string()) return "status " + std::to_string(static_cast<int>(s->get<double>())) + ": " + it->get<std::string>();
      return "status " + std::to_string(static_cast<int>(s->get<double>()));
    }
  return {};
}

struct Context {
  const Hints & hints;
  Result & result;
  size_t skipped = 0;
  std::set<std::string> unused;
};

// The keys of the rows, in the order they first appear.
std::vector<std::string> keysOf(const std::vector<const json *> & rows) {
  std::vector<std::string> keys;
  std::set<std::string> seen;
  for (size_t i = 0; i < rows.size() && i < 50; i++)
    for (const auto & item : rows[i]->items())
      if (seen.insert(item.key()).second) keys.push_back(item.key());
  return keys;
}

const json * firstValue(const std::vector<const json *> & rows, const std::string & key) {
  for (const json * row : rows)
    if (const auto it = row->find(key); it != row->end() && !it->is_null()) return &*it;
  return nullptr;
}

struct Columns {
  std::map<Role, std::string> key;  // the key of each role
  std::vector<std::string> labels;  // the keys of text that label the series
};

// Which key is which role in rows that are structs. False, with `why`, where there is no time or no value.
bool columnsOf(const std::vector<const json *> & rows, const Hints & hints, Columns & out, std::string & why, Context & ctx) {
  const std::vector<std::string> keys = keysOf(rows);
  for (const auto & [name, key] : hints.fields)
    if (const Role r = roleByHintName(name); r != Role::None) out.key[r] = key;
  for (const std::string & key : keys) {
    const Role r = roleByName(key);
    if (r != Role::None && !out.key.count(r)) out.key[r] = key;
  }
  if (!out.key.count(Role::Time))
    for (const std::string & key : keys)
      if (const json * v = firstValue(rows, key); v && looksLikeTime(*v) && !(v->is_number())) {
        out.key[Role::Time] = key;
        ctx.result.notes.push_back("the time is " + key + ", told by its values");
        break;
      }
  if (!out.key.count(Role::Time)) {
    why = "no field is a time";
    return false;
  }
  std::set<std::string> used;
  for (const auto & item : out.key) used.insert(item.second);
  // A close beats a value when the rows have both.
  if (out.key.count(Role::Close) && out.key.count(Role::Value)) {
    ctx.unused.insert(out.key[Role::Value]);
    out.key.erase(Role::Value);
  }
  if (!out.key.count(Role::Close) && !out.key.count(Role::Value) && !out.key.count(Role::Bid) && !out.key.count(Role::Ask) && !out.key.count(Role::Open)) {
    // The one number left is the value.
    std::vector<std::string> numeric;
    for (const std::string & key : keys) {
      const json * v = firstValue(rows, key);
      double d = 0;
      if (!used.count(key) && v && !v->is_boolean() && number(*v, d)) numeric.push_back(key);
    }
    if (numeric.size() != 1) {
      why = numeric.empty() ? "the rows have no value" : "the rows have several numbers and none is named as a value (" + numeric[0] + ", " + numeric[1] + "...)";
      return false;
    }
    out.key[Role::Value] = numeric[0];
    used.insert(numeric[0]);
    ctx.result.notes.push_back("the value is " + numeric[0] + ", the one number there is");
  }
  for (const std::string & key : keys) {
    if (used.count(key)) continue;
    const json * v = firstValue(rows, key);
    if (!v) continue;
    double d = 0;
    if (v->is_string() && !number(*v, d) && !looksLikeTime(*v)) out.labels.push_back(key);
    else ctx.unused.insert(key);
  }
  return true;
}

Bar barOf(const json & row, const Columns & c, bool & ok) {
  Bar b;
  ok = false;
  const auto get = [&](Role r) -> const json * {
    const auto k = c.key.find(r);
    if (k == c.key.end()) return nullptr;
    const auto it = row.find(k->second);
    return it == row.end() ? nullptr : &*it;
  };
  const json * t = get(Role::Time);
  if (!t || !timeOf(*t, b.time)) return b;
  if (const json * e = get(Role::End)) {
    Timestamp end = 0;
    if (timeOf(*e, end)) b.end = end;
  }
  if (const json * v = get(Role::Open)) b.open = numberOrUndefined(*v);
  if (const json * v = get(Role::High)) b.high = numberOrUndefined(*v);
  if (const json * v = get(Role::Low)) b.low = numberOrUndefined(*v);
  if (const json * v = get(Role::Close)) b.close = numberOrUndefined(*v);
  if (const json * v = get(Role::Value)) b.close = numberOrUndefined(*v);
  if (const json * v = get(Role::Volume)) b.volume = numberOrUndefined(*v);
  if (const json * v = get(Role::Bid)) b.bid = numberOrUndefined(*v);
  if (const json * v = get(Role::Ask)) b.ask = numberOrUndefined(*v);
  ok = isDefined(b.close) || isDefined(b.open) || isDefined(b.bid) || isDefined(b.ask);
  return b;
}

void finish(Series & s) {
  std::stable_sort(s.bars.begin(), s.bars.end(), [](const Bar & a, const Bar & b) { return a.time < b.time; });
  std::vector<Bar> kept;
  for (const Bar & b : s.bars) {
    if (!kept.empty() && kept.back().time == b.time) kept.back() = b;  // the later row of one start stands
    else kept.push_back(b);
  }
  s.bars = std::move(kept);
  std::vector<int64_t> steps;
  for (const Bar & b : s.bars)
    if (b.end > b.time) steps.push_back(b.end - b.time);
  if (steps.empty())
    for (size_t i = 1; i < s.bars.size(); i++) steps.push_back(s.bars[i].time - s.bars[i - 1].time);
  if (!steps.empty()) {
    std::nth_element(steps.begin(), steps.begin() + static_cast<std::ptrdiff_t>(steps.size() / 2), steps.end());
    s.barSeconds = steps[steps.size() / 2];
  }
}

void add(Context & ctx, Series s, Shape shape) {
  finish(s);
  if (ctx.result.series.empty()) ctx.result.shape = shape;
  ctx.result.series.push_back(std::move(s));
}

using Labels = std::vector<std::pair<std::string, std::string>>;

bool readNode(const json & node, const Labels & labels, Shape inherited, Context & ctx, std::string & why);

// Rows that are structs with a time: the series, one for each combination of the labels the rows differ in.
bool readStructs(const json & node, const Labels & labels, Shape shape, Context & ctx, std::string & why) {
  std::vector<const json *> rows;
  for (const json & row : node)
    if (row.is_object()) rows.push_back(&row);
    else ctx.skipped++;
  Columns columns;
  if (!columnsOf(rows, ctx.hints, columns, why, ctx)) return false;
  std::vector<Series> out;
  std::map<std::vector<std::string>, size_t> at;
  for (const json * row : rows) {
    bool ok = false;
    const Bar bar = barOf(*row, columns, ok);
    if (!ok) {
      ctx.skipped++;
      continue;
    }
    std::vector<std::string> values;
    for (const std::string & key : columns.labels) {
      const auto it = row->find(key);
      values.push_back(it != row->end() && it->is_string() ? it->get<std::string>() : std::string());
    }
    const auto found = at.find(values);
    size_t index = 0;
    if (found == at.end()) {
      index = out.size();
      at[values] = index;
      Series s;
      s.labels = labels;
      for (size_t i = 0; i < columns.labels.size(); i++) s.labels.push_back({columns.labels[i], values[i]});
      out.push_back(std::move(s));
    } else {
      index = found->second;
    }
    out[index].bars.push_back(bar);
  }
  if (out.empty()) {
    Series s;
    s.labels = labels;
    out.push_back(std::move(s));
  }
  // A label that is the same in every series does not tell them apart: it stays, as what the answer says of them.
  if (out.size() > 1) ctx.result.notes.push_back("the rows are of " + std::to_string(out.size()) + " series, told apart by " + [&] {
                                                   std::string names;
                                                   for (const std::string & k : columns.labels) names += (names.empty() ? "" : ", ") + k;
                                                   return names;
                                                 }());
  for (Series & s : out) add(ctx, std::move(s), shape);
  return true;
}

bool readRowsOfArrays(const json & node, const Labels & labels, Shape inherited, Context & ctx, std::string & why) {
  const size_t width = node[0].size();
  Series s;
  s.labels = labels;
  for (const json & row : node) {
    Bar b;
    Timestamp t = 0;
    if (!row.is_array() || row.size() != width || !timeOf(row[0], t)) {
      ctx.skipped++;
      continue;
    }
    b.time = t;
    switch (width) {
      case 2: b.close = numberOrUndefined(row[1]); break;
      case 5:
      case 6:
        b.open = numberOrUndefined(row[1]);
        b.high = numberOrUndefined(row[2]);
        b.low = numberOrUndefined(row[3]);
        b.close = numberOrUndefined(row[4]);
        if (width == 6) b.volume = numberOrUndefined(row[5]);
        break;
      default:
        why = "the rows are arrays of " + std::to_string(width) + ": two (time, value), five (time, open, high, low, close) or six (and a volume) are understood";
        return false;
    }
    if (!isDefined(b.close)) {
      ctx.skipped++;
      continue;
    }
    s.bars.push_back(b);
  }
  add(ctx, std::move(s), inherited == Shape::Named || inherited == Shape::Nested ? inherited : Shape::RowsOfArrays);
  return true;
}

bool readColumns(const json & node, const Labels & labels, Shape inherited, Context & ctx, std::string & why) {
  // The same as rows of structs, once turned over.
  json rows = json::array();
  size_t length = 0;
  for (const auto & item : node.items()) length = item.value().size();
  for (size_t i = 0; i < length; i++) {
    json row = json::object();
    for (const auto & item : node.items()) row[item.key()] = item.value()[i];
    rows.push_back(std::move(row));
  }
  return readStructs(rows, labels, inherited == Shape::Named || inherited == Shape::Nested ? inherited : Shape::Columns, ctx, why);
}

// Rows that are structs and have no time but hold the rows of a series (or several) in a member: each is a series, labelled by its text.
bool readNested(const json & node, const Labels & labels, Context & ctx, std::string & why) {
  for (const json & holder : node) {
    if (!holder.is_object()) {
      ctx.skipped++;
      continue;
    }
    Labels own = labels;
    const json * child = nullptr;
    for (const auto & item : holder.items()) {
      if (item.value().is_string()) own.push_back({item.key(), item.value().get<std::string>()});
      else if (seriesLike(item.value()) || (item.value().is_array() && item.value().empty())) {
        if (child) {
          why = "a row holds more than one array of rows (" + item.key() + "): say which with the rows hint";
          return false;
        }
        child = &item.value();
      }
    }
    if (!child) {
      ctx.skipped++;
      continue;
    }
    if (child->empty()) {
      Series s;
      s.labels = own;
      add(ctx, std::move(s), Shape::Nested);
      continue;
    }
    if (!readNode(*child, own, Shape::Nested, ctx, why)) return false;
  }
  return true;
}

bool hasTime(const json & row, const Hints & hints) {
  if (!row.is_object()) return false;
  for (const auto & [name, key] : hints.fields)
    if (name == "time" && row.contains(key)) return true;
  for (const auto & item : row.items())
    if (roleByName(item.key()) == Role::Time) return true;
  for (const auto & item : row.items())
    if (looksLikeTime(item.value()) && !item.value().is_number()) return true;
  return false;
}

bool readNode(const json & node, const Labels & labels, Shape inherited, Context & ctx, std::string & why) {
  if (node.is_array()) {
    if (node.empty()) {
      Series s;
      s.labels = labels;
      add(ctx, std::move(s), inherited == Shape::None ? Shape::Structs : inherited);
      return true;
    }
    if (node[0].is_array()) return readRowsOfArrays(node, labels, inherited, ctx, why);
    if (node[0].is_object()) {
      if (hasTime(node[0], ctx.hints)) return readStructs(node, labels, inherited == Shape::Nested || inherited == Shape::Named ? inherited : Shape::Structs, ctx, why);
      return readNested(node, labels, ctx, why);
    }
    why = "the rows are neither arrays nor structs";
    return false;
  }
  if (node.is_object()) {
    if (columnar(node)) return readColumns(node, labels, inherited, ctx, why);
    for (const auto & item : node.items()) {
      Labels own = labels;
      own.push_back({"key", item.key()});
      if (!readNode(item.value(), own, Shape::Named, ctx, why)) return false;
    }
    return true;
  }
  why = "the answer is not rows";
  return false;
}

// The node that holds the series: the answer's own, or the one member of it that does. Null, with `why`, where there is none or two.
const json * locate(const json & doc, std::string & why) {
  const json * node = &doc;
  for (int depth = 0; depth < 6; depth++) {
    if (node->is_array()) return node;
    if (!node->is_object()) {
      why = "the answer is not a series";
      return nullptr;
    }
    if (const std::string error = failure(*node); !error.empty()) {
      why = error;
      return nullptr;
    }
    if (columnar(*node) || named(*node)) return node;
    std::vector<const json *> candidates;
    std::vector<std::string> names;
    for (const auto & item : node->items())
      if (seriesLike(item.value()) || holdsSomething(item.value()) || (item.value().is_array() && item.value().empty())) {
        candidates.push_back(&item.value());
        names.push_back(item.key());
      }
    if (candidates.empty()) {
      why = "no series in the answer";
      return nullptr;
    }
    if (candidates.size() > 1) {
      std::vector<const json *> preferred;
      for (size_t i = 0; i < candidates.size(); i++)
        if (preferredName(names[i]) && !candidates[i]->empty()) preferred.push_back(candidates[i]);
      if (preferred.size() == 1) candidates = preferred;
      else {
        std::string list;
        for (const std::string & n : names) list += (list.empty() ? "" : ", ") + n;
        why = "ambiguous: the answer holds more than one series (" + list + "); say which with the rows hint";
        return nullptr;
      }
    }
    node = candidates[0];
  }
  why = "the series is nested too deep";
  return nullptr;
}

}  // namespace

std::string Series::label(const std::string & key) const {
  for (const auto & [k, v] : labels)
    if (k == key) return v;
  return {};
}

bool parseTime(const std::string & in, Timestamp & out) {
  size_t a = 0, b = in.size();
  while (a < b && std::isspace(static_cast<unsigned char>(in[a]))) a++;
  while (b > a && std::isspace(static_cast<unsigned char>(in[b - 1]))) b--;
  const std::string s = in.substr(a, b - a);
  if (s.empty()) return false;
  if (std::all_of(s.begin(), s.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) || c == '.'; })) {
    char * end = nullptr;
    const double d = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size() || !std::isfinite(d) || d <= 0) return false;
    out = ofNumber(d);
    return true;
  }
  if (s.size() == 10) {
    const auto day = dayNumberOfIsoDate(s);
    if (!day) return false;
    out = (*day - kUnixEpochDayNumber) * 86400;
    return true;
  }
  if (s.size() < 19) return false;
  size_t at = 19;
  if (at < s.size() && s[at] == '.')
    while (++at < s.size() && std::isdigit(static_cast<unsigned char>(s[at]))) {}
  const auto instant = instantOfIsoTimestamp(at == s.size() ? s + "Z" : s);  // no zone: UTC
  if (!instant) return false;
  out = *instant;
  return true;
}

Result read(const std::string & body, const Hints & hints) {
  Result result;
  const json doc = json::parse(body, nullptr, /*allow_exceptions=*/false);
  if (doc.is_discarded()) {
    result.error = "not JSON";
    return result;
  }
  std::string why;
  const json * node = nullptr;
  if (!hints.rows.empty()) {
    try {
      const json::json_pointer pointer(hints.rows);
      if (doc.contains(pointer)) node = &doc.at(pointer);
    } catch (const std::exception &) {
    }
    if (!node) {
      result.error = "nothing at " + hints.rows;
      return result;
    }
  } else {
    node = locate(doc, why);
    if (!node) {
      result.error = why;
      return result;
    }
  }
  Context ctx{hints, result};
  if (!readNode(*node, {}, Shape::None, ctx, why)) {
    result.series.clear();
    result.shape = Shape::None;
    result.error = why;
    return result;
  }
  if (result.series.empty()) {
    result.error = "no series in the answer";
    return result;
  }
  if (ctx.skipped > 0) result.notes.push_back(std::to_string(ctx.skipped) + " rows that are not bars left out");
  if (!ctx.unused.empty()) {
    std::string names;
    for (const std::string & k : ctx.unused) names += (names.empty() ? "" : ", ") + k;
    result.notes.push_back("fields not used: " + names);
  }
  result.ok = true;
  return result;
}

}  // namespace series
