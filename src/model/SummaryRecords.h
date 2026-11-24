#pragma once

#include "model/Summary.h"

#include <string>
#include <string_view>
#include <vector>

// The records a queue is kept as, and the rules for a queue's bars. Little-endian, whatever the machine.
//
//   a candle record (everything except a fund and an index), 28 bytes:
//       uint32 slot       the bar's key: a slot number for an intraday resolution, a day number for a date resolution
//       double open
//       float  high, low, close   each as the difference from the open
//       uint32 volume     0 is no volume; a larger one saturates at the maximum; fractions are rounded
//     A bar with no open takes its close as its base, with its high and low deltas undefined (NaN).
//   a value record (a fund's NAV, an index's value of the day), 12 bytes:
//       uint32 slot, double value
//     decoded as a bar whose open, high, low and close are that value and whose volume is undefined.
//
// A queue is a blob: a version byte, a kind byte and the records. The bars of the model stay as they are (doubles); `normalized()` is
// what one of them reads back as, so that "the same bar again" is decided on the stored form.
namespace summary {

inline constexpr uint8_t kFormat = 1;
inline constexpr size_t kCandleRecordBytes = 28, kValueRecordBytes = 12;
inline constexpr uint32_t kMaxVolume = 0xFFFFFFFFu;

// Whether a bar can be kept: a key that fits 32 bits and a close. One that cannot is not in a queue (it stays in the data).
bool fits(const Candle &);
// The bar as it is read back from its record: the one the memory holds, so that two bars are the same where their records are.
Candle normalized(QueueKind, const Candle &);
// Whether two bars are the same bar (undefined numbers are the same).
bool sameBar(const Candle &, const Candle &);

// The queue as a blob. Bars that do not fit are left out.
std::string pack(QueueKind, const std::vector<Candle> & bars);
// The queue of a blob; false where it is none of ours (another version, a length that is no whole number of records).
bool unpack(std::string_view blob, QueueKind & kind, std::vector<Candle> & bars);

// The instant a bar's key stands for, in seconds: the start of an intraday slot, the midnight (UTC) of a day.
int64_t instantOf(Resolution, int64_t key);
// Puts a bar in a time-ordered list: in place of the one with its key, else where it belongs.
void put(std::vector<Candle> & bars, const Candle &);
// Cuts a queue to the window of `keep` seconds measured back from its last bar (not from the clock), by popping from the old end.
// How many were popped.
size_t trim(std::vector<Candle> & bars, Resolution, int64_t keep);
// What a stored queue becomes when `b` is said after it (replacing it, or merged into it and cut).
void fold(StoredQueue & a, StoredQueue && b);

}  // namespace summary
