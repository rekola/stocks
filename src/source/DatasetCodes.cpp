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

std::string suppliesAllDataClash(const std::vector<std::unique_ptr<Source>> & sources) {
  const Source * server = nullptr;
  for (const auto & source : sources)
    if (source->suppliesAllData()) server = source.get();
  if (!server) return {};
  std::string others;
  for (const auto & source : sources)
    if (source.get() != server) others += (others.empty() ? "" : ", ") + source->name();
  if (others.empty()) return {};
  return server->name() + " supplies every dataset; it runs alone, so it cannot run with " + others +
         " (give --source " + server->name() + " by itself)";
}
