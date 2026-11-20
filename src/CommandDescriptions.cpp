#include "CommandDescriptions.h"

#include <map>

std::string commandDescription(std::string_view name) {
  static const std::map<std::string, std::string, std::less<>> descriptions = {
      {"clear-sort", "Show the rows in the order the source gave them"},
      {"command-palette", "Search the commands and run one"},
      {"focus-table", "Move the keyboard to the table"},
      {"focus-tags", "Move the keyboard to the preset tags"},
      {"next-preset", "Show the next preset"},
      {"previous-preset", "Show the previous preset"},
      {"quit", "Leave stocks"},
      {"reverse-sort", "Reverse the order of the sorted column"},
      {"show-commodities", "Show the commodities preset"},
      {"show-currencies", "Show the currencies preset"},
      {"show-nasdaq-helsinki", "Show the Nasdaq Helsinki preset"},
      {"sort-by-base", "Sort by the base symbol, ascending"},
      {"sort-by-change", "Sort by the change over the last day, ascending"},
      {"sort-by-dataset", "Sort by the name of the dataset the row comes from, ascending"},
      {"sort-by-name", "Sort by the instrument's name, ascending"},
      {"sort-by-price", "Sort by the price, ascending"},
      {"sort-by-quote", "Sort by the quote symbol, ascending"},
  };
  auto it = descriptions.find(name);
  return it == descriptions.end() ? std::string() : it->second;
}
