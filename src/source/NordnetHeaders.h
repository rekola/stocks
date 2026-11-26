#pragma once

#include "source/Http.h"

#include <string>

// The headers Nordnet's endpoints expect of a browser on Nordnet's site (a Chrome on Windows, see browserHeaders), in one place; no
// cookie is sent. The site is www.nordnet.fi, so the search there is same-origin and api.prod.nntech.io is cross-site. Only the
// Nordnet adapter's files include this.
namespace nordnet {

// The instrument search (the lists).
inline HttpHeaders searchHeaders(const std::string & url) {
  return browserHeaders(url, {{"Accept", "application/json"},
                              {"Accept-Language", "en-US,en;q=0.9,fi;q=0.8"},
                              {"Origin", "https://www.nordnet.fi"},
                              {"Referer", "https://www.nordnet.fi/"},
                              {"Client-Id", "NEXT"}});
}

// The indicator lists and the price time series: they answer to a Nordic locale only (the names come in that language), and the
// language asked for is that one too.
inline HttpHeaders marketDataHeaders(const std::string & url) {
  return browserHeaders(url, {{"Accept-Language", "fi-FI,fi;q=0.9"},
                              {"Origin", "https://www.nordnet.fi"},
                              {"Referer", "https://www.nordnet.fi/"},
                              {"x-locale", "fi-FI"}});
}

// An instrument's own page: the same, and the descriptions come in the language asked for.
inline HttpHeaders detailHeaders(const std::string & url) {
  return browserHeaders(url, {{"Accept-Language", "en-US,en;q=0.9,fi;q=0.8"},
                              {"Origin", "https://www.nordnet.fi"},
                              {"Referer", "https://www.nordnet.fi/"},
                              {"x-locale", "fi-FI"}});
}

}  // namespace nordnet
