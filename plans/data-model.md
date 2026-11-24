# Data model

Status: built as an immutable persistent store (`src/model/`: `Types.h`, `Store.h`, `MemoryStore.cpp`, tests in
`tests/ModelTests.cpp`). The table shows what the store holds, filled by a data source (or made-up data) on a thread of its own.
The order book and a universal scraper are not built yet.

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

**News** is one more kind of fact, kept per dataset and about any nodes. An item is its dataset
and the dataset's own id for it (what it is, so saying it again changes nothing and the store never
replaces one by another: a dataset that gives duplicates under different ids cleans them up itself, and
two datasets with one story have two items). It has a time, a headline, a body (the preview) and a url, a list of
the nodes it is about (instruments, markets, countries, any node: none for the news of the world, which
needs no node of its own) and attributes: its `kind` in the common vocabulary (`press-release`,
`analysis`, `news`, as an instrument has its kind), the dataset's own `type`, `agency`, `language`. A selection of
the news is made as one of the instruments is, by what it says under a predicate (a preset), and the
newest first is the order. A country is a node (key kind four, its ISO alpha-2 code).

On an event key `(instrument, dataset)` (something said about the company itself, so it has no market
or quote currency):

- **Corporate actions**: the ex-date, a kind (`Split`, `Dividend`, `Other`), a
  value (new shares per old for a split, amount per share for a dividend), the
  currency instrument of a dividend and a note. Same time and kind replaces.

Both are facts for the prediction models as much as for the screen, so they sit
in the store as data, not as annotations on the table.

Later: predictions from the machine learning models, as their own datasets on
the same keys.

## What the application shows

**A preset is a selection of edges**, and the table shows **one row per edge** in it. If two datasets have data for the same
instrument on a market, they are two rows. Merging such rows is later work.

Today a preset is one market by name (or All), the simplest selection, and it will not stay the only one. The ones to expect:

- **A set of markets.** "Commodities" is the commodity markets together (the futures market of each exchange, and the spot-like
  ones), a regional preset the markets of several countries, All every market.
- **A filter** on the edges of those markets: by the base instrument's kind or type (every commodity, wherever it trades), by the
  country of the market, by the dataset.
- **The user's own preset: the edges she follows.** An explicit list of edges (four keys each), added to and removed from with
  commands on a row (follow, unfollow). It is the user's data, not a fact of any node, so it lives in a file of her own beside the
  facts file (and, with a server, per user). An edge's four keys are the same in every run, so the list survives a restart, which
  it could not while ids belonged to one run.
- **A combination**, such as what she follows together with everything in a preset.

A preset is then data and not code: a name and a selection (markets, filters, a list of edges), read from a file; the built-in ones
are a file that comes with the program, and hers is another. `Snapshot::edges(market)` becomes a selection that gives the edges
of several markets (`edgeMarkets()` is where it starts), and the table is told what it shows by `Quotes::follow` as now.
Open: how a selection names "these markets", by a list of names or by a fact on the market ("group: commodities"). A fact keeps
the preset file short, but then whose fact it is (which dataset says that a market is a commodity market) is the statements
question in `server.md`.

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
  `appendNews`, `appendActions`. Writers take a lock one at a time and
  publish the new snapshot with one pointer swap; `snapshot()` never waits. A
  write that changes nothing publishes nothing, and each one that does makes the
  next `version()`.
- Reads on a snapshot: lookups by id and name, `edges(market)` (keys with
  candles or a book in a market), `candles(key, resolution, range)`,
  `lastCandles(key, resolution, n)`, `resolutions(key)` (the resolutions an edge has),
  `books(key, range)`, `bookAt(key, time)`, `news(query)` (a time range, a node, a selection, newest first), `actions`, and
  `events(instrument)` for the datasets that have actions for an instrument.
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
country) are kept in a local SQLite file, so a restart shows what an earlier run was told. Only these: the edges, the bars and
everything else on them are not kept, and there is no server in this step.

- **A fact** is `(node, predicate, value, updated)`. `updated` is when a source last *said* it, whether or not the value
  was the same as before, taken by the store where the write happens (UTC, milliseconds), and only to within the recheck
  interval (`StoreOptions::recheck_interval`, an hour by default): a value said again that was last said less than the
  interval ago changes nothing at all, so a refresh that repeats thousands of facts every few minutes writes none of them;
  an older one gets the time of the write. A changed value is written at once. It is not when the value last
  changed: the data is loaded too seldom to know that, so no such claim is made. `Snapshot::attributes()` and
  `Snapshot::fact()` return it (`Attribute::updated`); `Attribute` equality ignores it. A write of an unchanged value that is
  older than the interval moves `updated` and makes **no new version**, so nothing that watches the version (the table) wakes; a changed value makes one.
- **A node is named by its key** (`src/model/NodeKey.h`, see "Node keys"), the same number in every run, so the file
  needs no matching up: a fact is kept under the key of its node. A node is what is said of it, and a node that has no
  edge is simply not shown by anything.
- **Restoring** reads the file into the store before the sources run: the facts are the node's facts, found by what they say
  (`findByAttribute`, `findInstrumentByIsin`, `findMarket`), and no version is made and no edge. A source that loads some
  predicates leaves the others as an earlier run left them, and a value said again with the same value changes nothing but
  its `updated`. A source that looks a node up before it says it (a currency by its symbol) finds the restored one and does
  not say it again, so its `updated` stands.
- **No two markets share a MIC or an id at a source** (`isUnique()` in `NodeIdentity.cpp`). The store keeps the first and
  refuses the second, and nothing changes (no version). Each refusal is told once through `StoreOptions::notice`, which the
  program shows on the status line. A resolve that gives the MIC of one market and the id of another finds the first and
  does not take the id from the second: that is two records of one market, which only merging them would settle, and it is
  not attempted. A file that already says the same of two markets is told of when it is read, and the lowest key is the
  market. Not enforced in the file itself: a unique index there would fail a whole batch for one conflict, and would put the
  predicates into every backend.
- **Layers** (`src/model/`): `Facts.h` has the plain `StoredFact`/`FactBatch` and the `FactStore` interface (`loadAll()`,
  `apply(batch)`), which a server-backed store can implement instead. `FactWriter` takes batches from the store's writers
  and gives them to the `FactStore` on a thread of its own, so a write never waits for the disk: facts that changed go
  after a short delay, facts only said again wait up to ten seconds and go with other batches, one fact per key, so a source
  that says ten thousand known things again costs one transaction. The destructor flushes. `SqliteFactStore` (SQLiteCpp,
  behind `openSqliteFactStore()`, in no header) is the SQLite one.
- **The file**: one table `facts(node_key INTEGER, predicate, value, tag, updated)` keyed by `(node_key, predicate)`, WAL
  mode, a transaction per batch, the schema version in `PRAGMA user_version` (one schema, `kSchemaVersion`). A file of any other version, older or newer, has its tables dropped and is started over in place, in one transaction, and says so once; nothing is migrated.
  `value` has no declared type, so SQLite's own null, integer, real and text are kept as given (a real that is whole stays a
  real), a truth value is an integer with `tag` 1 and a link to a node the integer of its key with `tag` 2 (schema 5). An undefined number is not said, so it is not kept. A file that
  is not a database or is damaged is moved aside (`.bad-<time>`, with its WAL files) and
  an empty one started; where no file can be made the store keeps nothing and says so. Nothing throws out of `SqliteFactStore`.
- **The user's own file** is another `FactStore`, `portfolios.sqlite` (`--portfolios FILE`, `--no-portfolios`): the facts of the portfolio
  nodes and their transactions, routed by the kind of node, never in `facts.sqlite` and with a schema version of its own; see
  [portfolios.md](portfolios.md), "Persistence".
- **Where**: `--facts FILE`, else `facts.sqlite` in the user's data directory (`XDG_DATA_HOME` or `~/.local/share/stocks`;
  `%LOCALAPPDATA%\stocks` on Windows); `--no-facts` runs without. `makeMemoryStore()` with no argument keeps nothing, which
  is what the tests use.
- **Links.** A value may be a node: `NodeKey` is one more alternative of `Value` (`asNode()`), said and replaced like any
  value (a link to no key says nothing), and found by the same index as a text (`Snapshot::findLinked(type, predicate,
  target)`: who has this node as their `ceo`). In the file it is the integer of the key with `tag` 2 (`tag` 1 is a truth
  value), and the attribute viewer shows it as the name of the node it names, drawn as a link. No validity period, and no reverse
  index across predicates.
- **Not done**: any loader that writes new facts (they will be ordinary attributes through the same writes), and a rule for which source stands when two disagree beyond the one below.
- **Whose name and type stand**: an instrument's `name` and `type` are last said, so two sources or two listings with different
  words would take turns on every pass. The sources decide what they say, not the store: Nordnet says them from the home
  listing (the market whose `country` is the ISIN's first two letters) and from another listing only where none are said yet;
  Yahoo and the fund sources say them only where none are said yet. `kind` and `isin` are the same wherever they come from.

## Node keys

A node has no record of its own: it is its **key** (`src/model/NodeKey.h`), the same number in every run, and what is said of it
is facts. A table shows listing edges only, so what nodes the store holds does not matter. `InstrumentId`, `MarketId` and
`DatasetId` hold a key, so a market cannot be given where an instrument is wanted; zero is no key. The calls that make or find a
node return its id (`addInstrument()`, `resolveInstrument()`, `addMarket()`, `resolveMarket()`, `addDataset()`) and what is
said of it is read with `Snapshot::name()`, `symbol()`, `kind()`, `isin()`, `said()` and `attribute()`; `nodeCount()` and
`edgeMarkets()` take the place of sequences of nodes.

A key is one 64-bit number below 2^63 (a positive `int64` in SQLite), with the kind of node in bits 62-60 and its identity in the
low sixty (country 4 and attribute 6 are planned, 5 is the portfolio, and 7 is an escape: see "Kinds and bits" below):

| kind | node | low 60 bits |
|---|---|---|
| 0 | instrument | its ISIN: 26 x 26 country letters and nine letters or digits make 56 bits, plus one so that AA0000000000 is not zero; the check digit is not stored, since `isinCheckDigit()` gives it back |
| 1 | currency or unit | its code: up to ten letters or digits, six bits each (0 is the end, 1 to 62 are `0-9A-Za-z`), case kept (`pts`) |
| 2 | market | its MIC, the ISO 10383 market code: four characters of `A-Z0-9`, packed in the same way as a currency's |
| 3 | dataset | its code, packed in the same way as a currency's |
| 5 | portfolio | sixty random bits, minted when the user makes one; in text a `P` and fifteen hex digits (`mintPortfolioKey()`, `portfolioKey()`) |

- **An instrument is an ISIN**, or a currency or a unit. An index, a fund or a commodity is an instrument like a share, with an
  ISIN: no kind of its own. One given no ISIN that is not a currency or a unit gets it from its adapter: the ISINs an
  adapter supplies (what its source gives none for, and the physical items) are named constants in `src/source/KnownIsins.h`, the real ones where
  there is one (zinc `XC0005705584`, lead `XC0005705527`, the BRIC 50 India index `US26075E1047`) and else ours, `ZZ` (the user-assigned code element
  of ISO 3166, in which no numbering agency issues ISINs), nine digits numbered on from 1 and the check digit. An adapter names the constant, so a
  missing one does not compile; `static_assert`s make an invalid ISIN, a repeated ISIN and a gap in the numbering fail to compile too, and the head of
  the file says how to add one. Nothing is made up from a text: the program's store refuses an instrument that comes with none
  (`StoreOptions::invent_isins` off) and the status line says so; a store left to invent (the tests) numbers them as they come, from `ZZ999999999` down. One that cannot be keyed, or whose ISIN is not valid, is refused and told of
  (`StoreOptions::notice`), and what is said of an edge with a part that was refused is said of nothing.
- **A market is its MIC**, which never changes, so nothing is minted and nothing is looked up by a name or a provider's id: a source
  that has only an id for a market maps it to a MIC (Nordnet: `KnownMarkets`) and leaves out what it cannot map, saying so. A code
  that is no MIC makes no market (told of), and what is said of an edge with no market **throws** (`std::invalid_argument`); the
  source runner says so and goes on. Three codes stand for what has no venue: **XXXX** for an index, a spot price or a currency
  pair (the quoted price of no exchange), **XOFF** for a fund priced by its company; a real venue, a futures market or a currency
  market, has its own MIC. The name and the country are facts of the market (the name is the MIC until something says otherwise).
  A preset that shows currency pairs or commodities is therefore a selection: market XXXX and the kind of the base. Files of the
  second schema lose their markets on opening (the sources say them again).
- **A dataset** is its **code**, one to ten letters or digits that its adapter defines and never changes (`source/DatasetCodes.h`; a
  fund schema names its own); two sources may not write the same one. Its name is a fact that is the code until something says
  otherwise. A name can change, so a dataset is looked up by its code.
- **Order** of rows: by the keys (an instrument's ISIN, then the quote and the dataset). A key's order means nothing of its own, so
  nothing relies on it, and a heading sorts by what a person looks for. Listing by symbol was tried and costs the symbol lookup of
  every edge on every refresh (about 60 ms of 100 on a market of 50,000 rows).
- **Size.** An `EdgeKey` is four keys of eight bytes. On 50,000 edges of 60 daily bars (`stocks_period_bench`, Release) the store
  takes 310 MB against 317 with the earlier ids, and the edge list, a refresh, a period, a sort and a frame cost the same within
  the noise: the bars are nearly all of the memory.
- The server adopts these keys (`server.md`, "Keys").

### Kinds and bits

A key is a positive `int64`: bit 63 is the sign and is never set, which leaves **63 bits**. Sixty of them are the identity (bits 59-0). The
other three, **bits 62-60, are the kind**: eight kinds, of which four are built and three are planned, so that **one is left over**
and is an escape that cannot run out. `NodeKey::kind()` is already `value >> 60`, which gives 0 to 7 with bit 63 clear, so adding a
kind changes the enum and the table, and no key that exists.

| kind | bits 62-60 | node | state |
|---|---|---|---|
| 0 | 000 | instrument | built |
| 1 | 001 | currency or unit | built |
| 2 | 010 | market | built |
| 3 | 011 | dataset | built |
| 4 | 100 | country | planned (below) |
| 5 | 101 | portfolio | built (below) |
| 6 | 110 | attribute | planned (below) |
| 7 | 111 | **escape**: the identity's top eight bits (59-52) are a second-level kind, and the other 52 bits its identity | reserved |

The escape gives 256 more kinds with 52 bits each, so a new kind never needs a new bit. Nothing is put in it until a kind is needed that
the first seven do not fit. Identities that are a packed code (currency, market, dataset, country, attribute) use at most the low
sixty bits and none depends on the kind bits.

### Countries (planned)

A country is a node of its own kind: kind 4 (bit 62), so every key above stays what it is and keys stay
below 2^63. Its key is its ISO 3166-1 alpha-2 code, packed as a code is (`FI`, `SE`; `XK` for Kosovo, which the standard leaves to
its users). Countries that no longer exist (`SU`, `YU`) are kept: markets and ISINs of them do.

- **What is said of one**: its name, its alpha-3 and numeric codes, its currency and its region, said once and not for every
  market of it.
- **Where it is used.** A market's `country` fact is the country's two letters, so it already names the node: the key is
  `countryKey(text)`, and nothing needs a link before one exists. A preset by country ("every market in Finland") selects on
  it. The first two letters of an ISIN are the country of the issuer or of the numbering agency, which a preset by the country of
  an instrument can use (some are no country: `XS` is international, `EU` the European union). Later the node-valued links
  below make a company's country of domicile and a currency's country links like any other.
- **Open**: whether the `country` fact of a market becomes a node-valued link when there are such links, and the names of a
  country in several languages.

### Attributes (planned)

An attribute (a predicate such as `name`, `isin`, `shares`, `pe`) is a node of its own kind: kind 6. A fact is then `(node, attribute
node, value, updated)` and not `(node, string, value, updated)`, so an attribute has facts of its own: its label ("P/E"), the type of
value it takes (number, text, truth, node), its unit and how it is shown. A new attribute needs no change to the program.

- **Its key is its code**, packed as a currency's (up to ten characters of `0-9A-Za-z`, so `PE`, `MCAP`, `DIVYIELD`), and its label is a
  fact, as a currency's name is: a long or odd name is what the label is for. Open: whether ten characters are enough, or an
  attribute needs a numbered table instead.
- **The facts file** keeps the attribute's key and not its text, so the `predicate` column becomes an integer.
- Not built; it changes `Facts.h`, `Store.h` and every source that says a fact, so it is its own change, before the facts below grow.

### Portfolios

A portfolio is a node of its own kind: kind 5, with a minted key (sixty random bits, as it has no natural identity). It has no
record: **it exists when something is said of it**, a fact or a transaction, and is not listed (`Snapshot::portfolios()`, by name
then key; an unnamed one under its key shortened) when nothing is. What is said of it:

- facts: `name`, `currency` (a link to a currency node) and `ledger` (a link to a dataset node: whose transactions its holdings
  are derived from; the user's own, `kUserDataset`, if none is said);
- **transactions** as events on `(portfolio, dataset)` (`LedgerKey`, `Snapshot::transactions()`, `appendTransactions()`, `removeTransactions()`), each with a
  minted id, replaced by it and kept by day then id. A transaction on a market that is no place to buy (`portfolio::tradable`:
  XXXX) is refused with `std::invalid_argument` and nothing of the call is written;
- **position edges** `portfolio -> instrument` (market, the ledger's dataset), derived and not stored twice:
  `portfolio::holdings(snapshot, portfolio)` is the positions of the one dataset the portfolio names, each with its quantity series
  (`Position::history`). Nothing in a file can disagree with the transactions.

`removeAttribute()` and `removePortfolio()` take back what was said (only a portfolio's: a source's facts are not the user's to
remove). It is the user's own data, so it is kept in a file of its own (`StoreOptions::portfolios`, step 4 of the plan). See
[portfolios.md](portfolios.md).

### Not done

- Any loader that writes new facts (they will be ordinary attributes through the same writes).
- **Links between nodes**, beyond what is built (a value may be a node: see "Links" above): a validity period on a link, and the
  reverse links of a node across all predicates ("is the CEO of"), which are not indexed. See `server.md`.
- A rule for which source stands when two disagree, beyond whose name and type stand (above); otherwise the last said stands.

## Steps

1. The simple order-book view for the prediction models, once the questions below are settled.

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
  the candle source has no discovery, so its adapter has lists of its own, each row with
  the source's ticker for the instrument and the code (ISO 10383) of its market, which finds
  the market the other adapters made (a row with none is on the market its file names), and
  reads them when it loads. They
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
  CURRENCY), and keeps the provider's own type beside it (the `type` fact) as what
  the mapping came from. A certificate is a certificate whoever lists it. Selecting
  edges by type is not used yet (see server.md for what is hard about it).

- Prices are `double`. Nothing here settles trades.
- The model has a slot for corporate actions and news, kept as their
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
