#include "source/IsinFile.h"

#include "source/DataDirectory.h"

#include <fstream>
#include <sstream>

bool loadIsinRegistry(IsinRegistry & registry, std::string & error) {
  const std::filesystem::path dir = findDataDirectory("isins");
  if (dir.empty()) {
    error = "no data/isins directory found";
    return false;
  }
  std::ifstream in(dir / "registry.csv");
  if (!in) {
    error = (dir / "registry.csv").string() + " cannot be read";
    return false;
  }
  std::stringstream text;
  text << in.rdbuf();
  if (!IsinRegistry::parse(text.str(), registry, error)) {
    error = (dir / "registry.csv").string() + ", " + error;
    return false;
  }
  return true;
}
