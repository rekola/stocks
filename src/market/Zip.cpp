#include "market/Zip.h"

#include <miniz.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace market {

namespace {

// A reader over memory that ends with the scope.
struct Reader {
  mz_zip_archive zip;
  bool open = false;
  Reader(std::string_view data, std::string & error) {
    std::memset(&zip, 0, sizeof zip);
    open = mz_zip_reader_init_mem(&zip, data.data(), data.size(), 0) != 0;
    if (!open) error = std::string("not a zip: ") + mz_zip_get_error_string(mz_zip_get_last_error(&zip));
  }
  ~Reader() {
    if (open) mz_zip_reader_end(&zip);
  }
  Reader(const Reader &) = delete;
  Reader & operator=(const Reader &) = delete;
};

}  // namespace

bool listZip(std::string_view zip, std::vector<ZipEntry> & entries, std::string & error) {
  entries.clear();
  Reader reader(zip, error);
  if (!reader.open) return false;
  const unsigned count = mz_zip_reader_get_num_files(&reader.zip);
  for (unsigned i = 0; i < count; i++) {
    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(&reader.zip, i, &stat)) {
      error = "bad central directory";
      return false;
    }
    if (stat.m_is_directory) continue;
    entries.push_back({stat.m_filename, stat.m_uncomp_size, i});
  }
  return true;
}

bool readZipEntry(std::string_view zip, const ZipEntry & entry, std::string & data, std::string & error) {
  data.clear();
  Reader reader(zip, error);
  if (!reader.open) return false;
  data.resize(entry.size);
  // miniz checks the CRC and the size while it extracts
  if (!mz_zip_reader_extract_to_mem(&reader.zip, entry.index, data.data(), data.size(), 0)) {
    error = "cannot read " + entry.name + ": " + mz_zip_get_error_string(mz_zip_get_last_error(&reader.zip));
    data.clear();
    return false;
  }
  return true;
}

const ZipEntry * findZipEntry(const std::vector<ZipEntry> & entries, std::string_view suffix) {
  for (const ZipEntry & e : entries) {
    if (e.name.size() < suffix.size()) continue;
    const std::string_view tail = std::string_view(e.name).substr(e.name.size() - suffix.size());
    if (std::equal(tail.begin(), tail.end(), suffix.begin(), [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); })) return &e;
  }
  return nullptr;
}

std::string makeZip(const std::vector<std::pair<std::string, std::string>> & files, bool compress) {
  mz_zip_archive zip;
  std::memset(&zip, 0, sizeof zip);
  mz_zip_writer_init_heap(&zip, 0, 0);
  for (const auto & [name, content] : files)
    mz_zip_writer_add_mem(&zip, name.c_str(), content.data(), content.size(), compress ? MZ_DEFAULT_LEVEL : MZ_NO_COMPRESSION);
  void * buffer = nullptr;
  size_t size = 0;
  mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size);
  std::string out(static_cast<const char *>(buffer), size);
  mz_zip_writer_end(&zip);
  std::free(buffer);
  return out;
}

}  // namespace market
