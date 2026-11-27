// What the user does to her portfolios from the screen: buy and sell (a form over a row), new, rename and delete a portfolio, and
// undo and redo of each. Browser's own file has the screen; this has the commands that write.
#include "AttributeView.h"
#include "Browser.h"
#include "Quotes.h"
#include "model/Dates.h"

#include <cellui/ChoiceModal.h>
#include <cellui/FormModal.h>

#include <algorithm>
#include <chrono>

using namespace cellui;

namespace {

std::string lower(std::string s) {
  for (char & c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
std::string upper(std::string s) {
  for (char & c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}
std::string trimmed(const std::string & s) {
  size_t a = 0, b = s.size();
  while (a < b && s[a] == ' ') a++;
  while (b > a && s[b - 1] == ' ') b--;
  return s.substr(a, b - a);
}

int64_t systemToday() {
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  return dayNumberOfInstant(seconds);
}

std::string codeOf(const Snapshot & snapshot, InstrumentId currency) {
  std::string code = snapshot.symbol(currency);
  return code.empty() ? snapshot.name(NodeRef(currency)) : code;
}

// What a form field says, as the form reads it: a comma is a decimal point.
std::optional<double> num(const std::string & t) { return parseFormNumber(t); }

struct Names {
  std::vector<std::pair<std::string, PortfolioId>> list;
  std::optional<PortfolioId> find(const std::string & name) const {
    const std::string wanted = lower(trimmed(name));
    for (const auto & [n, id] : list)
      if (lower(n) == wanted) return id;
    return std::nullopt;
  }
};
Names namesIn(const Snapshot & snapshot) {
  Names out;
  for (PortfolioId id : snapshot.portfolios()) out.list.emplace_back(snapshot.name(NodeRef(id)), id);
  return out;
}

}  // namespace

PortfolioId Browser::currentPortfolio() const {
  const PortfolioId shown = context_ == Context::Portfolio ? portfolio_ : views_[static_cast<size_t>(Context::Portfolio)].portfolio;
  if (!shown.valid()) return {};
  const auto all = store_->snapshot()->portfolios();
  return std::find(all.begin(), all.end(), shown) != all.end() ? shown : PortfolioId{};
}

void Browser::wrote() {
  last_refresh_ = std::chrono::steady_clock::now();
  pending_ = false;
  refresh();
  app_.requestFrame();
}

void Browser::record(Change change) {
  undo_.push_back(std::move(change));
  redo_.clear();  // a new change ends what was taken back
}

void Browser::undo() {
  if (undo_.empty()) {
    status_.setMessage("Nothing to undo");
    return;
  }
  Change change = std::move(undo_.back());
  undo_.pop_back();
  try {
    change.undo(*store_);
  } catch (const std::exception & e) {
    status_.setMessage(std::string("Cannot undo: ") + e.what());
    return;
  }
  const std::string what = change.what;
  redo_.push_back(std::move(change));
  wrote();
  status_.setMessage("Undid: " + what);
}

void Browser::redo() {
  if (redo_.empty()) {
    status_.setMessage("Nothing to redo");
    return;
  }
  Change change = std::move(redo_.back());
  redo_.pop_back();
  try {
    change.redo(*store_);
  } catch (const std::exception & e) {
    status_.setMessage(std::string("Cannot redo: ") + e.what());
    return;
  }
  const std::string what = change.what;
  undo_.push_back(std::move(change));
  wrote();
  status_.setMessage("Redid: " + what);
}

// A name and a currency, and the portfolio is made. The key is minted; nothing is written until the form is accepted.
std::optional<PortfolioId> Browser::makePortfolio(const std::string & title) {
  auto snapshot = store_->snapshot();
  const Names names = namesIn(*snapshot);
  std::string currency;
  if (const auto eur = snapshot->findInstrument("EUR"); eur && snapshot->kind(*eur) == InstrumentKind::Currency) currency = "EUR";
  FormField name{"Name", FieldKind::TEXT, "", "", true};
  name.check = [names](const std::string & t) { return names.find(t) ? "A portfolio called " + trimmed(t) + " exists" : std::string(); };
  FormField code{"Currency", FieldKind::TEXT, currency, "EUR", true};
  code.check = [snapshot](const std::string & t) {
    const auto id = snapshot->findInstrument(upper(trimmed(t)));
    return id && snapshot->kind(*id) == InstrumentKind::Currency ? std::string() : "No currency " + upper(trimmed(t)) + " is known";
  };
  FormModal form(app_.widgets(), app_.root(), app_.backend().eventLoop(), title, {name, code});
  app_.requestFrame();
  const int result = form.showModal();
  app_.requestFrame();
  if (result != 0) {
    status_.setMessage("No portfolio made");
    return std::nullopt;
  }
  const std::string made = trimmed(form.text(0));
  const InstrumentId cur = *snapshot->findInstrument(upper(trimmed(form.text(1))));
  const PortfolioId id{mintPortfolioKey().value};
  const std::vector<Attribute> facts = {{predicate::name, made}, {predicate::currency, NodeKey{cur.value}}};
  store_->setAttributes(NodeRef(id), facts);
  record({"make portfolio " + made, [id](Store & s) { s.removePortfolio(id); }, [id, facts](Store & s) { s.setAttributes(NodeRef(id), facts); }});
  portfolio_ = id;  // it is the one shown, when the Portfolio context is
  views_[static_cast<size_t>(Context::Portfolio)].portfolio = id;
  wrote();
  status_.setMessage("Made portfolio " + made + " in " + codeOf(*store_->snapshot(), cur));
  return id;
}

void Browser::renamePortfolio() {
  const PortfolioId id = currentPortfolio();
  if (!id.valid()) {
    status_.setMessage("No portfolio to rename: make one with new-portfolio");
    return;
  }
  auto snapshot = store_->snapshot();
  const std::string old = snapshot->name(NodeRef(id));
  const bool had_name = snapshot->fact(NodeRef(id), predicate::name).has_value();
  const Names names = namesIn(*snapshot);
  FormField name{"Name", FieldKind::TEXT, old, "", true};
  name.check = [names, id](const std::string & t) {
    const auto other = names.find(t);
    return other && *other != id ? "A portfolio called " + trimmed(t) + " exists" : std::string();
  };
  FormModal form(app_.widgets(), app_.root(), app_.backend().eventLoop(), "Rename portfolio " + old, {name});
  app_.requestFrame();
  const int result = form.showModal();
  app_.requestFrame();
  if (result != 0) {
    status_.setMessage("Not renamed");
    return;
  }
  const std::string now = trimmed(form.text(0));
  if (now == old) {
    status_.setMessage("The name is the same: " + old);
    return;
  }
  store_->setAttribute(NodeRef(id), {predicate::name, now});
  record({"rename " + old + " to " + now,
          [id, old, had_name](Store & s) {
            if (had_name) s.setAttribute(NodeRef(id), {predicate::name, old});
            else s.removeAttribute(NodeRef(id), predicate::name);
          },
          [id, now](Store & s) { s.setAttribute(NodeRef(id), {predicate::name, now}); }});
  wrote();
  status_.setMessage("Renamed " + old + " to " + now);
}

void Browser::deletePortfolio() {
  const PortfolioId id = currentPortfolio();
  if (!id.valid()) {
    status_.setMessage("No portfolio to delete");
    return;
  }
  auto snapshot = store_->snapshot();
  const std::string name = snapshot->name(NodeRef(id));
  // What is said of it, kept so that undo can say it again: its facts and every transaction in every dataset.
  const std::vector<Attribute> facts = snapshot->attributes(NodeRef(id));
  std::vector<std::pair<LedgerKey, std::vector<portfolio::Transaction>>> ledgers;
  size_t count = 0;
  for (DatasetId d : snapshot->ledgers(id)) {
    const LedgerKey key{id, d};
    const auto all = snapshot->transactions(key);
    ledgers.push_back({key, std::vector<portfolio::Transaction>(all.begin(), all.end())});
    count += all.size();
  }
  const std::string what = count == 0 ? "Delete it" : "Delete it and its " + std::to_string(count) + (count == 1 ? " transaction" : " transactions");
  ChoiceModal ask(app_.widgets(), app_.root(), app_.backend().eventLoop(), "Delete portfolio " + name + "?", {what, "Keep it"}, 1);
  app_.requestFrame();
  const int answer = ask.showModal();
  app_.requestFrame();
  if (answer != 0) {
    status_.setMessage("Kept " + name);
    return;
  }
  store_->removePortfolio(id);
  record({"delete portfolio " + name,
          [id, facts, ledgers](Store & s) {
            s.batch([&](Batch & b) {
              if (!facts.empty()) b.setAttributes(NodeRef(id), facts);
              for (const auto & [key, list] : ledgers) b.appendTransactions(key, list);
            });
          },
          [id](Store & s) { s.removePortfolio(id); }});
  wrote();
  status_.setMessage("Deleted portfolio " + name + " (undo brings it back)");
}

// Buys or sells what a row shows, in a portfolio.
void Browser::trade(portfolio::Side side) {
  const bool buying = side == portfolio::Side::Buy;
  auto snapshot = store_->snapshot();
  const int row = action_row_.value_or(table_.selectedRow());
  auto quotes = std::dynamic_pointer_cast<Quotes>(table_.model());
  auto positions = std::dynamic_pointer_cast<Positions>(table_.model());
  if ((!quotes && !positions) || row < 0 || row >= table_.model()->rowCount()) {
    status_.setMessage("No row selected");
    return;
  }
  InstrumentId instrument, quote;
  MarketId market;
  std::string label, offered;
  bool buyable = true;
  if (quotes) {
    const EdgeKey & edge = quotes->edge(row);
    instrument = edge.base;
    quote = edge.quote;
    market = edge.market;
    label = snapshot->symbol(edge);
    if (label.empty()) label = snapshot->name(NodeRef(edge.base));
    buyable = portfolio::tradable(*snapshot, edge);
    const auto last = snapshot->quote(edge);
    if (last && isDefined(last->last)) offered = realText(last->last);  // the last price
  } else {
    const portfolio::PositionValue & v = positions->row(row);
    instrument = v.position.instrument;
    quote = v.position.quote;
    market = v.position.market;
    label = positions->text(row, Positions::SYMBOL);
    if (label.empty()) label = snapshot->name(NodeRef(instrument));
    offered = realText(v.price ? v.price->close : v.position.average_cost);
  }
  // The same function the buttons ask: a command run from the palette does not look at the row.
  if (!buyable) {
    status_.setMessage(label + " cannot be " + (buying ? "bought" : "sold") + ": it is priced here, not traded");
    return;
  }
  if (snapshot->portfolios().empty()) {
    // The first buy offers to make a portfolio.
    if (!makePortfolio("Make a portfolio first")) return;
    snapshot = store_->snapshot();
  }
  const Names names = namesIn(*snapshot);
  PortfolioId target = currentPortfolio();
  if (!target.valid() && last_traded_.valid() && names.find(snapshot->name(NodeRef(last_traded_)))) target = last_traded_;
  if (!target.valid()) target = names.list.front().second;
  const std::string quote_code = codeOf(*snapshot, quote);
  const int64_t today = today_ ? today_() : systemToday();
  const uint64_t id = portfolio::mintId();

  FormField portfolio_field{"Portfolio", FieldKind::TEXT, snapshot->name(NodeRef(target)), "", true};
  portfolio_field.check = [names](const std::string & t) {
    return names.find(t) ? std::string() : "No portfolio called " + trimmed(t) + " (new-portfolio makes one)";
  };
  FormField quantity{"Quantity", FieldKind::NUMBER, "", "", true};
  quantity.check = [](const std::string & t) { return num(t) && *num(t) <= 0 ? std::string("Quantity must be above zero") : std::string(); };
  FormField price{"Price " + quote_code, FieldKind::NUMBER, offered, "", true};
  price.check = [](const std::string & t) { return num(t) && *num(t) < 0 ? std::string("Price cannot be negative") : std::string(); };
  FormField date{"Date", FieldKind::DATE, dateText(today), "YYYY-MM-DD", true};
  FormField fee{"Fee " + quote_code, FieldKind::NUMBER, "", "0", false};
  fee.check = [](const std::string & t) { return num(t) && *num(t) < 0 ? std::string("Fee cannot be negative") : std::string(); };
  FormField note{"Note", FieldKind::TEXT, "", "", false};

  // The transaction the texts of the form make, none while they do not make one.
  auto make = [=](const std::vector<std::string> & t) -> std::optional<std::pair<PortfolioId, portfolio::Transaction>> {
    const auto who = names.find(t[0]);
    const auto q = num(t[1]), p = num(t[2]);
    const auto day = dayNumberOfIsoDate(trimmed(t[3]));
    const std::string fee_text = trimmed(t[4]);
    const auto f = fee_text.empty() ? std::optional<double>(0) : num(fee_text);
    if (!who || !q || !p || !day || !f || *q <= 0 || *p < 0 || *f < 0) return std::nullopt;
    portfolio::Transaction x;
    x.id = id;
    x.day = *day;
    x.side = side;
    x.instrument = instrument;
    x.market = market;
    x.quote = quote;
    x.quantity = *q;
    x.price = *p;
    x.fee = *f;
    x.note = trimmed(t[5]);
    return std::make_pair(*who, x);
  };

  const std::string market_name = snapshot->name(NodeRef(market));
  FormModal form(app_.widgets(), app_.root(), app_.backend().eventLoop(), std::string(buying ? "Buy " : "Sell ") + label + (market_name.empty() ? "" : " on " + market_name),
                 {portfolio_field, quantity, price, date, fee, note});
  // The live total: what it costs with the fee, or what it brings in less the fee.
  form.setSummary([=](const std::vector<std::string> & t) {
    const auto q = num(t[1]), p = num(t[2]);
    if (!q || !p) return std::string("Total -");
    const auto f = trimmed(t[4]).empty() ? std::optional<double>(0) : num(t[4]);
    const double fee_amount = f ? *f : 0;
    return "Total " + fixed(*q * *p + (buying ? fee_amount : -fee_amount), 2) + " " + quote_code + (fee_amount > 0 ? (buying ? " with the fee" : " less the fee") : "");
  });
  // A sell of more than is held is refused by the model's arithmetic: it is said here, as the quantity is typed.
  if (!buying) {
    form.setCheck([=](const std::vector<std::string> & t) -> std::string {
      const auto made = make(t);
      if (!made) return {};
      const LedgerKey ledger{made->first, portfolio::ledgerOf(*snapshot, made->first)};
      const auto existing = snapshot->transactions(ledger);
      std::vector<portfolio::Transaction> all(existing.begin(), existing.end());
      all.push_back(made->second);
      // Refused now and not before: what this one would spoil, which is not always itself (an earlier sell of the same day may be put out).
      std::vector<uint64_t> spoiled_before;
      for (const portfolio::Refusal & r : portfolio::derivePositions(std::vector<portfolio::Transaction>(existing.begin(), existing.end())).refused) spoiled_before.push_back(r.id);
      const portfolio::Derived derived = portfolio::derivePositions(all);
      const bool refused = std::any_of(derived.refused.begin(), derived.refused.end(), [&](const portfolio::Refusal & r) {
        return std::find(spoiled_before.begin(), spoiled_before.end(), r.id) == spoiled_before.end();
      });
      if (!refused) return {};
      double held = 0;
      for (const portfolio::Position & p : portfolio::holdings(*snapshot, made->first).positions)
        if (p.instrument == instrument && p.market == market && p.quote == quote) held = p.quantity;
      return std::string("Sells more than is held: you hold ") + (held > 0 ? realText(held) : std::string("none")) + " in " + snapshot->name(NodeRef(made->first));
    });
  }
  app_.requestFrame();
  const int result = form.showModal();
  app_.requestFrame();
  if (result != 0) {
    status_.setMessage(buying ? "Nothing bought" : "Nothing sold");
    return;
  }
  const auto made = make(form.texts());
  if (!made) return;  // not possible: the form accepts only what makes one
  const LedgerKey ledger{made->first, portfolio::ledgerOf(*store_->snapshot(), made->first)};
  const portfolio::Transaction transaction = made->second;
  try {
    store_->appendTransactions(ledger, {transaction});
  } catch (const std::invalid_argument & e) {
    status_.setMessage(std::string("Not ") + (buying ? "bought" : "sold") + ": " + e.what());
    return;
  }
  const std::string who = store_->snapshot()->name(NodeRef(made->first));
  const std::string text = std::string(buying ? "Bought " : "Sold ") + realText(transaction.quantity) + " " + label + " at " + realText(transaction.price) + " " + quote_code + " in " + who;
  record({lower(text.substr(0, 1)) + text.substr(1), [ledger, id](Store & s) { s.removeTransactions(ledger, {id}); },
          [ledger, transaction](Store & s) { s.appendTransactions(ledger, {transaction}); }});
  last_traded_ = made->first;
  wrote();
  status_.setMessage(text);
}
