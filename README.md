# stocks

A stock data browser for the terminal and a GTK window, built on
[cellui](https://github.com/rekola/cellui).

So far it is the browser's skeleton: a row of preset tags (commodities, Nasdaq
Helsinki, currencies) over a sortable, scrollable table with a price trace per
row, all over made-up numbers. There is no data model yet.

    cmake -B build
    cmake --build build -j
    build/stocks           # in the terminal
    build/stocks --gtk     # in a window, when cellui is built with GTK
    build/stocks --plain   # one frame as text

Keys: arrows, PgUp/PgDn, Home/End move; `<` `>` pick the sort column and `r`
reverses it; Tab moves between the tags and the table; `q` quits. Clicking a
heading sorts; clicking a tag or row chooses it; the wheel scrolls.

To build against a local cellui checkout: `-DFETCHCONTENT_SOURCE_DIR_CELLUI=/path/to/cellui`.
Needs libunistring (`libunistring-dev`).
