#include "source/MarketServerSource.h"

#include "source/InstrumentLists.h"
#include "source/KnownMarkets.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <functional>
#include <set>
#include <stdexcept>

using json = nlohmann::json;

namespace {

std::string stringOf(const json & object, const char * key) {
  if (!object.is_object()) return {};
  const auto it = object.find(key);
  return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::string upper(std::string s) {
  for (char & c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

// A key's text as the unit or currency code the store makes a node of: the points and per cent units are known by their own codes, whose case
// the text of a key does not keep ("PTS").
std::string codeOf(const std::string & text) {
  const std::string u = upper(text);
  if (u == upper(kPointsUnit)) return kPointsUnit;
  if (u == upper(kPercentUnit)) return kPercentUnit;
  return text;
}

// The key of a node text of the edge: an ISIN (twelve characters) or a code.
std::optional<NodeKey> baseOrQuoteKey(const std::string & text) { return text.size() == 12 ? instrumentKey(text) : currencyKey(text); }

// The kind the server names, as the model's; none for a name the model has not got.
std::optional<InstrumentKind> kindOf(const std::string & name) {
  if (name == kindName(InstrumentKind::Other)) return InstrumentKind::Other;
  const InstrumentKind kind = kindOfName(name);
  if (kind == InstrumentKind::Other) return std::nullopt;
  return kind;
}

}  // namespace

MarketServerSource::MarketServerSource(Options options) : options_(std::move(options)) {
  while (!options_.url.empty() && options_.url.back() == '/') options_.url.pop_back();
  history_ = std::make_shared<MarketServerHistory>(options_.url, options_.history_lifetime);
}

void MarketServerSource::write(const std::string & body, Store & store, const std::function<void(const std::string &)> & said,
                               std::vector<std::string> & datasets) {
  const json doc = json::parse(body, nullptr, /*allow_exceptions=*/false);
  if (doc.is_discarded() || !doc.is_object() || !doc.contains("instruments") || !doc["instruments"].is_array())
    throw std::runtime_error("the answer is not a list of instruments");
  std::set<std::string> codes;
  store.batch([&](Batch & b) {
    for (const json & row : doc["instruments"]) {
      const std::string id = stringOf(row, "id");
      // One message for each instrument that cannot be made; the rest go on.
      const auto skip = [&](const std::string & why) { said("skipped " + (id.empty() ? std::string("an instrument with no id") : id) + ": " + why); };
      if (!row.is_object() || id.empty()) {
        skip("it has no id");
        continue;
      }
      const json edge = row.value("edge", json::object()), dataset = row.value("dataset", json::object()), listing = row.value("listing", json::object()),
                 base = row.value("base", json::object());
      const std::string baseText = stringOf(edge, "base"), quoteText = stringOf(edge, "quote"), marketText = stringOf(edge, "market"),
                        datasetText = stringOf(edge, "dataset");
      const auto baseKey = baseOrQuoteKey(baseText), quoteKey = baseOrQuoteKey(quoteText);
      const auto marketKey_ = marketKey(marketText);
      const auto datasetKey_ = datasetKey(datasetText);
      if (!baseKey || !quoteKey || !marketKey_ || !datasetKey_) {
        skip("its edge is not one this application can read");
        continue;
      }
      const std::string kindText = stringOf(base, "kind");
      const auto kind = kindOf(kindText);
      if (!kind) {
        skip("the kind " + (kindText.empty() ? std::string("(none)") : kindText) + " is not known");
        continue;
      }
      if (quoteKey->kind() == NodeKind::Instrument) {
        skip("it is quoted in an instrument, which the server does not describe");
        continue;
      }

      // The dataset as the server has it, its name too (the code itself where none was said).
      std::string datasetName = stringOf(dataset, "name");
      if (upper(datasetName) == upper(datasetText)) datasetName.clear();
      const DatasetId datasetId = b.addDataset(datasetText, datasetName);
      const MarketId marketId = b.addMarket(marketText, marketNameOfMic(marketText));

      // The base node as the server made it: an instrument by its ISIN and kind, a currency or a unit by its code.
      InstrumentId baseId;
      const std::string isin = stringOf(base, "isin"), baseName = stringOf(base, "name");
      if (!isin.empty()) baseId = addInstrumentUnlessSaid(b, "", baseName, *kind, isin, stringOf(listing, "type"));
      else if (*kind == InstrumentKind::Currency) baseId = currencyNode(b, baseText);
      else if (*kind == InstrumentKind::Unit) baseId = quoteNode(b, codeOf(baseText));
      else baseId = b.addInstrument(baseText, baseName.empty() ? baseText : baseName, *kind);
      const InstrumentId quoteId = quoteNode(b, codeOf(quoteText));

      // The edge must be the server's, or a client and a server would not mean the same by a node.
      const EdgeKey key{baseId, quoteId, marketId, datasetId};
      if (!baseId.valid() || !quoteId.valid() || !marketId.valid() || !datasetId.valid() || baseId.value != baseKey->value || quoteId.value != quoteKey->value ||
          marketId.value != marketKey_->value || datasetId.value != datasetKey_->value || marketServerEdgeId(key) != id) {
        skip(isin.empty() ? "its nodes could not be made as the server's" : "its ISIN " + isin + " is refused or does not make its base");
        continue;
      }
      b.setListing({key, stringOf(listing, "symbol"), stringOf(listing, "identifier"), stringOf(listing, "order_book"), stringOf(listing, "type"),
                    stringOf(listing, "name")});
      b.addRow(key);  // a row now, with no price or trace until its bars are loaded
      codes.insert(datasetText);
    }
  });
  datasets.assign(codes.begin(), codes.end());
}

void MarketServerSource::report(SourceListener & listener, const std::string & text) {
  if (text == lastReport_) return;  // a server that stays down is said once
  lastReport_ = text;
  listener.message("market-server: " + text);
}

void MarketServerSource::load(Store & store, Http & http, SourceListener & listener) {
  if (listener.cancelled()) return;
  const HttpResult r = http.get(options_.url + "/instruments");
  if (listener.cancelled() || http.cancelled()) return;  // no answer because it was stopped: nothing to say
  if (r.status == 0 || r.status >= 500) {
    reached_ = false;
    report(listener, "no answer from " + options_.url + (r.status ? " (HTTP " + std::to_string(r.status) + ")" : r.error.empty() ? std::string() : " (" + r.error + ")") +
                         ", trying again in " + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(options_.retry).count()) + " s");
    return;
  }
  reached_ = true;
  if (r.status == 403 || r.status == 429) {
    report(listener, r.status == 429 ? "rate limited" : "refused");
    return;
  }
  if (r.status == 400) {
    const json doc = json::parse(r.body, nullptr, /*allow_exceptions=*/false);
    const std::string said = doc.is_object() ? stringOf(doc, "error") : std::string();
    report(listener, "the request was refused as wrong (a bug of the application): " + (said.empty() ? "no message" : said));
    return;
  }
  if (!r.ok()) {
    report(listener, "HTTP " + std::to_string(r.status) + " from " + options_.url + " (is it a market server?)");
    return;
  }
  const uint64_t before = store.snapshot()->version();
  std::vector<std::string> datasets;
  try {
    write(
        r.body, store,
        [&](const std::string & text) {
          // Said once each, whatever the number of times the list is read.
          const std::string id = text.substr(0, text.find(':'));
          if (skipped_.insert(id).second) listener.message("market-server: " + text);
        },
        datasets);
  } catch (const std::exception & e) {
    report(listener, e.what());
    return;
  }
  lastReport_.clear();
  history_->setDatasets(std::move(datasets));  // before the rows are told of: what is wanted of them is asked of the history
  if (store.snapshot()->version() != before) listener.changed();
}
