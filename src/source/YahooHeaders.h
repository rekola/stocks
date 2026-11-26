#pragma once

#include "source/Http.h"

// The headers Yahoo's chart endpoint gets from a browser (a Chrome on Windows, see browserHeaders); no cookie is sent. Only the
// Yahoo adapter's files include this.
namespace yahoo {

inline const HttpHeaders & headers() {
  static const HttpHeaders h = browserHeaders({{"Origin", "https://finance.yahoo.com"},
                                               {"Referer", "https://finance.yahoo.com/"},
                                               {"Sec-Fetch-Site", "same-site"}});
  return h;
}

}  // namespace yahoo
