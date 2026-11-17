# stocks

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
reverses it; Tab moves between the tags and the table; `q` quits. Clicking a
heading sorts; clicking a tag or row chooses it; the wheel scrolls. The table
fills the screen and follows it when it is resized.

cellui is not pinned while the two are developed together: the build fetches its
`main`. To build against a local checkout:
`-DFETCHCONTENT_SOURCE_DIR_CELLUI=/path/to/cellui`. Needs libunistring
(`libunistring-dev`).

The data model is planned in [plans/data-model.md](plans/data-model.md).
