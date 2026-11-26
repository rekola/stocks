#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Reading a zip file held in memory (miniz does the work): the entries' names and sizes, and the contents of one, checked against
// its CRC. Writing is for the tests.
namespace market {

struct ZipEntry {
  std::string name;
  uint64_t size = 0;  // uncompressed
  unsigned index = 0;
};

// The files of a zip (not its directories), in order. False, with `error`, where `zip` is not a zip.
bool listZip(std::string_view zip, std::vector<ZipEntry> & entries, std::string & error);
// The contents of an entry.
bool readZipEntry(std::string_view zip, const ZipEntry & entry, std::string & data, std::string & error);
// The first entry whose name ends in `suffix` (compared without case); nullptr for none.
const ZipEntry * findZipEntry(const std::vector<ZipEntry> & entries, std::string_view suffix);

// A zip of the given files (name and contents), each stored or deflated: for tests.
std::string makeZip(const std::vector<std::pair<std::string, std::string>> & files, bool compress);

}  // namespace market
