#pragma once

#include "source/DatasetCodes.h"
#include "source/Source.h"

#include <chrono>
#include <string>
#include <vector>

// The Bank of Finland's daily Euribor rates (1 week, 1, 3, 6 and 12 months), from the XML report it publishes: one request for the
// whole history it keeps (from 2021), newest first. No key. Each rate is an index, quoted in per cent as the bank gives it (not
// accumulated into a price: see the notes in the .cpp), with one daily bar of its own on the market XXXX. The dataset is "suomenpank".
class SuomenPankkiSource : public Source {
 public:
  explicit SuomenPankkiSource(std::chrono::milliseconds refresh = std::chrono::hours(6)) : refresh_(refresh) {}

  std::string name() const override { return "suomenpankki"; }
  std::vector<std::string> datasetCodes() const override { return {kSuomenPankkiDataset}; }
  std::string description() const override { return "Bank of Finland: the daily Euribor rates, 1 week to 12 months (no key)"; }
  void load(Store &, Http &, SourceListener &) override;
  std::chrono::milliseconds refreshInterval() const override { return refresh_; }

  // One report into the store. False, with `error` set, when it holds no rates. What was skipped (a rate it does not know, a day
  // or a value it cannot read) is said in `notes`, one line for each kind.
  static bool parse(const std::string & xml, Store &, std::string & error, std::vector<std::string> & notes);

 private:
  std::chrono::milliseconds refresh_;
};
