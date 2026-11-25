#pragma once

#include "model/Types.h"

#include <functional>
#include <memory>
#include <set>
#include <utility>
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
  TimestampMs updated = 0;  // when a source last said it (milliseconds UTC), whether or not the value changed, to within the recheck interval
};

// One transaction of a portfolio as it is kept: an event on the key (portfolio, dataset), one among those with its minted `id`.
// `time` is the day of it (a day number) and `body` the rest, which only the portfolio code reads (portfolio/Transaction.h).
struct StoredEvent {
  uint64_t portfolio = 0, dataset = 0, id = 0;
  int64_t time = 0;
  std::string body;
};

// What one write to the store asks the disk to do. The first two lists are told apart so that a writer can be quick with what
// changed and slow with what was only said again. The rest is the user's own data (a portfolio's), which is kept in a file of
// its own and written at once: facts taken back, a node taken back whole (its facts and its events), and transactions put and
// taken back. In a batch, what is taken back goes first, then what is put.
struct FactBatch {
  std::vector<StoredFact> changed;    // a new value: kept soon
  std::vector<StoredFact> rechecked;  // the same value said again, only `updated` moved: may wait and be written with others
  std::vector<std::pair<uint64_t, std::string>> removed;  // (node, predicate) said no more
  std::vector<uint64_t> removed_nodes;                     // everything of the node: its facts and its events
  std::vector<StoredEvent> events;                         // put: replaces the one with the same (portfolio, dataset, id)
  std::vector<StoredEvent> events_removed;                 // taken back: only the three keys are read
  bool empty() const {
    return changed.empty() && rechecked.empty() && removed.empty() && removed_nodes.empty() && events.empty() && events_removed.empty();
  }
};

// Where facts are kept between runs. The calls block on the medium (FactWriter keeps the store from waiting on them) and none
// throws: an implementation that cannot read or write says so through its own log and carries on, because the application
// runs without a place to keep things.
class FactStore {
 public:
  virtual ~FactStore() = default;
  // Everything there is, in no particular order, read once at start-up.
  virtual std::vector<StoredFact> loadAll() = 0;
  // The facts of these nodes only (their keys), in no particular order: what a restore that goes by the edges wants of each chunk. A node
  // with none says nothing. The default reads everything and keeps those; a store that can look a node up does.
  virtual std::vector<StoredFact> loadNodes(const std::vector<uint64_t> & nodes) {
    std::vector<StoredFact> out;
    const std::set<uint64_t> wanted(nodes.begin(), nodes.end());
    for (StoredFact & f : loadAll())
      if (wanted.count(f.node)) out.push_back(std::move(f));
    return out;
  }
  // The events that are kept, in no particular order (only the portfolio file has any).
  virtual std::vector<StoredEvent> loadEvents() { return {}; }
  // Applies a batch as one unit: the facts put in place of those with the same (node, predicate).
  virtual void apply(const FactBatch &) = 0;
};
