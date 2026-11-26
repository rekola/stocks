#pragma once

#include <filesystem>
#include <optional>
#include <string>

// The raw files an adapter downloads, kept exactly as they came: the only copy of the finest data there is (the 1-minute quotes
// are not stored in the database), kept for training later. Files are only ever added; the one file that is replaced is a
// month still being filled, with a longer one (see `store`'s `replace`). Nothing in here deletes.
namespace market {

class RawCache {
 public:
  explicit RawCache(std::filesystem::path directory) : directory_(std::move(directory)) {}

  const std::filesystem::path & directory() const { return directory_; }
  std::filesystem::path pathOf(const std::string & fileName) const { return directory_ / fileName; }
  bool has(const std::string & fileName) const;
  // The bytes of a file, or none (not there, or not readable).
  std::optional<std::string> read(const std::string & fileName) const;
  // Writes the bytes to a temporary file beside the target and renames it over it, so a stop in the middle leaves the old file or the
  // new one, never half of one. `error` says what went wrong.
  bool store(const std::string & fileName, const std::string & bytes, std::string & error) const;

 private:
  std::filesystem::path directory_;
};

}  // namespace market
