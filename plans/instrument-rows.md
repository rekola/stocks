# One row per instrument

Status: built on this branch (steps 1 to 4), after the owner's answers: gold and every commodity is buyable where a member is on a real venue (no rule by kind); staleness is
relative to the newest member with a window of **one hour** where the newest is live (said its price within an hour of now), and of **four days** where none is (weekend, night: all the last prices are
combined, not the freshest alone); no adapter says which is live; a position's cost is the pooled average of the buys (applies to cost only: it is money paid, nothing to take a median of; prices of datasets use the median); a preset of one market
shows that market's members only; the table says nothing of a row's markets (Show info does). Below is the plan as first written: where it differs, the owner's answers win
(buyable: no commodity or kind rule; stale: 1 hour; the market preset shows the market alone).

## Goal

A row is `(base, quote)`, not `(market, base, quote)`. `GroupKey` loses its market; `membersOf` returns the edges of every market and
dataset of the pair; the row's price, change, high/low and trace are made of all of them by the machinery that is there (`mixOf`,
`combine`, `groupChange`, `groupRange`, `groupPeriodView`). Nothing in the store, the sources or the adapters changes: the store keeps
edges as they were written.

The quote currency stays in the key. A share on XHEL in EUR and on XSTO in SEK are two rows until prices are converted to one currency,
and converting is not done here: a row's members must be comparable without it (Group.h says so), and a rate is never mixed with a price.

## What exists already (checked)

- `Mix`/`Member` (Group.h) hold, for each member, its edge, price, time, whether it is used (`Used`, `Stale`, `Off`, `NoPrice`) and its
  offset from the combined price. The info dialog already shows them (`sourcesSection` in AttributeView.cpp, which `buildInfo`
  gets from `Quotes::mix(row)`): one line per dataset with price, offset, time and why it is left out, then a Summary and Series
  section for each member. So "opening a row" is **Show info**; no view has to be made, only the labels (a member is named by market *and*
  dataset) and the Market section (one for each market of the members, not the representative edge's).
- The table has no expandable rows or sub-rows (cellui's `Table` knows rows, a trace, buttons). Rows that open in place would be a cellui
  feature; this plan does not need one.
- `Quotes` already works on groups: `groups_`, `addRows` (an edge of a group that is a row is no new row). What changes is that the
  representative edge `edge(r)` stands for more than one market.

## Decisions and how they are carried out

### 1. The group key, the rows (step 1)

- `GroupKey{base, quote}`; `groupOf(edge)`; `operator<`, `std::hash`. `membersOf` filters on quote only.
- `appendEdges`/`allQuoteEdges`/`quoteEdges` give one edge for each group over all markets: the first member by key (market, dataset), so
  a row's representative does not change while data arrives. `quoteEdges(market)` returns the groups that have a member on that market.
  Dedupe with a hash set over the markets (they are no longer contiguous).
- **The market column goes** (`Quotes::MARKET`, its label cache, its sort key, `Browser::setColumns`'s three widths). Its width goes
  to the Name; the narrow layout (80 columns) is unchanged otherwise, and its comment about widths is redone.
- What names a row: the symbol and the name of the representative edge's listing, as now.
- **What a row shows of its markets** (the owner's idea and the others, below): the table shows none of it by default.
- Info dialog: the Sources lines are named `market · dataset` (the market's name, else its MIC), the per-member sections get the same
  suffix, the Market section is one for each distinct market of the members, and the latest news is about the instrument, the members'
  markets and their countries. A row opened from the Watchlist or the portfolio shows the same.
- `PeriodView`/trace: unchanged code, now across markets. See "Staleness".

### 2. Staleness when markets close at different times

`kStaleAfter` is 4 days against the newest member. A market that is closed on a holiday for up to three days would still count, with a
three-day-old price in a mean or median of two, and the trace carries a member's last close forward without any limit (`groupPeriodView`:
"a member that has none at that time counts with its last close before it"), so in an intraday trace a market that closed at 18:30 stays
in the median while another trades on. Both are the same problem and one rule fixes both:

- A member is stale when it is more than **30 hours** behind the newest (one trading day, with a margin for a market that closes
  earlier than the one that is open longest, or a daily fix such as the LBMA's; a weekend is no problem, since all are then equally old).
  The number is a proposal; the tests show the case it is for and the cases it must not break (two markets that close an hour apart;
  a daily source beside a live one; a holiday on one market).
- The trace's carry-over is limited by the same window: at time `t` a member counts with its last close only if that bar is within the
  window of `t`.
- With one member left, it is the row (no combine), as now.
- Tests first show the drag: a member 3 days old beside one live gives the mean today and not after.

### 3. Lists (step 2)

- `list_members` is `(list, base, quote)`; `StoredMember` loses `market`; `whole()` checks base and quote.
- Watchlists may start over, but carrying them is one statement, so the file of schema 3 or 4 is upgraded in place (as `upgradeV3`
  does), collapsing the members that differ only in market. Schema becomes **5** (below).

### 4. The portfolio (step 3)

What decides the schema is what the file stores. The transactions are `events` rows with a JSON body that already carries `market` and
needs nothing more: the position is **derived** from them and never stored. So:

- A position is `(portfolio, instrument, currency)`. `derivePositions` keys on `(instrument, quote)`; `Position::market` goes;
  quantity, average cost and the history are the one position's.
- **No transaction is changed, moved or dropped by the upgrade.** `market` stays in the body as a note: a transaction made from now on
  has none (a row has no market), old ones keep theirs, and `Transaction::market` is optional. The load no longer drops a transaction
  by its market (`Snapshot::loadEvents` does today: that is the one place a transaction could vanish, and it goes). The Buy form says
  "Buy NOKIA", not "on Nasdaq Helsinki". The transactions list shows an old transaction's market in its note column.
- **Quantity is always preserved; cost is the one number that can move.** Two positions with different average costs, a sell on one
  of them: separately `A: 10 @ 100, B: 10 @ 200, sell 5 of A` is 5 @ 100 + 10 @ 200 = cost 2500; one position pools the cost
  (average 150) and is cost 2250 after the same sell. A sell has to come off the pooled position, because the owner wants to sell on any market, so there is no
  separate cost to keep. Only a file in which a sell was made while another market's lot, bought dearer or cheaper, was held changes. A
  buy-only history is exact. Before the program writes to the owner's file, a read-only check on a *copy* (a test helper, and a
  command if the owner wants one) prints each position's quantity and cost before and after, so any difference is seen first. See question 1.
- The events need no change, so the version bump to 5 is for the lists: `upgradeLists` (schema 3 or 4 to 5) runs in one transaction and
  says what it did. A file of any other version is handled as today: moved aside whole (`.old-<time>`, never emptied or overwritten) and
  the program says so; a test checks the moved file is byte for byte the old one.
- Tests: a file of schema 4 with the same share bought on two markets and a transaction on a third: opened, one position, quantity and
  cost as derived, every event still in the file byte for byte; a file of 3 and of 1 reach 5; a file of 6 is moved aside whole; the
  lists of 4 with two markets of one pair are one member.
- **The price a position is valued at** is the group's combined price (`mixOf(...).price`), with the members' state shown in Show info.
  Where members disagree (`disagreement`), the combined price is the median (three or more), the mean (two), the only one (one), so a
  single wrong dataset does not move the value; the dialog says how much they disagree. No warning column. Valuation uses every dataset,
  as `latestPrice` does today (the Markets table's dataset switches do not change a portfolio). A position's change, trace and chart use
  the group's (`groupChange`, `groupPeriodView`).
- **Buyable.** `tradable` is by the group: `tradable(snapshot, GroupKey)`:
  1. never an index, a currency pair or a unit as a kind, nor a rate (the quote is a rate unit);
  2. a **commodity** is bought only where it is held in hand: every member is on XOFF (the physical items). This keeps a spot price that has
     one fixing on a real venue (gold on XXXX and on the London Bullion Market, XLBM) from becoming buyable by its XLBM member;
  3. anything else is buyable if at least one member is on a market that is a real venue or XOFF (not XXXX).
  `tradable(MarketId)` stays for what asks about one market. The writes (`requireBuyable`) and the buttons ask the same function.
  Tests: each of the six kinds; gold with XXXX and XLBM members is not buyable, a physical gold bar is; Nokia with an XXXX member from a list
  and an XHEL one is.

### 5. Presets (step 4)

- `Preset::also`, `Quotes::Scope::also` and the XLBM line go. Commodities, Currencies, Indices and Funds are market-less selections
  by kind. Commodities leaves out the rows that are only on XOFF (they are the Physical preset's: a gold bar is not a commodity price).
- A preset with a market (Nasdaq Helsinki, Physical) selects **the groups that have a member there**, and the row is the whole group,
  all markets. See question 2.
- All: every group.

### 6. Sort key and display of a row's market(s)

No column. The sort of the other columns is unchanged. See question 3 for what, if anything, the table says of the members.

## Combined bars (for later: not built, not precluded)

The continuous series across markets for machine learning is not part of this. It is **computed when read from the members' bars and
never stored** (no series is derived from another in the store): `data-model.md` gets a section saying how, so that it can be built next:

- the group's slots are the union of its members' bar times at one resolution (a member of another resolution is not mixed in);
- in a slot, the bar's close is `combine` of the members' closes of that slot (median of three or more, mean of two, the one); a member
  that has no bar in the slot counts with its last close only within the stale window (above), and not at all before its first bar;
- the volume of a slot is the **sum** of the members' volumes of that slot, never combined (a median of volumes is no volume); a slot
  where none says a volume has none;
- open, high and low are left to that step (combine each as `groupRange` does, or the first and the extreme of the closes).

## Steps

1. Group key, rows, the open-row labels, staleness and carry-over limit (tests: GroupTests, BrowserTests, a table of two markets).
2. The lists and the file's schema 5 with `upgradeLists` and its test on files of schema 3 and 4.
3. The portfolio: derivation, tradable, valuation, forms, transactions list, load, check command, tests.
4. Presets, `also`, the guide, glossary, README, `plans/data-model.md` (the preset and merge notes; combined bars) and `plans/portfolios.md`.

Each step builds, passes `stocks_tests` and `market_tests`, and each new test is shown to fail without its change.

## For the owner

1. **Cost on a merge.** Pooled average cost across markets (quantity unchanged; cost changes only where a sell was made on one market
   while another market's lot cost differently). The alternative is to keep a lot for each market for the cost and refuse a sell of more than
   that market's lot, which is against "buy and sell on any market". Pool it?
2. **Market presets.** Does Nasdaq Helsinki mean "rows that have a member on XHEL" shown whole (my plan), or the XHEL members only (a row's
   price is then XHEL's: a row of one market again, which this change is meant to end)?
3. **What the table says of a row's members**, if anything (the options are in the reply that came with this file).
