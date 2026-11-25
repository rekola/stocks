#pragma once

// What the sources of metal prices share: a price is for the unit that goes with the instrument's ISIN, whatever the provider quotes it in (Nordnet's
// copper is per metric tonne, so a provider that quotes it per pound is multiplied to be per tonne).
inline constexpr double kPoundsPerTonne = 1000.0 / 0.45359237;  // a metric tonne is 1000 kg, a pound 0.45359237 kg
