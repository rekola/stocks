#pragma once

#include <string>
#include <vector>

// Where an adapter's schema is: a file of the data directory that the adapter's kind has (data/funds, data/metals), found where the program's data is (here, beside
// the program, installed, or in the source tree), or a file by its path. The funds and the metals share it.

// The text of the schema `nameOrPath`: a name (no path, no extension) is the file `directory`/NAME.json of the data, anything else a file as it is. `path` is the
// file read. False, with `error`, where there is no such schema or it cannot be read.
bool readSchemaFile(const std::string & directory, const std::string & nameOrPath, std::string & text, std::string & path, std::string & error);

// The names of the schemas the data directory `directory` has, sorted (the file names without .json); none where there is no such directory.
std::vector<std::string> schemaNames(const std::string & directory);
