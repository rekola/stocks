#pragma once

#include <string>
#include <vector>

// The registry of the ISINs this program gives to what has none of its own (data/isins/registry.csv): an instrument whose source gives no
// ISIN is found here by its symbol, else by its name. Nothing is ever made up from the text: the ISIN of a thing is written down once, with
// who it is, so whoever adds one sees what is there and a collision is a mistake the loader and a test refuse.
//
// Two kinds of ISIN are in it. One that exists (zinc is XC0005705584: the commodities and indices that no country numbers have ISINs under
// the prefix XC) is only written down, for a source that does not give it. One of ours is in the private namespace `ZZ` (the user-assigned
// code element of ISO 3166: no numbering agency issues ISINs there), `ZZ`, nine digits numbered on from the last one in the file, and the
// check digit.
class IsinRegistry {
 public:
  struct Entry {
    std::string isin, symbol, name, note;
    int line = 0;  // in the file it was read from
  };

  // The file's text: lines of `isin,symbol,name,note` (the note is the rest of the line and may hold commas), `#` for a comment, blank lines.
  // False, with `error` saying which line and what is wrong, where an ISIN is not valid, a row names nothing, or two rows would collide: the
  // same ISIN, the same symbol, the same name, or a symbol that is another row's name.
  static bool parse(const std::string & text, IsinRegistry & out, std::string & error);

  // The ISIN of a thing by its symbol, else by its name (as the source says them, written exactly); empty where none is registered.
  std::string find(const std::string & symbol, const std::string & name) const;
  const std::vector<Entry> & entries() const { return entries_; }

 private:
  std::vector<Entry> entries_;
};
