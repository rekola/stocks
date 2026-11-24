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
- **A discovery adapter** with unofficial endpoints for both discovery (the
  instruments) and history; its discovery is built (see Adapters).

Later, and the reason the reads have to be good: market regime embeddings and
their visualizations, which read long histories of many instruments at once.

## Overview

    server side only:
    adapters (a discovery source, a candle source, central bank rates, ...)
        -> ingest (dedupe, version, write directly)
            -> catalog        instruments, markets, datasets, edges, aliases   (MariaDB)
            -> bars           candles, every interval                          (ClickHouse)
            -> books          order book snapshots                             (ClickHouse, later)
            -> events         announcements, corporate actions
        -> API (HTTP)         the only way in or out for everything else
            -> desktop app (a `server` adapter fills its Store), tools, research jobs

## Keys

Every node has a **key**, one 64-bit number that means the same in every run, in the app, in the catalog and on the wire
(`src/model/NodeKey.h`, and "Node keys" in `data-model.md`). The server adopts it as it is, so that nothing
anywhere translates between ids, and no table holds an "internal id" of its own:

- an **instrument** is its ISIN, packed (56 bits, the check digit left out); a **currency or a unit** its code (up to ten letters or
  digits); a **dataset** its code, which its adapter defines and never changes; a **market** its MIC (four characters, `A-Z0-9`), which never changes; XXXX is no market (indices, spot prices, currency pairs) and XOFF what is priced off exchange (funds);
- the kind of node is in the top bits, and a key is below 2^62, so it is a positive `BIGINT` in MariaDB and a `UInt64` in ClickHouse;
- in text, an instrument is its ISIN, a currency or dataset its code and a market its MIC (`describe()`), which is
  what a URL carries;
- more kinds of node take the kind values 4 to 7 (the kind is bits 62-60), and keys stay below 2^63, so none of the keys above changes.
  The planned ones are the **country** (kind 4: its key is its ISO 3166-1 alpha-2 code, see "Countries" in `data-model.md`), the
  **portfolio** (5, a minted key) and the **attribute** (6); kind 7 is an escape with a second-level kind in the identity ("Kinds and
  bits" in `data-model.md`), so an issuer or a person would come after in that. An index is not a kind: it is an instrument, identified by its ISIN like a share.

**A market's key is made from what it is**, like the others, so any machine makes the same one and no catalog mints keys. The
`markets` table holds the MIC as the key and the name, country and time zone as facts. A provider's id for a market is not kept: the
adapter maps it to a MIC (a table in the adapter, or an endpoint of the provider that lists markets with their MICs), and a listing on
a market it cannot map is left out until it can, since data with no market is refused.

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
      base UInt64, quote UInt64, market UInt64, dataset UInt64,  -- the edge's four keys (see Keys), as in the app
      resolution UInt8,            -- a code: min1 ... day, week, month, quarter (see Calendars and resolutions)
      t         Int64,             -- the start of the bar: UTC seconds, or a day number for a date resolution
      o, h, l, c Float64,
      volume    Float64,
      version   UInt64             -- the ingest batch that wrote this row
    ) ENGINE = ReplacingMergeTree(version)
      ORDER BY (market, base, quote, dataset, resolution, t)

- **The edge is four keys, not an `edge_id`.** A hash of the four keys would need a table to look it up in and a collision to
  reason about; the keys are stable and compact, and the four columns repeat down a sorted run, which the column store compresses
  to almost nothing. The order is the app's (`EdgeKey`'s): a market first, so that a preset (every edge of a market) is one range.
  Open: measure it with the slice of step 1, and whether `dataset` first suits the research reads better.
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
  interval. So datasets are named for the provider, not for an
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
  for order book snapshots and any ticks). A provider that gives milliseconds
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
  `min30`, `hour1`, `hour4`, `day`, `week`, `month`, `quarter` (one provider's vocabulary;
  the others' map onto it). It is a symbolic **code** (a small number or an enum naming
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
    UTC that a provider may put on its daily bars is only a marker for the date, so the date is
    what is stored; storing 00:00 UTC would invent a time and a time zone. With dates,
    daylight saving and time zones do not enter at all, and bars of different markets
    line up by date, which is what comparing instruments needs.
- **Slot numbers, as in the legacy platform (decided).** The key of a bar is a number,
  not an instant:
  - a **date resolution** by a day number (Date::Calc's, confirmed; weeks start on
    Monday, confirmed);
  - an **intraday resolution by its slot number in UTC**: the UTC Unix seconds of the
    start divided by the length of the resolution, rounded down. The hour number is the
    UTC seconds divided by 3600, a 10-minute slot number the seconds divided by 600, and
    so on. UTC, so no time zone and no daylight saving: no repeated or skipped hour.
  - **absolute week numbers** are to come from the day numbers (the owner will check
    how); **months and quarters have no numbering yet**. Until then a week, month or
    quarter bar is keyed by the day number of its first day.
  The resolution codes are the legacy platform's, which the owner will supply with the
  details of the plan; they include resolutions the providers do not have (10 minutes),
  and the providers' own (5 and 15 minutes, 4 hours) are added to them. In the app
  (`Dates.h`): `slotOfInstant`, `instantOfSlot`, `onSlotBoundary`.
- **The old data of the discovery source comes from the legacy system.** Its history is imported from
  there, so the old bars need not be fetched again; the import has to map the
  legacy codes and numbers onto these, and say which dataset the bars belong to.
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
  a discovery tick time) needs the market's time zone: the date of the instant
  there. A chart's meta may carry the exchange's UTC offset. Until a
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
millisecond tick times converted on the way in. Two provider facts found on the
way:

- **A candle source may stamp a daily bar at the start of the trading day in the exchange's time
  zone**: the open for Helsinki (07:00 or 08:00 UTC), local midnight for gold futures in
  New York (04:00 UTC in summer, 05:00 in winter) and for currencies in London (23:00 UTC
  of the day before in summer, 00:00 in winter; so its FX day runs from London
  midnight, not from UTC midnight). Its meta gives only today's offset from UTC
  (`gmtoffset`), which is an hour off for half the year, so reading the stamp in that
  offset puts summer bars on the day before (330 of the 500 gold bars). The date taken
  is the one of the **nearest** local midnight, which absorbs the hour.
- A discovery gives an instant (the last tick), so its bar's date is the UTC date
  of that tick, which is right for the European markets seen so far.

### Comparing across markets: the day is not the same window

The market regime embeddings are calculated for **time slots**, and what a slot means
must be explicit, because bars of different markets are not always comparable:

- A **day** is a window defined by its market's calendar. A Nasdaq Helsinki day is the
  trading session of that date (about 09:00 to 18:30 Helsinki time). A day of a 24-hour
  market is 24 hours, cut by a convention (London midnight for some currency series,
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

### Open: bars that do not start on the hour (the US exchanges)

Measured on one candle source: the hourly, 30-minute and 5-minute bars of a US share (AAPL) start at
**09:30 New York time** (14:30 UTC in winter, 13:30 in summer), because the exchange
opens at 09:30; Helsinki's hourly bars start on the hour. A bar that starts at :30
does not fall on an hour number (`onSlotBoundary` says so), and bars of the US and of
Helsinki would not line up hour for hour, which would disturb the market regime work.
Data collected on the assumption that hourly bars start on the hour needs correcting.

Kept as an open question. The owner's idea: **reconstruct the US hourly bars on the fly**
from 30-minute bars (or finer), onto the hour grid, instead of storing what the provider
calls hourly. What it brings and what to decide:

- The finer bars are only available for a short window (30 and 5 minutes 60 days
  back at one provider, a week at another), so the hourly history of the US shares that can be
  reconstructed is as deep as the 30-minute history we have collected.
- An hour slot on the UTC grid then holds two half-hours that belong to different
  session hours: 14:00 to 15:00 UTC in winter holds only 14:30 to 15:00 (the open) of the
  session, and the last slot holds the close. The first and last hour of the session are
  half-filled; a rule is needed (keep them as partial, or mark them).
- The US session moves by an hour in UTC with daylight saving (the open is 14:30 UTC in
  winter and 13:30 in summer). Helsinki is 7 hours ahead of New York on most days (337
  of the 365 days of 2026) but only 6 on the other 28, in the weeks between the two
  changing their clocks on different dates (mid to end of March, and the last week of
  October). Slots by UTC hour therefore mean different parts of the session over the
  year: the calendar question again (see [calendars.md](calendars.md)).
- Whether a provider's own coarse bars and bars reconstructed from finer ones are
  different series (they are different data, as noted for the pyramid).

### A provider's history is adjusted afterwards: retroactive split adjustment is required

Measured at a candle source: it **adjusts the history retroactively for splits**, in the prices and in
the volume. AAPL's 4-for-1 split on 2020-08-31: the close of 2020-08-28 is returned as
124.81 (it was 499.23 on the day) and its volume multiplied by 4; the split is listed in
the chart's events (`date`, `numerator`, `denominator`, here 4 and 1) and the same for
NESTE.HE's 3-for-1 in April 2019. The separate `adjclose` series is also adjusted for
dividends (114.54 against 118.28 for 2020-08-20). It has no unadjusted prices to ask
for, so handling retroactive adjustment is a **necessary feature** of the collector. What this means:

- "Stored as reported" cannot hold for such a provider's prices: what is stored is **adjusted as
  of the time it was fetched**. A bar fetched before a split and the same day's bar
  fetched after differ.
- A collector that only appends new days leaves old bars on the old scale after a split
  and so produces a jump. It must notice a new split in the events (or that the last
  stored bars disagree with the same bars fetched now) and **fetch the whole history
  again**, so that the old bars are replaced; in the versioned, insert-only store this
  makes new versions of the old bars and keeps the earlier ones, so "as of version N"
  still gives what was known then.
- Dividends are not applied to the OHLC prices (only to `adjclose`); the dividend events
  are stored as corporate actions as before.
- Whether the intraday bars are adjusted the same way is not measured.
- Whether the discovery source's history is adjusted is not known; the legacy import will show
  how the old data was handled.

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
`(market, base, quote, dataset, ts)` (the edge's keys, as for bars), versioned like bars.

Open: where books come from (a provider may offer market depth; crypto exchanges
publish theirs openly), how often, retention and downsampling, and the
"fill price for a quantity" series the prediction models want (stored or computed;
see data-model.md).

## Catalog and events

- **Catalog** (MariaDB, decided): instruments, markets, datasets, edges (an
  instrument on a market in a currency, as a dataset reports it), aliases per
  provider (its ids for the instrument and the listing), adapter state (what was
  loaded, when, how far). A version sequence for ingest batches. New tables in a
  schema of their own, next to the legacy ones.
  **The catalog is the server's database of what things are and what is said of them** (MariaDB), apart from the bars and the books
  (ClickHouse). It has no table for each kind of node, and no auto-increment id anywhere: it is the shape of the app's facts file
  (`data-model.md`) with a few tables added, all joined on the node key (see Keys), so the bars, the catalog and a client's facts
  speak of a node by one number. A new kind of node or a new predicate is rows, not a table.
  - **`facts`**: what is said of any node, instrument, market, dataset or country alike: (node key, predicate, value, when it was
    last said), and with statements, who said it (a dataset key). A dataset's code, name and what its days and prices mean are
    facts of it, as a market's MIC and country are of the market.
  - **`edges`**: the fat edges, the four keys of an edge and what the market says of the instrument there (the symbol, its own id,
    the order book, the provider's type, the name), kept apart from the facts as in the app.
  - **`aliases`**: each provider's ids for an instrument, a market and a listing.
  - Adapter state, and the sequence of ingest batches.
- **Nodes and edges: a node is seen only through an edge.** All the possible nodes
  of the universe exist in the catalog, and an edge exists only when it has data. A
  node with no edge is invisible, because every preset selects edges, so there is
  nothing to show for it. There is no empty edge: discovery finds nodes and, where it
  can, data that makes edges (see Adapters). Nodes are instruments today and may be
  other kinds later (the model's `EdgeKey` has instruments at both ends, which would
  then widen).
- **One vocabulary of instrument types, decided.** The providers' types (`UETF`, `KETF`,
  `PTC`, `EQUITY`, `FUTURE`, `CURRENCY`, ...) are combined into one
  vocabulary, which names certificates: company, ETF, certificate, currency, commodity,
  index, other (`InstrumentKind`). Each adapter maps its provider's types onto it and
  keeps the provider's own type beside it (the `type` fact), which is what the
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
- **Events**: corporate actions, per instrument and dataset, as in the model; stored like bars (rows,
  versioned). **News**: items by dataset and the dataset's own id, with the nodes each is about, as in
  the model.
- **Identity across providers**: the instrument's key is its **ISIN** (an ETF listed on two
  markets is one instrument with two edges: 16 such pairs among a few thousand ETFs), plus aliases (each
  provider's ids and symbol, FIGI). Nothing else is without one: a currency or a unit is its code (a
  currency pair is an edge between two currencies), and anything else that has no ISIN of its
  own gets one of ours (`privateIsin()`, `ZZ` and nine characters from what it is). Open: how aliases are matched when a
  new provider is added.

## A graph of statements (proposed by the owner)

The idea: the catalog is a **semantic graph**, like Wikidata's. A node is universal (an
instrument, a currency, a market, an issuer, an index, ...), and what is known about it
is a set of **statements** that say who says them, such as "this node has the type PTC,
according to one provider" and "its ticker is NOKIA.HE, according to another".

- **A statement** is (subject node, predicate, value or object node, dataset, optional
  qualifiers: valid from and to, market, rank). The dataset is the reference: who says
  it. Statements from different datasets about the same predicate sit side by side,
  never merged, as the bars of two providers do.
- **What goes in**: the provider-specific type, the provider's identifiers (the
  instrument id and the order book id its history is asked for by, the ticker),
  labels and descriptions (per language and per dataset: providers name the same
  instrument differently), the ISIN, the issuer, a fund's category and fees.
- **Edges** are the statements whose value is another node. The price-history edge
  (base, quote, market, dataset) is the special case whose payload is a series of bars
  per resolution. Others come with the same machinery: issued by, tracks (an ETF
  tracks an index), underlying of (a certificate's asset), listed on, same as.
- **What it replaces.** Today a node keeps one value per predicate, the last said (the facts of
  `data-model.md`), so with two providers saying different things whichever wrote last wins. With statements
  these become resolved values: a rule picks what to show (a preferred dataset per predicate, as Wikidata's
  ranks do), and the others remain. The app and the API read the resolved value, as `Snapshot::name()` and
  `attribute()` do now.
- **Types become classes with a hierarchy** (bull certificate, certificate, ETF, fund,
  ...), so a vocabulary that has to grow does so by adding a subclass, selecting by a
  class includes its subclasses, and the mapping from a provider's type to ours is
  itself a statement ("a provider's PTC is a certificate").
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
  discard); the resolution rules; whether the API exposes statements or only the resolved view; and
  how the app's `Store` holds them. Nodes that are not instruments get keys of their own kind (see Keys): a
  market's is its MIC, an issuer's or a person's would be minted, and an index is an instrument with an ISIN.

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
  run the provider adapters directly when there is no server.

### Adapters

| Adapter | Gives | Status |
|---|---|---|
| **Discovery source** | Instruments and today's bar of each, then history, by unofficial endpoints | Discovery of ETFs, certificates, funds, stocks, indices, currencies and commodities is built, and daily and coarser history for the rows on screen; intraday history and the server's full backfill are next. |
| Legacy MariaDB | The existing bars (one row per bar), served by the server | Later. To learn: its schema, and how its instruments map to ours; whether the API reads it in place or it is copied into ClickHouse. |
| **Candle source** | Daily candles, dividends, splits; no discovery, so its adapter has instrument lists of its own (`data/lists`), which go when every adapter has discovery | Built in the app; slow one instrument at a time, for development and personal use. |
| Central bank rates | Daily reference rates, every currency since 1999, in one file | Candidate. |
| A paid bulk provider | Discovery and a whole exchange per request | Open: which, and what its licence allows to be shown. |

**Discovery**, from the endpoints' behaviour as observed. Every list is loaded the same
way, with no category per endpoint: an instrument is whatever its list holds, keeping its
own type. Discovery **repeats every few minutes** (a few dozen requests and tens of MB of
JSON per round), and **each listing's price block is today's bar**, stored in the daily
series: that is what makes the edge, and a reload replaces the bar. A list is worth loading
only if it is not mostly noise (an unfiltered list of leveraged certificates is 64,000
instruments, nearly all of them bull and bear products; a filter by type leaves about a
thousand). More lists are one entry each.

- **Pacing**: requests one at a time with a pause between them, pages of 500 to 1000 rows.
  A refusal (403, 429) stops everything; another failure ends only that list. No retry,
  backoff or cache yet. On the server discovery runs on a schedule, not on every start,
  and each list has **its own reload interval**: funds are priced once a day, a market's
  stocks only matter while it is open (waits for the calendars).
- **Results go to the store a page at a time**, as they arrive, so rows can appear while the
  rest loads.
- **Markets** may have only a numeric id in the answer. They are named from the id where it
  is known (inferred from the data) and by a placeholder otherwise. The exchange code in a
  listing's slug is not used: it can be the home exchange while the listing trades elsewhere.
  A market is its MIC (see Keys): a listing on one the adapter cannot map to a MIC is left out and said so. The segment named
  in a row's own venue list ("First North", "Spotlight") is a market of its own.
- **What a row holds**: instrument info (id, name, symbol, ISIN, type, currency, issuer,
  clearing place), market info, a price block with a timestamp, returns, fund info and
  identifiers. Every row has an ISIN; the same ISIN listed on two markets is one instrument
  with two edges; a symbol is never shared by two ISINs. A few rows (about 0.4%) have no
  price or tick time: nodes without an edge.
- **Stored**: an instrument (ISIN, name, type), the quote currency and, where the listing
  has traded, today's bar (open, high, low, last, volume, tick time; its `close` is the
  previous close). The daily bar is stamped at midnight UTC of its date, as the history's
  are. The provider's own ids (instrument, market, listing identifier, the order book its
  history is asked for by) are what history requests need: the adapter keeps them, and on
  the server they become aliases in the catalog.
- **Funds** are priced once a day (a NAV), at a time that differs by fund. As in the legacy
  system a fund's bar has only the close, the NAV; its open, high, low and volume are
  **undefined** (NaN in the store: a number a provider does not give is undefined, never
  zero, so volume, which will weight the bars, is written when there is one and not as zero
  otherwise). The bar is dated by the tick. Fund history is a different request flavour.
  When a NAV is calculated and when the provider gets it is not known, so the fund list is
  reloaded hourly; the first reload that shows a new tick time is when the provider had
  it, to within the interval. **Idea of the owner: nowcast the NAV** shortly before the
  order cut-off and trade fund shares without transaction fees, where a small edge might be
  usable. It needs the cut-off and NAV times per fund and the market's close that day: a
  calendar question (calendars.md) and a model one (a fund tracks its holdings, so the
  nowcast is of the underlying).
- **Indicator lists** (indices, currency pairs, commodities) are one answer each: a row has
  the provider's id, an ISIN for some, a name, the order book id, a symbol, a type, a country
  for most indices, and the price (high, low, last with a time, the previous close and the
  development), **no open and no volume** (undefined in the bar). Their market is the list's
  (`Indices`, `Currencies`, `Commodities`), a pair is an edge between two currency nodes
  named "EUR/USD", an index is in points, and a commodity is in dollars (the rows give no
  currency; the legacy table has them all in USD). Names may arrive in a local language only.

**Intraday depth, as measured** (not as documented; these are the limits per request, and
they decide how history has to be collected):

| | 1 min | 5 to 30 min | 1 hour | 1 day and coarser |
|---|---|---|---|---|
| **Candle source** | last 8 days | last 60 days | last 2 years | from the first trade |
| **Discovery source** | up to a week | up to a week | up to a year | back to the instrument's start |

- Providers give only a window of fine bars, so minute-level history is built by
  **collecting regularly**, not by one deep request. A collector that asks often enough
  (the window is days or weeks) and stores what it gets accumulates a history longer than
  any request can return.
- **A provider may substitute a resolution that is not valid for the period, and say so
  only in a field of the answer.** The loader must read what the answer says it returned,
  never trust the resolution it asked for. Likewise a "maximum range" request may return
  coarse bars: full daily history needs an explicit period from zero to now.
- Daily bars are stamped 00:00 UTC of the trading date, hourly bars on the hour in UTC
  (session hours differ between summer and winter time). A `volume` of 0 is not to be
  trusted: a commodity or an ETC has none, so 0 is stored as undefined.
- A provider may give the coarse levels (week, month, quarter) itself, which bears on the
  pyramid question above: a provider's own coarse bars and ours made from finer ones are
  different data.
- Responses take 0.6 to 1.1 s each and every instrument needs one request per resolution,
  so the pacing of history is its own decision.

### The catalog's shape, as built and as decided

- **Every node keeps what is known about it as attributes, in the same way** (instruments,
  markets and datasets alike; the first use of the statements below, without the dataset
  yet): one value per predicate, `setAttribute(node, predicate, value)`, and a value can
  change: the last said stands (with two providers saying different things it is whichever
  wrote last, until statements say who says them), and an empty value says nothing. There is no
  `Instrument`, `Market` or `Dataset` record: a node is its key, and the app reads what is said of it (`name()`, `symbol()`,
  `kind()`, `attribute()`). A market is found by an attribute, the provider's id or its name, tried in that order, and an
  instrument is its ISIN.
- **Edges are rows of a fixed shape** (the owner's decision), plain C++ and kept apart from
  the nodes. A **fat edge is a Listing**: an instrument on a market in a quote currency
  from one dataset, a row carrying its own key, small, with the price series beside it
  under the same key, and the fields the market says: the symbol it calls the instrument,
  its own id for it ("XAIX" on one market and "419021" on another for one ETF), the order
  book its history is asked for by, the provider's own type. An edge is seen only once it
  has data; a listing said before the data is kept and shows with it. `findEdge(symbol, market)` finds the edge by what a market calls it, whoever
  said it.
- A **thin edge is a triple of a node**, (subject, predicate, object): what is known of the
  thing itself, which is not part of any market, so **no dataset says it** (an icon, the
  number of shares, who runs the company; three fields are an implementation detail, more
  may come). It is the same store as the nodes' own attributes (name, ISIN, kind, a
  market's code and country): one value per predicate, a value that changes replacing it
  (the last said stands until a rule picks the authoritative source), the object text, a
  whole number (a day is a day number), a real number or a truth value, and what is not
  given not said at all. What predicates there are is data: a new kind of fact is a new
  predicate, not a new type (two earlier shapes, four wide record types and then triples
  with a dataset, were the wrong ones). `attribute(node, predicate)` reads one and
  `attributes(node)` all; text is found by its value. **The instruments need data**, so the
  adapters say what a row says of the instrument, as a table of where each field is and
  what it is called, and the fat edge stays light. A currency has no ISIN and is its code;
  **nothing else is without an ISIN**.
- **A currency is the node the store has for its symbol**: a provider that has a currency
  gets that node (made, named as usually called, if none is there) and describes nothing.
- **Links between nodes (the value and its index are built in the app; the rest is planned).** A value may refer to another node: a
  company's `ceo` is the CEO's own node, a person, with attributes of its own. Issued by,
  tracks (an ETF's index), underlying of (a certificate's asset), director, major owner and
  index constituent are the same kind of link; today a certificate's underlying is only the
  provider's number and name. It needs:
  - a reference to a node as one more kind of `Value` (built: `NodeKey`, schema 5 of the facts file), and a node type for what is linked
    to (a person, an issuer, an index), made and found like the others, by attributes;
  - an index on the node-valued objects, beside the one on text (built: `findLinked()`), so that "who is the CEO of
    this" and "everything this person has run" are cheap;
  - **a validity period on a link**: a CEO changes, and the history of who was is worth
    keeping (valid from and to). So a row may have more than the three fields; "the latest
    value replaces" then applies per period, and a plain value such as the market cap keeps
    working as now;
  - in the attribute viewer, a node-valued attribute shown as a link to the node it names (built; it is drawn as a link and not
    yet followed),
    with the reverse links ("is the CEO of") listed on the target.
- **Private ISINs**: a thing that has none gets one of our own, made from what it is and
  not from who says it (there is one zinc): `ZZ` (ISO 3166-1's user-assigned code element;
  no numbering agency issues ISINs under it), a 64-bit hash of its exact text in nine
  base-36 characters, and the check digit (`privateIsin()`). Zinc
  is `ZZSEZBQVPB31`. Used for indicator rows with no ISIN (some metals and indices).
- **A source with no discovery carries the ISIN in its data files** next to the ticker: it
  makes its instrument from the file, by its ISIN, which is the same node another source
  made, and its own edge has its ticker as the symbol. Nothing waits on the load order. A
  currency has no ISIN and is its code; any other row without one is refused by the list
  loader, and a test checks every ISIN in the files. The ISINs of the shares come from a
  discovery source's stock list (the list-building script fetches it).
- **Commodities from a futures-based source stay off.** Every such series is a futures
  contract (its chart names the contract: "Gold Dec 26"), not the commodity's price, and the
  units differ (copper is 5.49 dollars a pound in one and 12,150 dollars a tonne in the
  other). A spot-like series (quoted to four decimals, which a futures tick is not) is a
  different instrument. So no ISIN of the commodity applies to a futures series; if one is
  wanted it is an instrument of its own (a contract or a continuous series, with no ISIN),
  for its long history, which a model may need.
- **Indices** may have a pseudo-exchange in a source's data, which is the index's
  calculator, not a place where it trades. So **an index is on a market named for the kind
  (`Indices`, as the pairs are on `Currencies` and the commodities on `Commodities`) and is
  quoted in points**, a unit node (kind Unit, symbol `pts`), not a currency: no price
  conversion applies to it, and the currency an index is calculated in is a fact about the
  instrument. An index with no ISIN of its own waits for one or gets a private one.
- **What a row says, and where it goes.** Kept as thin edges: the issuer, the icon URL,
  whether it is tradable or shortable, the collateral value, **how many accounts hold it** (a
  retail signal: worth sampling as a series, now only the latest), for stocks the market cap,
  P/E, P/B, P/S, EPS, dividend per share and yield, **the next report, the next dividend and
  the next general meeting** (scheduled events: they belong in the corporate-action and
  announcement model for prediction, once that is wired); for funds and ETFs the
  administrator, brand, category, type, dividend strategy, trading frequency, SFDR article,
  risk class, rating, ESG score, fees, assets under management, minimum investment, start
  date and the universe country and currency; for certificates the leverage, direction (long
  or short), the high-risk flag, the market view, management fee, first trading date and the
  underlying. On the fat edge: symbol, the market's id for it, the order book, the type. On
  the market: the country, from the rows. Not kept: the yields over periods and the annual
  growth (derived from prices we will hold), a tick-size id (means nothing without a table),
  a duplicate order book id, the slug, the clearing place. A source's chart metadata may
  also give the first trade date (kept, as where the history begins) and the exchange's time
  zone (kept on the market).
- **Edges are undirected**: the legacy platform wrote a pair in both directions (`invert`:
  the price of USD in gold beside gold in USD). One edge is stored, in one direction, and the
  other is read from it: the reciprocal of the close, and a bar's high and low swap (the
  high of the inverse is the reciprocal of the low); the volume has no inverse. Not built.

### The history loader

A thread of its own, like the icon loader: the table says which rows are on screen and a
screen on either side (`want()`, most wanted first, replacing what was wanted), and the
loader fills those edges, one request at a time with a short pause, writing the bars to the
store as they arrive (the table follows through the store's change). It does not load
17,000 instruments at start: only what is looked at, once per session per edge; what a
persistent store keeps will make it once per edge ever. Only edges with an order book id and
without bars (fewer than 20 daily ones) are loaded.

- **How much**: the interactive program needs only a small snapshot, the last 60 days a
  row's trace draws, so an edge is **one request** of about three months of daily bars
  (65 for a stock, 92 for a commodity that trades every day, 62 for a fund's NAV; 3 to
  10 KB). Nothing more is asked for in the interactive use.
- **The whole history is a server's job**, loaded once and kept (a backfill). The loader has
  it as `Depth::Full`, tested and not used by the program: the whole period, daily, stored
  under the resolution the answer **returned**, not what was asked for (a stock's answer is
  daily; a commodity's or a fund's comes back weekly, so the longest period that gives days
  is asked for next, with the pause between the requests). Weeks, months and quarters are
  keyed by their first day.
- A fund's points are a time and a value: a bar with only a close. Intraday bars (1 to 30
  minutes, 1 and 4 hours) are stored under their slot numbers, for the table's day and week
  periods (`Period.h`: each period names the series that feeds it and its base price). The
  history's last bar replaces today's bar from the discovery, which the next discovery
  reload replaces again.
- Failures: a 404 is remembered for the edge for the session; a 403 or 429 stops all
  requests for the session (said once in the status line); no answer or a 5xx pauses a
  minute and asks again.

### Suggested, added to the plan

Not decided; in the order I would do them.

- **Keep every reload as a snapshot series.** A discovery reload replaces today's bar each
  time and throws the snapshot away. Keeping each snapshot's last price with its tick time,
  as a series per edge, collects an **intraday series for every discovered instrument**
  without a single history request: by the afternoon dozens of samples each, enough to
  estimate current correlations, which the regime embeddings need for the present
  (embeddings.md, "A sketch of the present"). To decide: these are samples, not bars (no
  high or low between them), so they belong in a dataset of their own, and the true
  5-minute bars from the history endpoint, when they exist, are the better data for the same
  slots. To measure on a weekday: how often the tick time and price change between two
  reloads, how many instruments tick in five minutes, whether the prices are delayed, and
  the storage (about 3,400 instruments by about 100 snapshots a day).
- **Keep the facts the discovery already receives and drops**: a certificate's leverage and
  direction, the issuer, a fund's category and fees. The embeddings select the instruments
  that describe the market by leverage and direction (embeddings.md), and names are not
  reliable for it. They are statements about the instrument in the graph; until the graph
  exists, fields next to the type.
- **Keep the provider's ids as aliases**: the instrument id, the market id and sub id, the
  identifier on the market, the order book ids. The adapter holds them in memory for the
  history step, but they are lost when it stops; they are catalog data (aliases per
  provider).
- **Record when an instrument was first and last seen in discovery.** Every reload sees which
  listings exist; a listing that disappears has been delisted or removed. Recording the
  first and last time each was seen starts building the **listing and delisting dates** the
  embeddings need against survivorship bias (embeddings.md) from today, at no cost; the
  legacy data may have the earlier ones. Checking which instruments in the legacy data no
  longer exist is a first step.
- **Calendars for the discovered markets**: the generator and the reader for the first few
  exchanges (calendars.md), which gives the trading date of a tick (now the UTC date) and
  the expected bars of a session for finding gaps in the history.
- **Look at the traffic.** A full reload is dozens of requests and tens of MB of JSON every
  few minutes. Most stocks are not open at the same time and funds change once a day, so
  the lists should be reloaded at their own intervals; not done yet. The prices are in the
  same answers as the instruments, so reloading the lists is how prices are refreshed. If
  a metadata endpoint gives prices for many ids at once, the lists could be reloaded rarely
  (daily) and the prices often and cheaply.

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
    GET /v1/edges?markets=...&kind=...&country=...&summary=1
        the same for a selection (a set of markets, filters: see "What the application shows" in `data-model.md`); the user's own
        preset, the edges she follows, is a list of four-key edges that she keeps and the
        client sends, or the server keeps per user once there is one
    GET /v1/series/{base}/{quote}/{market}/{dataset}?interval=1d&from=...&to=...&asof=VERSION
        a columnar chunk; an edge is its four keys in text: an ISIN or a code, a code,
        the market's fifteen hex digits, a dataset's code
    GET /v1/books/{base}/{quote}/{market}/{dataset}?from=...&to=...
    GET /v1/events/{instrument}
    GET /v1/changes?since=VERSION
        what changed (long poll or a stream), so the app follows the server

With this the two-phase loading from the app makes sense: the summary of every
row in one request, history only for the rows on screen.

**User data is not market data.** The user's own preset (above) can be kept per user, but portfolios are private and cannot be rebuilt
from any source, so the server would hold a copy per authenticated user, as nodes, facts and events in the same change feed scoped to that user
(merged between devices as a union), and never as the only one. [portfolios.md](portfolios.md), "The server", has the design; no endpoint is sketched until there are users.

In the app: a `Store` backed by the server (`RemoteStore`), which builds the
`Snapshot` from chunks; a series chunk is the unit its persistent vectors are made
of, and a version from the server is the snapshot's version. Nodes need no translation: the keys are
the server's, so a chunk's edge is the app's `EdgeKey` as it is; only a market the client minted before
it met the server is re-keyed (see Keys).

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
3. The discovery adapter: discovery of every list (built), then the metadata endpoints
   and the history loader with the legacy import; and, suggested, the snapshot series,
   the facts and ids kept, first and last seen, calendars for its markets and a look at
   the traffic (see "Suggested, added to the plan").
4. The API, and `RemoteStore` in the app.
5. Serving the legacy MariaDB data.
6. Order books, once the policy is decided.
7. Exports for research (Parquet), and the regime embeddings.

## Open questions

- A graph of statements for the catalog: adopt it, and which predicates first.
- Market keys: the catalog mints them; how a client's own are reconciled, and the merge of two markets that turn out to be one.
- Whether the four-key bars order serves the research reads, measured with step 1.
- Calendars: where they come from, and how weeks and months are cut for 24-hour markets.
- Absolute week numbers (from the day numbers), month and quarter numbering, and the
  owner's resolution codes.
- The US hourly bars that start at :30: reconstruct the hourly bars on the fly from 30-minute
  ones, and how to treat the half-filled first and last hour.
- Adjusted history: how the collector notices a split and refetches, and how the legacy
  import treated adjustment.

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
