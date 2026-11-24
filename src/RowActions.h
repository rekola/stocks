#pragma once

// The buttons at the end of a row, by their place in the column: the info button on every row, and buy and sell on a row that can
// be bought (portfolio::tradable). The Table asks the model which a row has (TableModel::buttonShown); Browser says what each runs.
inline constexpr int kInfoButton = 0, kBuyButton = 1, kSellButton = 2;
