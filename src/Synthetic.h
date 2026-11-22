#pragma once

#include "model/Store.h"
#include "source/Source.h"
#include "source/InstrumentLists.h"

// Fills the store with made-up daily candles for the instruments of `list`, all from the dataset "synthetic",
// the same every run.
void fillSyntheticData(Store & store, const InstrumentList & list);

// The same as a source, for runs without a network.
class SyntheticSource : public Source {
 public:
  // Follows `list`, or the lists on disk that `spec` names (the same ones Yahoo is asked for).
  explicit SyntheticSource(InstrumentList list) : list_(std::move(list)) {}
  explicit SyntheticSource(ListSpec spec) : spec_(std::move(spec)), fromFiles_(true) {}

  std::string name() const override { return "synthetic"; }
  std::string description() const override { return "Made-up prices, the same every run"; }
  void load(Store & store, Http &, SourceListener & listener) override {
    InstrumentList list = list_;
    if (fromFiles_) {
      std::string error;
      list = resolveList(spec_, error);
      if (list.empty()) {
        listener.message("synthetic: " + error);
        return;
      }
    }
    fillSyntheticData(store, list);
    listener.changed();
  }

 private:
  InstrumentList list_;
  ListSpec spec_;
  bool fromFiles_ = false;
};
