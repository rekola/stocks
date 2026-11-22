#include "model/NodeIdentity.h"

namespace {

// The text of `predicate` where it is said and not empty; else nothing.
std::string textOf(const std::vector<Attribute> & said, const char * predicate) {
  for (const Attribute & a : said)
    if (a.predicate == predicate)
      if (const auto * text = std::get_if<std::string>(&a.value)) return *text;
  return {};
}

}  // namespace

StoredNode nodeKey(NodeType type, const std::vector<Attribute> & said) {
  switch (type) {
    case NodeType::Instrument: {
      if (std::string isin = textOf(said, predicate::isin); !isin.empty()) return {StoredType::Instrument, std::move(isin)};
      const std::string kind = textOf(said, predicate::kind);
      if (kind == kindName(InstrumentKind::Currency) || kind == kindName(InstrumentKind::Unit)) {
        if (std::string code = textOf(said, predicate::symbol); !code.empty()) return {StoredType::Currency, std::move(code)};
      }
      return {};
    }
    case NodeType::Dataset: {
      if (std::string name = textOf(said, predicate::name); !name.empty()) return {StoredType::Dataset, std::move(name)};
      return {};
    }
    case NodeType::Market:
      break;  // a surrogate
  }
  return {};
}

std::vector<std::pair<std::string, std::string>> recognisedBy(NodeType type, const std::vector<Attribute> & said) {
  std::vector<std::pair<std::string, std::string>> out;
  if (type != NodeType::Market) return out;
  for (const char * p : {predicate::mic, predicate::nordnetId, predicate::name})
    for (const Attribute & a : said)
      if (a.predicate == p)
        if (const auto * text = std::get_if<std::string>(&a.value); text && !text->empty()) out.emplace_back(p, *text);
  return out;
}
