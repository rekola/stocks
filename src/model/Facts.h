#pragma once

#include "model/Types.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

// What is kept of the node attributes between runs, and the interface of the place it is kept in. Nothing here knows
// SQLite: a store backed by a server writes the same facts somewhere else.
//
// A fact is kept under the key of its node (NodeKey.h): the same number in every run, so nothing has to be matched up when the
// facts are read, and a node is not made by being read: a node is what is said of it and the edges that have it as an end.

// One thing said of a node, as it is kept.
struct StoredFact {
  uint64_t node = 0;  // the node's key; the kind of node is in it
  std::string predicate;
  Value value;              // never NaN: an undefined number is not said, so it is not kept
  TimestampMs updated = 0;  // when a source last said it (milliseconds UTC), whether or not the value changed
};

// What one write to the store asks the disk to do. The two lists are told apart so that a writer can be quick with what changed
// and slow with what was only said again.
struct FactBatch {
  std::vector<StoredFact> changed;    // a new value: kept soon
  std::vector<StoredFact> rechecked;  // the same value said again, only `updated` moved: may wait and be written with others
  bool empty() const { return changed.empty() && rechecked.empty(); }
};

// Where facts are kept between runs. The calls block on the medium (FactWriter keeps the store from waiting on them) and none
// throws: an implementation that cannot read or write says so through its own log and carries on, because the application
// runs without a place to keep things.
class FactStore {
 public:
  virtual ~FactStore() = default;
  // Everything there is, in no particular order, read once at start-up.
  virtual std::vector<StoredFact> loadAll() = 0;
  // Applies a batch as one unit: the facts put in place of those with the same (node, predicate).
  virtual void apply(const FactBatch &) = 0;
};
