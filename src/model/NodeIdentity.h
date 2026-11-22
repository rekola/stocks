#pragma once

#include "model/Types.h"

#include <string>
#include <vector>

// What identifies a node from one run to the next, and so what its facts are kept under on disk (the one place that
// says so). The surrogate ids of a run mean nothing in the next.
//
//   instrument  its ISIN ("isin:..."); one without (a currency, whose symbol is its code) its symbol ("symbol:...")
//   market      its MIC ("mic:..."), else the id a source gives it ("nordnet-id:..."), else its name ("name:...")
//   dataset     its name ("name:...")
//
// These are the predicates the store resolves the nodes by, strongest first. A node with none of them said has no key yet:
// its facts are not kept until it has one, and when a stronger one is said the facts move to it.
std::string nodeKey(NodeType, const std::vector<Attribute> & said);
