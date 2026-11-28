# User guide

How to use `stocks`: the screens, the keys, the data sources, the options and where data is kept. Building and a
short start are in the [README](../README.md); the terms are in the [glossary](glossary.md).

- [The screen](#the-screen)
- [Keys and commands](#keys-and-commands)
- [Markets](#markets)
- [The period and charts](#the-period-and-charts)
- [The row menu](#the-row-menu)
- [Portfolios](#portfolios)
- [Lists and the Watchlist](#lists-and-the-watchlist)
- [News](#news)
- [Data sources](#data-sources)
- [Command-line options](#command-line-options)
- [Where data is kept](#where-data-is-kept)
- [Tests](#tests)

## The screen

There are three **contexts**: **Markets**, **Portfolio** and **News**. `Tab` goes to the next and `Shift+Tab` to the
previous. Each context has a row of **tags** at the top (presets that choose what the table shows) over a sortable,
scrollable **table**. When the tags do not fit on one line they wrap. Each context keeps its own tag, sort and scroll
position while the program runs.

The table fills the screen and follows it when it is resized. Click a heading to sort, a tag or row to choose it;
the wheel scrolls.

## Keys and commands

Every command is in the command palette (`M-x`, or `Ctrl+Shift+P` in the desktop keymap) with its description and
key. There are two keymaps: the terminal one (Emacs-like) and the desktop one.

| command | what it does | terminal keymap | desktop keymap |
|---|---|---|---|
| — | move | arrows, PgUp/PgDn, Home/End | arrows, PgUp/PgDn, Home/End |
| `next-context`, `previous-context` | Markets, Portfolio, News | `Tab`, `Shift+Tab` | `Tab`, `Shift+Tab` |
| `show-info` | everything known about the row (`Esc` closes it) | `i`, `C-x =` | `i` |
| `show-chart` | a large chart of the row | `c` | `c` |
| `next-period`, `previous-period` | step through the periods | `]`, `[` | `]`, `[` |
| `show-period-day` … `-year10` | choose a period | `C-c d w m q y 3 t` | `Alt+1` … `Alt+7` |
| `row-menu` | the menu of the selected row | | |
| `buy`, `sell` | the trade form for the row | | |
| `new-portfolio` | make a portfolio: a name and a currency | | |
| `rename-portfolio`, `delete-portfolio` | the portfolio shown; delete asks first | | |
| `show-transactions` | the transactions of the selected position | | |
| `add-to-watchlist`, `remove-from-watchlist` | put the row on the Watchlist, or take it off | | |
| `undo` | take back your last change: a trade, a new, renamed or deleted portfolio | `C-x u`, `C-/` | `Ctrl+Z` |
| `redo` | make it again | `C-M-_` | `Ctrl+Y` |
| `quit` | leave | `q`, `C-x C-c` | `q`, `Ctrl+Q` |

## Markets

The tags of Markets are presets: Commodities, Nasdaq Helsinki, Currencies (including the crypto coins), Indices, Funds, Physical, All and
the Watchlist. Each row is one instrument in one unit (a share in euro, gold in dollars), whatever markets and datasets price it (see the
[data model](../plans/data-model.md)): the price, the change, the low and high and the trace are the median of their values (the mean where
there are two), so a share listed on two markets and given by three sources is one row, and one wrong source does not move it. A source that has said
nothing for an hour while another has (a market that has closed, a delayed or a daily feed) is left out, so the row follows what is live; where none is live, the
newest stands. A share in euro on one market and in kronor on another is two rows: prices are not converted. The table shows no market: **Show info**
has a Sources section with what each market and dataset says, when, and whether it counts, and the chart over the table says how many make the line and how
much they disagree at the cursor. The presets of kinds (Commodities, Currencies, Indices, Funds) and All take every market; **Nasdaq Helsinki** and **Physical**
are the exception, a preset of one market, and show what that market says: its own prices, and only its own.

The sources fill the table on a thread of their own. The table follows as the data arrives, a few times a second at
most, and reads only the rows it draws, so it stays quick with tens of thousands of rows.

`i` (or **Show info** in the row menu) shows everything known about the row, including the latest news of its
instrument, its markets and their countries.

## The period and charts

The **Change** column and the trace beside it always show the same period, and the headings say which
(`Change 3m`, `3 months`). The periods are `1d 1w 1m 3m 1y 3y 10y`; they are shown at the right end of the top row
(where there is room) and a click chooses one. The period is the same for every row and both contexts, is kept while
the program runs, and starts at three months.

- The change is the last price against the period's first (for the day, the previous close).
- Beside the change are the period's **low** and **high** (`10y low`, `10y high`), in the row's price unit (a rate in per cent), sortable and
  blank where not known. They come from the bars of the period where there are some (with the last price, so the price is always inside them), and
  before that from what a provider says (the day's range, Yahoo's 52 weeks for the year). On a display narrower than 110 cells the two columns
  are hidden, and the other columns are narrower; from 140 cells the trace is wider.
- For a rate (a row quoted in per cent, such as Euribor or €STR), the change is the difference in basis points.
- The colour is the sign of the change.
- A row with no bars for the period shows no number and no trace.
- The rows on screen load what the period needs, and load it again once it is old.

**Charts.** Click a row's trace, press `Enter` with the cursor on it, or press `c` to show the trace of the period as
a large chart over the dimmed screen. The line has the colour of the period's change (brighter where more was traded,
as in the table), the values are on the left and the first and last dates under it. A cursor reads the date and the
value under it: `Left` and `Right` move it, `Page Up`/`Page Down` by a tenth, `Home`/`End` to the ends, and so does
the pointer. `Esc`, `Enter`, `q` or a click outside the chart close it. (The chart is cellui's `ChartModal`, the
same line chart as the table's traces.)

## The row menu

At the end of each row is the **≡** button. Click it, or move the cursor onto it with `Right` and press `Enter`
(`row-menu` does the same from the palette). It lists:

- **Show info**
- **Buy** and **Sell**: only on a row that can be bought, that is one with a price on a real market or a fund priced by its company.
  A row whose prices are all on no market (an index, a spot price, a currency pair, an interest rate) cannot be bought, and `buy` and `sell` from the
  palette say so. Gold, which the London Bullion Market prices as well, can be. You buy the row, not a market: what you bought on two markets is one position.
- **Put on the Watchlist**, or **Take off** it if the row is on it.

A row of the Portfolio context always has Buy and Sell, and **Show transactions** instead of the list entries.

## Portfolios

A portfolio is what you hold: positions in instruments you bought, valued at the prices the program has.

**The Portfolio context** has one tag per portfolio (one is shown at a time; there is no "All"). Its table has a
row per position: symbol, name, quantity, average cost, price, value, weight, P&L, P&L %, and the change and trace of
the period. The value is in the portfolio's currency. Where a position has no price or exchange rate, it is shown in
its own currency, left out of the total, and the heading gets a star.

**Buying and selling.** Buy and Sell (from the row menu or the palette) open a form: portfolio, quantity, price (the
last price is offered), date (today), fee, note, and a live total. `Tab` moves between fields, `Enter` accepts and
`Esc` cancels. A sell of more than is held is refused as you type. Your first buy, with no portfolio yet, offers to
make one.

**Show transactions** (a row of the Portfolio context) lists what was done with that position: date, buy or sell,
quantity, price, fee, total, how many were held after it, and the note, in the order they are applied (by day, the
buys of a day first). A transaction that cannot be applied (a sell of more than was held) is red and says why. The
list is read-only for now.

**Undo and redo** cover trades and new, renamed and deleted portfolios, for the current session.

The portfolios are kept in `portfolios.sqlite` (see [Where data is kept](#where-data-is-kept)). The design is in
[plans/portfolios.md](../plans/portfolios.md).

## Lists and the Watchlist

The **Watchlist** preset shows the rows you put on it. A list member is one row, an instrument in a unit, whichever markets and datasets
say it. Lists are kept in `portfolios.sqlite`, and `undo` takes a change back.

To add another list, add a line to `kLists` and a preset in `src/Browser.cpp`; the menu entries and the
`add-to-<list>` and `remove-from-<list>` commands follow.

## News

The News context lists news, newest first: a time (UTC), a kind, the headline, what it is about (an instrument, a
market, a country, or nothing) and the dataset it came from. Its tags are presets: all, press releases, analyses,
news and general (about nothing in particular). `i` shows an item with its link and what it is about.

An item is identified by its dataset and the dataset's own id: two datasets with the same story show it twice.

## Data sources

Each source runs on a thread of its own. With no `--source`, these run: `auto`, `spfb`, `ecb`, `goldprice`,
`gold-api`, `kar`, `physical`, `sifter` and a `funds:NAME` for every schema in `data/funds`. `--source` chooses instead: one
name, or several separated by commas.

| source | what it gives |
|---|---|
| `auto` | the market data: shares, indices, currencies and more. If the first provider gives nothing (no network, a refusal), it falls back to another, which uses the [instrument lists](#instrument-lists), for the rest of the session |
| `spfb` | daily Euribor rates from the Bank of Finland |
| `ecb` | the euro short-term rate (€STR) from the ECB |
| `goldprice` | [spot prices](#spot-prices-of-metals) of gold, silver and copper |
| `gold-api` | [spot prices](#spot-prices-of-metals) of metals and two coins |
| `kar` | daily prices of gold, silver, platinum and palladium in dollars, with the history from 2019, no key |
| `physical` | estimated prices of [physical metals](#physical-metals) |
| `sifter` | NAV history of the Sifter Fund's PA, PB and PI share classes, no key |
| `funds:NAME` | a fund company described by `data/funds/NAME.json` (shown under All) |
| `synthetic` | made-up prices for the instruments in the lists, no network |

### Instrument lists

Some instruments come from lists on disk, `data/lists/*.csv`: Finland's shares and the largest of 16 other countries
(the US, Canada, the UK, Germany, France, Sweden, Switzerland, Spain, Italy and more: about 1,350 shares), the
currency pairs and a few indices. They are not complete; Asia-Pacific and Latin America are not covered (there is no
source of their ISINs yet).

Each row has seven fields:

1. `kind`
2. `base`
3. `quote`: what one unit of `base` costs in `quote`
4. `name`
5. the ticker the source uses
6. the ISIN (only a currency may lack one)
7. the market's code (ISO 10383, such as `XLON`), which puts the row on the market the other sources made for it.
   A row with no code is on no market (`XXXX`: an index, a currency pair).

The fallback asks for one instrument at a time, so loading all of them takes about a quarter of an hour and some
40 MB. `--market MIC` (repeatable) loads one market only; `--source synthetic` loads none. `--lists DIR` reads
another directory.

The scripts under `tools/lists` rebuild the files: the Finnish list and the currency pairs from their candidate
lists, the other countries from the largest companies of a stock list that has ISINs (each found by its ISIN, so the
ticker is certainly the same company).

### ISINs

An instrument whose source gives no ISIN (zinc, lead, some indices, the coins, the physical metals) gets one from
`src/source/KnownIsins.h`: the real ISIN where there is one, otherwise one of ours (`ZZ…`, numbered). The file's head
says how to add one, and it does not compile with an invalid or repeated ISIN. A row with no ISIN is not kept, and
the status line says so.

### Spot prices of metals

Two sources give them, each as its own dataset with its own row for the same instrument. The rows are never merged;
what reads a spot price picks one (see [Physical metals](#physical-metals)).

- **`goldprice`** ([goldprice.dev](https://goldprice.dev), no key) asks for gold, silver and copper in one request,
  in euros and dollars. It also has daily bars: the last 30 days of gold, silver and copper in dollars (the free plan has no
  more, and none in other currencies), which the month and quarter periods show as a trace. The 1, 3 and 10 years read weekly
  bars, which the API has none of: they are made of the daily bars the store keeps, so the history grows with every load
  (load at least once a month, or a gap is left), and a longer history from another source or a paid plan joins it. Its prices
  are 15 minutes late.
- **`kar`** (no key) asks for one request of the whole history of the daily prices of gold, silver, platinum and palladium
  in dollars per troy ounce, from November 2019 (gold from December 2020), one price a day, and makes the weekly bars of the 3 and 10 years of them. They are
  on the London Bullion Market, whose price the fix is, so a metal is one row with the spot prices of the others (on no market), and a daily fix is left out while a spot price is live. The page also gives euros, kroner and kronor, but those are
  the dollar's price converted with an exchange rate, and are left out. Rhodium is not carried.
- **`gold-api`** ([gold-api.com](https://gold-api.com), no key) lists its symbols (gold, silver, platinum, palladium,
  copper, Bitcoin and Ethereum) and asks the price of each in euros and dollars. The metals are in the Commodities
  preset and the coins in Currencies, beside the currency pairs (on a terminal without pictures their icon is ₿ or ⟠); all are in All. Its prices
  are quotes only: it has no history, so it writes no bars. The API takes prices from several providers and
  cross-checks them but does not guarantee them: verify a price before a financial decision.

These are spot prices and cannot be bought. A price is for the unit in the row's name: metals per troy ounce
(`Gold, per troy oz`), copper per metric tonne (`Copper, per tonne`; gold-api quotes it per pound and it is
converted), a coin per coin.

### Physical metals

`physical` estimates the price of metal held in hand, off exchange (market `Off exchange`, XOFF; dataset
`Estimate`), so it can be bought and sold like any other row:

- **Gold bar (1 g fine)**, held by the gram
- **Gold coin (1 oz fine)**, held by the troy ounce
- **Platinum bar (1 g fine)**, a sample

The price in each currency is the metal's spot price (per troy ounce) × the fine weight of a unit × a bid factor,
and a holding is valued at that, so the premium paid shows as a loss at first. The spot price used is the newest
quote; among those of the same day, the first source that `data/physical/items.csv` names (goldprice.dev before
gold-api.com), so when one stops answering the other is used.

**The bid factors in `data/physical/items.csv` are guesses, not any dealer's**: edit them to match your dealer. Add a
row there for other items (silver, scrap gold). The rows are in the Physical preset and in All. The price in the buy
form is yours to change. A dealer's real bid and ask list is planned ([plans/quotes.md](../plans/quotes.md)).

## Command-line options

    build/stocks [--ui auto|terminal|window] [--theme original|dark|light] [--source NAME[,NAME...]]
                 [--market MIC]... [--lists DIR]
                 [--facts FILE | --no-facts] [--portfolios FILE | --no-portfolios] [--summaries DIR | --no-summaries]

| option | |
|---|---|
| `--ui` | the terminal or a window (the window needs cellui built with GTK) |
| `--theme` | the colour theme |
| `--source` | the data sources (see [Data sources](#data-sources)) |
| `--market MIC` | load only this market from the instrument lists; repeatable |
| `--lists DIR` | read the instrument lists from another directory |
| `--facts FILE`, `--no-facts` | another facts file, or none |
| `--portfolios FILE`, `--no-portfolios` | another portfolio file, or keep portfolios for the session only |
| `--summaries DIR`, `--no-summaries` | another summaries directory, or none |

## Where data is kept

All in the user's data directory unless an option says otherwise:

| file | holds |
|---|---|
| `facts.sqlite` | what the sources say about instruments, markets and datasets (names, icons, a market's country). A cache: the sources rebuild it. |
| `summaries/<dataset>.sqlite` | per dataset, what the tables and portfolios need of each price: the last price, bid and ask, and the bars behind each trace. A summary, not the full history, which stays in memory. |
| `portfolios.sqlite` | your portfolios and lists. Your own data: never written to the cache, and a source cannot write to it. |

Because of these, the program opens at once and shows every price as the last run left it, even with no network. The
facts and prices are read on a thread of their own (what your portfolios hold first), and each trace appears when its
data has been read.

There are no migrations. A file of another schema is moved aside (`.old-<time>`) and a damaged one to
`.bad-<time>`, never emptied in place; the status line says so. If an instrument's ISIN changes under transactions
you typed in (as the gold bar and coin did when `src/source/KnownIsins.h` replaced their hashed ISINs),
`tools/rekey_isin.py` moves them.

The data the program ships with (`data/lists`, `data/funds`, `data/physical`) is looked for in the current
directory's `data`, beside the program (the build directory has a copy), in `share/stocks` beside the program's
directory (`cmake --install build --prefix PREFIX` puts it there) and in the source tree.

See [plans/data-model.md](../plans/data-model.md) ("The summary") for the design.

## Tests

    cmake --build build -j
    ctest --test-dir build --output-on-failure

- `stocks_tests` drives the browser on a test display (no terminal or window): keys in, what it shows out. A test
  fails for a command without a description: define every command with one
  (`app.commands().define(name, description, fn)`).
- `market_tests` tests the market-data server on SQLite files, and on MariaDB where `MARKET_TEST_DB_USER` (and
  `MARKET_TEST_DB_PASSWORD`, `MARKET_TEST_DB_HOST`, `_PORT`, `_SOCKET`) name a user that may make and drop schemas
  `market_test_*` and `stocks_test_*`. They say SKIPPED where it is not set.

CI builds on Linux (x86-64 and ARM64, with the GTK window), macOS and Windows, and runs `ctest`.
