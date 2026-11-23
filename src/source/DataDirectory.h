#pragma once

#include <filesystem>
#include <string>
#include <vector>

// Where the data the program ships with (data/lists, data/funds) is, wherever the program is: in the source tree it was built from, in a
// build directory (a copy is made beside the program when it is built), or installed (the data is in share/stocks, the program in bin).

// The directory the program is in; empty where the system does not say.
std::filesystem::path executableDirectory();

// The directories that may hold the data, best first: `data` in the current directory (to try a change without building), the data
// beside the program (the build directory), share/stocks beside the program's directory (an installation), and the source tree the program was built
// from.
std::vector<std::filesystem::path> dataRoots(const std::filesystem::path & executableDirectory);

// The first of `roots` that has a directory `name` (lists, funds); empty if none has.
std::filesystem::path findIn(const std::vector<std::filesystem::path> & roots, const std::string & name);

// The data directory `name` for this program, as the above say.
std::filesystem::path findDataDirectory(const std::string & name);
