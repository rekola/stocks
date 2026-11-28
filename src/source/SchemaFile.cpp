#include "source/SchemaFile.h"

#include "source/DataDirectory.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

bool readSchemaFile(const std::string & directory, const std::string & nameOrPath, std::string & text, std::string & path, std::string & error) {
  std::filesystem::path file = nameOrPath;
  std::error_code ec;
  if (nameOrPath.find_first_of("/\\") == std::string::npos && file.extension() != ".json") {
    file.clear();
    for (const std::filesystem::path & root : dataRoots(executableDirectory()))
      if (std::filesystem::is_regular_file(root / directory / (nameOrPath + ".json"), ec)) {
        file = root / directory / (nameOrPath + ".json");
        break;
      }
    if (file.empty()) {
      error = "there is no schema \"" + nameOrPath + "\" (data/" + directory + "/" + nameOrPath + ".json), nor a file of that name";
      return false;
    }
  }
  std::ifstream in(file);
  if (!in) {
    error = "cannot read the schema file " + file.string();
    return false;
  }
  std::stringstream s;
  s << in.rdbuf();
  text = s.str();
  path = file.string();
  return true;
}

std::vector<std::string> schemaNames(const std::string & directory) {
  std::vector<std::string> names;
  std::error_code ec;
  const std::filesystem::path dir = findDataDirectory(directory);
  if (dir.empty()) return names;
  for (const auto & entry : std::filesystem::directory_iterator(dir, ec))
    if (entry.is_regular_file(ec) && entry.path().extension() == ".json") names.push_back(entry.path().stem().string());
  std::sort(names.begin(), names.end());
  return names;
}
