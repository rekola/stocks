#pragma once

#include "source/Source.h"

#include <algorithm>
#include <string>
#include <vector>

// The rows of one load that an adapter leaves out because a part of their key cannot be named (an ISIN with a wrong check digit, a code
// that does not fit): counted, with a few of them as examples, and said once at the end of the load. The store refuses an edge or an
// event with a part that is none by throwing, which discards the whole batch; the adapter knows the row, so it skips it and says so.
class LeftOut {
 public:
  // A row left out, named by what a person can look it up by (its ISIN, else its symbol or name).
  void add(const std::string & what) {
    count_++;
    if (examples_.size() < kExamples && !what.empty() && std::find(examples_.begin(), examples_.end(), what) == examples_.end())
      examples_.push_back(what);
  }
  size_t count() const { return count_; }

  // "nordnet: etflist: 2 rows left out for an instrument or a quote that cannot be named (IE00BHZRR031, ...)", where `who` is
  // "nordnet: etflist: " and `one` and `many` what the rows are called. Nothing if none was left out.
  void say(SourceListener & listener, const std::string & who, const char * one = "row", const char * many = "rows") const {
    if (count_ == 0) return;
    std::string text = who + std::to_string(count_) + " " + (count_ == 1 ? one : many) +
                       " left out for an instrument or a quote that cannot be named";
    if (!examples_.empty()) {
      text += " (";
      for (size_t i = 0; i < examples_.size(); i++) text += (i ? ", " : "") + examples_[i];
      if (count_ > examples_.size()) text += ", ...";
      text += ")";
    }
    listener.message(text);
  }

 private:
  static constexpr size_t kExamples = 3;
  size_t count_ = 0;
  std::vector<std::string> examples_;
};
