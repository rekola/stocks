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
- The adapters run on the server and write what they load to the database, and
  they are the same code as the desktop app's (`Source`, the universe lists,
  httpclient).
- **The existing MySQL stays**, as a source of good data, though the new system
  does not need it. It is read-only to the new system, through an adapter.
- **Next adapter: Nordnet**, which has unofficial endpoints for both discovery
  (the instruments) and history.

Later, and the reason the reads have to be good: market regime embeddings and
their visualizations, which read long histories of many instruments at once.

## Overview

    adapters (Nordnet, legacy MySQL, Yahoo, ECB, ...)
        -> ingest (dedupe, version, write)
            -> catalog        instruments, markets, datasets, edges, aliases
            -> bars           candles, every interval
            -> books          order book snapshots
            -> events         announcements, corporate actions
        -> API  -> desktop app (a Store backed by the server), tools, research jobs

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
| **PostgreSQL** | PostgreSQL licence, C | **Proposed for the catalog** (small, relational, transactional). |
| **DuckDB + Parquet** | MIT, C++ / Apache 2.0 | For exports to research jobs, an offline cache in the app, and backups of cold history. Not the server. |
| MySQL / MariaDB | GPL, C++ | Kept as it is, read-only, for the legacy data. |
| TimescaleDB | Apache 2.0 core, but compression and continuous aggregates under a source-available licence | Out: not open source in the parts that matter. |
| QuestDB | Apache 2.0, mostly Java | Out: Java. |
| Kafka, Cassandra, Druid, Pinot, Elasticsearch | Java | Out. |

To check before relying on it: each licence and language claim above is from
memory; confirm them when a choice is made.

Server language, proposed: **C++**, so that the adapters, the model types and
httpclient are shared with the app, not rewritten. HTTP server library, ClickHouse
client (native protocol, or its HTTP interface with a binary format) and the build
are open. Another language is fine if the owner prefers it, as long as the adapters
stay one codebase.

## Bars

Candles of every interval, from daily back to 1987 down to intraday.

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

Open:

- **Intervals to store.** Raw ticks, or 1-minute bars as the base with the rest
  derived (materialized views), or each provider's own intervals? Derived bars
  must say how they were made.
- **Intraday depth.** Which instruments get minute bars, and how far back; this
  decides the size by orders of magnitude.
- **Time.** UTC milliseconds everywhere. Sessions, holidays and time zones belong
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
- Minute bars: a 9-hour exchange session is about 570 bars a day, about 140,000
  a year per instrument; FX around the clock is about 370,000. Ten years of minute
  bars for 10,000 instruments is about 10^10 bars, in the order of hundreds of GB.
- Order books depend wholly on depth and frequency: 10 levels a side once a second
  through a session is about 8 million snapshots a year per instrument, so a
  policy (which instruments, how often, how long to keep full detail) is needed
  before anything is stored.

## Order books

Snapshots of bids and asks at a moment, as in the app's model. Proposed: one row
per snapshot with the levels as arrays (prices and sizes), ordered by
`(edge_id, ts)`, versioned like bars.

Open: where books come from (Nordnet may offer market depth; crypto exchanges
publish theirs openly), how often, retention and downsampling, and the
"fill price for a quantity" series the prediction models want (stored or computed;
see data-model.md).

## Catalog and events

- **Catalog** (PostgreSQL, proposed): instruments, markets, datasets, edges,
  symbol aliases per provider, adapter state (what was loaded, when, how far).
  A version sequence for ingest batches.
- **Events**: announcements and corporate actions, per instrument and dataset, as
  in the model; stored like bars (rows, versioned).
- **Identity across providers**: an internal instrument id, plus aliases (each
  provider's symbol, ISIN, FIGI). Open: how aliases are matched when a new
  provider is added.

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

### Adapters

| Adapter | Gives | Status |
|---|---|---|
| **Nordnet** | Discovery (instruments) and history, by unofficial endpoints | Next. To learn: the endpoints, the history depth and intervals, intraday, market depth, authentication and sessions, rate limits, and what Nordnet's terms allow. |
| Legacy MySQL | The existing data, read-only | Later. To learn: its schema (one row per bar), and how its instruments map to ours. |
| Yahoo | Daily candles, dividends, splits; no discovery | Built in the app (`YahooSource`); slow one by one, for development and personal use. |
| ECB | Daily reference rates, every currency since 1999, in one file | Candidate. |
| A paid bulk provider | Discovery and a whole exchange per request | Open: which, and what its licence allows to be shown. |

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

One machine to start, ClickHouse and PostgreSQL on it. Open: the machine,
backups (and restore tests), monitoring, upgrades, and where cold history lives
when it outgrows the disk.

## Steps (not started)

1. Decide the storage (ClickHouse and PostgreSQL, as proposed) and measure it:
   load a real slice (one year of minute bars for a thousand instruments, and the
   daily history of the legacy MySQL) and time the reads that matter (a table
   summary, one series, a long scan across many).
2. The schema, in a file in this repository, and the ingest service with one
   adapter writing to it.
3. The Nordnet adapter: learn the endpoints, discovery first.
4. The API, and `RemoteStore` in the app.
5. The legacy MySQL adapter.
6. Order books, once the policy is decided.
7. Exports for research (Parquet), and the regime embeddings.

## Open questions

- Which instruments get intraday, and how far back; how many instruments in all.
- Order book depth and frequency, and where they come from.
- Raw ticks, or minute bars as the base.
- Adjusted prices: query-time, or derived data.
- ClickHouse with `FINAL`, `argMax` or a "current" table, for the long scans.
- The server's language and HTTP stack (C++ proposed).
- Chunk format, authentication, who reads what.
- Identity across providers: aliases, ISIN, FIGI.
- What each provider's terms allow, and what the service may show to others.
- Backups, hosting, monitoring.
