#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Reading a zip file held in memory: the entries' names and sizes, and the contents of one. Stored and deflated entries (the
// two methods there are in practice); zip64 is not read. Needs zlib. Writing is only for tests (`makeZip`).
namespace market {

struct ZipEntry {
  std::string name;
  uint64_t compressedSize = 0, size = 0;
  uint16_t method = 0;
  uint32_t crc = 0;
  uint64_t localHeaderOffset = 0;
};

// The entries of a zip, in the order of its central directory. False, with `error`, where `zip` is not a whole zip.
bool listZip(std::string_view zip, std::vector<ZipEntry> & entries, std::string & error);
// The contents of an entry, checked against its size and CRC.
bool readZipEntry(std::string_view zip, const ZipEntry & entry, std::string & data, std::string & error);
// The first entry whose name ends in `suffix` (compared without case); nullptr for none.
const ZipEntry * findZipEntry(const std::vector<ZipEntry> & entries, std::string_view suffix);

// A zip of the given entries (name and contents), each stored or deflated: for tests.
std::string makeZip(const std::vector<std::pair<std::string, std::string>> & files, bool deflate);

}  // namespace market
