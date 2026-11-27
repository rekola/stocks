# stocks

[![Linux x86-64](https://github.com/rekola/stocks/actions/workflows/ci-linux-x86_64.yml/badge.svg)](https://github.com/rekola/stocks/actions/workflows/ci-linux-x86_64.yml)
[![Linux ARM64](https://github.com/rekola/stocks/actions/workflows/ci-linux-arm64.yml/badge.svg)](https://github.com/rekola/stocks/actions/workflows/ci-linux-arm64.yml)
[![Windows x86-64](https://github.com/rekola/stocks/actions/workflows/ci-windows-x86_64.yml/badge.svg)](https://github.com/rekola/stocks/actions/workflows/ci-windows-x86_64.yml)
[![macOS ARM64](https://github.com/rekola/stocks/actions/workflows/ci-macos-arm64.yml/badge.svg)](https://github.com/rekola/stocks/actions/workflows/ci-macos-arm64.yml)

![Screenshot](https://raw.githubusercontent.com/rekola/stocks/refs/heads/main/assets/screenshot1.png)

A stock data browser for the terminal and a GTK window, built on
[cellui](https://github.com/rekola/cellui).

A row of preset tags (commodities, Nasdaq Helsinki, currencies, indices, funds, all of them, and your Watchlist) over a sortable,
scrollable table with a price trace per row. The rows are the edges of a
[data model](plans/data-model.md) that data sources fill on a thread of their
own; the table follows as the data arrives, a few times a second at most, and reads
only the rows it draws, so it stays quick with tens of thousands. One data source runs at
a time; the default, `auto`, picks one, and falls back to another for the rest of the
session if the first gives nothing (no network, a refusal).

    cmake -B build
    cmake --build build -j
    build/stocks   # Start with the default backend and sources
    build/stocks --ui terminal   # in the terminal
    build/stocks --ui window     # in a window, when cellui is built with GTK
    build/stocks --source synthetic   # made-up prices, no network
    build/stocks --market XHEL   # load only that market
    build/stocks --source spfb,ecb   # several sources: the daily Euribor rates from the Bank of Finland and the euro short-term rate from the ECB
    build/stocks --source funds:NAME   # funds:NAME is a fund company by its schema, data/funds/NAME.json (shown under All)
    build/stocks --source sifter   # the NAV history of the Sifter Fund's PA, PB and PI share classes, no key

Keys: arrows, PgUp/PgDn, Home/End move; `Tab` and `Shift+Tab` switch the context (below); `M-x` (`Ctrl+Shift+P` on
the desktop keymap) opens the command palette, where every command is listed with
its description and key; `q` (`C-x C-c`, `Ctrl+Q`) quits. `i` (`C-x =`) or the ≡ menu button at the end of a row (its first entry) shows everything known about the row (Esc closes it). Clicking a
heading sorts; clicking a tag or row chooses it; the wheel scrolls. The table
fills the screen and follows it when it is resized.

**Portfolios.** A portfolio is what you hold: positions in instruments you bought, valued at the prices the store has. There are three
contexts, **Markets** (the presets above), **Portfolio** and **News**; `Tab` goes to the next (`next-context`) and `Shift+Tab` to the previous
(`previous-context`), in both keymap profiles. The Portfolio context has the portfolios as its tags (by name; one is shown at a time, there is
no "All") over a table of its positions: symbol, name, quantity, average cost, price, value (in the portfolio's currency, with a star in the
heading where a position has no price or rate to it, which is then shown in its own currency and left out of the total), weight,
P&L, P&L % and the change and trace of the period (the period tags apply to both contexts). Each context keeps its own tag, sort and
scroll position while the program runs.

**News.** The News context lists the news, newest first, whatever it is about: a time (UTC), a kind, the headline, whom it is about (an
instrument, a market, a country, or nothing) and the dataset that said it. Its tags are presets: all, press releases, analyses, news
and general (about nothing in particular), each a selection by what the item says, as the presets of the markets are. `i` shows an
item with its link and what it is about, and the info of a row of Markets lists the latest news of its instrument, market and country.
An item is its dataset and the dataset's own id: two datasets with the same story show it twice, and nothing is replaced.

**Charts.** A click on a row's trace (or `Enter` with the cursor on it, or `c` for the selected row; the command is `show-chart`) shows the
trace of the period chosen as a large chart over the dimmed screen, with no window around it: the line in the colour of the period's change (brighter
where more was traded, as in the table), the
values on the left, the first and last date under it, and a cursor with a reading of the date and the value under it. `Left` and `Right` move
the cursor (`Page Up`/`Page Down` by a tenth, `Home`/`End` to the ends), and so does the pointer; `Esc`, `Enter`, `q` or a click away from the
chart close it. The chart is cellui's `ChartModal`, drawn by the same line chart as the table's traces.

At the end of each row is the **menu button ≡**: a click with the mouse, or move the cursor onto it with `Right` (`Enter` presses it; `row-menu`
opens the menu of the selected row from the palette). It opens a list for that row: **Show info**, **Buy** and **Sell**, and **Put on the Watchlist** (or
**Take off** it, where the row is on it). Buy and sell are only on a row on a real market or a fund priced by its company: an index, a spot
price, a currency pair and an interest rate (Euribor, the €STR) have the rest, and `buy` and `sell` run from the palette on such a row say it
cannot be bought. A row of the Portfolio context always has them, and no list (a list is of rows of Markets). Buy and sell open a form: portfolio, quantity, price
(the last one is offered), date (today), fee, note, and a live total; `Tab` moves between the fields, `Enter` accepts, `Esc` cancels, and
a sell of more than is held is refused as you type. `buy` and `sell` have no key; both are in the palette. The first buy with no
portfolio offers to make one.

**Show transactions** (a row of the Portfolio context only) lists what the portfolio says was done with that position, as a table: date, buy or
sell, quantity, price, fee, total, how many were held after it, and the note, in the order they are applied (by day, the buys of a day first). A
transaction the arithmetic leaves out (a sell of more than was held) is in red and says why. It only shows them: nothing is edited there yet.

**Lists.** The **Watchlist** preset shows the rows you put on it. A row is one edge (the instrument on a market, from a dataset), and so is a list's
member: Nokia from Nordnet on the list does not put Nokia from Yahoo there. They are kept in `portfolios.sqlite`, beside the portfolios, and `undo`
takes a change back. More lists are one line each (`kLists` and a preset in `src/Browser.cpp`), with the menu entries and the commands
`add-to-<list>` and `remove-from-<list>` following. When the presets do not fit on a line they go on to the next.

| command | what it does | terminal keymap | desktop keymap |
|---|---|---|---|
| `next-context`, `previous-context` | Markets, Portfolio, News | `Tab`, `Shift+Tab` | `Tab`, `Shift+Tab` |
| `new-portfolio` | a name and a currency | | |
| `rename-portfolio`, `delete-portfolio` | the portfolio shown; delete confirms | | |
| `buy`, `sell` | the row's form (its menu) | | |
| `row-menu` | the menu of the selected row | | |
| `show-transactions` | the transactions of the selected position, as a table (its menu) | | |
| `add-to-watchlist`, `remove-from-watchlist` | puts the selected row on the Watchlist, or takes it off | | |
| `undo` | takes back your last change: a trade, a new, renamed or deleted portfolio | `C-x u`, `C-/` | `Ctrl+Z` |
| `redo` | makes it again | `C-M-_` | `Ctrl+Y` |

**Spot prices of metals.** Two sources give them, each a dataset of its own with its own row for the same instrument (the rows are never merged in the store; what reads a spot picks one, see below). `goldprice` ([goldprice.dev](https://goldprice.dev), no key; it runs by default) asks one request for the prices of gold, silver and copper (it stores euros and dollars) and has daily bars: the last 30 days of gold in dollars without a key, which the month shows as a trace. Its prices come 15 minutes late. `gold-api` ([gold-api.com](https://gold-api.com), no key; it runs by default) lists its
symbols (gold, silver, platinum, palladium, copper, and Bitcoin and Ethereum) and asks the price of each in euros and in dollars: each is an
instrument with a price in both currencies on the market of what is priced and not traded, in the Commodities preset (the metals) and in
All. They are spot prices and cannot be bought, and a live price is a quote only: it writes no bar (a bar is history, and this API has none). The API takes them from several data providers (another answers when one is down) and cross-checks them, but does not guarantee them: verify a price before a financial decision. A price is for the unit its row's name says: the metals by the troy ounce (`Gold, per troy oz`), copper by the metric tonne (`Copper, per tonne`: the API quotes it per pound and it is converted, so it is the same number as another source's copper of the same ISIN), a coin by the coin.

**Physical metals.** `physical` is a source (it runs beside the default one) that estimates the metals held in hand, off exchange (XOFF),
from the spot prices the others give (in euros and in dollars, each its own price: the one with the newest quote, and of those of one day the first that `data/physical/items.csv` names, goldprice.dev's before gold-api.com's, so one that stops answering is passed by the other),
so they are bought and sold like any other row: **Gold bar (1 g fine)**, held by the gram, **Gold coin (1 oz fine)**, held by the troy
ounce, and a sample, **Platinum bar (1 g fine)**. Their price in each currency is their metal's spot price in it (per troy ounce) times the fine weight of a unit
and a bid factor, and a holding is valued at that, so the premium paid shows as a loss at first. **The factors in `data/physical/items.csv` are
guesses, not any dealer's**: edit them to the dealer you use. They are in the `Physical` preset (market `Off exchange`, dataset `Estimate`; silver and scrap gold would go there too: a row of `data/physical/items.csv` each) and in `All`;
`buy` on the row, and the price in the form is yours to change. A dealer's real list, bid and ask, is planned
(`plans/quotes.md`).

Your portfolios are your own data: they are kept in `portfolios.sqlite` beside `facts.sqlite` in the data directory (`--portfolios FILE`
chooses another, `--no-portfolios` keeps them for the session only). It is never the cache's file, which the sources rebuild: a portfolio
is not written to it and a source cannot write one. A file of another schema is moved aside (`.old-<time>`) and a damaged one to
`.bad-<time>`, never emptied in place; the status line says so. Undo and redo are for the session. If an instrument's ISIN ever changes under transactions you typed in (the gold bar and coin did, when `src/source/KnownIsins.h` replaced the hashed ISINs), `tools/rekey_isin.py` moves them; the program itself migrates nothing. See
[plans/portfolios.md](plans/portfolios.md).

The period: the Change column and the trace beside it always show the same period, and the headings say which
(`Change 3m`, `3 months`). The tags `1d 1w 1m 3m 1y 3y 10y` at the right end of the top row (where there is room) show it and a click
chooses it; `]` and `[` (`next-period`, `previous-period`) step through them, `C-c d w m q y 3 t` (`Alt+1` to `Alt+7` on the
desktop keymap) choose one (`show-period-day`, `-week`, `-month`, `-quarter`, `-year`, `-year3`, `-year10`). It is one for every row, kept while the
program runs, and starts at the three months. The change is the last price against the period's first (for the day, the
previous close); a rate's (a row quoted in per cent: Euribor, €STR) is the difference in basis points, and the colour of both is its sign. A row with no bars for the period shows no number and no trace. The rows on
screen load what the period needs, and again once it is old.

cellui is not pinned while the two are developed together: the build fetches its
`main`. CI builds on Linux (x86-64 and ARM64, with the GTK window), macOS and
Windows, and runs `ctest`. To build against a local checkout:
`-DFETCHCONTENT_SOURCE_DIR_CELLUI=/path/to/cellui`. Needs libunistring
(`libunistring-dev`) and, except on Windows, libcurl (`libcurl4-openssl-dev`);
on Windows HTTP goes through WinHTTP. Both are behind
[httpclient](https://github.com/rekola/httpclient), which builds itself (and
folds host names with the libunistring that is already needed, or a utf8proc if
that is installed), fetched at a pinned commit
like [immer](https://github.com/arximboldi/immer) and
[nlohmann/json](https://github.com/nlohmann/json).

The data the program ships with (`data/lists`, `data/funds`) is found in the current directory's `data`, beside the program (the build
directory has a copy), in `share/stocks` beside the program's directory (`cmake --install build --prefix PREFIX` puts it there) and in the source tree.

Some instruments come from lists on disk, `data/lists/*.csv`: Finland's shares and the
largest of 16 other countries (the US, Canada, the UK, Germany, France, Sweden, Switzerland,
Spain, Italy and more: about 1,350 shares), the currency pairs and a few indices; not complete.
Rows have seven fields: `kind`, `base`, `quote` (what one unit of `base` costs in `quote`),
`name`, the ticker the source uses, the ISIN, which only a currency may lack, and the
market's code (ISO 10383, such as `XLON`), which puts the row on the market the other sources
made for it. A row with no code is on no market (`XXXX`: an index, a currency pair).
`--lists DIR` reads another directory. Such a source is asked for one
instrument at a time, so loading all of them takes a quarter of an hour and some 40 MB:
`--market MIC` (repeatable) loads one market, and `--source synthetic`
loads none.
The scripts under `tools/lists` rebuild the files: the Finnish list and the currency pairs
from their candidate lists, the other countries from the largest companies of a stock list
that has ISINs (each found by its ISIN, so the ticker is certainly the same company).
Asia-Pacific and Latin America are not covered: there is no source of their ISINs yet.

An instrument whose source gives it no ISIN (zinc, lead and some of the indices, the coins, the physical metals) has one its
adapter names from `src/source/KnownIsins.h`: the real ISIN where there is one, else one of ours (`ZZ…`, numbered). The file's head says how
to add one, and it does not compile with an invalid or repeated ISIN. A row with none there is not kept, and the status line says so.

What the sources say of instruments, markets and datasets (names, icons, a market's country) is kept between runs in a
SQLite file, `facts.sqlite` in the user's data directory (`--facts FILE` chooses another, `--no-facts` runs without it); your portfolios are in
`portfolios.sqlite` beside it (`--portfolios FILE`, `--no-portfolios`); see
[plans/data-model.md](plans/data-model.md). What the tables and the portfolios need of each price (the last price, bid and ask of every edge,
and the bars behind each sparkline) is kept in a small file for each dataset, `summaries/<dataset>.sqlite` beside them (`--summaries DIR`,
`--no-summaries`): it is a summary, not the history, which stays in memory ("The summary" in the data model). The program opens at once: the
facts and the edges are read on a thread of their own while it runs, what your portfolios hold first, so the tables show every price as
the last run left it, with no network, and each trace appears when its queue has been read from the file.

The data model is planned in [plans/data-model.md](plans/data-model.md), the server, its API and the bars in [plans/server.md](plans/server.md), and the market calendars in [plans/calendars.md](plans/calendars.md).

The market regime embeddings are planned in [plans/embeddings.md](plans/embeddings.md).

## Tests

    cmake --build build -j
    ctest --test-dir build --output-on-failure

`stocks_tests` drives the browser on a test display (no terminal or window): keys
in, what it shows out. A test fails for a command without a description: define
every command with one (`app.commands().define(name, description, fn)`).
