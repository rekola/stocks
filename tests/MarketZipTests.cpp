#include "TestFramework.h"
#include "market/RawCache.h"
#include "market/Zip.h"
#include "MarketTestSupport.h"

using namespace market;

TEST(market_zip_round_trip_stored_and_deflated) {
  const std::string text(5000, 'x');
  for (bool compress : {false, true}) {
    const std::string zip = makeZip({{"a.csv", text}, {"b.txt", "gaps"}}, compress);
    std::vector<ZipEntry> entries;
    std::string error, data;
    CHECK(listZip(zip, entries, error));
    CHECK(entries.size() == 2);
    const ZipEntry * csv = findZipEntry(entries, ".CSV");
    CHECK(csv && csv->name == "a.csv" && csv->size == text.size());
    CHECK(csv && readZipEntry(zip, *csv, data, error) && data == text);
    CHECK(compress == (csv && csv->compressedSize < csv->size));
  }
}

TEST(market_zip_refuses_what_is_not_a_zip_or_is_damaged) {
  std::vector<ZipEntry> entries;
  std::string error, data;
  CHECK(!listZip("", entries, error));
  CHECK(!listZip("<html>an error page</html> and some more text to be long enough", entries, error));
  std::string zip = makeZip({{"a.csv", "hello hello hello hello"}}, false);
  CHECK(listZip(zip, entries, error));
  zip[40] ^= 0x55;  // inside the data: the checksum catches it
  CHECK(!readZipEntry(zip, entries[0], data, error));
  CHECK(!listZip(zip.substr(0, zip.size() - 10), entries, error));
}

TEST(market_raw_cache_stores_whole_files_and_reads_them_back) {
  TempDir dir;
  RawCache cache(dir.path / "sub");
  CHECK(!cache.has("a.zip") && !cache.read("a.zip"));
  std::string error;
  CHECK(cache.store("a.zip", std::string("PK\0\1binary", 10), error));
  CHECK(cache.has("a.zip") && *cache.read("a.zip") == std::string("PK\0\1binary", 10));
  CHECK(cache.store("a.zip", "longer", error) && *cache.read("a.zip") == "longer");
  int files = 0;
  for (const auto & e : std::filesystem::directory_iterator(dir.path / "sub")) files += e.is_regular_file();
  CHECK(files == 1);  // no temporary file left
}
