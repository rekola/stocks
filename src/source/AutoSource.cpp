#include "source/AutoSource.h"

namespace {

// Passes everything on and counts the changes: a load that changed the store gave something.
class Counting : public SourceListener {
 public:
  explicit Counting(SourceListener & inner) : inner_(inner) {}
  void changed() override {
    changes++;
    inner_.changed();
  }
  void message(const std::string & m) override { inner_.message(m); }
  bool cancelled() const override { return inner_.cancelled(); }
  int changes = 0;

 private:
  SourceListener & inner_;
};

}  // namespace

void AutoSource::load(Store & store, Http & http, SourceListener & listener) {
  if (decided_) {
    chosen()->load(store, http, listener);
    return;
  }
  Counting counted(listener);
  primary_->load(store, http, counted);
  if (listener.cancelled()) return;  // stopped, not failed: decide at the next load
  decided_ = true;
  if (counted.changes > 0) return;
  failed_ = true;
  listener.message(primary_->name() + " gave nothing: using " + fallback_->name());
  fallback_->load(store, http, listener);
}
