# Server, API and bars

Status: plan, nothing built. A living document: it holds what is decided, what is
proposed (so it can be argued with) and what is open. The desktop app's model is
in [data-model.md](data-model.md); this is the other end of it.

## Goals and constraints

Decided:

- **A server that loads data all the time** from adapters and serves it to the
  desktop app, to other tools and to research jobs. It holds **all the
  instruments**, with **no limit on the amount of data**.
- **History back to 1987** where there is any (nice to have; needed for the market
  regime embeddings and their visualizations). **Intraday** data is needed.
  **Order books** are useful.
- **Open source only, and nothing Java.** Self-hosted by the owner.
- **Nobody outside the server touches the databases.** The desktop app and any
  other client use the HTTP API and do not know what the databases are. The
  adapters running *on the server* write directly to the databases; an adapter in a
  client never reads one, it reads the API (the app's `server` adapter, below).
- The adapters are the same code on the server and in the app (`Source`, the
  universe lists, httpclient); they differ in where they write.
- **The existing MariaDB stays**, with the good data in it (one row per bar). The
  new system does not need it, but the server keeps it and serves what is in it.
  The catalog of the new system lives in MariaDB too (see Catalog).
- **Intraday for every instrument, for a few years** (1 hour, 15 minute and
  5 minute bars; see Bars). Daily bars go back as far as there is data.
- **Next adapter: Nordnet**, which has unofficial endpoints for both discovery
  (the instruments) and history. The endpoints are already researched; ETF
  discovery is the first part (see Adapters).

Later, and the reason the reads have to be good: market regime embeddings and
their visualizations, which read long histories of many instruments at once.

## Overview

    server side only:
    adapters (Nordnet, Yahoo, ECB, ...)
        -> ingest (dedupe, version, write directly)
            -> catalog        instruments, markets, datasets, edges, aliases   (MariaDB)
            -> bars           candles, every interval                          (ClickHouse)
            -> books          order book snapshots                             (ClickHouse, later)
            -> events         announcements, corporate actions
        -> API (HTTP)         the only way in or out for everything else
            -> desktop app (a `server` adapter fills its Store), tools, research jobs

## Immutability and a server that never stops loading

The desktop app's `Snapshot` is immutable, and the server takes data all the time:
these do not conflict, because the two things that are immutable are different.

- **In the client**, an immutable snapshot is what a reader holds while it reads.
  New data makes a new snapshot that shares almost everything with the old one;
  it never makes the old one wrong. Appending is cheap, so a constant stream of
  updates costs little.
- **On the server**, nothing is edited in place either: **data is only added.** A
  bar, a book snapshot or an announcement is a row; a correction is a new row
  with a higher **version**. A version is the ingest sequence number, one per
  batch. This is how the column stores work inside anyway (immutable parts,
  merged in the background).
- What that buys: a reader can ask for the data **as of version N** and get the
  same answer every time (reproducible research and embeddings); a client can ask
  for **what changed since version N** (live updates for the app); caches and
  replicas are simple, because a version of a series never changes.
- What it costs: reading the current value of a series means taking the highest
  version of each bar, which needs care in the schema (see Bars).

## Technology

Constraints: open source (OSI licence), not Java, self-hosted.

| | Licence, language | Verdict |
|---|---|---|
| **ClickHouse** | Apache 2.0, C++ | **Proposed for bars and books.** Columnar, compresses time series well, fast range scans, bulk inserts, runs on one machine. |
| **MariaDB** (the existing one) | GPL, C++ | **Decided for the catalog**, and for the legacy bars, which stay as they are. |
| PostgreSQL | PostgreSQL licence, C | Not worth the move (see below). |
| **DuckDB + Parquet** | MIT, C++ / Apache 2.0 | For exports to research jobs, an offline cache in the app, and backups of cold history. Not the server. |
| TimescaleDB | Apache 2.0 core, but compression and continuous aggregates under a source-available licence | Out: not open source in the parts that matter. |
| QuestDB | Apache 2.0, mostly Java | Out: Java. |
| Kafka, Cassandra, Druid, Pinot, Elasticsearch | Java | Out. |

To check before relying on it: each licence and language claim above is from
memory; confirm them when a choice is made.

**Why not PostgreSQL for the catalog.** It would give richer column types (arrays,
JSON with indexes, ranges), partial indexes and transactional schema changes. The
catalog is small and plain, so none of that is needed, and MariaDB has most of it
anyway. Running one engine fewer is worth more. The only new engine is ClickHouse,
and it can read MariaDB tables itself (its MySQL table engine and `mysql()`
function work against MariaDB, to be confirmed), which helps with the legacy data.

**Server language, proposed: C++ for the HTTP API too.** The reasons: the wire
format (the columnar chunks) is then one piece of code shared by the server and the
app, instead of two implementations that can drift; the model, the adapters and
httpclient are shared; one toolchain and one CI. The cost is that an internet-facing
service in C++ must be hardened: a reverse proxy (nginx or Caddy) in front for TLS,
limits and authentication, a small read-only surface, sanitizer builds in CI, and
fuzzing of what parses untrusted input (requests, and the providers' JSON). HTTP
server library candidates (all open source): Drogon, cpp-httplib, Boost.Beast, Crow;
pick by trying them on the real endpoints. The ClickHouse client (native protocol
through clickhouse-cpp, or its HTTP interface with a binary format) is open.

## Bars

Candles of several intervals: daily back as far as there is data (1987 where it
exists), and intraday for every instrument for a few years.

Sketch, to be argued with (ClickHouse):

    bars(
      edge_id   UInt64,            -- (base, quote, market, dataset), as in the app
      resolution UInt8,            -- a code: min1 ... day, week, month, quarter (see Calendars and resolutions)
      t         Int64,             -- the start of the bar: UTC seconds, or a day number for a date resolution
      o, h, l, c Float64,
      volume    Float64,
      version   UInt64             -- the ingest batch that wrote this row
    ) ENGINE = ReplacingMergeTree(version)
      ORDER BY (edge_id, resolution, t)

- One row per bar: the column store makes that cheap (delta-coded timestamps,
  float codecs, compression), unlike a row store. Prices are doubles, as in the
  app.
- The current value of a bar is the row with the highest version; as of version
  N is the highest version up to N. Open: whether to query with `FINAL`, with
  `argMax`, or to keep a second "current" table fed by a materialized view, for
  the long scans that embeddings do.
- A dataset is part of the edge key, so two providers' bars for the same
  instrument sit side by side, as in the app.

Decided:

- **Intervals: 1 day, 1 hour, 15 minutes and 5 minutes** for now. Each is loaded
  and stored **separately**, as the provider gives it: nothing is derived from
  another (no hourly bars made from 5-minute ones), and there are no ticks.
- **Depth differs by interval.** Daily goes back as far as the data does; the
  intraday intervals go back a few years, the 5-minute ones the least.
- **Every instrument gets intraday**, not a selection.
- **The interval is not part of the edge key.** An edge (an instrument on a market in
  a currency, as a dataset gives it) has its data in different forms: a series per
  interval. So datasets are named for the provider (`nordnet`, `yahoo`), not for an
  interval, and `bars` is keyed by edge, then resolution. The app's `Store` does this
  now: `candles(edge, interval, range)` and `intervals(edge)`.

Open:

- **A pyramid of bars.** Coarser levels (weekly, monthly) beside the daily and
  intraday ones, so that an instrument without intraday data can still be compared
  with one that has it: compare at the finest interval they all have. Whether it
  helps is not clear yet. What it would give: charts and embeddings over decades
  read a few thousand weekly bars instead of tens of thousands of daily ones; and a
  common resolution for every instrument. What it costs: derived data has to say how
  it was made (weeks and months follow the exchange's calendar and time zone), and
  it can be made on demand from the finer series (a query, or a materialized view)
  instead of stored. A provider's own coarse bars and ours made from finer ones are
  different data and would be different series. Proposed: derive on demand first,
  store a level only when a read is too slow.
- **Time, decided.** An instant is **whole Unix seconds, UTC; never milliseconds** (the
  Unix habit: whole seconds when they are enough, floating-point seconds otherwise, as
  for order book snapshots and any ticks). A provider that gives milliseconds (Nordnet)
  is converted where it comes in. A date is a day number (see Calendars and resolutions).
  A provider's intraday bars follow its own session times, so the bar's start is what
  the provider says, converted. Sessions, holidays and time zones belong to the market
  record and are not designed yet.
- **Corporate actions and adjustment.** Prices are stored as reported and the
  actions are stored beside them. Adjusting for splits and dividends (needed for
  1987 to be usable) as a query-time view, or as a derived dataset? A provider's
  history may already be adjusted, and that has to be recorded per dataset.
- **Partitioning and codecs**, measured, not guessed.

### Calendars and resolutions

Decided: **days and weeks are not fixed lengths**, any more than months and quarters.
Daylight saving gives days of 23 and 25 hours, and sessions, weeks and holidays follow
the exchange's calendar.

- A series' **resolution** says how big its bars are: `min1`, `min2`, `min5`, `min15`,
  `min30`, `hour1`, `hour4`, `day`, `week`, `month`, `quarter` (Nordnet's vocabulary;
  Yahoo's maps onto it). It is a symbolic **code** (a small number or an enum naming
  one of these), not a number of seconds: a day is not `86 400` s. The app's
  `Store` already takes this code (`Resolution`), not a length.
- **Two kinds of time axis** (proposed, after the owner's earlier system):
  - **Intraday resolutions** (`min1` to `hour4`) are keyed by an **instant**: UTC
    seconds, the start of the bar. These are fixed lengths, and a bar's end is its
    start plus the resolution.
  - **Date resolutions** (`day`, `week`, `month`, `quarter`) are keyed by a **date**, as a
    **day number**: the absolute number of the day in the proleptic Gregorian calendar,
    1 being 1 January of the year 1 (`Date_to_Days` of Date::Calc, Python's
    `date.toordinal()`): 1998-05-01 is 729510, 2026-11-20 is 739940, and the Unix epoch
    1970-01-01 is 719163 (so a Unix day number is this minus 719163). A day bar is keyed
    by its trading date; a week, month or quarter bar by the day number of its first day.
  - A daily bar is not an instant. Trading does not start at midnight, and the 00:00
    UTC that Nordnet puts on its daily bars is only a marker for the date, so the date is
    what is stored; storing 00:00 UTC would invent a time and a time zone. With dates,
    daylight saving and time zones do not enter at all, and bars of different markets
    line up by date, which is what comparing instruments needs.
- **Coarser date levels follow from day bars by date arithmetic alone**: the week of a
  date, its month and its quarter need no calendar of the market, so the pyramid of
  weekly, monthly and quarterly bars (see the question above) can be made from daily
  bars without the market's hours or time zone. Only making daily bars from intraday
  ones needs the market's calendar (which instants belong to which trading date).
- Conversion is pure arithmetic (no time zone database): a day number to year, month and
  day and back, and the weekday from the number modulo 7. In ClickHouse a day number
  fits 32 bits and consecutive trading days differ by 1, which compresses better than
  timestamps do.
- **The legacy data**: if the existing MariaDB tables already hold day numbers in this
  convention, using the same convention here means no conversion when they are served
  or migrated. To be checked against its schema.
- **From an instant to a trading date** (where a provider gives only instants, such as
  Nordnet's discovery tick time) needs the market's time zone: the date of the instant
  there. Yahoo's chart meta carries the exchange's UTC offset (`gmtoffset`). Until a
  market has a calendar, the app takes the UTC date, which is right for the European
  markets seen so far and wrong for markets east of UTC+9 around their open.
- **24-hour markets** (currencies, many commodity futures): the owner recalls that
  daylight saving may not apply to them. What "a day" is there is a convention that
  differs: a cut at UTC midnight has no daylight saving; a cut at a fixed local time in
  a financial centre (from memory FX is often cut at 17:00 New York, CME futures at
  17:00 Chicago) moves with that centre's daylight saving. To be checked per dataset,
  not assumed.
- So a **calendar is a property of the market** (time zone, session hours or 24 hours,
  where the day rolls, holidays), and a dataset says which day convention its daily and
  weekly bars use. It is needed to compare instruments across markets, to align series,
  and to make coarser bars from finer ones: which is why "derive on demand" in the
  pyramid question needs the calendar, and why a provider's own daily bars can differ
  from daily bars made from its hourly ones.
- Open: the source of the calendars (the IANA time zone database, plus per-exchange
  session and holiday rules), how weeks and months are cut for 24-hour markets, and
  which day starts a week (Monday, as in ISO 8601, is the proposal).

**Built** (in the app's `Store`, `Dates.h`): `Resolution` codes; seconds for instants and
floating-point seconds for order book snapshots; day numbers for the date resolutions
with weekday, week, month and quarter starts and the date of an instant in a time zone;
Nordnet's millisecond tick times converted on the way in. Two provider facts found on the
way:

- **Yahoo stamps a daily bar at the start of the trading day in the exchange's time
  zone**: the open for Helsinki (07:00 or 08:00 UTC), local midnight for gold futures in
  New York (04:00 UTC in summer, 05:00 in winter) and for currencies in London (23:00 UTC
  of the day before in summer, 00:00 in winter; so a Yahoo FX day runs from London
  midnight, not from UTC midnight). Its meta gives only today's offset from UTC
  (`gmtoffset`), which is an hour off for half the year, so reading the stamp in that
  offset puts summer bars on the day before (330 of the 500 gold bars). The date taken
  is the one of the **nearest** local midnight, which absorbs the hour.
- Nordnet's discovery gives an instant (the last tick), so its bar's date is the UTC date
  of that tick, which is right for the European markets seen so far.

### Comparing across markets: the day is not the same window

The market regime embeddings are calculated for **time slots**, and what a slot means
must be explicit, because bars of different markets are not always comparable:

- A **day** is a window defined by its market's calendar. A Nasdaq Helsinki day is the
  trading session of that date (about 09:00 to 18:30 Helsinki time). A day of a 24-hour
  market is 24 hours, cut by a convention (London midnight for Yahoo's currencies,
  17:00 New York or Chicago for some futures, UTC midnight for others). So **a 24-hour
  market's day is not directly comparable to a Nasdaq Helsinki day**: they cover
  different hours.
- **The matching hours are comparable**: the intraday bars (5 minute, 15 minute, hourly)
  of both markets over the same UTC hours, for example those of Helsinki's session.
  This is the way to compare a 24-hour market with an exchange: by the slots they share.
- **Different 24-hour market days might be comparable** to each other, when they are cut
  the same way; if they are cut at different hours (London midnight against 17:00 New
  York), they are shifted windows and are not, though their hours can be matched too.
- So a slot is not "a date" alone: it is (the window's definition, the window's start).
  The **window definition** (the calendar: time zone and rule, session hours or 24 hours,
  where the day rolls) must be recorded for every date series, on the market and on the
  dataset whose daily convention it is, and a comparison must check it: two date bars
  are comparable if their definitions are the same; otherwise they are compared through
  the intraday bars of the hours they have in common. In the graph of statements the
  calendar is a node, and "market has calendar" a statement.
- For the embeddings this means building the slots from intraday bars aligned by UTC
  time where the markets differ, and from date bars where the definitions match.

### Rough size (to be measured)

Assumptions, not measurements.

- Daily bars, 1987 to now: about 10,000 per instrument. For 100,000 instruments
  with full history (an upper bound; most have less) that is about a billion bars,
  in the order of 10 to 20 GB compressed.
- Intraday, per instrument and year, for a 9.5-hour exchange session: 5-minute bars
  about 29,000, 15-minute about 9,600, hourly about 2,400 (FX around the clock has
  about 2.5 times as many). For a few years (say 3) of all of them over 100,000
  instruments: about 10^10 bars, in the order of hundreds of GB compressed. The
  5-minute bars are three quarters of that.
- Order books depend wholly on depth and frequency: 10 levels a side once a second
  through a session is about 8 million snapshots a year per instrument, so a
  policy (which instruments, how often, how long to keep full detail) is needed
  before anything is stored.

## Order books

**Not now.** Planned here so that the rest does not make them hard to add; there
are no answers yet to where they come from or how much to keep.

Snapshots of bids and asks at a moment, as in the app's model. Proposed: one row
per snapshot with the levels as arrays (prices and sizes), ordered by
`(edge_id, ts)`, versioned like bars.

Open: where books come from (Nordnet may offer market depth; crypto exchanges
publish theirs openly), how often, retention and downsampling, and the
"fill price for a quantity" series the prediction models want (stored or computed;
see data-model.md).

## Catalog and events

- **Catalog** (MariaDB, decided): instruments, markets, datasets, edges (an
  instrument on a market in a currency, as a dataset reports it), aliases per
  provider (its ids for the instrument and the listing), adapter state (what was
  loaded, when, how far). A version sequence for ingest batches. New tables in a
  schema of their own, next to the legacy ones.
- **Nodes and edges: a node is seen only through an edge.** All the possible nodes
  of the universe exist in the catalog, and an edge exists only when it has data. A
  node with no edge is invisible, because every preset selects edges, so there is
  nothing to show for it. There is no empty edge: discovery finds nodes and, where it
  can, data that makes edges (see Nordnet). Nodes are instruments today and may be
  other kinds later (the model's `EdgeKey` has instruments at both ends, which would
  then widen).
- **One vocabulary of instrument types, decided.** Nordnet's types (`UETF`, `KETF`,
  `PTC`) and Yahoo's (`EQUITY`, `FUTURE`, `CURRENCY`, ...) are combined into one
  vocabulary, which names certificates: company, ETF, certificate, currency, commodity,
  index, other (`InstrumentKind`). Each adapter maps its provider's types onto it and
  keeps the provider's own type beside it (`Instrument::type`), which is what the
  mapping came from; the mapping table is in `src/source/InstrumentTypes.cpp` and in
  the server's catalog it becomes a table. New provider types start as "other" until
  mapped.
- **Selecting edges by type is not used yet**, and it has its difficulties: the same
  instrument can be typed by several providers (which wins? today the first); the
  vocabulary will need to grow (bull and bear certificates, funds, ...); a type belongs
  to the node while a preset selects edges, so each selection looks the node up (fine
  in memory, an index in the database); and nodes that are not instruments may not
  have the same types. `Snapshot::edges(market, filter)` selects by kind or type
  today. Open: what a preset's selection looks like (market, kind, ...).
- **Events**: announcements and corporate actions, per instrument and dataset, as
  in the model; stored like bars (rows, versioned).
- **Identity across providers**: an internal instrument id, plus the **ISIN** where
  there is one (an ETF listed on two markets is one instrument with two edges: 16
  such pairs among Nordnet's ETFs), plus aliases (each provider's ids and symbol,
  FIGI). Open: how aliases are matched when a new provider is added, and what
  identifies instruments with no ISIN (currency pairs, futures).

## A graph of statements (proposed by the owner)

The idea: the catalog is a **semantic graph**, like Wikidata's. A node is universal (an
instrument, a currency, a market, an issuer, an index, ...), and what is known about it
is a set of **statements** that say who says them, such as "this node has the type PTC,
according to Nordnet" and "its ticker is NOKIA.HE, according to Yahoo".

- **A statement** is (subject node, predicate, value or object node, dataset, optional
  qualifiers: valid from and to, market, rank). The dataset is the reference: who says
  it. Statements from different datasets about the same predicate sit side by side,
  never merged, as the bars of two providers do.
- **What goes in**: the provider-specific type, the provider's identifiers (Nordnet's
  instrument id and the order book id its history is asked for by, Yahoo's ticker),
  labels and descriptions (per language and per dataset: Yahoo and Nordnet name the same
  instrument differently), the ISIN, the issuer, a fund's category and fees.
- **Edges** are the statements whose value is another node. The price-history edge
  (base, quote, market, dataset) is the special case whose payload is a series of bars
  per resolution. Others come with the same machinery: issued by, tracks (an ETF
  tracks an index), underlying of (a certificate's asset), listed on, same as.
- **What it replaces.** Today an instrument keeps the first writer's name, type and
  ISIN, so the first provider to mention it wins (`Instrument` is a record of
  first-writer values). With statements these become resolved values: a rule picks what
  to show (a preferred dataset per predicate, as Wikidata's ranks do), and the others
  remain. `Instrument` stays as that resolved view for the app and the API.
- **Types become classes with a hierarchy** (bull certificate, certificate, ETF, fund,
  ...), so a vocabulary that has to grow does so by adding a subclass, selecting by a
  class includes its subclasses, and the mapping from a provider's type to ours is
  itself a statement ("Nordnet's PTC is a certificate").
- **Currencies need no provider's opinion.** A currency is identified by its ISO 4217
  code, and being the quote of an edge makes it a currency; its class is asserted by the
  system, not by a dataset. (Today the quote currencies are added by the adapters, and
  a provider's type shows on one only if it happened to be the first to add it: the kind
  of accident a statement model removes.)
- **What stays out**: bars, order books and anything else numeric and long. They stay
  in the column store; the graph holds the identity and metadata of the edge, not its
  points.
- **Guardrails**, so that it does not become free-form: a registry of predicates (name,
  type of value, whether it is one per dataset, whether it is an identifier); indexes on
  (subject, predicate) and (predicate, value); about 100,000 nodes times 20 statements is
  2 million rows, which MariaDB handles without trouble; and no general triple-store or
  query-language engine, only these tables and the few queries the presets need.
- **Open**: which predicates first (provider type, provider ids, labels, descriptions,
  ISIN are the candidates, since they are what the adapters already collect and
  discard); the resolution rules; how nodes that are not instruments (issuers, indexes,
  markets) get ids; whether the API exposes statements or only the resolved view; and
  how the app's `Store` holds them.

## Ingest

- An adapter is the app's `Source`, with a different sink: the server's ingest
  service instead of the in-memory store. Open: the exact interface, so one adapter
  serves both.
- A **backfill** (history, slow, once) and a **live** part (new data, often) per
  adapter; each remembers how far it got.
- **Politeness per provider**: rate limits, pauses, a stop on refusals and bans.
  Unofficial endpoints can change or block without notice, so each adapter says
  what it assumes and fails loudly.
- **Provenance**: each adapter writes its own dataset; nothing is merged on write.
- **Idempotent**: loading the same data twice makes no difference except the
  version.

- **In a client** (the app) an adapter never reads a database. The app has a
  `server` adapter that reads the HTTP API and fills its store, and it can still
  run the provider adapters directly (Yahoo, Nordnet) when there is no server.

### Adapters

| Adapter | Gives | Status |
|---|---|---|
| **Nordnet** | Discovery (instruments) and history, by unofficial endpoints | Endpoints researched by the owner. **Discovery of the ETFs and the PTC certificates is built** (see below); history and intraday are next. |
| Legacy MariaDB | The existing bars (one row per bar), served by the server | Later. To learn: its schema, and how its instruments map to ours; whether the API reads it in place or it is copied into ClickHouse. |
| Yahoo | Daily candles, dividends, splits; no discovery, so its adapter has instrument lists of its own (`data/yahoo`), which go when every adapter has discovery | Built in the app (`YahooSource`, dataset `yahoo`); slow one by one, for development and personal use. |
| ECB | Daily reference rates, every currency since 1999, in one file | Candidate. |
| A paid bulk provider | Discovery and a whole exchange per request | Open: which, and what its licence allows to be shown. |

**Nordnet discovery** (`NordnetSource`), from the endpoints' behaviour as observed.
Every list is loaded the same way, with no category per endpoint: an instrument is
whatever its list holds, keeping its own `instrument_type`. Discovery **repeats every
five minutes** (about 7 requests and 20 MB of JSON each time), and **each listing's
price block is today's bar**, stored in the daily series: that is what makes the edge,
and a reload replaces the bar. The
lists so far are the ETFs (`etflist`) and the certificates of type PTC
(`certificatelist?apply_filters=instrument_type=PTC`). **Without that filter the
certificate list holds 64,861 instruments** (almost all bull and bear certificates),
which is not worth loading; the filter gives 1,097. More lists are one entry each.

- **Pacing**: requests are made one at a time, with 500 ms between them, a page of
  500 (ETFs) or 1000 (certificates). Both lists together are 7 requests and about
  10 seconds, most of it the server's own time (about 2 to 3 seconds a page). A 403
  or 429 stops everything; another failure ends only that list. There is no retry,
  no backoff and no cache yet: every load repeats all 7 requests. On the server
  discovery runs on a schedule (a day is plenty), not on every start.
- **Results go to the store a page at a time**, as they arrive, not all at the end:
  each page is written and the UI told, so rows can appear while the rest loads.
- **Markets** have only a numeric id in the answer. They are named from the id where
  it is known (4 Xetra, 11 Nasdaq Stockholm, 14 Nasdaq Copenhagen, 24 Nasdaq
  Helsinki, 53 to 55 First North Sweden, Finland and Denmark; inferred from the data)
  and `Nordnet market N` otherwise. The slug's exchange code is not used: a listing
  can carry the code of its home exchange while it trades on another market (a
  Norwegian certificate on First North Sweden has the slug `onse`).

The ETF list:

- `GET https://www.nordnet.fi/api/2/instrument_search/query/etflist?sort_order=desc&sort_attribute=yield_1y&limit=N&offset=M`,
  with the headers `Accept: application/json`, `Origin` and `Referer`
  (`https://www.nordnet.fi`) and `Client-Id: NEXT`. **No cookie is needed**; the
  browser cookie in the original script is not part of the request and must never
  be committed.
- The answer has `total_hits` (2304 ETFs when it was tried) and `results`. `limit`
  100 pages it in 24 requests; 500 works too, in 5.
- A result has `instrument_info` (id, name, symbol, ISIN, type, currency, issuer,
  clearing place), `market_info` (market id and sub id, the identifier on that
  market), `price_info` (last, open, high, low, change, volume, bid and ask, with a
  timestamp), `historical_returns_info`, `fund_info` (category, fees, risk, ESG),
  `exchange_info`, and `nnx_info` (ids and a slug that ends in the exchange code).
- Observed: 2277 ETFs on market 4 (Xetra, EUR), 26 on market 11 (Stockholm, SEK),
  1 on market 14 (Copenhagen, DKK). Every row has an ISIN; the same ISIN is listed
  on two markets 16 times; a symbol is never shared by two ISINs. Market ids carry
  no name in this answer; the slug suffix (`xeta`, `xsto`, `xcse`) does.
- Stored: an instrument (with its ISIN, name and type) and the quote currency, and,
  where the listing has traded, today's bar (`price_info`: open, high, low, last,
  `turnover_volume`, `tick_timestamp`; its `close` is the previous close), under the
  dataset `nordnet`. The daily bar starts at midnight UTC of its date, as the history's
  do. 12 of 3401 listings had no price or tick time: they are nodes without an edge. Nordnet's own ids (instrument id, market id, identifier) are
  what the history requests need: the adapter keeps them for the history step, and
  on the server they become aliases in the catalog.

**Intraday depth, as measured** (not as documented; these are the limits per request,
and they decide how history has to be collected):

| | 1 min | 5 min, 15 min | 1 hour | 1 day and coarser |
|---|---|---|---|---|
| **Yahoo** (chart, NOKIA.HE) | last 8 days | last 60 days (2, 5, 15, 30 min) | last 2 years (3 years is refused) | from the first trade (2000 for this one) |
| **Nordnet** (price-time-series, the ETC and a liquid ETF) | `MIN_1` for periods up to `WEEK_1` | `MIN_5` and `MIN_15` for periods up to `WEEK_1` (about a week) | `HOUR_1` up to `YEAR_1` | `DAY` back to the instrument's start (2019 for the ETC) |

- So both providers give only a window of fine bars: minute-level history is built by
  **collecting regularly**, not by one deep request. A collector that asks often
  enough (the window is days or weeks) and stores what it gets accumulates a history
  longer than any request can return.
- My first notes said Nordnet's 5 and 15 minute bars never came back; that was wrong.
  They came back as hourly only because I asked for periods of a month and more. For
  `DAY_1` and `WEEK_1` the valid resolutions are `MIN_1 MIN_2 MIN_5 MIN_15 MIN_30`
  (and, for a week, also `HOUR_1 HOUR_4 DAY`). A second Nordnet endpoint, for stocks,
  also has minute bars; the owner will supply it.
- **Yahoo's `range=max` is a trap**: it returns monthly bars whatever the interval
  asked for. Full daily history needs `period1=0&period2=<now>` (6825 daily bars for
  NOKIA.HE); the same works for `1wk`, `1mo`, `5d` and `3mo`. Yahoo has its own weekly
  and monthly bars, like Nordnet.

**Nordnet history** (`api.prod.nntech.io`), as observed, not built yet:

- `GET https://api.prod.nntech.io/market-data/v3/price-time-series/period/<PERIOD>/identifier/<ID>?resolution=<RESOLUTION>`
  with `Accept: */*`, `Origin` and `Referer` (`https://www.nordnet.fi`) and `x-locale: fi-FI`.
  The other browser headers (`Sec-*`, `Priority`) and a cookie are not needed.
- `<ID>` is the listing's **`nnx_info.market_data_order_book_id`** (checked with the
  BNPP RICI ETC example). Whether the numeric instrument id also works is untested.
- The answer has `pricePoints` (`timeStamp` in ms, `open`, `high`, `low`, `last`,
  `volume`), the previous close, the period's high and low, `returns` per period, and
  `validResolutionsForPeriod`, `availablePeriods`, `returnedPeriod`,
  `returnedResolution`.
- Periods: `DAY_1 WEEK_1 MONTH_1 MONTH_3 MONTH_6 YTD YEAR_1 YEAR_3 YEAR_5 ALL`.
  Resolutions: `QUARTER`, `MONTH`, `WEEK`, `DAY`, `HOUR_4`, `HOUR_1`, `MIN_30`, `MIN_15`,
  `MIN_5`, `MIN_2`, `MIN_1`; which are valid depends on the period.
- **The endpoint substitutes a resolution that is not valid for the period, and says
  so only in `returnedResolution`.** The loader must read that field, never trust the
  one it asked for. For the BNPP ETC: `YEAR_1` gives `HOUR_1`, `HOUR_4`, `DAY`,
  `WEEK` and `MONTH`; `YEAR_3` and `YEAR_5` give `DAY` and coarser; `ALL` gives `DAY`,
  `WEEK`, `MONTH` and `QUARTER` (1543 daily bars back to 2019-07-29, 139 KB, 1.1 s);
  hourly goes back one year; minute bars only for `DAY_1` and `WEEK_1` (see the table
  above).
- Daily bars are stamped 00:00 UTC of the trading date, hourly bars on the hour in UTC
  (the session hours differ between summer and winter time); `volume` was 0 for most
  bars of the ETC, so it is not to be trusted yet.
- **Nordnet gives the coarse levels itself** (`WEEK`, `MONTH`, `QUARTER`), which bears
  on the pyramid question above: a provider's own coarse bars and ours made from finer
  ones are different data.
- Pacing is not decided: responses took 0.6 to 1.1 s each, and every instrument needs
  one request per resolution.

Licences and terms matter for a service that shows the data to others: check what
each provider allows before it is on by default.

## API

Proposed: HTTP, with the bulk of the data as compact **columnar chunks** (a
column of timestamps, a column per price, compressed), not rows of JSON; the
catalog and small answers as JSON. Versioned, so a client can pin a version.

Sketch:

    GET /v1/markets, /v1/datasets
    GET /v1/instruments?market=...
    GET /v1/edges?market=...&summary=1
        one answer for every edge of a market: latest bar, change and a short
        trace, precomputed at ingest, so a table fills with one request
    GET /v1/series/{edge}?interval=1d&from=...&to=...&asof=VERSION
        a columnar chunk
    GET /v1/books/{edge}?from=...&to=...
    GET /v1/events/{instrument}
    GET /v1/changes?since=VERSION
        what changed (long poll or a stream), so the app follows the server

With this the two-phase loading from the app makes sense: the summary of every
row in one request, history only for the rows on screen.

In the app: a `Store` backed by the server (`RemoteStore`), which builds the
`Snapshot` from chunks; a series chunk is the unit its persistent vectors are made
of, and a version from the server is the snapshot's version.

Open: the chunk format (Arrow IPC, or something plain and packed), authentication,
limits per client, and whether research jobs read the database directly or only
through exports.

## Self-hosting

One machine to start, ClickHouse and the MariaDB on it. Open: the machine,
backups (and restore tests), monitoring, upgrades, and where cold history lives
when it outgrows the disk.

## Steps (not started)

1. Decide the storage (ClickHouse for bars, MariaDB for the catalog) and measure it:
   load a real slice (one year of minute bars for a thousand instruments, and the
   daily history of the legacy MySQL) and time the reads that matter (a table
   summary, one series, a long scan across many).
2. The schema, in a file in this repository, and the ingest service with one
   adapter writing to it.
3. The Nordnet adapter: discovery of ETFs and certificates (built), then its history
   and intraday, then more lists as they are wanted.
4. The API, and `RemoteStore` in the app.
5. Serving the legacy MariaDB data.
6. Order books, once the policy is decided.
7. Exports for research (Parquet), and the regime embeddings.

## Open questions

- A graph of statements for the catalog: adopt it, and which predicates first.
- Calendars: where they come from, and how weeks and months are cut for 24-hour markets.
- Day numbers for date resolutions (the owner's convention, proposed), and whether the
  legacy tables already use it.

- How many instruments in all, and how far back each intraday interval goes.
- A pyramid of coarser bars, and whether derived levels are stored or made on demand.
- Order book depth, frequency and sources (not now).
- Adjusted prices: query-time, or derived data.
- ClickHouse with `FINAL`, `argMax` or a "current" table, for the long scans.
- The HTTP library and the ClickHouse client (C++ for the API is proposed).
- Chunk format, authentication, who reads what.
- Identity across providers: aliases, ISIN, FIGI.
- What each provider's terms allow, and what the service may show to others.
- Backups, hosting, monitoring.
