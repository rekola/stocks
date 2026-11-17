# Data model

Status: plan, nothing built. Decisions marked **Decided** come from the
discussion so far; everything under "Open questions" is not locked.

## Goals

- One in-memory model that the table, later the charts and the prediction
  models all read, filled by any number of data sources.
- Facts from different sources about the same thing live side by side, with
  their origin, and are never silently merged or overwritten.
- Prices belong to no currency: a price is the exchange rate between two
  instruments, so stocks, commodities, currencies and indices need no special
  cases.
- In memory first. Later a server collects data and fills the same tables when
  the application starts, so the model sits behind an interface a server-backed
  store can implement. C++ throughout.

## The graph

**Nodes are instruments.** One type for everything that can be priced or that
prices something: a company's share, a currency, a commodity, an index. An
instrument has an id, a symbol, a name and a kind (`company`, `currency`,
`commodity`, `index`, ...). A currency is an instrument like any other, which is
what lets Forex be currency to currency.

**Edges are always between two instruments.** An edge `base -> quote` says
"one unit of `base` costs this much `quote`". Usually `base` is a company and
`quote` a currency (NOKIA -> EUR); for Forex both are currencies (EUR -> USD).

**Decided: the key of an edge** is

    (base instrument, quote instrument, market, dataset)

- **market**: the same instrument can be listed on several markets with
  different prices (Nasdaq Helsinki, Nasdaq Stockholm, a dark pool). A market is
  its own small record (id, name, and later its currency, calendar, time zone).
- **dataset**: where a fact came from, with at least a name; other fields can be
  added later (source, license, whether it is derived, such as a prediction).
  Because the dataset is in the key, two sources that disagree about the same
  pair on the same market both stay; nothing is merged on write.

**Decided: payloads on an edge.** For now:

- **Candles**: time-ordered OHLCV bars (open, high, low, close, volume) with a
  timestamp and an interval.
- **Order book snapshots**: the bids and asks standing at a moment (price and
  size per level). Quotes carry information even when nothing traded: they say
  that somebody would have bought or sold at a price. Order books are kept
  apart from candles because they are a different kind of fact, but they share
  the edge key.

Later, other payloads (predictions from the machine learning models, as their
own dataset) hang on the same keys.

## What the application shows

**Decided: a preset names a market, not instruments.** Choosing a tag
(Commodities, Nasdaq Helsinki, Currencies) selects markets. The system then
looks up which edges exist in them and shows **one table row per edge**, not per
instrument. If two datasets have data for the same instrument on the selected
market, they are two rows. Merging such rows is later work and not important
now.

A row needs: the instrument's symbol and name, the dataset, the latest price and
the change over the last interval (from the candle series), and the trace of
recent closes. So the table gets a Dataset column. The row's identity is the
edge key.

## Storage interface (in memory first)

A `Store` interface with an in-memory implementation:

- `instruments`, `markets`, `datasets`: add, find by id, find by symbol.
- `edges(market)`: the edge keys that have data in a market (what a preset
  needs).
- `candles(edge key, range)`, `append(edge key, candles)`.
- `book(edge key, time)`, `append(edge key, snapshot)`.

Data sources (a source adapter per provider, a CSV reader, later the server)
only ever write through this interface, and everything else reads through it.
A source may feed the same edge from several places, as separate datasets.

## Steps (not started)

1. Types: `Instrument`, `Market`, `Dataset`, `EdgeKey`, `Candle`,
   `BookSnapshot`, with tests.
2. In-memory `Store` with the lookups above, with tests.
3. Wire the table to it: a preset selects markets, rows are edges, a Dataset
   column, latest price and change from candles.
4. A first source that fills the store (synthetic or CSV), replacing the made-up
   rows.
5. The simple order-book view for the prediction models (see below), once the
   question is settled.

## Open questions

### One edge with the whole history, or one edge per candle?

Undecided. Either way the key above already contains dataset and market, so
conflicting facts coexist.

- **A series per key** (one edge object holding a time-ordered candle vector):
  appending a candle is cheap, a range read is a slice, the table's
  latest/change/trace come straight from the tail, and there are far fewer
  keys. A correction to an old candle needs an in-place edit, and the series
  needs a policy for gaps.
- **One edge per candle** (the timestamp becomes part of the key): corrections
  and late data are just new facts, and a dataset can disagree with itself over
  time. But the number of keys explodes (years of daily bars for thousands of
  instruments), reads need an index by time, and almost every consumer wants a
  series anyway.

A middle road: store series, expose the same read interface either way, so the
choice can change without touching callers. To be settled before step 1 is
finished.

### The order book in a simple form for the prediction models

The prediction models want something simpler than the full book: *if I want to
buy 1000 shares, what price would I have paid, given the book?* Walking the
book answers it for one snapshot: for a buy of quantity Q, take the asks from
the best price up, level by level, until Q is filled; the effective price is
the volume-weighted average of the fills (and the sell side is the same over
the bids). The limit as Q goes to zero is the best ask (or bid), and the
difference from the mid price is the cost of the size, which a model can use
directly.

That much is arithmetic. What is not settled:

- **Stored or computed?** Compute from the snapshot when asked
  (`fillPrice(book, side, quantity)`), or also store a derived series at fixed
  quantities as its own dataset on the same edge, so models read a plain time
  series.
- **Which quantities?** A fixed set (100, 1000, 10000 shares), a size relative
  to typical volume, or a notional amount in the quote currency, which compares
  across instruments better than shares do.
- **A book too thin for Q.** No price (an empty value), the price of what could
  be filled plus a flag for the unfilled part, or a penalty price. A model
  needs to tell "thin" from "missing".
- **Time alignment.** Snapshots do not arrive at candle boundaries. Take the
  last snapshot before the bar's close, an average over the bar, or keep them
  separate and let the model resample.
- **Fees, tick size and hidden liquidity** are ignored in the plain walk;
  whether any belongs in the data layer or in the model.

### Smaller ones

- **Prices as `double`, or fixed-point / decimal?** `double` is simplest and
  adequate for display and models; exact decimals matter for anything that
  settles.
- **Corporate actions** (splits, dividends): store prices as reported and keep
  adjustment factors as their own series, or store adjusted prices?
- **Time**: UTC timestamps with an explicit interval per series; how sessions,
  holidays and time zones are represented (probably on the market).
- **Identity across sources**: one provider calls it `NOKIA.HE`, another
  `NOK1V`. A symbol alias table per dataset, or a canonical instrument id
  assigned when a source is added.
- **Threading**: the store is read on the UI thread while sources fill it from
  other threads. Likely a single writer posting changes to the UI thread
  (cellui's `ThreadInbox`) so the UI never locks; to be decided with the first
  source.
