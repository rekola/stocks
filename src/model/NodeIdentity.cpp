#include "model/NodeIdentity.h"

bool isUnique(NodeType type, const std::string & p) { return type == NodeType::Market && (p == predicate::mic || p == predicate::nordnetId); }
