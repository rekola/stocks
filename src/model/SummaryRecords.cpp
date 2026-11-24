#include "model/SummaryRecords.h"

#include "model/Dates.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace summary {

namespace {

static_assert(sizeof(float) == 4 && sizeof(double) == 8, "the records are IEEE 754");

const float kNanFloat = std::numeric_limits<float>::quiet_NaN();

uint32_t volumeOf(double v) {
  if (!isDefined(v) || v <= 0) return 0;
  if (v >= static_cast<double>(kMaxVolume)) return kMaxVolume;
  return static_cast<uint32_t>(std::llround(v));  // a fraction is rounded; below one half it is no volume
}

float delta(double value, double base) { return isDefined(value) ? static_cast<float>(value - base) : kNanFloat; }
double sum(double base, float d) { return std::isnan(d) ? kUndefined : base + static_cast<double>(d); }

// The stored form of a bar.
struct Record {
  uint32_t slot = 0;
  double open = 0;
  float high = kNanFloat, low = kNanFloat, close = kNanFloat;
  uint32_t volume = 0;
};

Record candleRecord(const Candle & c) {
  Record r;
  r.slot = static_cast<uint32_t>(c.time);
  if (isDefined(c.open)) {
    r.open = c.open;
    r.high = delta(c.high, c.open);
    r.low = delta(c.low, c.open);
    r.close = delta(c.close, c.open);
  } else {
    r.open = c.close;  // no open: the close is the base, and the high and the low are not told from it
    r.close = 0;
  }
  r.volume = volumeOf(c.volume);
  return r;
}

Candle candleOf(const Record & r) {
  Candle c;
  c.time = r.slot;
  c.open = r.open;
  c.high = sum(r.open, r.high);
  c.low = sum(r.open, r.low);
  c.close = sum(r.open, r.close);
  c.volume = r.volume ? static_cast<double>(r.volume) : kUndefined;
  return c;
}

Candle valueBar(uint32_t slot, double value) {
  Candle c;
  c.time = slot;
  c.open = c.high = c.low = c.close = value;
  c.volume = kUndefined;
  return c;
}

void put32(std::string & s, uint32_t v) {
  for (int i = 0; i < 4; i++) s.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}
void put64(std::string & s, uint64_t v) {
  for (int i = 0; i < 8; i++) s.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}
uint32_t get32(const unsigned char * p) {
  uint32_t v = 0;
  for (int i = 3; i >= 0; i--) v = v << 8 | p[i];
  return v;
}
uint64_t get64(const unsigned char * p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; i--) v = v << 8 | p[i];
  return v;
}

}  // namespace

bool fits(const Candle & c) { return c.time >= 0 && c.time <= static_cast<int64_t>(std::numeric_limits<uint32_t>::max()) && std::isfinite(c.close); }

Candle normalized(QueueKind kind, const Candle & c) {
  if (kind == QueueKind::Value) return valueBar(static_cast<uint32_t>(c.time), c.close);
  return candleOf(candleRecord(c));
}

bool sameBar(const Candle & a, const Candle & b) {
  const auto same = [](double x, double y) { return x == y || (!isDefined(x) && !isDefined(y)); };
  return a.time == b.time && same(a.open, b.open) && same(a.high, b.high) && same(a.low, b.low) && same(a.close, b.close) && same(a.volume, b.volume);
}

std::string pack(QueueKind kind, const std::vector<Candle> & bars) {
  std::string out;
  out.reserve(2 + bars.size() * (kind == QueueKind::Value ? kValueRecordBytes : kCandleRecordBytes));
  out.push_back(static_cast<char>(kFormat));
  out.push_back(static_cast<char>(kind));
  for (const Candle & c : bars) {
    if (!fits(c)) continue;
    if (kind == QueueKind::Value) {
      put32(out, static_cast<uint32_t>(c.time));
      put64(out, std::bit_cast<uint64_t>(c.close));
    } else {
      const Record r = candleRecord(c);
      put32(out, r.slot);
      put64(out, std::bit_cast<uint64_t>(r.open));
      put32(out, std::bit_cast<uint32_t>(r.high));
      put32(out, std::bit_cast<uint32_t>(r.low));
      put32(out, std::bit_cast<uint32_t>(r.close));
      put32(out, r.volume);
    }
  }
  return out;
}

bool unpack(std::string_view blob, QueueKind & kind, std::vector<Candle> & bars) {
  bars.clear();
  if (blob.size() < 2 || static_cast<uint8_t>(blob[0]) != kFormat) return false;
  const uint8_t k = static_cast<uint8_t>(blob[1]);
  if (k > static_cast<uint8_t>(QueueKind::Value)) return false;
  kind = static_cast<QueueKind>(k);
  const size_t size = kind == QueueKind::Value ? kValueRecordBytes : kCandleRecordBytes;
  if ((blob.size() - 2) % size != 0) return false;
  const auto * p = reinterpret_cast<const unsigned char *>(blob.data()) + 2;
  const size_t n = (blob.size() - 2) / size;
  bars.reserve(n);
  for (size_t i = 0; i < n; i++, p += size) {
    if (kind == QueueKind::Value) {
      bars.push_back(valueBar(get32(p), std::bit_cast<double>(get64(p + 4))));
    } else {
      Record r;
      r.slot = get32(p);
      r.open = std::bit_cast<double>(get64(p + 4));
      r.high = std::bit_cast<float>(get32(p + 12));
      r.low = std::bit_cast<float>(get32(p + 16));
      r.close = std::bit_cast<float>(get32(p + 20));
      r.volume = get32(p + 24);
      bars.push_back(candleOf(r));
    }
  }
  return true;
}

int64_t instantOf(Resolution r, int64_t key) { return isDateResolution(r) ? (key - kUnixEpochDayNumber) * 86400 : instantOfSlot(r, key); }

void put(std::vector<Candle> & bars, const Candle & c) {
  if (bars.empty() || bars.back().time < c.time) {
    bars.push_back(c);
    return;
  }
  const auto at = std::lower_bound(bars.begin(), bars.end(), c.time, [](const Candle & b, int64_t t) { return b.time < t; });
  if (at != bars.end() && at->time == c.time) *at = c;
  else bars.insert(at, c);
}

size_t trim(std::vector<Candle> & bars, Resolution r, int64_t keep) {
  if (bars.empty() || keep <= 0) return 0;
  const int64_t from = instantOf(r, bars.back().time) - keep;
  const auto first = std::find_if(bars.begin(), bars.end(), [&](const Candle & c) { return instantOf(r, c.time) >= from; });
  const size_t popped = static_cast<size_t>(first - bars.begin());
  bars.erase(bars.begin(), first);
  return popped;
}

void fold(StoredQueue & a, StoredQueue && b) {
  if (!b.merge) {
    a = std::move(b);
    return;
  }
  for (const Candle & c : b.bars) put(a.bars, c);
  trim(a.bars, b.resolution, b.keep);
  a.kind = b.kind;
  a.fetched = b.fetched;
  a.keep = b.keep;
}

}  // namespace summary
