#pragma once

// The codes of the datasets the built-in sources write (NodeKey.h: one to ten letters or digits). A code is permanent: what is
// kept under it keeps its meaning, so it is never changed, and a dataset whose meaning changes gets a new code.
inline constexpr const char * kNordnetDataset = "nordnet";
inline constexpr const char * kYahooDataset = "yahoo";
inline constexpr const char * kSyntheticDataset = "synthetic";
inline constexpr const char * kSuomenPankkiDataset = "SPFB";  // the Bank of Finland's publication of the Euribor rates
inline constexpr const char * kEcbDataset = "ECB";  // the European Central Bank's data portal
// Not a source: the dataset of what the user typed in herself, the transactions of a portfolio whose `ledger` says no other
// (portfolio/Portfolio.h). A later import of a broker's export gets a dataset of its own.
inline constexpr const char * kUserDataset = "user";
// Not a provider's: the estimate of what the physical metals (a gold bar by the gram, a gold coin by the ounce, a platinum bar) are worth, made from
// their spot prices and the factors in data/physical (PhysicalSource). A dealer's real list is a dataset of its own and wins where there is one.
inline constexpr const char * kPhysicalDataset = "PHYSICAL";
// gold-api.com (api.gold-api.com): the spot prices of metals (and a few coins) in dollars and euros, GoldPriceSource.
inline constexpr const char * kGoldPriceDataset = "GOLDPRICE";
