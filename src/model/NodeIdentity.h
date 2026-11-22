#pragma once

#include "model/Facts.h"
#include "model/Types.h"

#include <string>
#include <utility>
#include <vector>

// What a node's facts are kept under on disk (the one place that says so). The surrogate ids of a run mean nothing in the
// next, so a fact is kept under a type and a key that do. The type says what the key is, so a key has no prefix:
//
//   instrument  its ISIN
//   currency    its code, for a currency or a unit (kind), the instruments with no ISIN
//   dataset     its name
//   market      a surrogate key the store mints when it first keeps a fact of the market (`nodeKey()` gives none): a market
//               has no identifier every one of them has (some have no MIC), so what is said of it, its MIC among that, is
//               a fact like any other. A market of a later run is found by what it is said to be (`recognisedBy()`)
//
// A node with nothing that identifies it said has no key yet: its facts are not kept until it has one. An instrument with no
// ISIN that is not a currency or a unit has none either.
StoredNode nodeKey(NodeType, const std::vector<Attribute> & said);

// What a market is recognised by, strongest first, as (predicate, text): the facts of an earlier run say which market is the
// one with this MIC, this id at a source or this name. Empty for a node that is kept under a key of its own (nodeKey()).
std::vector<std::pair<std::string, std::string>> recognisedBy(NodeType, const std::vector<Attribute> & said);

// Whether no two nodes of the type may say the same text of the predicate: what says which node is meant. Two markets do not
// have the same MIC or the same id at a source. The store keeps the first and refuses the second.
bool isUnique(NodeType, const std::string & predicate);
