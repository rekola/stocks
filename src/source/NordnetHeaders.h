#pragma once

#include "source/Http.h"

// The headers Nordnet's endpoints expect of a browser (a Chrome on Windows, see browserHeaders), in one place; no cookie is sent. Only the Nordnet adapter's files include this.
namespace nordnet {

// The instrument search (the lists).
inline const HttpHeaders & searchHeaders() {
  static const HttpHeaders headers = browserHeaders({{"Accept", "application/json"},
                                      {"Accept-Language", "en-US,en;q=0.9,fi;q=0.8"},
                                      {"Origin", "https://www.nordnet.fi"},
                                      {"Referer", "https://www.nordnet.fi/"},
                                      {"Client-Id", "NEXT"},
                                      {"Sec-Fetch-Site", "same-origin"}});
  return headers;
}

// The indicator lists and the price time series: they answer to a Nordic locale only (the names come in that language).
inline const HttpHeaders & marketDataHeaders() {
  static const HttpHeaders headers = browserHeaders({{"Accept", "*/*"},
                                      {"Origin", "https://www.nordnet.fi"},
                                      {"Referer", "https://www.nordnet.fi/"},
                                      {"x-locale", "fi-FI"},
                                      {"Sec-Fetch-Site", "same-origin"}});
  return headers;
}

// An instrument's own page: the same, and the descriptions come in the language asked for.
inline const HttpHeaders & detailHeaders() {
  static const HttpHeaders headers = browserHeaders({{"Accept", "*/*"},
                                      {"Accept-Language", "en-US,en;q=0.9,fi;q=0.8"},
                                      {"Origin", "https://www.nordnet.fi"},
                                      {"Referer", "https://www.nordnet.fi/"},
                                      {"x-locale", "fi-FI"},
                                      {"Sec-Fetch-Site", "same-origin"}});
  return headers;
}

}  // namespace nordnet
