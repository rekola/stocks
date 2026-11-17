# stocks

[![Linux x86-64](https://github.com/rekola/stocks/actions/workflows/ci-linux-x86_64.yml/badge.svg)](https://github.com/rekola/stocks/actions/workflows/ci-linux-x86_64.yml)
[![Linux ARM64](https://github.com/rekola/stocks/actions/workflows/ci-linux-arm64.yml/badge.svg)](https://github.com/rekola/stocks/actions/workflows/ci-linux-arm64.yml)
[![Windows x86-64](https://github.com/rekola/stocks/actions/workflows/ci-windows-x86_64.yml/badge.svg)](https://github.com/rekola/stocks/actions/workflows/ci-windows-x86_64.yml)
[![macOS ARM64](https://github.com/rekola/stocks/actions/workflows/ci-macos-arm64.yml/badge.svg)](https://github.com/rekola/stocks/actions/workflows/ci-macos-arm64.yml)

A stock data browser for the terminal and a GTK window, built on
[cellui](https://github.com/rekola/cellui).

So far it is the browser's skeleton: a row of preset tags (commodities, Nasdaq
Helsinki, currencies) over a sortable, scrollable table with a price trace per
row, all over made-up numbers. There is no data model yet.

    cmake -B build
    cmake --build build -j
    build/stocks                 # on the display cellui picks
    build/stocks --ui terminal   # in the terminal
    build/stocks --ui window     # in a window, when cellui is built with GTK

Keys: arrows, PgUp/PgDn, Home/End move; `<` `>` pick the sort column and `r`
reverses it; Tab moves between the tags and the table; `M-x` (`Ctrl+Shift+P` on
the desktop keymap) opens the command palette, where every command is listed with
its description and key; `q` (`C-x C-c`, `Ctrl+Q`) quits. Clicking a
heading sorts; clicking a tag or row chooses it; the wheel scrolls. The table
fills the screen and follows it when it is resized.

cellui is not pinned while the two are developed together: the build fetches its
`main`. CI builds on Linux (x86-64 and ARM64, with the GTK window), macOS and
Windows, and runs `ctest`. To build against a local checkout:
`-DFETCHCONTENT_SOURCE_DIR_CELLUI=/path/to/cellui`. Needs libunistring
(`libunistring-dev`).

The data model is planned in [plans/data-model.md](plans/data-model.md).

## Tests

    cmake --build build -j
    ctest --test-dir build --output-on-failure

`stocks_tests` drives the browser on a test display (no terminal or window): keys
in, what it shows out. A test fails for a command without a description: add a
line to `src/CommandDescriptions.cpp` with every new command.
