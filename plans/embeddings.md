# Market regime embeddings

Status: plan, nothing built. A living document, started from the data side. The
mathematics is being worked on by another agent, so the sections about models and
evaluation are my thoughts to be replaced or overruled, and are marked as such; the
sections about what the data can and cannot give are what the data side knows. Related
plans: [server.md](server.md) (the data, versions, the API), [calendars.md](calendars.md)
(sessions, open masks) and [data-model.md](data-model.md) (the app's model).

## Goal

Give every **time slot** a vector, an embedding, that summarizes the state of the markets
in that slot, so that slots in a similar state are close together. What it is for:

- **Visualizations**: the history from 1987 as a path through a space (a projection),
  coloured by time or by regime; a timeline of regimes; for any slot, its nearest
  analogues in history ("when did the market last look like this").
- **Regimes**: clusters or segments of the history (calm, trending, crisis, ...), found
  from the data and not drawn by hand.
- **Conditioning other models**: the prediction models read the regime embedding of a
  slot as an input.

## Slots and grids

Decided or stated by the owner:

- The embeddings are for time slots, and **the slot grid is global, in UTC**: with, say,
  15-minute slots, at every slot every market is either open or not, so there is always a
  specific slot for each market. A change of daylight saving moves where in a session a
  slot falls, not which slot it is, and the phase may not matter.
- Daily history goes back to 1987; intraday (5 minutes, 15 minutes, an hour) only for a
  few years. So there are **families of embeddings**, one per grid: a daily family over
  the whole history, and intraday families over the recent years. Whether they share a
  space or are related (the pyramid of coarser levels) is open.
- A 24-hour market's day is not directly comparable to a Nasdaq Helsinki day, but the
  matching hours are, and different 24-hour days might be (server.md, calendars.md).

Slot keys are the numbers the data uses: a slot number in UTC for intraday, a day number
for dates (data-model.md). A daily slot is a trading date, whose window depends on the
market's day definition: a daily family must say which definition it uses, or treat the
markets' days as what they are, each in its own window.

## What the data can give (from the data side)

For each slot, each market, and each instrument that traded in it:

- the **bar**: open, high, low, close, volume, with the bar's resolution; no bar when
  the instrument did not trade, and **a mask that says why** (market closed, lunch break,
  holiday, weekend, no trade in an open market, no data collected). These are different
  and must not be filled in alike.
- **where the slot is in the session** (calendars.md): minutes since the open, to the
  close, the fraction of the session, the weekday and the local time.
- the instrument's **kind and type** (company, ETF, certificate, currency, commodity, index)
  and its currency, market and calendar. A certificate or an inverse or leveraged ETF is
  not a market-state signal; selecting by type is what lets a model leave them out.
- corporate actions (splits, dividends) as events, and adjusted prices where adjustment is
  needed (below).
- later: order book snapshots (not collected yet).

Derived inputs the data side can compute on the grid: returns, ranges, realized
volatility from finer bars, volume relative to the instrument's own recent history.

## Data issues the embeddings must live with

- **Adjusted history.** A return across a split is wrong unless the prices are adjusted, and
  Yahoo adjusts its history *afterwards* (server.md), so a series changes when a split
  happens. The legacy data never handled splits, so its old bars are raw and a split is a
  jump. Embeddings must be built from a **pinned version** of the data (the server answers
  "as of version N") and recomputed, or flagged, when the inputs they used change.
  Returns should come from adjusted prices; volume must be adjusted too (Yahoo's is).
- **Survivorship.** The providers list the instruments that exist today. Everything that
  was delisted, merged or went bankrupt is missing from discovery, so a history built from
  the current lists flatters the past. The data side needs listing and delisting dates,
  and the legacy data may hold delisted instruments. Until then, the 1987-to-now
  embeddings are conditioned on survival, and should say so.
- **A changing set of instruments.** Far fewer instruments have bars in 1987 than today
  (Yahoo's Helsinki history for NOKIA.HE, for one, starts in 2000). A model that takes one input per instrument cannot have a fixed width.
  Either the input is **aggregates** (indices, asset-class and market composites built from
  whatever exists in each slot) or the model takes a **set** of instruments with a mask.
  Which one is a mathematical choice with a data consequence: aggregates need the
  composites to be defined consistently over time.
- **Different quote currencies.** Returns in local currency or in one common currency; the
  currency pairs are themselves instruments. A choice to make and to record.
- **Different day windows and grids** (calendars.md): a daily family built from markets with
  different day definitions mixes windows; the intraday slots on the UTC grid do not.
- **Cross-listings.** One ETF listed on two markets is one instrument and two edges with
  two calendars; counting it twice double-weights it.
- **Structure changes**: tick sizes, trading hours, auction rules and market closures change
  over 40 years. Hours before about 2006 are not known (calendars.md).
- **Bar labelling.** A bar is labelled by its start, so a feature must not use a bar before
  it has closed; a US hourly bar starts at :30 (server.md) and is rebuilt onto the hour
  grid from finer bars.
- **Thin data.** Many instruments trade rarely; the lists include certificates and funds
  with sparse prices. Their zero-volume bars and stale prices should not look like calm.

## The mathematics (placeholders; my thoughts)

To be replaced by the other agent. What I would want decided, with the options I know of.

- **What a regime is.** A persistent state of the whole market with its own dynamics
  (level and clustering of volatility, direction, correlation between assets, the liquidity
  of trading), not a one-slot feature. The embedding should reflect the state, and be
  smooth over time where the state is.
- **Inputs, in order of simplicity**: (1) a hand-made feature vector per slot (returns and
  volatility of a set of aggregates, cross-asset correlations over a window, dispersion,
  breadth, volume) reduced with PCA or a similar projection, as an interpretable
  baseline; (2) the same features learned: an autoencoder or variational autoencoder over
  windows of slots; (3) self-supervised or contrastive learning, where slots near in time
  are positives; (4) sequence models over masked, variable-width inputs. And, as a
  comparison and not an embedding, hidden Markov or regime-switching models that give a
  discrete state.
- **Windows.** A slot's embedding may depend on the slots before it (a state is not
  visible in one bar). The window length is a parameter, and it is where leakage comes from.
- **Masks.** A model must take the open and the missing as inputs, not impute them. A
  24-hour market is open in nearly every slot, an exchange in about a quarter of them.
- **The grid and the phase.** If slots are on the UTC grid with a mask, the phase of the
  slot in the session may not matter; if it does, the calendars give it as a feature.
- **Many families.** Daily (1987 to now), hourly, 15 minute, 5 minute: how they relate, and
  whether the hourly state can be compared with the daily one.

## Evaluating an embedding (my thoughts)

Without a target, "good" has to be defined by checks:

- **Known regimes**: do the 1987 crash, the 2000 to 2003 decline, 2008, the 2020 crash and
  the 2022 rates shock separate from calm periods and from each other, and do they come back
  as neighbours of each other where they should?
- **Stability**: does retraining or a small change in the data move a slot far?
- **Usefulness out of sample**: does the regime of a slot predict later volatility,
  drawdowns or correlation better than simple baselines (trailing realized volatility)?
- **Persistence**: do regimes last as long as the dynamics say, or flicker?
- **No leakage**: a slot's embedding uses only data up to that slot, as known then
  (adjustments aside, see above); tests that shuffle or shift the future must show no gain.
- **Train and test split by time**, with a gap, not at random; overlapping windows leak.

## Computing and storing

Proposed, from the server plan.

- Embeddings are a **derived dataset**: (model id, resolution, slot key, vector), kept in
  the column store with the model's id, the code version, the **data version they were
  built from** and the random seed, so a result can be reproduced and an old and a new
  embedding compared.
- The mathematics probably runs in Python (PyTorch or similar) on exports from the server
  (Parquet or Arrow, as in server.md), not inside the C++ server; the results are written
  back through the server's ingest as a dataset of its own. To confirm with the other agent.
- The server should offer the research job a **tensor export**: for a resolution and a range
  of slots and a set of instruments (selected by type and market), the features and the
  masks, from a pinned version. That is the interface between the data and the mathematics.
- Recomputation when inputs change (a split makes a new version of the old bars): which
  embeddings it invalidates.

## Visualizations

What a client needs from the server, and what it shows:

- a **projection** of the embeddings to two or three dimensions for a range of slots,
  coloured by time or by regime; the projection method belongs to the mathematics (it must be
  stable enough that adding a day does not redraw the whole map);
- a **timeline strip** of regimes over the years, and of a chosen feature beneath it;
- for a slot, its **nearest analogues** and a drill-down into the features and the
  instruments behind them;
- the app has chart widgets (traces, heatmaps, images) to show these in the terminal and in the
  window; what is large is computed on the server and sent as a small answer.

## Open questions

- What a slot is for the daily family: one trading date per market, or one date on a common
  definition; and the intraday grid (5, 15 minutes or an hour).
- Aggregates or a set of instruments as the input, and how the aggregates are defined over
  40 years.
- Local or common currency for returns.
- How survivorship is handled until delisted instruments are in the data.
- Which instruments are in or out (types, leverage, thin trading), selected by type.
- How an embedding is versioned against data that is adjusted afterwards.
- The interface to the mathematics: the tensor export, its format, and where the work runs.
- Order books: what they would add, and whether any of this should wait for them.

## Steps (not started)

1. Agree with the mathematics side on the slot grids, the input set (aggregates or sets) and
   the interface (the tensor export).
2. Define the first aggregates from the data that exists (the Yahoo and Nordnet daily
   bars, the currency pairs and the commodity futures), with the masks.
3. A baseline: hand-made features on the daily grid, a projection, and the checks above
   against the known crises.
4. The calendars and the open masks for the intraday grid (calendars.md).
5. Survivorship and adjustment handled in the data (server.md), then the 1987 history.
6. The learned embeddings, and the visualizations in the app.
