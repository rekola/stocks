#include "market/Zip.h"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace market {

namespace {

uint16_t u16(std::string_view s, size_t at) {
  return static_cast<uint16_t>(static_cast<uint8_t>(s[at]) | static_cast<uint8_t>(s[at + 1]) << 8);
}
uint32_t u32(std::string_view s, size_t at) { return u16(s, at) | static_cast<uint32_t>(u16(s, at + 2)) << 16; }

void put16(std::string & out, uint16_t v) {
  out.push_back(static_cast<char>(v & 0xff));
  out.push_back(static_cast<char>(v >> 8));
}
void put32(std::string & out, uint32_t v) {
  put16(out, static_cast<uint16_t>(v & 0xffff));
  put16(out, static_cast<uint16_t>(v >> 16));
}

}  // namespace

bool listZip(std::string_view zip, std::vector<ZipEntry> & entries, std::string & error) {
  entries.clear();
  constexpr size_t kEndSize = 22;
  if (zip.size() < kEndSize) {
    error = "not a zip: too short";
    return false;
  }
  // The end-of-central-directory record is the last 22 bytes unless a comment follows it (at most 65535 bytes).
  size_t end = std::string_view::npos;
  const size_t lowest = zip.size() > kEndSize + 65535 ? zip.size() - kEndSize - 65535 : 0;
  for (size_t at = zip.size() - kEndSize + 1; at-- > lowest;)
    if (u32(zip, at) == 0x06054b50) {
      end = at;
      break;
    }
  if (end == std::string_view::npos) {
    error = "not a zip: no end record";
    return false;
  }
  const size_t count = u16(zip, end + 10);
  size_t at = u32(zip, end + 16);
  if (count == 0xffff || at == 0xffffffffu) {
    error = "zip64 is not supported";
    return false;
  }
  for (size_t i = 0; i < count; i++) {
    if (at + 46 > zip.size() || u32(zip, at) != 0x02014b50) {
      error = "bad central directory";
      return false;
    }
    ZipEntry e;
    e.method = u16(zip, at + 10);
    e.crc = u32(zip, at + 16);
    e.compressedSize = u32(zip, at + 20);
    e.size = u32(zip, at + 24);
    const size_t nameLength = u16(zip, at + 28), extraLength = u16(zip, at + 30), commentLength = u16(zip, at + 32);
    e.localHeaderOffset = u32(zip, at + 42);
    if (at + 46 + nameLength + extraLength + commentLength > zip.size()) {
      error = "bad central directory";
      return false;
    }
    e.name = std::string(zip.substr(at + 46, nameLength));
    entries.push_back(std::move(e));
    at += 46 + nameLength + extraLength + commentLength;
  }
  return true;
}

bool readZipEntry(std::string_view zip, const ZipEntry & entry, std::string & data, std::string & error) {
  data.clear();
  const size_t local = entry.localHeaderOffset;
  if (local + 30 > zip.size() || u32(zip, local) != 0x04034b50) {
    error = "bad local header of " + entry.name;
    return false;
  }
  const size_t start = local + 30 + u16(zip, local + 26) + u16(zip, local + 28);
  if (start + entry.compressedSize > zip.size()) {
    error = "entry " + entry.name + " runs past the end of the file";
    return false;
  }
  const std::string_view packed = zip.substr(start, entry.compressedSize);
  if (entry.method == 0) {
    data.assign(packed);
  } else if (entry.method == 8) {
    data.resize(entry.size);
    z_stream stream{};
    if (inflateInit2(&stream, -15) != Z_OK) {
      error = "zlib: cannot start";
      return false;
    }
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(packed.data()));
    stream.avail_in = static_cast<uInt>(packed.size());
    stream.next_out = reinterpret_cast<Bytef *>(data.data());
    stream.avail_out = static_cast<uInt>(data.size());
    const int status = inflate(&stream, Z_FINISH);
    const bool whole = status == Z_STREAM_END && stream.total_out == entry.size;
    inflateEnd(&stream);
    if (!whole) {
      error = "cannot inflate " + entry.name;
      data.clear();
      return false;
    }
  } else {
    error = "entry " + entry.name + " uses compression method " + std::to_string(entry.method);
    return false;
  }
  if (data.size() != entry.size || crc32(0, reinterpret_cast<const Bytef *>(data.data()), static_cast<uInt>(data.size())) != entry.crc) {
    error = "entry " + entry.name + " does not match its checksum";
    data.clear();
    return false;
  }
  return true;
}

const ZipEntry * findZipEntry(const std::vector<ZipEntry> & entries, std::string_view suffix) {
  for (const ZipEntry & e : entries) {
    if (e.name.size() < suffix.size()) continue;
    const std::string_view tail = std::string_view(e.name).substr(e.name.size() - suffix.size());
    if (std::equal(tail.begin(), tail.end(), suffix.begin(), [](char a, char b) { return std::tolower(a) == std::tolower(b); })) return &e;
  }
  return nullptr;
}

std::string makeZip(const std::vector<std::pair<std::string, std::string>> & files, bool compress) {
  std::string out, central;
  for (const auto & [name, content] : files) {
    std::string packed = content;
    uint16_t method = 0;
    if (compress) {
      z_stream stream{};
      deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
      std::string buffer(deflateBound(&stream, static_cast<uLong>(content.size())), '\0');
      stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(content.data()));
      stream.avail_in = static_cast<uInt>(content.size());
      stream.next_out = reinterpret_cast<Bytef *>(buffer.data());
      stream.avail_out = static_cast<uInt>(buffer.size());
      deflate(&stream, Z_FINISH);
      buffer.resize(stream.total_out);
      deflateEnd(&stream);
      packed = std::move(buffer);
      method = 8;
    }
    const uint32_t crc = static_cast<uint32_t>(crc32(0, reinterpret_cast<const Bytef *>(content.data()), static_cast<uInt>(content.size())));
    const uint32_t offset = static_cast<uint32_t>(out.size());
    put32(out, 0x04034b50);
    put16(out, 20), put16(out, 0), put16(out, method), put16(out, 0), put16(out, 0x21);
    put32(out, crc), put32(out, static_cast<uint32_t>(packed.size())), put32(out, static_cast<uint32_t>(content.size()));
    put16(out, static_cast<uint16_t>(name.size())), put16(out, 0);
    out += name;
    out += packed;
    put32(central, 0x02014b50);
    put16(central, 20), put16(central, 20), put16(central, 0), put16(central, method), put16(central, 0), put16(central, 0x21);
    put32(central, crc), put32(central, static_cast<uint32_t>(packed.size())), put32(central, static_cast<uint32_t>(content.size()));
    put16(central, static_cast<uint16_t>(name.size())), put16(central, 0), put16(central, 0), put16(central, 0), put16(central, 0);
    put32(central, 0), put32(central, offset);
    central += name;
  }
  const uint32_t centralOffset = static_cast<uint32_t>(out.size());
  out += central;
  put32(out, 0x06054b50);
  put16(out, 0), put16(out, 0), put16(out, static_cast<uint16_t>(files.size())), put16(out, static_cast<uint16_t>(files.size()));
  put32(out, static_cast<uint32_t>(central.size())), put32(out, centralOffset), put16(out, 0);
  return out;
}

}  // namespace market
