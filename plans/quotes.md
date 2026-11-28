# Quotes: bid and ask, and physical gold

Status: plan, nothing built.

Candles say what traded. For an instrument that trades seldom, or not at all in a period, they say nothing, while the order book or a
dealer's price list says what someone would pay and what someone would take. This plan adds that as data of its own, and uses it for
the one holding that has no exchange: physical gold.

## The quote series

**Decided direction: quotes are a series beside the candles, never fields of `Candle`.** The key is the edge key, as for candles and
order books (`data-model.md`: one series per key, a series for each kind of fact, none derived from another in the store).

Why not extend `Candle` (bid and ask equal when something traded, apart when nothing did):

- The spread exists whether or not anything traded, and it is the liquidity signal. A bar with trades usually still had a gap between
  the best bid and the best ask at its close. Collapsing the two to the trade price keeps the spread only where it matters least.
- "Was there a trade" already has an answer, `volume`. Encoding it as bid equal to ask makes a locked or briefly crossed market look
  like a trade.
- Cost. Bars are nearly all of the store's memory (310 MB for 50,000 edges); most edges will never have quotes, and a candle with
  nine numbers instead of five would be paid for by all of them.

Two series, as for candles (raw, and bars as the provider or the recorder gives them):

- **Quotes**: the best bid and best ask standing at a moment, with their sizes where known: `{time, bid, bid_size, ask, ask_size}`,
  time-ordered, a quote at a time already in the series replaces it, an undefined number (`kUndefined`) where a side was empty or
  not given. This is the top of what `BookSnapshot` holds, kept cheaply for the many edges that have no depth. An order book
  source that has depth still writes books; its top is a quote, so both are not needed for one edge.
- **Quote bars**: one bar per time slot at a resolution, keyed exactly as the candles of that resolution (a slot number, or a
  day number), so the two join on time: `{time, bid_open, bid_close, ask_open, ask_close, mean_spread}`, the spread time-weighted over
  the slot. A slot with no trades has a quote bar and no candle; a slot with trades has both, and its spread is still there. A
  source that gives only trades fills none. The best bid reached and the best ask reached (`bid_high`, `ask_low`) are the extremes that
  say something; the lowest bid and the highest ask are mostly stale quotes. They are added if a model asks for them (open).

Models are free to predict mid and spread (a spread is never negative) rather than bid and ask: both are derived from what is stored.

**Which instruments.** Stocks (the spread is a real cost for the small ones), currency pairs where a source quotes two sides, crypto
and certificates. Not indices and rates (calculated or fixed: Euribor, the €STR) and not a fund priced once a day by its company.

**Sources.** Nordnet's price block already carries `bid`, `ask`, `bid_volume` and `ask_volume` in the lists the Nordnet source loads
(seen in `tests/fixtures/nordnet`; how current and how complete it is in the live answer is not yet checked). That is a snapshot at
each reload, so a history exists only from when it is recorded: the first writer of quotes is that source, on each reload. Yahoo's
quote endpoint is said to carry them and is not checked. Crypto exchanges' public order books are free and keyless (`data-model.md`,
"Not done"). Each adapter alone knows its endpoint and fields (CLAUDE.md, "Providers"): nothing here names one.

**Not here: the period's high and low.** The same price block (and Yahoo's meta) also says the session's high and low, and Yahoo the 52 weeks'.
They are not quotes and not bars: they are the summary's per-period highs and lows (`data-model.md`, `Ranges`, written with `setRanges`),
which the bars override where they cover a period. A series of quotes does not feed them. And the summary's queues are of the kind their writer
says (`QueueKind`): a source that gives one value per bar (Euribor, the €STR, a NAV) writes `Scalar`; the quote series here is another series,
and takes no part in that.

**Valuation.** A holding is marked at the **bid**, what could be realised, and a purchase is offered at the **ask**; a mid price
overstates what a holder has, most for what is sold at a premium. Where there is no quote, the last close, as now, shown as such.
`portfolio::latestPrice` gets a rule for it; the Positions table gets a column for what the price is (bid, last).

## Physical gold

What is owned is a quantity of fine gold, bought from a dealer off any exchange. Its price is the dealer's, which differs from the spot
price by a premium that in Finland is large for coins.

**Decided: it is held on XOFF**, the market of what is traded off exchange (`kOffExchange`, today the funds). `tradable` already accepts
it, so the buttons, the commands and the model's write need no change, and an index, a currency pair or a rate stay refused. No new market code.

**The first items are two generic instruments**, each an instrument like any other (a private ISIN, `resolveInstrument`), priced in
the unit it is held in, so the quantity typed is the quantity held:

- **Gold bar, in grams** (fine gold: a 5 g or a 100 g bar of 999.9 is 5 or 100 of them);
- **Gold coin, in troy ounces** (fine gold: a 1 oz coin of any alloy is 1 of them).

Specific products (an American Eagle, a Krugerrand, a named bar) come later, and only where a dealer dataset prices them; a product is
then one more instrument with its own edge, held beside the generic ones in one portfolio. Neither is valued at more than melt value
plus what a dealer pays: collector value is not modelled.

**Where their rates come from.** Nothing publishes a rate for "a gold bar in grams", so there are two layers, and the second wins where there is one:

1. **An estimate from the spot price**, as a dataset of its own (an estimating adapter, code e.g. `GOLDEST`, never mistaken for a
   dealer's): for each item, the gold spot edge the store has (per troy ounce, in its quote currency; the portfolio's own conversion
   does the currency) divided by 31.1035 for the bar (grams), times a bid and an ask factor read from a data file, `data/gold.csv`
   (item, unit, bid factor, ask factor: for example a coin sells for a few per cent under spot plus and buys for a few above, the real figures to be
   found from dealers). It writes a daily bar (the close at the ask side) and the quote (bid and ask) each time the spot moves, on XOFF. The
   estimate is labelled as one in the info dialog's Sources, and its factors are the one place the guess lives.
2. **A dealer's price list**, scraped (a source of its own, its own dataset, one adapter, nothing else names it): the same two items, or a
   product, with the dealer's real bid and ask. Where an item has an edge from a dealer dataset it is the price (`latestPrice`: a
   dataset that gives quotes beats an estimating one; among dealers the best bid for a holding, the best ask for a purchase: open).

The spot price itself has to come from a source. Nordnet's indicator lists carry some metals (a private ISIN is made for what has none,
and zinc is one), so gold may already be among them; **not checked**, and if no source has it, a spot source for gold is the first step
of this part.

**Valuation** marks a holding at the bid (above), so a gold holding at the estimate shows what the dealer's discount costs, and the
cost is what was paid, premium included.

## Steps

1. This plan, and a look at which sources already say bid and ask (Nordnet's live answer, Yahoo's).
2. The store: `Quote` and `QuoteBar`, their series and `Snapshot` reads (`quotes(key, range)`, `quoteBars(key, resolution, range)`),
   `appendQuotes`, `appendQuoteBars`, a facts-file question none (not kept between runs, like bars).
3. Nordnet records its top of book on each reload.
4. Valuation at the bid, and `latestPrice` for it.
5. Gold: a spot source if no source has one, the two generic instruments and the estimating adapter with `data/gold.csv`.
6. The dealer source, and the products it prices.

## Open

- The bid and ask factors of the estimate, and where they are found from (a dealer's list, once one is scraped).
- Whether the estimate writes a daily bar as well as a quote, or a quote only (a row of the table needs a daily bar today).
- Whether a holding is marked at the best bid among dealers or at one chosen dealer's.
- Whether `bid_high` and `ask_low` are kept, and whether the sizes are.
- Raw quotes kept for how long, and whether bars are made by the store or by the recorder.
- Purity: 999.9 is taken as fine gold; whether a lower purity (22k) is a separate item or typed as the fine weight.
- **Review the metal datasets for converted prices.** Check each of them (`goldprice`, `gold-api`, `kar`, and the ones to come) for whether the price in a currency other than the dollar is real market data or the dollar's price converted at an exchange rate. A converted price says nothing the dollar's does not and, if it carries a bid and an ask, shows the cost of converting a currency as if it were the metal's spread: `kar`'s euro, krone and krona rows were of this kind and are not carried. Some prices are expected to be dropped, the physical estimator (and the portfolio's own conversion) giving the other currencies from the dollar's. How to tell: the converted price equals the dollar's divided or multiplied by the exchange rate at the same instant, to the rounding.
