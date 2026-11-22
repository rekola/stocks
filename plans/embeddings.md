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
- the instrument's **kind and type**, its currency, market and calendar, and what the
  provider says about its **leverage and direction**. The last matters for which instruments
  describe the market, see the next section.
- corporate actions (splits, dividends) as events, and adjusted prices where adjustment is
  needed (below).
- later: order book snapshots (not collected yet).

Derived inputs the data side can compute on the grid: returns, ranges, realized
volatility from finer bars, volume relative to the instrument's own recent history.

## Which instruments describe the market

The state of the market is read from instruments that carry **independent information**.
A product that is a mechanical function of another instrument adds nothing new, and
distorts what it is added to:

- **Leveraged, inverse and knock-out products** (a "bear" certificate at 20 times) are
  functions of their underlying: they move with it, multiplied, flipped in sign, reset
  daily or wiped out when a barrier is hit. In an aggregate or a correlation matrix they
  count the underlying again, with the wrong sign or a fat tail, and they have finite
  lives. Nordnet's unfiltered certificate list has 64,861 of them (in a sample of 300,
  all short, leverage 2 to 22, many flagged high-risk). The adapter loads only the `PTC`
  type, so they are not in the data.
- **Plain trackers are another matter.** Of the first 1,000 `PTC` certificates, 993 have
  leverage 1 and 933 are long (64 are inverse), mostly ETCs and ETNs that follow a commodity or
  an index. They also duplicate an underlying (a gold ETC follows gold), but are real
  instruments on another venue, in another currency and at other hours, and can stand in
  for an underlying we have no series of. So they are not "not a signal"; they are **not
  independent of what they track**, and including both counts the underlying twice.
- **Selection must use the structured facts, not names**: the certificate list has
  `static_leverage` and `etp_info.direction`, and filtering by words in the name gives
  false positives ("Gold Bullion Securities" contains "bull"; "Daily Hedged" is a currency
  hedge). So leverage and direction are kept as facts about the instrument (statements in
  the graph of server.md), not just its type, and a selection reads them: for example "kind
  ETF or tracker, leverage 1, direction long".
- **ETFs** need the same care for leveraged and inverse funds; the type alone does not say.
- The default proposed: exclude leveraged, inverse and knock-out products from the
  estimation of the state; keep plain trackers where they add coverage, and never together
  with what they track in the same aggregate.

## What else defines a regime (feature catalogue)

Correlation is one description of a market state. Others, grouped by what the data we hold can compute.
The groups stay separate features with their own time scales (fast ones move in minutes, slow ones in
months), so the map can show the present at several speeds.

**From prices alone (every instrument has them)**
- *Volatility level and term structure*: realised volatility over 5 minutes, 1 hour and 1 day, and
  their ratios.
- *Return distribution shape*: skew, kurtosis, frequency of large moves.
- *Dispersion*: the cross-sectional spread of returns. High correlation with low dispersion is one
  block moving; high dispersion is stock picking. Related to correlation but not the same.
- *Breadth*: share of instruments up, new highs against new lows.
- *Trend and autocorrelation* at each time scale (momentum against mean reversion).
- *Intraday shape*: where the range is made (open, lunch, close) and the gap from the previous close.
- *Spectral structure*: variance explained by the first principal component of the correlation
  matrix, the effective number of independent bets.

**From volume and activity**
- Turnover against the instrument's own history for that time of day.
- Concentration of volume in few names against spread out.
- The time-of-day volume profile, which shifts under stress.

**From the instrument types already discovered**
- *Leverage and direction*: relative turnover and price of long against short leveraged
  certificates, a direct sentiment gauge (hence keeping `static_leverage` and direction as facts,
  server.md).
- *Safe haven against risk assets*: gold, the yen and the franc against the equity indices; the sign
  pattern across assets is a classic marker (hence currencies and commodities).
- ETF premium or discount to its underlying where both are present.

**From the order book (later)**: spread, depth, imbalance, quote update rate. Liquidity regimes often
turn before price regimes.

**From outside prices**
- Announcement and corporate-action density (earnings season, profit warnings, ex-dividend days);
  the data model has a place for them.
- Calendar effects: weekday, month end, expiries, holidays, half days. Known in advance, so they are
  features of the regime, not something to discover (calendars.md).
- The macro schedule (central bank decisions, data releases) and, if ever added as instruments,
  interest rates and the yield curve.

**Cautions.** Volatility, dispersion and correlation all rise in a sell-off, so several of these
will be strongly correlated. Whether they separate or collapse onto one axis is something the map
shows: colour the map by each feature in turn as a cheap test. Which groups the data we hold can
compute today: all of "prices alone", volume, and leverage/direction (once kept); the rest wait for
the order book, announcements, calendars or other instruments.

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
  (Yahoo's Helsinki history for NOKIA.HE, for one, starts in 2000). A model needs the same
  shape of input at every slot, so how to describe "the market" at a slot when the number of
  instruments goes from a few dozen to thousands is a real choice (see "Input: aggregates
  or a set").
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

### Input: aggregates or a set

The question, put plainly. At each slot the model is given numbers that describe the
market. Two ways to build them:

- **Aggregates.** We decide in advance a handful of summary series and feed their values:
  for example the return of a Nordic equity composite, of a US one, of the euro against the
  dollar, of gold and oil, the average return of everything that traded, the dispersion
  between instruments, the fraction that rose. Every slot has the same few dozen numbers,
  the early years work as long as the composites can be built from what existed, and each
  number means something. The cost: **we** decide what the market is made of, and what is
  not in a summary is lost; and a composite must be defined consistently over 40 years
  (which instruments, which weights, when one enters or leaves).
- **A set with a mask.** We feed the model every instrument that traded in the slot (a few
  dozen in the early years, thousands now), as a set whose size varies, or as a fixed vector
  with a mask marking the instruments that did not trade, and the model learns what matters.
  Nothing is thrown away by hand. The cost: more compute and a model built for sets
  (a transformer or a deep-sets network), sparse and not comparable inputs in the early years,
  harder to interpret, and every instrument must be well identified in time (when it existed).

The correlation structure is the regime (the owner's point), and it lives in the set, not in
a few composites: how instruments move together is a matrix over *all* the instruments of a
window. So a **hybrid** is natural, and is what I propose for the first version: compute the
correlation matrix from the set that exists at each slot, and summarize it into a fixed
number of features that do not depend on how many instruments there are: the average
correlation, the share of the variance in the largest eigenvalue (how much the market moves
as one), the spectrum of eigenvalues, the dispersion, the breadth. These are aggregates
computed from the set. A learned set model can come later and be compared with them.

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

## The map first, and where "now" is

The first product is a **map**, before any prediction (the owner's plan):

- a **UMAP of the embeddings of the whole history**, annotated (the crises, the rate
  shocks, the dates of known events), and a **cursor** that shows where the current slot
  is positioned in it.
- **One map per market where the markets cannot be compared**, as at the day level, where
  the days are different windows (calendars.md); the slots on the UTC grid of the intraday
  family are comparable across markets and can share one map.
- **The map must be stable and extendable.** UMAP does not give a new point a position by
  itself. A new slot is placed in the fitted map by transforming it (approximate), and the map
  is refitted rarely, with the old map as the reference so that its orientation does not
  change. A map that is redrawn when a day is added is of no use for a cursor, and the
  embedding must be the same function for every slot, past and present.

### A sketch of the present

The current slot is the hard one. The embeddings are built from windows of history,
including correlations, and for the current day only the beginning of the window exists.

- **What the discovery endpoints give now.** Every five minutes the Nordnet discovery
  reloads about 3,400 instruments, each with its last price and the day's open, high and low,
  the previous close, the day's change and volume, the tick time, and its returns over a
  day, a week, a month, three months, the year to date, a year, three, five years and since
  the start. That is a **cross-section**: breadth (how many are up), the dispersion of the
  returns, the average return, a range-based volatility proxy, the volume against normal.
  A cross-section needs no history of its own, so a *sketch embedding* from those features
  is possible. What it **cannot** give is correlations, which need a time series. (The prices
  are marked as not real-time, so they are probably delayed; to measure.)
- **The collector we already have.** If each five-minute reload is kept, instead of
  replacing today's bar, we collect for free an **intraday series of last prices for every
  instrument**, sampled every five minutes: by the afternoon there are dozens of samples per
  instrument, and correlations can be estimated from their changes. Caveats: samples are not
  bars (no high or low between them), prices of thin instruments are stale, instruments of
  different markets are asynchronous, and only the open markets move. This would need the
  reloads to store each snapshot as a series of its own (a 5-minute sample series per edge).
- **True intraday bars for a reference basket.** For a set of liquid instruments (say 50 to
  100) the history endpoint gives 5-minute bars for the day (`DAY_1` or `WEEK_1`, server.md);
  a few requests per update. The current correlations would come from the basket, not from all
  3,400 instruments.
- **Uncertainty.** Early in a session the window is short and the estimate noisy; the
  cursor should say so (a halo, or a trail of how its position moved during the day) and not
  jump around.
- **What the sketch is compared against.** The embedding of the history must be defined
  with the features available at the time, or the sketch is a different function from the
  embeddings it is placed among: either history is embedded with the same reduced features
  as the sketch, or the sketch is mapped into the full space by a learned regression, and
  its error is known. The choice belongs to the mathematics.

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
- The input: the hybrid (correlation features computed from the set) first, aggregates and a
  learned set model to compare; and how any aggregates are defined over 40 years.
- Which leverage and direction facts select the instruments, and where they are kept.
- A sketch of the present: from the cross-section only, with a collected series of the
  five-minute snapshots, or with a reference basket of intraday bars; and how its error is known.
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
