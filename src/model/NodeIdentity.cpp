#include "model/NodeIdentity.h"

#include <initializer_list>

namespace {

// The text of the first of `predicates` that is said (and not empty), as "predicate:text".
std::string firstOf(const std::vector<Attribute> & said, std::initializer_list<const char *> predicates) {
  for (const char * p : predicates)
    for (const Attribute & a : said)
      if (a.predicate == p)
        if (const auto * text = std::get_if<std::string>(&a.value); text && !text->empty()) return std::string(p) + ':' + *text;
  return {};
}

}  // namespace

std::string nodeKey(NodeType type, const std::vector<Attribute> & said) {
  switch (type) {
    case NodeType::Instrument:
      return firstOf(said, {predicate::isin, predicate::symbol});
    case NodeType::Market:
      return firstOf(said, {predicate::mic, predicate::nordnetId, predicate::name});
    case NodeType::Dataset:
      return firstOf(said, {predicate::name});
  }
  return {};
}
