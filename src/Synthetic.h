#pragma once

#include "model/Store.h"

// Fills the store with made-up daily candles for the three presets: one market each (Commodities, Nasdaq Helsinki,
// Currencies), all from the dataset "synthetic", the same every run.
void fillSyntheticData(Store & store);
