# Data model

Status: steps 1 and 2 are built (`src/model/`: `Types.h`, `Store.h`,
`MemoryStore.cpp`, tests in `tests/ModelTests.cpp`). The table still shows the
made-up rows. The order book is stored but not used by anything yet.

## Goals

- One in-memory model that the table, later the charts and the prediction
  models all read, filled by any number of data sources.
- Facts from different sources about the same thing live side by side, with
  their origin, and are never silently merged or overwritten.
- Prices belong to no currency: a price is the exchange rate between two
  instruments, so stocks, commodities, currencies and indices need no special
  cases.
- In memory first. Later a server collects data and fills the same tables when
  the application starts, so the model sits behind an interface (`Store`) that a
  server-backed store can implement.

## The graph

**Nodes are instruments**: a company's share, a currency, a commodity, an
index. An instrument has an id, a symbol, a name and a kind. A currency is an
instrument like any other, which is what lets Forex be currency to currency.

**Edges are between two instruments.** An edge `base -> quote` says "one unit
of `base` costs this much `quote`" (NOKIA -> EUR, EUR -> USD).

**The key of an edge** is `(base, quote, market, dataset)`:

- **market**: the same instrument can be listed on several markets with
  different prices. A market is its own small record (id, name; later its
  currency, calendar and time zone).
- **dataset**: where a fact came from (a provider, a file, a prediction model).
  Because it is in the key, two sources that disagree about the same pair on
  the same market both stay; nothing is merged on write.

## What hangs on the keys

**Decided: one series per key**, never one object per bar.

On an edge key (a price of something):

- **Candles**: a time-ordered series of OHLCV bars, one bar per start time, all
  the same interval. The interval belongs to the series; a dataset with two
  intervals (1 minute and 1 day) is two datasets. A bar at a time already in the
  series replaces it, so corrections need no special path.
- **Order book snapshots**: the bids and asks standing at a moment (price and
  size per level), time-ordered. Kept apart from candles because they are a
  different kind of fact, but under the same key. Quotes carry information even
  when nothing traded.

On an event key `(instrument, dataset)` (something said about the company
itself, so it has no market or quote currency):

- **Announcements**: time, category, headline, body, url. Press releases,
  filings, profit warnings. Same time and headline replaces; same time with
  another headline is a second announcement.
- **Corporate actions**: the ex-date, a kind (`Split`, `Dividend`, `Other`), a
  value (new shares per old for a split, amount per share for a dividend), the
  currency instrument of a dividend and a note. Same time and kind replaces.

Both are facts for the prediction models as much as for the screen, so they sit
in the store as data, not as annotations on the table.

Later: predictions from the machine learning models, as their own datasets on
the same keys.

## What the application shows

**A preset names a market, not instruments.** Choosing a tag (Commodities,
Nasdaq Helsinki, Currencies) selects markets. The system looks up which edges
have data there and shows **one table row per edge**. If two datasets have data
for the same instrument on the selected market, they are two rows. Merging such
rows is later work.

A row needs the instrument's symbol and name, the dataset, the latest price and
the change over the last interval (from `lastCandles`), and the trace of recent
closes. So the table gets a Dataset column. The row's identity is the edge key.

## Store

`Store` (`src/model/Store.h`) is the only way in or out; `makeMemoryStore()` is
the in-memory implementation.

- Adding an instrument, market or dataset by symbol/name returns the record
  already there if there is one, so sources can add what they use without
  checking first.
- Lookups by id and by name; lists of all three.
- `edges(market)`: the keys with data in a market (candles or a book).
- `appendCandles`, `candles(key, range)`, `lastCandles(key, n)`.
- `appendBooks`, `books(key, range)`, `bookAt(key, time)` (the snapshot standing
  at a time).
- `appendAnnouncements`/`announcements`, `appendActions`/`actions`, and
  `events(instrument)` for the datasets that say anything about an instrument.
- Reads return values, never references into the store. Time ranges are
  half-open `[from, to)`; time is UTC milliseconds.

## Steps

1. ~~Types, with tests.~~ Done.
2. ~~In-memory `Store`, with tests.~~ Done.
3. Wire the table to it: a preset selects markets, rows are edges, a Dataset
   column, latest price and change from candles. Retire `Quotes.h`'s
   `Instrument` (it has the same name as the model's, which now lives in
   `namespace stocks`).
4. A first source that fills the store (synthetic or CSV), replacing the
   made-up rows.
5. The simple order-book view for the prediction models, once the questions
   below are settled.

## Decided

- Prices are `double`. Nothing here settles trades.
- The model has a slot for corporate actions and announcements, kept as their
  own series (above). Prices are stored as reported; adjusting for splits and
  dividends is computed from the corporate actions when a model asks, never
  written back over the bars.

## Open questions

### The order book in a simple form for the prediction models

The prediction models want something simpler than the full book: *if I want to
buy 1000 shares, what price would I have paid, given the book?* For a buy of
quantity Q, take the asks from the best price up, level by level, until Q is
filled; the effective price is the volume-weighted average of the fills (the
sell side is the same over the bids). The limit as Q goes to zero is the best
ask (or bid), and the difference from the mid price is the cost of the size.

That much is arithmetic. What is not settled:

- **Stored or computed?** Compute from the snapshot when asked
  (`fillPrice(book, side, quantity)`), or also store a derived series at fixed
  quantities as its own dataset on the same edge, so models read a plain time
  series.
- **Which quantities?** A fixed set (100, 1000, 10000 shares), a size relative
  to typical volume, or a notional amount in the quote currency, which compares
  across instruments better than shares do.
- **A book too thin for Q.** No price, the price of what could be filled plus a
  flag for the unfilled part, or a penalty price. A model needs to tell "thin"
  from "missing".
- **Time alignment.** Snapshots do not arrive at candle boundaries. Take the
  last snapshot before the bar's close (`bookAt` does this), an average over the
  bar, or keep them separate and let the model resample.
- **Fees, tick size and hidden liquidity** are ignored in the plain walk;
  whether any belongs in the data layer or in the model.

### Smaller ones

- **Time**: sessions, holidays and time zones are probably on the market.
- **Identity across sources**: one provider calls it `NOKIA.HE`, another
  `NOK1V`. Today a symbol is one instrument; a symbol alias table per dataset,
  or a canonical id assigned when a source is added, is needed once a second
  provider arrives.
- **Threading**: the store is read on the UI thread while sources fill it from
  other threads. Today a store belongs to one thread. Likely a single writer
  posting changes to the UI thread (cellui's `ThreadInbox`) so the UI never
  locks; to be decided with the first source.
- **Gaps in a series**: the store does not fill or mark missing bars; whether it
  should is decided when a source shows what its gaps look like.
