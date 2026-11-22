# stocks

[![Linux x86-64](https://github.com/rekola/stocks/actions/workflows/ci-linux-x86_64.yml/badge.svg)](https://github.com/rekola/stocks/actions/workflows/ci-linux-x86_64.yml)
[![Linux ARM64](https://github.com/rekola/stocks/actions/workflows/ci-linux-arm64.yml/badge.svg)](https://github.com/rekola/stocks/actions/workflows/ci-linux-arm64.yml)
[![Windows x86-64](https://github.com/rekola/stocks/actions/workflows/ci-windows-x86_64.yml/badge.svg)](https://github.com/rekola/stocks/actions/workflows/ci-windows-x86_64.yml)
[![macOS ARM64](https://github.com/rekola/stocks/actions/workflows/ci-macos-arm64.yml/badge.svg)](https://github.com/rekola/stocks/actions/workflows/ci-macos-arm64.yml)

A stock data browser for the terminal and a GTK window, built on
[cellui](https://github.com/rekola/cellui).

A row of preset tags (commodities, Nasdaq Helsinki, currencies, indices, and all of them) over a sortable,
scrollable table with a price trace per row. The rows are the edges of a
[data model](plans/data-model.md) that data sources fill on a thread of their
own; the table follows as the data arrives, a few times a second at most, and reads
only the rows it draws, so it stays quick with tens of thousands. The default sources are
Nordnet's discovery (stocks, ETFs, funds, certificates, indices, currencies and
commodities: today's bar of each, reloaded every five minutes) and then Yahoo Finance
(daily candles, dividends and splits for the instruments in the lists on disk);
both are unofficial, need no key, and are for personal use.

    cmake -B build
    cmake --build build -j
    build/stocks                 # on the display cellui picks
    build/stocks --ui terminal   # in the terminal
    build/stocks --ui window     # in a window, when cellui is built with GTK
    build/stocks --source synthetic   # made-up prices, no network
    build/stocks --market "Nasdaq Helsinki"   # load only that list
    build/stocks --source nordnet     # Nordnet's ETFs and certificates (about 3400), reloaded every 5 minutes

Keys: arrows, PgUp/PgDn, Home/End move; Tab moves between the tags and the table; `M-x` (`Ctrl+Shift+P` on
the desktop keymap) opens the command palette, where every command is listed with
its description and key; `q` (`C-x C-c`, `Ctrl+Q`) quits. `i` (`C-x =`) or the 🛈 button at the end of a row shows everything known about the row (Esc closes it). Clicking a
heading sorts; clicking a tag or row chooses it; the wheel scrolls. The table
fills the screen and follows it when it is resized.

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

The instruments the tags show are lists on disk, `data/yahoo/*.csv`, one file
per market (Nasdaq Helsinki, Currencies, Commodities: about 220 instruments, not
complete). A file starts with `# market: NAME`, then rows of
`kind,base,quote,name,yahoo,isin` (what one unit of `base` costs in `quote`, the
ticker Yahoo uses for it, and the ISIN, which only a currency may lack); `--lists DIR` reads another directory. Yahoo is
asked for one instrument at a time, so loading all of them takes over a minute
and about 6 MB: `--market NAME` (repeatable) loads one list, and
`--source synthetic` loads none. `tools/yahoo/build_lists.py` rebuilds the
files from its candidate lists, keeping the tickers Yahoo answers for.

The data model is planned in [plans/data-model.md](plans/data-model.md), the server, its API and the bars in [plans/server.md](plans/server.md), and the market calendars in [plans/calendars.md](plans/calendars.md).

The market regime embeddings are planned in [plans/embeddings.md](plans/embeddings.md).

## Tests

    cmake --build build -j
    ctest --test-dir build --output-on-failure

`stocks_tests` drives the browser on a test display (no terminal or window): keys
in, what it shows out. A test fails for a command without a description: define
every command with one (`app.commands().define(name, description, fn)`).
