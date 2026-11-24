#pragma once

// The codes of the datasets the built-in sources write (NodeKey.h: one to ten letters or digits). A code is permanent: what is
// kept under it keeps its meaning, so it is never changed, and a dataset whose meaning changes gets a new code.
inline constexpr const char * kNordnetDataset = "nordnet";
inline constexpr const char * kYahooDataset = "yahoo";
inline constexpr const char * kSyntheticDataset = "synthetic";
inline constexpr const char * kSuomenPankkiDataset = "SPFB";  // the Bank of Finland's publication of the Euribor rates
