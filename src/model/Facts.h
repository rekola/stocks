#pragma once

#include "model/Types.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

// What is kept of the node attributes between runs, and the interface of the place it is kept in. Nothing here knows
// SQLite: a store backed by a server writes the same facts somewhere else.
//
// Node ids (`NodeRef`) are surrogates made per run, so a fact is kept under a type and a key that identify its node: an
// instrument's ISIN, a currency's code, a market's surrogate key, a dataset's name (NodeIdentity.h). The
// nodes themselves are not kept: one exists when it has edges, and edges are not kept. A fact whose node is not seen yet is
// held until the node is made again.

// What kind of thing a fact's node is, as it is kept: the kind says what its key is (NodeIdentity.h). The values are those of
// the files, and so do not change.
enum class StoredType : uint8_t {
  Instrument = 0,  // key: its ISIN
  Market = 1,      // key: a surrogate the store made
  Dataset = 2,     // key: its name
  Currency = 3,    // a currency or a unit, the instruments with no ISIN; key: its code
};

// One thing said of a node, as it is kept.
struct StoredFact {
  StoredType type = StoredType::Instrument;
  std::string node;  // the node's key: "FI0009000681", "3f9c0a7d5e21b864", "provider-a", "EUR" (by `type`)
  std::string predicate;
  Value value;              // never NaN: an undefined number is not said, so it is not kept
  TimestampMs updated = 0;  // when a source last said it (milliseconds UTC), whether or not the value changed
};

// The key of a node's facts: what to forget when its identity changes.
struct StoredNode {
  StoredType type = StoredType::Instrument;
  std::string key;
  friend bool operator==(const StoredNode &, const StoredNode &) = default;
};

// What one write to the store asks the disk to do. The two lists of facts are told apart so that a writer can be quick
// with what changed and slow with what was only said again.
struct FactBatch {
  std::vector<StoredNode> forget;     // all the facts of these nodes go (a node found by a stronger identity moves its facts)
  std::vector<StoredFact> changed;    // a new value: kept soon
  std::vector<StoredFact> rechecked;  // the same value said again, only `updated` moved: may wait and be written with others
  bool empty() const { return forget.empty() && changed.empty() && rechecked.empty(); }
};

// Where facts are kept between runs. The calls block on the medium (FactWriter keeps the store from waiting on them) and none
// throws: an implementation that cannot read or write says so through its own log and carries on, because the application
// runs without a place to keep things.
class FactStore {
 public:
  virtual ~FactStore() = default;
  // Everything there is, in no particular order, read once at start-up.
  virtual std::vector<StoredFact> loadAll() = 0;
  // Applies a batch as one unit: the nodes forgotten first, then the facts put in place of those with the same
  // (node type, node key, predicate).
  virtual void apply(const FactBatch &) = 0;
};
