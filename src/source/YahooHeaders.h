#pragma once

#include "source/Http.h"

#include <string>

// The headers Yahoo's chart endpoint gets from a browser on finance.yahoo.com (a Chrome on Windows, see browserHeaders); no cookie is
// sent. Only the Yahoo adapter's files include this.
namespace yahoo {

inline HttpHeaders headers(const std::string & url) {
  return browserHeaders(url, {{"Origin", "https://finance.yahoo.com"}, {"Referer", "https://finance.yahoo.com/"}});
}

}  // namespace yahoo
