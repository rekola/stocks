#pragma once

#include "model/Store.h"

#include <cellui/Table.h>

#include <memory>
#include <string>
#include <vector>

// The news a selection (a kind, or all) holds, newest first, as a table: when, what kind, the headline, whom it is about and which
// dataset said it. A row is an item; the items are read from the snapshot once, when the model is made, so a newer snapshot is a
// new list (`setSnapshot` keeps the selection).
class NewsTable : public cellui::TableModel {
 public:
  enum Column { TIME, KIND, HEADLINE, ABOUT, SOURCE };

  // The newest `limit` items that `keep` accepts (all of them where it is empty).
  NewsTable(std::shared_ptr<const Snapshot> snapshot, std::function<bool(const NewsItem &)> keep, size_t limit = 5000);
  // The same selection from a newer snapshot.
  void setSnapshot(std::shared_ptr<const Snapshot> snapshot);

  int rowCount() const override { return static_cast<int>(items_.size()); }
  std::string text(int r, int c) const override;
  std::optional<double> number(int r, int c) const override;

  const NewsItem & item(int r) const { return items_[static_cast<size_t>(r)]; }
  const std::shared_ptr<const Snapshot> & snapshot() const { return snapshot_; }
  // An item's identity across a refresh: its dataset and its id.
  static std::string keyOf(const NewsItem &);
  // "2026-11-20 14:03", UTC.
  static std::string timeText(Timestamp);
  // The names of the nodes an item is about, with commas: "Nokia Oyj, Finland". Empty for the news of the world.
  static std::string aboutText(const Snapshot &, const NewsItem &);

 private:
  std::shared_ptr<const Snapshot> snapshot_;
  std::function<bool(const NewsItem &)> keep_;
  size_t limit_;
  std::vector<NewsItem> items_;
};
