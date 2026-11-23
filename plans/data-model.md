# Data model

Status: steps 1 to 4 are built (`src/model/`: `Types.h`, `Store.h`,
`MemoryStore.cpp`, tests in `tests/ModelTests.cpp`), as an immutable persistent
store. The table shows what the store holds, filled by a data source (or made-up data)
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
  (hourly bars on US exchanges start at :30 at some providers) cannot be keyed by its slot without
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

## What is kept between runs

The node attributes (what the sources said of instruments, markets and datasets: names, icons, share counts, a market's
country) are kept in a local SQLite file, so a restart shows what an earlier run was told. Only these: nodes themselves
are not kept (a node exists when it has edges, and the edges, the bars and everything else on them are not kept), and
there is no server in this step.

- **A fact** is `(node, predicate, value, updated)`. `updated` is when a source last *said* it, whether or not the value
  was the same as before, taken by the store where the write happens (UTC, milliseconds). It is not when the value last
  changed: the data is loaded too seldom to know that, so no such claim is made. `Snapshot::attributes()` and
  `Snapshot::fact()` return it (`Attribute::updated`); `Attribute` equality ignores it. A write of an unchanged value moves
  `updated` and makes **no new version**, so nothing that watches the version (the table) wakes; a changed value makes one.
- **The key of a node** is not its id, which belongs to one run, but what identifies it (`src/model/NodeIdentity.cpp`, the
  one place): an instrument's ISIN; a currency's or a unit's code (the instruments with no ISIN); a dataset's name. The stored
  node type says which, so a key has no prefix. Facts of a node that has no key yet are not written until it has (an instrument
  with no ISIN that is no currency or unit has none), and when a stronger one is learned the facts move to it (the old rows are
  forgotten in the same batch). **A market has a surrogate key**, 16 random hex digits the store makes when it first keeps a fact of the
  market: some markets have no MIC, so the MIC is a fact of the market like its name or its id at a source, not its key. The
  key never changes however much is learned of the market. A market of a later run is *recognised* by what it is said to be: the
  held market that has the same MIC, else the same id at a source, else the same name (`recognisedBy()`), and a market none
  of the held ones is said to be is new.
- **Restoring makes no node.** The file is read into the store before the sources run and kept as *held* facts under their
  node keys. A node that is made again (a source resolves it by its ISIN, a market by its MIC, source id or name) takes what is held for it, for the
  predicates that have not been said in this run; so a source that loads some predicates leaves the others as an earlier
  run left them, and a value said again with the same value changes nothing but its `updated`. A node with no edges
  is still not seen: `instruments()` of a fresh store is empty, and nothing selects a node that has none.
- **No two markets share a MIC or an id at a source** (`isUnique()` in `NodeIdentity.cpp`). The store keeps the first and
  refuses the second: a MIC or an id that another market has, in the store or among the held facts of an earlier run, is not
  said of this one, and nothing changes (no version). Each refusal is told once through `StoreOptions::notice`, which the
  program shows on the status line. A resolve that gives the MIC of one market and the id of another finds the first and
  does not take the id from the second: that is two records of one market, which only merging them would settle, and it is
  not attempted while edges refer to markets by id. A file that already says the same of two markets is told of when it is
  read, and the lowest key is the market. Not enforced in the file itself: a unique index there would fail a whole batch for
  one conflict, and would put the predicates into every backend.
- **Layers** (`src/model/`): `Facts.h` has the plain `StoredFact`/`FactBatch` and the `FactStore` interface (`loadAll()`,
  `apply(batch)`), which a server-backed store can implement instead. `FactWriter` takes batches from the store's writers
  and gives them to the `FactStore` on a thread of its own, so a write never waits for the disk: facts that changed go
  after a short delay, facts only said again wait up to ten seconds and go with other batches, one fact per key, so a source
  that says ten thousand known things again costs one transaction. The destructor flushes. `SqliteFactStore` (SQLiteCpp,
  behind `openSqliteFactStore()`, in no header) is the SQLite one.
- **The file**: one table `facts(node_type, node_key, predicate, value, is_bool, updated)` keyed by
  `(node_type, node_key, predicate)` (node type 0 instrument, 1 market, 2 dataset, 3 currency or unit), WAL mode, a transaction per batch, the schema version in `PRAGMA user_version` (1).
  `value` has no declared type, so SQLite's own null, integer, real and text are kept as given (a real that is whole stays a
  real) and a truth value is an integer with `is_bool` set. An undefined number is not said, so it is not kept. A file that
  is not a database, is damaged or has a newer `user_version` is moved aside (`.bad-<time>`, with its WAL files) and
  an empty one started; where no file can be made the store keeps nothing and says so. Nothing throws out of `SqliteFactStore`.
- **Where**: `--facts FILE`, else `facts.sqlite` in the user's data directory (`XDG_DATA_HOME` or `~/.local/share/stocks`;
  `%LOCALAPPDATA%\stocks` on Windows); `--no-facts` runs without. `makeMemoryStore()` with no argument keeps nothing, which
  is what the tests use.
- **Next, when this is merged: nodes without records or ids**, planned in "Nodes without records" below.
- **Not done**: any loader that writes new facts (they will be ordinary attributes through the same writes), node-valued links,
  and a rule for which source stands when two disagree (the last said stands).

## Nodes without records

Planned, to be done after the facts are kept (see above). A table shows listing edges only, so what nodes the store holds does
not matter, and a node should have no data of its own, not even that it exists, beyond the facts said of it and the edges that
have it as an end. So there are no `Instrument`, `Market` or `Dataset` records, no `InstrumentId`, `MarketId`, `DatasetId` or
`NodeRef`, and no sequences of nodes. The surrogate ids are handed out per run in the order nodes are first resolved, are used as
positions in those sequences and as parts of `EdgeKey`, and so cannot be kept. What replaces them is a **key** that means the
same thing in every run, so that the facts file, the edges and the server all name a node in the same way.

**The key** is one 64-bit number, below 2^62 (so a positive `int64` in SQLite), with the kind of node in its top two bits and the
identity of the node in the low sixty. The API has a type for each kind (`InstrumentKey`, `MarketKey`, `DatasetKey`) over the same
representation, so a market cannot be given where an instrument is wanted. Zero is no key.

| bits 61-60 | node | low 60 bits |
|---|---|---|
| 0 | instrument | its ISIN: 26 x 26 country letters and nine letters or digits make 56 bits; the check digit is not stored, since `isinCheckDigit()` gives it back |
| 1 | currency or unit | its code: up to ten letters or digits, six bits each (0 is the end, 1 to 62 are `0-9A-Za-z`), case kept (`pts`) |
| 2 | market | sixty random bits, made by the store the first time a fact or an edge needs the market |
| 3 | dataset | its code, packed in the same way as a currency's |

- **ISINs are not interned.** They fit: 2 letters and 9 characters of 36 make 6.9 x 10^16 values, under 2^56. The ISINs we make
  ourselves (`ZZ` and nine characters, `privateIsin()`) are valid ISINs and pack the same way. Only a valid ISIN is accepted at the
  edge of the store (`isValidIsin()`, as now); a row with another is reported and not stored.
- **An instrument is an ISIN, or a currency or a unit.** Nothing else has a key: a source that gives an instrument no ISIN gives
  it one of ours or the row is skipped and reported, as the lists already require.
- **Currency and unit codes** (the easy one): `EUR`, `USD`, and units such as `pts`. The code is its own key, up to ten
  characters of `0-9A-Za-z`, compared exactly. A longer code or another character is refused with a notice, as a duplicate MIC is.
  Nothing is looked up: unpacking gives the text.
- **Markets** have no identifier all of them have (some have no MIC), so a market's key is a surrogate, and its MIC, its id at a
  source and its name are facts of it like any other. No two markets say the same MIC or the same id at a source (`isUnique()`).
  A market is made when a source resolves one by what it says it is (a lookup of facts by text, as now) and none is there: the
  key is minted and nothing else is made. Merging two markets that turn out to be one is not attempted. Open: the key is minted
  where the store is, so it is stable on one machine only. With a server the catalog has to be the one that mints, and a client
  asks it, or the markets two machines learn of are matched by their MICs and ids when they meet.
- **Datasets** cannot be keyed by name: a name is what a person reads, and it should be free to change. Each adapter defines its
  **dataset code**, a constant in the adapter, up to ten characters of `0-9A-Za-z` (`nordnet`, `yahoo`, `synthetic`), and the
  name, the provider and what the dataset's days and prices mean are facts of the dataset, keyed by the code. A schema-driven
  adapter (a fund company's schema file) names its code in the schema. Rules: a code is permanent, and a dataset whose
  meaning changes gets a new code, since bars under the old one keep the old meaning; two adapters with the same code is a
  mistake the program refuses to start with. Open: ten characters may be too few once prediction models each write a dataset
  of their own; the key would have to grow to 128 bits, which only `EdgeKey` and the file would feel.

**What goes, and what takes its place**

- `held_`, adoption, `keyOf_`, `heldMarkets_` and the notion of a held market: the facts are one map from a node's key to its
  facts, loaded from the file, and there is nothing to restore into and no second kind of "not yet a node". Whether a node
  exists is whether it has facts or appears in an edge; no record says so. The index from a fact's text to the keys that say it
  (`findByAttribute`, what resolving and the unique rule use) stays and is the only registry.
- `Instrument`, `Market` and `Dataset` as stored views, rebuilt on every write (`instruments_.set(...)`), and
  `instruments()`, `markets()`, `datasets()`. A market's name is `attribute(key, name)`. Whether an `Instrument` (symbol, name,
  kind, ISIN, type) stays as a convenience built from the facts when asked is to be decided with the table, which reads a few
  facts of each row. "The markets with data" and "the instruments with data" come from the edges, as `edges(market)` already does.
- The `resolve*`/`add*` calls return keys. They make a market key only when it is not found, and say its facts.
- `EdgeKey`, `EventKey`, `SeriesKey`, `Listing` and their hashes hold keys. An `EdgeKey` is four of them, 32 bytes where it is 16
  now; measure the persistent maps on a full store before anything else depends on the size.

**What else it touches**

- **Order.** Today rows come in the store's key order, which is the order the nodes were made. A key's order means nothing (a
  random market, an ISIN as a number), so the order of rows, of `edges()` and of anything shown is by a name or symbol, stated
  where it is used. The tests that rely on creation order change.
- **The file** has `facts(node_key INTEGER, predicate, value, is_bool, updated)` keyed by `(node_key, predicate)`: the node type is in the key, so the
  column and the key's prefix are both gone, and the schema is version 2. Old rows are converted (an ISIN or a code is packed, a
  market's 16 hex digits give its low sixty bits, a dataset's name maps to its adapter's code) or the file is discarded: it only
  holds what the sources said, and says it again at the next load. Decide then.
- **The server** (`server.md`): its `edge_id` becomes a hash of the four keys, or a catalog sequence, and "an internal
  instrument id plus the ISIN" becomes the packed ISIN. A currency pair's quote and base are currency codes, which settles
  what identifies instruments with no ISIN there. The market key authority above is its open question.
- **The program:** `Quotes` caches dataset names by id, `AttributeView` shows the facts of a node by `NodeRef`, the sources and
  nearly every test pass `.id` around. A key is shown in messages as text (the ISIN, the code, the market's name or its hex),
  by one function.
- **Order of work**, each step with the tests passing: the key types and their packing, with round-trip tests (every ISIN class,
  a private ISIN, the longest code, a code too long); the facts map and its index in place of the node records and the held
  facts; `EdgeKey` and the series maps; the sources, each with its dataset code; the table and its order; the file, schema 2;
  `server.md`.

## Steps

1. ~~Types, with tests.~~ Done.
2. ~~In-memory `Store`, with tests.~~ Done.
3. ~~Wire the table to it~~ Done: a preset is a market of the same name, rows
   are edges (`QuoteRow` is the display row built from a snapshot), a Dataset
   column, latest price and change from the candles. Rows come in the store's
   key order.
4. ~~A first real source~~ Done: a candle source (see Sources). The table follows the
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
- **Instrument lists** (`src/source/InstrumentLists.cpp`, `data/lists/*.csv`):
  the candle source has no discovery, so its adapter has lists of its own, one file per
  market, with the source's ticker for each instrument, and reads them when it loads. They
  stand in until every adapter finds its own instruments, and then they go; the synthetic
  source follows them meanwhile. They are checked against the source by
  `tools/lists/build_lists.py` and are not complete: about 100 Helsinki shares, 30
  commodity futures, 88 currency pairs. No other part of the program knows them.
- **Candle source** (its own dataset): one chart request per
  instrument (a range, daily bars, dividend and split events), a second host as a
  fallback, a pause between requests, a stop on rate limiting. Days without a
  price are skipped; dividends and splits become corporate actions (a split is
  new shares per old). Unofficial, no key, for personal use. Its history is
  already adjusted for splits retroactively, so the series does not show a split's
  jump; the action is stored beside it. Handling that adjustment is a necessary
  feature (see server.md).
- **Discovery source** (no key, no login): discovery from an unofficial instrument
  search. It loads every list it knows in the same way, with no category of its own for
  each: ETFs, certificates, funds, stocks and the indicator lists, a page at a time into
  the store, and again every few minutes (`Source::refreshInterval`, run by `SourceRunner`). Each listing is an
  instrument (a node, with its ISIN and the provider's own type) and, from the price block
  of the listing, today's daily bar under the source's dataset, replaced at each
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
  maps its provider's types onto it (`src/source/InstrumentTypes.cpp`: UETF
  and KETF are ETFs and PTC are certificates; EQUITY, ETF, INDEX, FUTURE and
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
