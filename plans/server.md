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
      interval  UInt32,            -- seconds; 86400 is a day
      ts        Int64,             -- UTC milliseconds, the start of the bar
      o, h, l, c Float64,
      volume    Float64,
      version   UInt64             -- the ingest batch that wrote this row
    ) ENGINE = ReplacingMergeTree(version)
      ORDER BY (edge_id, interval, ts)

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
  interval, and `bars` is keyed by edge, then interval. The app's `Store` keeps one
  interval per edge today and refuses a second; holding several is the change to
  make with the history step.

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
- **Time.** UTC milliseconds everywhere. A provider's intraday bars follow its own
  session times, so the bar's start is what the provider says, converted. Sessions, holidays and time zones belong
  to the market record and are not designed yet.
- **Corporate actions and adjustment.** Prices are stored as reported and the
  actions are stored beside them. Adjusting for splits and dividends (needed for
  1987 to be usable) as a query-time view, or as a derived dataset? A provider's
  history may already be adjusted, and that has to be recorded per dataset.
- **Partitioning and codecs**, measured, not guessed.

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
- **Why an edge exists before it has data.** Discovery finds *listings*: this
  instrument is traded on that market in that currency. That is catalog knowledge
  and it comes first, from the provider's instrument lists; the bars come later,
  from a separate and much slower step (one request per instrument and interval).
  Without the edge, a found instrument would be invisible until its history was
  loaded, "not loaded yet" could not be told from "does not exist", and the
  history step would have nothing to ask for. In the server's catalog this is simply
  the edges table; in the app's `Store` it is `addEdge`.
- **Events**: announcements and corporate actions, per instrument and dataset, as
  in the model; stored like bars (rows, versioned).
- **Identity across providers**: an internal instrument id, plus the **ISIN** where
  there is one (an ETF listed on two markets is one instrument with two edges: 16
  such pairs among Nordnet's ETFs), plus aliases (each provider's ids and symbol,
  FIGI). Open: how aliases are matched when a new provider is added, and what
  identifies instruments with no ISIN (currency pairs, futures).

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
| Yahoo | Daily candles, dividends, splits; no discovery | Built in the app (`YahooSource`, dataset `yahoo`); slow one by one, for development and personal use. |
| ECB | Daily reference rates, every currency since 1999, in one file | Candidate. |
| A paid bulk provider | Discovery and a whole exchange per request | Open: which, and what its licence allows to be shown. |

**Nordnet discovery** (`NordnetSource`), from the endpoints' behaviour as observed.
Every list is loaded the same way, with no category per endpoint: an instrument is
whatever its list holds, and its kind comes from its own `instrument_type`. The
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
- Stored: an instrument (with its ISIN and name), the quote currency, the market and
  the edge, under the dataset `nordnet`. Nordnet's own ids (instrument id, market id, identifier) are
  what the history requests need: the adapter keeps them for the history step, and
  on the server they become aliases in the catalog.

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
