#pragma once

#include "model/Types.h"

#include <string>

// What makes a node the node it is. A node is named by its key (NodeKey.h): an instrument by its ISIN, a currency or a unit by
// its code, a dataset by its code, a market by a key minted when it was first said. Of a market no more is known than what is
// said of it, so it is found again by that: its MIC, its id at a source, its name.
//
// Whether no two nodes of the type may say the same text of the predicate: what says which node is meant. Two markets do not
// have the same MIC or the same id at a source. The store keeps the first and refuses the second.
bool isUnique(NodeType, const std::string & predicate);
