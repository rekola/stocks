# stocks

[![Linux x86-64](https://github.com/rekola/stocks/actions/workflows/ci-linux-x86_64.yml/badge.svg)](https://github.com/rekola/stocks/actions/workflows/ci-linux-x86_64.yml)
[![Linux ARM64](https://github.com/rekola/stocks/actions/workflows/ci-linux-arm64.yml/badge.svg)](https://github.com/rekola/stocks/actions/workflows/ci-linux-arm64.yml)
[![Windows x86-64](https://github.com/rekola/stocks/actions/workflows/ci-windows-x86_64.yml/badge.svg)](https://github.com/rekola/stocks/actions/workflows/ci-windows-x86_64.yml)
[![macOS ARM64](https://github.com/rekola/stocks/actions/workflows/ci-macos-arm64.yml/badge.svg)](https://github.com/rekola/stocks/actions/workflows/ci-macos-arm64.yml)

A stock data browser for the terminal and a GTK window, built on
[cellui](https://github.com/rekola/cellui).

A row of preset tags (commodities, Nasdaq Helsinki, currencies, indices, funds, and all of them) over a sortable,
scrollable table with a price trace per row. The rows are the edges of a
[data model](plans/data-model.md) that data sources fill on a thread of their
own; the table follows as the data arrives, a few times a second at most, and reads
only the rows it draws, so it stays quick with tens of thousands. One data source runs at
a time; the default, `auto`, picks one, and falls back to another for the rest of the
session if the first gives nothing (no network, a refusal).

    cmake -B build
    cmake --build build -j
    build/stocks                 # on the display cellui picks
    build/stocks --ui terminal   # in the terminal
    build/stocks --ui window     # in a window, when cellui is built with GTK
    build/stocks --source synthetic   # made-up prices, no network
    build/stocks --market XHEL   # load only that market
    build/stocks --source nordnet,funds:NAME   # several sources, at once; funds:NAME is a fund company by its schema, data/funds/NAME.json (shown under All)

Keys: arrows, PgUp/PgDn, Home/End move; Tab moves between the tags and the table; `M-x` (`Ctrl+Shift+P` on
the desktop keymap) opens the command palette, where every command is listed with
its description and key; `q` (`C-x C-c`, `Ctrl+Q`) quits. `i` (`C-x =`) or the 🛈 button at the end of a row shows everything known about the row (Esc closes it). Clicking a
heading sorts; clicking a tag or row chooses it; the wheel scrolls. The table
fills the screen and follows it when it is resized.

The period: the Change column and the trace beside it always show the same period, and the headings say which
(`Change 3m`, `3 months`). The tags `1d 1w 1m 3m 1y` at the right end of the top row (where there is room) show it and a click
chooses it; `]` and `[` (`next-period`, `previous-period`) step through them, `C-c d w m q y` (`Alt+1` to `Alt+5` on the
desktop keymap) choose one (`show-period-day`, `-week`, `-month`, `-quarter`, `-year`). It is one for every row, kept while the
program runs, and starts at the three months. The change is the last price against the period's first (for the day, the
previous close), and the colour of both is its sign. A row with no bars for the period shows no number and no trace. The rows on
screen load what the period needs, and again once it is old (the day after 15 minutes, the week after an hour, the year after
a day; see `HistoryLoader.h`).

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

What the sources say of instruments, markets and datasets (names, icons, a market's country) is kept between runs in a
SQLite file, `facts.sqlite` in the user's data directory (`--facts FILE` chooses another, `--no-facts` runs without it); see
[plans/data-model.md](plans/data-model.md).

The data model is planned in [plans/data-model.md](plans/data-model.md), the server, its API and the bars in [plans/server.md](plans/server.md), and the market calendars in [plans/calendars.md](plans/calendars.md).

The market regime embeddings are planned in [plans/embeddings.md](plans/embeddings.md).

## Tests

    cmake --build build -j
    ctest --test-dir build --output-on-failure

`stocks_tests` drives the browser on a test display (no terminal or window): keys
in, what it shows out. A test fails for a command without a description: define
every command with one (`app.commands().define(name, description, fn)`).
