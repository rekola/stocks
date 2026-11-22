#include "source/Countries.h"

#include <string_view>
#include <unordered_map>

namespace {

// Lower case for what the names use: ASCII and the Nordic and Czech capitals that occur in them.
std::string lower(const std::string & s) {
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c >= 'A' && c <= 'Z') {
      out += static_cast<char>(c + 32);
    } else if (c == 0xC3 && i + 1 < s.size() && static_cast<unsigned char>(s[i + 1]) >= 0x80 && static_cast<unsigned char>(s[i + 1]) <= 0x9E) {
      out += static_cast<char>(0xC3);  // Latin-1 capitals (Ä, Ö, Å, ...) are the lower case 0x20 further
      out += static_cast<char>(static_cast<unsigned char>(s[++i]) + 0x20);
    } else if (c == 0xC5 && i + 1 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0xA0) {
      out += "\xC5\xA1";  // Š
      i++;
    } else {
      out += static_cast<char>(c);
    }
  }
  return out;
}

const std::unordered_map<std::string_view, const char *> kCountries = {
    {"afganistan", "AF"}, {"albania", "AL"}, {"algeria", "DZ"}, {"andorra", "AD"}, {"angola", "AO"}, {"anguilla", "AI"},
    {"antigua ja barbuda", "AG"}, {"argentiina", "AR"}, {"armenia", "AM"}, {"aruba", "AW"}, {"australia", "AU"},
    {"azerbaidžan", "AZ"}, {"bahama", "BS"}, {"bahamasaaret", "BS"}, {"bahrain", "BH"}, {"bangladesh", "BD"},
    {"barbados", "BB"}, {"belgia", "BE"}, {"belize", "BZ"}, {"benin", "BJ"}, {"bermuda", "BM"}, {"bhutan", "BT"},
    {"bolivia", "BO"}, {"bosnia ja hertsegovina", "BA"}, {"botswana", "BW"}, {"brasilia", "BR"}, {"brunei", "BN"},
    {"bulgaria", "BG"}, {"burkina faso", "BF"}, {"chile", "CL"}, {"costa rica", "CR"}, {"curaçao", "CW"},
    {"egypti", "EG"}, {"el salvador", "SV"}, {"ecuador", "EC"}, {"espanja", "ES"}, {"etelä-afrikka", "ZA"},
    {"etelä-korea", "KR"}, {"etiopia", "ET"}, {"fidži", "FJ"}, {"filippiinit", "PH"}, {"gabon", "GA"}, {"gambia", "GM"},
    {"georgia", "GE"}, {"ghana", "GH"}, {"gibraltar", "GI"}, {"guatemala", "GT"}, {"guernsey", "GG"}, {"honduras", "HN"},
    {"hongkong", "HK"}, {"indonesia", "ID"}, {"intia", "IN"}, {"irak", "IQ"}, {"iran", "IR"}, {"irlanti", "IE"},
    {"islanti", "IS"}, {"israel", "IL"}, {"italia", "IT"}, {"itävalta", "AT"}, {"jamaika", "JM"}, {"japani", "JP"},
    {"jersey", "JE"}, {"jordania", "JO"}, {"kambodža", "KH"}, {"kamerun", "CM"}, {"kanada", "CA"}, {"kap verde", "CV"},
    {"kazakstan", "KZ"}, {"kenia", "KE"}, {"kiina", "CN"}, {"kirgisia", "KG"}, {"kolumbia", "CO"}, {"kreikka", "GR"},
    {"kroatia", "HR"}, {"kuuba", "CU"}, {"kuwait", "KW"}, {"kypros", "CY"}, {"latvia", "LV"}, {"libanon", "LB"},
    {"liberia", "LR"}, {"liechtenstein", "LI"}, {"liettua", "LT"}, {"luxemburg", "LU"}, {"macao", "MO"},
    {"madagaskar", "MG"}, {"malawi", "MW"}, {"malesia", "MY"}, {"malta", "MT"}, {"mansaari", "IM"}, {"marokko", "MA"},
    {"marshallinsaaret", "MH"}, {"mauritius", "MU"}, {"meksiko", "MX"}, {"moldova", "MD"}, {"monaco", "MC"},
    {"mongolia", "MN"}, {"montenegro", "ME"}, {"mosambik", "MZ"}, {"namibia", "NA"}, {"nigeria", "NG"},
    {"norja", "NO"}, {"norsunluurannikko", "CI"}, {"oman", "OM"}, {"pakistan", "PK"}, {"panama", "PA"},
    {"papua-uusi-guinea", "PG"}, {"paraguay", "PY"}, {"peru", "PE"}, {"pohjois-makedonia", "MK"}, {"portugali", "PT"},
    {"puerto rico", "PR"}, {"puola", "PL"}, {"qatar", "QA"}, {"katar", "QA"}, {"ranska", "FR"}, {"romania", "RO"},
    {"ruanda", "RW"}, {"ruotsi", "SE"}, {"saksa", "DE"}, {"saudi-arabia", "SA"}, {"senegal", "SN"}, {"serbia", "RS"},
    {"singapore", "SG"}, {"slovakia", "SK"}, {"slovenia", "SI"}, {"sri lanka", "LK"}, {"suomi", "FI"}, {"sveitsi", "CH"},
    {"taiwan", "TW"}, {"tanska", "DK"}, {"tansania", "TZ"}, {"thaimaa", "TH"}, {"trinidad ja tobago", "TT"},
    {"tunisia", "TN"}, {"turkki", "TR"}, {"tšekki", "CZ"}, {"uganda", "UG"}, {"ukraina", "UA"}, {"unkari", "HU"},
    {"uruguay", "UY"}, {"usa", "US"}, {"yhdysvallat", "US"}, {"uusi-seelanti", "NZ"}, {"uzbekistan", "UZ"},
    {"venezuela", "VE"}, {"venäjä", "RU"}, {"vietnam", "VN"}, {"viro", "EE"}, {"alankomaat", "NL"}, {"sambia", "ZM"},
    {"zimbabwe", "ZW"}, {"färsaaret", "FO"}, {"brittiläiset neitsytsaaret", "VG"}, {"caymansaaret", "KY"},
    {"cayman-saaret", "KY"}, {"yhdistyneet arabiemiirikunnat", "AE"}, {"yhdistynyt kuningaskunta", "GB"},
};

}  // namespace

std::optional<std::string> countryCodeOfFinnishName(const std::string & name) {
  // The map's keys are views of string literals, which outlive it.
  auto it = kCountries.find(lower(name));
  if (it == kCountries.end()) return std::nullopt;
  return std::string(it->second);
}
