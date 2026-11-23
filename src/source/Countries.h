#pragma once

#include <optional>
#include <string>

// The ISO 3166 alpha-2 code of a country by its name in Finnish, the language the provider's company pages are asked in
// (they come in capitals too: "SAKSA"). Nothing for a name not known; a country is never guessed.
std::optional<std::string> countryCodeOfFinnishName(const std::string & name);
