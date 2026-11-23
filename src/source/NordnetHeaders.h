#pragma once

#include "source/Http.h"

// The headers Nordnet's endpoints expect of a browser, in one place; no cookie is sent. Only the Nordnet adapter's files include this.
namespace nordnet {

// The instrument search (the lists).
inline const HttpHeaders & searchHeaders() {
  static const HttpHeaders headers = {{"Accept", "application/json"},
                                      {"Accept-Language", "en-US,en;q=0.9,fi;q=0.8"},
                                      {"Origin", "https://www.nordnet.fi"},
                                      {"Referer", "https://www.nordnet.fi/"},
                                      {"Client-Id", "NEXT"}};
  return headers;
}

// The indicator lists and the price time series: they answer to a Nordic locale only (the names come in that language).
inline const HttpHeaders & marketDataHeaders() {
  static const HttpHeaders headers = {{"Accept", "*/*"},
                                      {"Origin", "https://www.nordnet.fi"},
                                      {"Referer", "https://www.nordnet.fi/"},
                                      {"x-locale", "fi-FI"}};
  return headers;
}

// An instrument's own page: the same, and the descriptions come in the language asked for.
inline const HttpHeaders & detailHeaders() {
  static const HttpHeaders headers = {{"Accept", "*/*"},
                                      {"Accept-Language", "en-US,en;q=0.9,fi;q=0.8"},
                                      {"Origin", "https://www.nordnet.fi"},
                                      {"Referer", "https://www.nordnet.fi/"},
                                      {"x-locale", "fi-FI"}};
  return headers;
}

}  // namespace nordnet
