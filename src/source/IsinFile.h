#pragma once

#include "model/IsinRegistry.h"

#include <string>

// The registry of the ISINs of what has none, `data/isins/registry.csv` beside the program (DataDirectory.h). False, with `error` saying what
// is wrong (no file, or the line of a row that is not valid or collides with another), where it cannot be had.
bool loadIsinRegistry(IsinRegistry &, std::string & error);
