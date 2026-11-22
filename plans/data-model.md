# Data model

Status: steps 1 to 4 are built (`src/model/`: `Types.h`, `Store.h`,
`MemoryStore.cpp`, tests in `tests/ModelTests.cpp`), as an immutable persistent
store. The table shows what the store holds, filled by Yahoo (or made-up data)
on a thread of its own. The order book, announcements and a universal scraper
are not built yet.

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

- **Candles**: time-ordered series of OHLCV bars, one bar per start time. **The
  resolution is not part of the key**: an edge has a series for each resolution it has
  data in (5 minutes, 15 minutes, an hour, a day, ...), the same edge with its data in
  different forms, each as the provider gave it and none derived from another. A bar
  at a time already in the series replaces it, so corrections, and today's bar kept up
  to date, need no special path.
  **Time.** An instant is whole Unix seconds, UTC (never milliseconds; where whole
  seconds are not enough, as for order books, floating-point seconds). A resolution is
  a code (`Resolution`: `Min1` to `Hour4`, `Day`, `Week`, `Month`, `Quarter`), not a
  length: days, weeks and months are not fixed lengths. The key of a bar is a number, as
  in the legacy platform. **Intraday bars are keyed by their slot number**: the UTC
  seconds of the start divided by the length of the resolution, rounded down (the hour
  number is the UTC seconds divided by 3600). The **date resolutions are keyed by a
  date, as a day number** (the absolute day number of the proleptic Gregorian calendar,
  1 = 1 January of the year 1, as in Date::Calc's `Date_to_Days`; `Dates.h`): a daily bar
  by its trading date, a week, month or quarter bar by its first day (a Monday for a
  week; absolute week numbers are to come from the day numbers, and months and quarters
  have no numbering yet). A daily bar is not an instant, and providers' midnight stamps
  on them are only markers of the date. A bar that does not start on a slot boundary
  (Yahoo's hourly bars on US exchanges start at :30) cannot be keyed by its slot without
  losing its offset: an open question, see server.md.
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
the change over the last day (from `lastCandles` of the daily series; an edge with
only intraday bars has no row yet), and the trace of recent closes. So the table gets a Dataset column. The row's identity is the edge key.

## Store and snapshots

**Decided: the store is an immutable, persistent value** (`immer` vectors and
maps). `Snapshot` (`src/model/Store.h`) is one version of everything: it never
changes, copying it is cheap, and a write makes a new snapshot that shares almost
all of its storage with the one before. A reader takes `store.snapshot()` once
and reads it as long as it likes, with no locks, and sees one consistent world.
`Store` is the write side and the source of snapshots; `makeMemoryStore()` is
the in-memory one, and a server-backed store would hand out snapshots too.

- Writes: `addInstrument`/`addMarket`/`addDataset` (by symbol or name: the
  record already there is returned), `appendCandles`, `appendBooks`,
  `appendAnnouncements`, `appendActions`. Writers take a lock one at a time and
  publish the new snapshot with one pointer swap; `snapshot()` never waits. A
  write that changes nothing publishes nothing, and each one that does makes the
  next `version()`.
- Reads on a snapshot: lookups by id and name, `edges(market)` (keys with
  candles or a book in a market), `candles(key, resolution, range)`,
  `lastCandles(key, resolution, n)`, `resolutions(key)` (the resolutions an edge has),
  `books(key, range)`, `bookAt(key, time)`, `announcements`, `actions`, and
  `events(instrument)` for the datasets that say anything about an instrument.
- Series come back as `Seq<T>` (an immer `flex_vector`): slices share storage
  with the snapshot, and a reference into one is valid as long as it is held.
  Time ranges are half-open `[from, to)`, in seconds or in day numbers as the series is keyed.
- Appending at the end of a series is effectively constant time; a correction or
  late item in the middle is logarithmic. Tested with readers and writers on
  several threads, and clean under ThreadSanitizer.
- The UI takes a snapshot per refresh. The writer tells it that a new version
  exists through cellui's `ThreadInbox`; the UI never waits on the store.

## Steps

1. ~~Types, with tests.~~ Done.
2. ~~In-memory `Store`, with tests.~~ Done.
3. ~~Wire the table to it~~ Done: a preset is a market of the same name, rows
   are edges (`QuoteRow` is the display row built from a snapshot), a Dataset
   column, latest price and change from the candles. Rows come in the store's
   key order.
4. ~~A first real source~~ Done: Yahoo (see Sources). The table follows the
   store as sources write to it.
5. The simple order-book view for the prediction models, once the questions
   below are settled.

## Sources

A **source** (`src/source/Source.h`) is an adapter for one provider: a blocking
`load(store, http, listener)` that reads the provider and writes what it finds
into the store as its own dataset. The app has several, and the user picks which
run (`--source NAME`, repeatable; later a setting). `SourceRunner` runs them one
after another on a thread of its own; its callbacks are posted to the UI thread
through cellui's `ThreadInbox` (`StoreChanged`, `SourceMessage`), where the
browser takes a new snapshot, keeping the sort and the selected row, and asks
for a frame. The UI never waits on a source.

- **HTTP** goes through [httpclient](https://github.com/rekola/httpclient)
  (`src/source/Http.h`): WinHTTP on Windows, libcurl elsewhere. Sources see only
  the small `Http` interface, so tests give them canned responses. httpclient has
  no build system of its own, so `CMakeLists.txt` compiles the two backend
  sources it needs.
- **Yahoo's instrument lists** (`src/source/InstrumentLists.cpp`, `data/yahoo/*.csv`):
  Yahoo has no discovery, so its adapter has lists of its own, one file per market,
  with Yahoo's ticker for each instrument, and reads them when it loads. They stand in
  until every adapter finds its own instruments, and then they go; the synthetic source
  follows them meanwhile. They are checked against Yahoo by
  `tools/yahoo/build_lists.py` and are not complete: about 100 Helsinki shares, 30
  commodity futures, 88 currency pairs. No other part of the program knows them.
- **Yahoo** (`YahooSource`, dataset `yahoo`): one chart request per
  instrument (`range`, daily bars, `events=div|split`), second host as a
  fallback, a pause between requests, a stop on rate limiting. Days without a
  price are skipped; dividends and splits become corporate actions (a split is
  new shares per old). Unofficial, no key, for personal use. Yahoo's history is
  already adjusted for splits, so the series does not show a split's jump; the
  action is stored beside it.
- **Nordnet** (`NordnetSource`, no key, no login): discovery from Nordnet's
  unofficial instrument search. It loads every list it knows in the same way, with no
  category of its own for each: the ETFs (about 2300) and the certificates of type PTC
  (about 1100), as 7 requests, a page at a time into the store, and again every five
  minutes (`Source::refreshInterval`, run by `SourceRunner`). Each listing is an
  instrument (a node, with its ISIN and Nordnet's own type) and, from the price block
  of the listing, today's daily bar under the dataset `nordnet`, replaced at each
  reload and added at a new day. **An edge exists only when it has data**: a node with
  no edge is not seen, and a listing that has not traded has no bar. An instrument
  listed on two markets is one instrument and two edges (an instrument with an ISIN is
  found by it, and by nothing else). Edges can be selected by the base instrument's
  type or kind (`Snapshot::edges(market, filter)`). The history step loads the earlier
  bars. Its endpoints are described in [server.md](server.md).
- **Synthetic**: made-up prices, for runs without a network.

Not done:

- A universal scraper: one adapter driven by a description of a page (where the
  rows, symbols, dates and numbers are) instead of a hand-written adapter per
  site, so new targets are data, not code. Needs a design of that description;
  JSON endpoints and HTML tables are the two shapes to cover.
- A disk cache of raw responses, so the app starts without a network and does
  not ask the provider for the same history on every launch (and asks only for
  what is new).
- A way to pick sources and filter them by use case at run time, and a refresh
  (`--source` and `--market` pick them at start-up).
- Announcements (a source for company news) and a real order-book source (crypto
  exchanges are the free ones).

## Decided

- **One vocabulary of instrument types.** `InstrumentKind` (company, currency,
  commodity, index, ETF, certificate, other) is the combined vocabulary: each adapter
  maps its provider's types onto it (`src/source/InstrumentTypes.cpp`: Nordnet's UETF
  and KETF are ETFs and PTC are certificates; Yahoo's EQUITY, ETF, INDEX, FUTURE and
  CURRENCY), and keeps the provider's own type beside it (`Instrument::type`) as what
  the mapping came from. A certificate is a certificate whoever lists it. Selecting
  edges by type is not used yet (see server.md for what is hard about it).

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
- **Gaps in a series**: the store does not fill or mark missing bars; whether it
  should is decided when a source shows what its gaps look like.

## Concurrency options

Decided: C, with immer (built). The others are kept for the reasoning. The need: the UI reads the store while sources fill it from other
threads, and prediction models will read long histories without freezing the
screen or seeing half an update. Today a store belongs to one thread.

**A. One owner, changes posted to it.** Sources send batches to the thread that
owns the store (cellui's `ThreadInbox`); only that thread touches the store.
Simplest and needs nothing new. Cost: a model on another thread cannot read,
only get copies, and a big append blocks the UI thread while it is applied.

**B. A lock around the store** (`std::shared_mutex`). Reads already return
values, so a read is a copy made under the lock. Easy to retrofit. Cost: a long
range read blocks the writer and the reverse; a model sees a different world on
each call unless it holds the lock across many calls.

**C. Immutable, persistent store (Clojure style).** The whole store is an
immutable value. A write builds a new version that shares almost all of its
structure with the old one (persistent maps and vectors, as in the `immer`
library: header-only, Boost licence, with a persistent vector and map, and
`flex_vector` that slices and concatenates cheaply). One writer publishes each
new version by swapping one pointer (`std::atomic<std::shared_ptr>`); a reader
takes the current version once and reads it for as long as it likes, with no
locks and no waiting.
- A model gets a consistent as-of view of everything (candles, books,
  announcements, actions) for free, and a backtest can hold the exact version
  it ran on.
- Old versions are cheap, so undo, comparing versions, and "what did we know at
  version N" cost almost nothing; reads can return slices that share storage
  instead of copying.
- Appending to the end of a series, the common case, is effectively constant
  time; a correction in the middle is logarithmic (base 32), not a copy of the
  series.
- Costs: a dependency (or our own structure), somewhat more memory and slower
  scans than a flat `std::vector`, and the interface splits in two: an
  immutable `Snapshot` with all the read methods, and a `Store` that takes
  writes and hands out snapshots. The current `Store` already separates reads
  from writes, so that split is mechanical. The UI would take a snapshot per
  refresh and the writer would tell it "version N exists" through the inbox.

**D. Copy-on-write chunks.** The dependency-free version of C. A series is a
list of immutable chunks (say 1024 bars) held by `shared_ptr`; an append copies
the last chunk and the list of chunks, not the series. Readers keep the chunks
they took. Same snapshot interface as C, less machinery, worse for corrections
far back and for sharing between versions of a whole store.

**E. Append-only series with lock-free readers.** Chunked storage that is
never moved, plus an atomic length: readers see the bars up to the length they
loaded. Very fast for pure appends, but corrections (the replace rule) and
announcements arriving out of order break it, so it would need to be combined
with one of the above.

**F. A database as the store** (SQLite in WAL mode, or a columnar format).
Snapshot reads and durability for free, and it points at the server later.
Costs: a heavy dependency, an impedance mismatch for series of bars, and the
model's types become a schema.

Chosen: C behind a `Snapshot`/`Store` split, because the prediction models are
the readers that most need a stable view, and the UI gets lock-free reads. A is
still the way to tell the UI that something changed.
