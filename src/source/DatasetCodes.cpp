#include "source/Source.h"

#include <map>

std::string datasetCodeClash(const std::vector<std::unique_ptr<Source>> & sources) {
  std::map<std::string, std::string> writtenBy;  // dataset code to the source that has it
  for (const auto & source : sources)
    for (const std::string & code : source->datasetCodes()) {
      const auto [at, fresh] = writtenBy.emplace(code, source->name());
      if (!fresh) return "the sources " + at->second + " and " + source->name() + " both write the dataset " + code + ", so one would put its data under the other's";
    }
  return {};
}
