#pragma once

#include "model/Types.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

// What is kept of the node attributes between runs, and the interface of the place it is kept in. Nothing here knows
// SQLite: a store backed by a server writes the same facts somewhere else.
//
// Node ids (`NodeRef`) are surrogates made per run, so a fact is kept under what identifies its node: an instrument's ISIN
// (or its symbol where it has none), a market's MIC, its source's id or its name, a dataset's name (NodeIdentity.h). The
// nodes themselves are not kept: one exists when it has edges, and edges are not kept. A fact whose node is not seen yet is
// held until the node is made again.

// One thing said of a node, as it is kept.
struct StoredFact {
  NodeType type = NodeType::Instrument;
  std::string node;  // the node's key: "isin:FI0009000681", "mic:XHEL", "name:..." (NodeIdentity.h)
  std::string predicate;
  Value value;              // never NaN: an undefined number is not said, so it is not kept
  TimestampMs updated = 0;  // when a source last said it (milliseconds UTC), whether or not the value changed
};

// The key of a node's facts: what to forget when its identity changes.
struct StoredNode {
  NodeType type = NodeType::Instrument;
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
