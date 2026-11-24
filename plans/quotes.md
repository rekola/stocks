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

**Valuation.** A holding is marked at the **bid**, what could be realised, and a purchase is offered at the **ask**; a mid price
overstates what a holder has, most for what is sold at a premium. Where there is no quote, the last close, as now, shown as such.
`portfolio::latestPrice` gets a rule for it; the Positions table gets a column for what the price is (bid, last).

## Physical gold

What is owned is a quantity of fine gold. Its price is a dealer's, which differs from the spot price by a premium that in Finland is
large for coins. Neither an exchange nor a fund is involved, so `tradable` (never XXXX) refuses it today.

- **A market for what is held and not traded.** A fourth convention beside XXXX (priced, not bought) and XOFF (a fund priced by its
  company): a code for what is held in hand (the code is open; four characters of `A-Z0-9` like a MIC). `tradable` accepts it, so
  the buttons, the commands and the model's write allow a holding there; an index, a currency pair or a rate stay refused.
- **Generic holdings by weight.** The instrument is the gold the store already has a spot price for, the quantity is fine troy ounces
  (grams may be typed and converted by the form: open), and a coin or a bar is only a preset in the buy form that fills in the fine
  weight (a 1 oz 22k coin is 1 oz of fine gold; a 5 g 999.9 bar is about 0.1607 oz). The presets are data in a file like the lists, not code.
  They are valued at the dealer's bid where a dealer dataset has one, else at spot, and the cost is what was paid, premium included, so
  the P&L shows what the premium cost.
- **Specific products as instruments, where a price source exists.** A dealer's price list (scraped; a source of its own with its own
  dataset, one adapter) gives each product a bid (what the dealer pays) and an ask (what it charges): an edge `product -> EUR` on
  the held market in that dataset, with a quote series and no candles. A holder may buy the generic gold or a product, and both are
  in one portfolio. Where no dealer prices a product it is not an instrument.
- **Limits.** Melt value for the generic kind; numismatic value is not modelled. A holding on the held market has no exchange to price
  it, so when no dealer and no spot has a price it shows as without one, and the total is marked incomplete as for a missing rate.

## Steps

1. This plan, and a look at which sources already say bid and ask (Nordnet's live answer, Yahoo's).
2. The store: `Quote` and `QuoteBar`, their series and `Snapshot` reads (`quotes(key, range)`, `quoteBars(key, resolution, range)`),
   `appendQuotes`, `appendQuoteBars`, a facts-file question none (not kept between runs, like bars).
3. Nordnet records its top of book on each reload.
4. Valuation at the bid, and `latestPrice` for it.
5. The held market and the relaxed `tradable`; the buy form's presets.
6. The dealer source.

## Open

- The code of the held market.
- Whether `bid_high` and `ask_low` are kept, and whether the sizes are.
- Raw quotes kept for how long, and whether bars are made by the store or by the recorder.
- Ounces or grams as the stored quantity, and whether the form takes both.
- Whether the generic gold is one instrument (spot, by weight) or one for each purity.
