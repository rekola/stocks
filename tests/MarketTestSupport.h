#pragma once

#include "market/Config.h"
#include "market/Backends.h"
#include "market/Zip.h"
#include "model/Dates.h"
#include "market/offline/HistData.h"
#include "source/Http.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// A database to test on, made and removed with the object: SQLite files in a directory of their own, or a scratch MariaDB schema where the
// environment names one: MARKET_TEST_DB_USER (those runs are skipped, and say so, without it), MARKET_TEST_DB_PASSWORD, MARKET_TEST_DB_HOST,
// MARKET_TEST_DB_PORT, MARKET_TEST_DB_SOCKET; the user needs the right to make and drop schemas named market_test_*.
struct ScratchDb {
  market::DbConfig config;
  bool available = false;
  explicit ScratchDb(const std::string & type) {
    static int counter = 0;
    const std::string unique = std::to_string(getpid()) + "_" + std::to_string(counter++);
    config.type = type;
    if (type == "sqlite") {
      available = true;
      config.path = (std::filesystem::temp_directory_path() / ("market_test_" + unique)).string();
      return;
    }
    const char * user = std::getenv("MARKET_TEST_DB_USER");
    if (!user || !market::hasBackend("mariadb")) return;
    available = true;
    config.user = user;
    if (const char * v = std::getenv("MARKET_TEST_DB_PASSWORD")) config.password = v;
    if (const char * v = std::getenv("MARKET_TEST_DB_HOST")) config.host = v;
    if (const char * v = std::getenv("MARKET_TEST_DB_PORT")) config.port = std::atoi(v);
    if (const char * v = std::getenv("MARKET_TEST_DB_SOCKET")) config.socket = v;
    config.name = "market_test_" + unique;
  }
  ~ScratchDb() {
    if (!available) return;
    try {
      market::dropBackends(config);
    } catch (...) {
    }
  }
};

inline void skipWithoutDatabase() { std::printf("  SKIPPED on MariaDB: MARKET_TEST_DB_USER is not set (or this build has no MariaDB)\n"); }

// Runs `body(database, backendName)` on each database there is to test on: SQLite files, and MariaDB where there is one.
template <class F>
void forEachDatabase(F && body) {
  for (const char * type : {"sqlite", "mariadb"}) {
    ScratchDb db(type);
    if (db.available) body(db.config, type);
    else skipWithoutDatabase();
  }
}

// A directory that goes away with the object.
struct TempDir {
  std::filesystem::path path;
  TempDir() {
    static int counter = 0;
    path = std::filesystem::temp_directory_path() / ("market_test_" + std::to_string(getpid()) + "_" + std::to_string(counter++));
    std::filesystem::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

// HistData's site, made up: a page with a token for any period (as the real one gives, data or none), and a zip for a period that has
// quotes: a row every `step` minutes on weekdays, from the start of the period (the file's EST) to `lastBar` (UTC seconds).
class FakeSite : public Http {
 public:
  Timestamp lastBar = 0;  // the last quote of the site, UTC seconds
  int step = 60;          // minutes between rows
  int firstYear = 2000;
  bool tokens = true;       // the page of a file has a token (a challenge or an error page has none)
  bool pairsOnPage = true;  // the list page has pairs (two of them), else none
  int failDownloads = 0;  // POSTs to answer with a 500 before one works
  std::vector<std::string> requests;  // "GET url" and "POST url datemonth"
  std::vector<HttpHeaders> postHeaders;
  std::vector<std::string> postBodies;

  HttpResult get(const std::string & url, const HttpHeaders &) override {
    requests.push_back("GET " + url);
    HttpResult r;
    r.status = 200;
    if (!tokens && url.find("/20") != std::string::npos) {
      r.body = "<html>Just a moment...</html>";
    } else if (url.find("/1-minute-bar-quotes/") != std::string::npos && url.find("/20") != std::string::npos) {
      // .../eurusd/2026/11: a token for any period, in the order the real page has (a form with the token, twice)
      const std::string token = "0123456789abcdef0123456789abcdef";
      r.body = "<html><form id=\"file_down\"><input type=\"hidden\" name=\"tk\" id=\"tk\" value=\"" + token +
               "\" /></form><form id=\"file_status\"><input type=\"hidden\" name=\"tk\" id=\"tk\" value=\"" + token + "\" /></form></html>";
    } else {
      r.body = pairsOnPage ? "<table><tr><td><a href=\"/download-free-forex-historical-data/?/ascii/1-minute-bar-quotes/EURUSD\"><strong>EUR/USD</strong></a>"
                             "<br/>(2000/May)</td><td><a href=\"/download-free-forex-historical-data/?/ascii/1-minute-bar-quotes/GBPUSD\"><strong>GBP/USD</strong></a>"
                             "<br/>(2000/May)</td><td><a href=\"/download-free-forex-historical-data/?/ascii/1-minute-bar-quotes/XAUUSD\"><strong>XAU/USD</strong></a>"
                             "<br/>(2009/March)</td><td><a href=\"/download-free-forex-historical-data/?/ascii/1-minute-bar-quotes/SPXUSD\"><strong>SPX/USD</strong></a>"
                             "<br/>(2010/November)</td></tr></table>"
                           : "<html>no pairs</html>";
    }
    return r;
  }

  HttpResult post(const std::string & url, const std::string & body, const HttpHeaders & headers) override {
    requests.push_back("POST " + url + " " + field(body, "datemonth"));
    postHeaders.push_back(headers);
    postBodies.push_back(body);
    HttpResult r;
    if (failDownloads > 0) {
      failDownloads--;
      r.status = 500;
      return r;
    }
    r.status = 200;
    const std::string period = field(body, "datemonth"), code = field(body, "fxpair");
    r.body = zipFor(code, period);
    return r;
  }

  int downloads() const {
    int n = 0;
    for (const auto & q : requests) n += q.rfind("POST", 0) == 0;
    return n;
  }

  // The rows of a period, as the CSV has them (also what the tests count against).
  std::vector<market::MinuteBar> rows(const std::string & period) const {
    std::vector<market::MinuteBar> out;
    const int year = std::atoi(period.substr(0, 4).c_str());
    const int first = period.size() == 6 ? std::atoi(period.substr(4).c_str()) : 1, last = period.size() == 6 ? first : 12;
    if (year < firstYear) return out;
    for (int month = first; month <= last; month++)
      for (unsigned day = 1; day <= 31; day++) {
        const int64_t days = daysSinceUnixEpoch(year, month, day);
        if (civilFromUnixDays(days).month != static_cast<unsigned>(month)) break;
        const int weekdayNumber = static_cast<int>(((days + 4) % 7 + 7) % 7);  // 0 Sunday (1970-01-01 was a Thursday)
        if (weekdayNumber == 0 || weekdayNumber == 6) continue;
        for (int minute = 0; minute < 24 * 60; minute += step) {
          const Timestamp t = days * 86400 + minute * 60 + 5 * 3600;  // EST to UTC
          if (t > lastBar) return out;
          const double base = 1.0 + static_cast<double>((t / 60) % 977) * 1e-4;
          out.push_back({t, base, base + 0.0003, base - 0.0002, base + 0.0001});
        }
      }
    return out;
  }

  static std::string csvOf(const std::vector<market::MinuteBar> & minutes) {
    std::string csv;
    for (const auto & m : minutes) {
      const Timestamp est = m.time - 5 * 3600;
      const int64_t days = est >= 0 ? est / 86400 : -((-est + 86399) / 86400);
      const Date d = civilFromUnixDays(days);
      const int rest = static_cast<int>(est - days * 86400);
      char line[96];
      std::snprintf(line, sizeof line, "%04d%02u%02u %02d%02d%02d;%.6f;%.6f;%.6f;%.6f;0\n", d.year, d.month, d.day, rest / 3600, rest % 3600 / 60, rest % 60,
                    m.open, m.high, m.low, m.close);
      csv += line;
    }
    return csv;
  }

 private:
  static std::string field(const std::string & body, const std::string & name) {
    const std::string key = name + "=";
    size_t at = body.find(key);
    if (at == std::string::npos || (at > 0 && body[at - 1] != '&')) return {};
    at += key.size();
    return body.substr(at, body.find('&', at) - at);
  }
  std::string zipFor(const std::string & code, const std::string & period) const {
    const auto minutes = rows(period);
    if (minutes.empty()) return {};  // the site's way of saying there is none
    const std::string stem = "DAT_ASCII_" + code + "_M1_" + period;
    return market::makeZip({{stem + ".csv", csvOf(minutes)}, {stem + ".txt", "gap report\n"}}, true);
  }
};

// 2026-11-26 12:00 UTC: the day these tests pretend it is.
inline constexpr Timestamp kTestNow = 1795694400;

inline histdata::Settings fastSettings(Timestamp now = kTestNow) {
  histdata::Settings s;
  s.pause = std::chrono::milliseconds(0);
  s.now = [now] { return now; };
  s.sleep = [](std::chrono::milliseconds) {};
  return s;
}
