#pragma once

#include "source/DatasetCodes.h"
#include "source/Source.h"

#include <chrono>
#include <string>
#include <vector>

// The European Central Bank's data portal (SDMX), its euro short-term rate dataset (EST): the €STR, the overnight rate that replaced
// Eonia, as published (an index in per cent), and the compounded €STR index (100 on 1 October 2019), the rate accrued over the real
// days by the ECB itself, so that it is comparable with a bond or a fund. One request gives the whole history, from 2019-10-01. No
// key. Both series have their own ISINs, which the ECB gives them, and one daily bar each on the market XXXX. The dataset is "ECB".
class EcbSource : public Source {
 public:
  explicit EcbSource(std::chrono::milliseconds refresh = std::chrono::hours(6)) : refresh_(refresh) {}

  std::string name() const override { return "ecb"; }
  std::vector<std::string> datasetCodes() const override { return {kEcbDataset}; }
  std::string description() const override { return "European Central Bank: the euro short-term rate (€STR) and its compounded index (no key)"; }
  void load(Store &, Http &, SourceListener &) override;
  std::chrono::milliseconds refreshInterval() const override { return refresh_; }

  // One SDMX answer (the structure-specific XML) into the store. False, with `error` set, when it holds none of the series wanted.
  // What was skipped (an observation with no date or no number) is said in `notes`.
  static bool parse(const std::string & xml, Store &, std::string & error, std::vector<std::string> & notes);

 private:
  std::chrono::milliseconds refresh_;
};
