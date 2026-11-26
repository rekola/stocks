#pragma once

#include <cstdint>
#include <string>

// What the user's own file (the portfolios) needs to be read after the keys of the codes changed. Schema 1 packed a code of a currency, a
// market, a dataset or a country in six bits a character, ten at most, with the case kept; schema 2 packs eleven in base 37, in capitals
// (NodeKey.h). A key of another kind is the same in both.

// The code a key of schema 1 stands for; empty for a key of another kind or one that is none.
std::string legacyKeyCode(uint64_t key);
// The same key in the way schema 2 packs it: the code in capitals. A key that is not one of a code (an instrument, a portfolio) or has no
// key of schema 2 is returned as it is.
uint64_t upgradeLegacyKey(uint64_t key);
