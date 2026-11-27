#include "market/RawCache.h"

#include <fstream>
#include <sstream>
#include <system_error>

namespace market {

bool RawCache::has(const std::string & fileName) const {
  std::error_code ec;
  return std::filesystem::is_regular_file(pathOf(fileName), ec);
}

std::optional<std::string> RawCache::read(const std::string & fileName) const {
  std::ifstream in(pathOf(fileName), std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream buffer;
  buffer << in.rdbuf();
  if (in.bad()) return std::nullopt;
  return buffer.str();
}

bool RawCache::store(const std::string & fileName, const std::string & bytes, std::string & error) const {
  std::error_code ec;
  std::filesystem::create_directories(directory_, ec);
  if (ec) {
    error = "cannot create " + directory_.string() + ": " + ec.message();
    return false;
  }
  const std::filesystem::path target = pathOf(fileName);
  const std::filesystem::path temporary = pathOf(fileName + ".part");
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.flush();
    if (!out) {
      error = "cannot write " + temporary.string();
      return false;
    }
  }
  std::filesystem::rename(temporary, target, ec);
  if (ec) {
    error = "cannot rename " + temporary.string() + ": " + ec.message();
    return false;
  }
  return true;
}

}  // namespace market
