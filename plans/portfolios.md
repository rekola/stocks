# Portfolios

Status: built, all eight steps ("Steps" below, with what is done and what is left). Built: 1 (node-valued facts), 2 (`src/portfolio/`), 3 (the model), 4 (the portfolio file), 5 (`Positions` and the contexts), 6 (the form dialog in cellui; buy, sell, the portfolio commands, undo and redo), 7 (the keys), 8 (the README). What is left is "Later" and "Open" below; the cellui changes (`FormModal`, `TableModel::buttonShown`, `Table::scrollToPosition`) are on cellui's branch and stocks fetches cellui's `main`, so stocks builds against a checkout (`-DFETCHCONTENT_SOURCE_DIR_CELLUI`) until they are merged.

A portfolio is what the user holds: positions in instruments she has bought, valued at the prices the store already has. It is
modelled as the instruments are: **a portfolio is a node**, and what there is to know of it is facts and edges in the same
[data model](data-model.md), written by the same calls and read from the same snapshots. It is the user's own data, so it is kept
in a file of her own (see "Persistence"), not in `facts.sqlite`.

## The model

- **A portfolio is a node and has no record of its own.** Like an instrument it is its key (`src/model/NodeKey.h`), and what is said of
  it is facts and edges. There is no list of portfolios and no create or delete: **a portfolio exists when something is said of it**
  and shows when it has an edge or a name; nothing else is needed to be one. A key with nothing said of it is not shown, as a node
  with no edge is not shown today.
- **Its key is minted**, since a portfolio has no natural identity (an instrument has its ISIN). Its kind is 5 (bits 62-60 = 101: see "Kinds and bits" in `data-model.md`, which
  also lays out country as 4 and attribute as 6), and its sixty bits of identity are random, drawn when the user makes one. Two devices mint without meeting and do not clash (a chance of about one in 10^18 per pair), so no UUIDs are needed.
- **What is said of a portfolio is of two kinds, as for an instrument:**
  - **Facts of the node**: `name` (what the user calls it, a literal) and `currency` (an edge to a currency node: the one its value
    is shown in). This is the naming. A fact is an edge to a value, which is a literal or a node; the node-valued ones are **not
    built yet** (`data-model.md`, "Not done"), so they come first (step 1). A portfolio with only a name shows, empty; one with positions and no name shows under a placeholder (its key,
    shortened) until it is named.
  - **Edges**: a **position** is an edge `portfolio -> instrument`: "one of this portfolio is this many of the instrument", which
    is the same sentence as "one of base costs this much quote", with a quantity in place of a price. Its market is the listing
    the user bought on, and its dataset is the ledger's (see the bullet on the dataset). The series on that edge is the **quantity held over time**. A
    position is therefore a row, as an edge is for the Markets table (a preset there is a selection of edges, and so is "every
    position of this portfolio").
- **The transactions are events on the portfolio**, on an event key `(portfolio, dataset)` like announcements and corporate actions
  (`data-model.md`): time, kind (buy, sell, later dividend and fee), the instrument and market, quantity, price, fee and note,
  each with its own minted id so that one can be edited or deleted (a delete is a removal, and the position is derived again).
  **The position edge is derived from them**: the quantity series is the running sum, the average cost the quantity-weighted mean of
  the buys. Nothing but the transactions is typed in; keeping only a quantity and a cost would lose the history that realized P&L,
  FIFO and a corrected mistake need.
- **What the dataset has to do with the transactions.** In the data model an event key is `(node, dataset)` and means *what this
  dataset says about this node* (the announcements and corporate actions of a company are the events of `(instrument, dataset)`, one
  series for each source that says them). So `(portfolio, dataset)` is *the transactions that this dataset says about this portfolio*,
  and the dataset is where they came from: the user's own entries (a dataset of its own, `source/DatasetCodes.h`), later a broker's
  export that she imports, or a backtest. Facts from different origins sit side by side and are never merged, which here means a
  trade typed by hand and the same trade imported are two entries in two datasets, not one entry replaced by the other.
- **A portfolio's holdings are derived from one dataset**, never from several added together (that would count a trade twice). The
  portfolio says which in a fact, `ledger`, an edge to a dataset node (the user's own by default). The position edge carries that
  dataset, as the edge of a price carries the dataset it came from. Importing the same file again replaces by transaction id inside its
  own dataset, so it does not double either. For the first version there is only the user's own dataset, and the rest is the room the
  model gives for free.
- **Only what is traded can be held.** A position and a transaction need a market that is a real venue or XOFF (funds, priced by
  their company): **never XXXX** (`kNoMarket`), the market of what is priced but not bought: indices, spot prices, currency pairs and the
  rate sources (Euribor from `SPFB`, the €STR from `ECB`). One function, `tradable(snapshot, edge)` in `src/portfolio/`, says so, and
  the buttons, the commands and the model's write all use it, so no path puts a holding on an index or a rate.
- **A position does not name a price edge.** It names the instrument and the market, and the price is whichever edge of that
  instrument on that market the store has (see "Valuation"), so a holding survives a change of provider.
- **Cost basis is the average cost** at first. FIFO and lots are later work, and are only a different derivation from the same transactions.
- **Greenfield** (CLAUDE.md): no migration, and the new node kind, the kept edges and events change the facts schema, so its
  version is bumped.

## Persistence

The model is one; the files are two, because the data differ in what losing them costs. `facts.sqlite` is a cache the sources fill
again. The portfolio file is the only copy of what the user typed, and it is private. They differ in backup, deletion, privacy and
sync (see "The server"), and share no table and no schema version.

- **`portfolios.sqlite`** beside `facts.sqlite` in the user's data directory; `--portfolios FILE` and `--no-portfolios`, as for facts.
  It is a second `FactStore` (`Facts.h`): the store gives it the facts of the portfolio nodes and, new, **their edges' series and
  events**, which nothing keeps today ("What is kept between runs": the edges and the bars on them are not kept). Routed by the kind of
  the node, so a source cannot write a portfolio's name into the cache and a portfolio is never in it.
- **What is built.** `openSqlitePortfolioStore()` (`SqliteFactStore.cpp`, the one place that names SQLite) is the second
  `FactStore`: `FactBatch` also carries what is taken back (`removed`, `removed_nodes`) and the transactions (`events`,
  `events_removed`), and `loadEvents()` reads them. `MemoryStore` routes by the kind of node and applies the portfolio part
  in the thread of the write, so the file is current when the call returns (`synchronous = FULL`). It keeps facts and events: the
  **position edges' series are derived** from the transactions on load, not kept a second time, where they could disagree with
  them. A file that is not a database or damaged is moved to `.bad-<time>`, one of another schema to `.old-<time>` (a name
  already there is never written over, `-2`...); where it cannot be moved nothing is kept and the status line says so. `--portfolios
  FILE`, `--no-portfolios` (portfolios then live for the session in memory); default `portfolios.sqlite` beside `facts.sqlite`.
- **Restoring** reads it before the sources run, as the facts are read, and makes no version.
- **A file of another schema is moved aside** (`.old-<time>`; a damaged one to `.bad-<time>`) and an empty one started, saying so once;
  never dropped in place, since the user typed it. Nothing is migrated.
- Writes are rare and by hand, so they are synchronous (no writer thread). Tables: `facts` as the facts file has it, and
  `events(portfolio, dataset, time, id, body)` for the transactions, the schema version in `PRAGMA user_version`.

## Valuation

What is built (`src/portfolio/Portfolio.h`): the **average cost includes the fees of the buys** (what the buys cost, divided by what
is held; a sell leaves it as it is and takes its share of the cost away, and a position that is closed and bought again starts
again). Within one day the buys are applied first, then by id, so a buy and a sell of one day do not depend on their random ids.
A transaction with no quantity, a negative price or fee, or a part that is none is refused, like a sell of more than is held;
`derivePositions` says which and why. A **conversion** goes only between currencies (kind currency, never a unit such as points
or per cent), over one edge in either direction or two through another currency; the store's reverse indexes `edgesOf(base)` and
`edgesTo(quote)` find them. **The price of a position** is the edge of the instrument on the position's market **in the
currency it was bought in** with the latest bar; one in another currency is not taken, so there is only one conversion.

- **A position row**: quantity (the last of the series), average cost, last price, value, P&L (absolute and per cent), weight in the
  portfolio, and the change and trace over the shared period.
- **The price** is the latest close of the daily series of an edge `instrument -> currency` on the position's market. When the store
  has more than one such edge (two datasets), the one with the latest bar stands; a rule the user can change is later work.
- **Currency.** A portfolio's `currency` fact is an instrument, usually EUR. A position's value is converted by the latest price of the
  edge `quote -> currency` in the store, or through a second edge when there is no direct one. Where there is no such edge the
  position is shown in its own currency, left out of the total, and the total is marked incomplete. No rate is invented.
- **A pure function** `derivePositions(transactions)` and `value(positions, snapshot, currency)` with unit tests; nothing in them
  touches the UI or the disk.

## Several portfolios

Nothing special is needed: there are as many portfolios as there are nodes with something said of them.

- **A position belongs to one portfolio** (the base of its edge), and so does a transaction.
- **One portfolio is shown at a time**, the selected tag. Portfolios are not added up: there is no "All" and no total across them, and
  so no choice of a currency to add them in (each shows in its own `currency` fact). A view over every portfolio, if it is wanted, is
  a selection of portfolio nodes and a currency to add in, and changes nothing here: it is later work.
- **Commands**: `new-portfolio` (asks a name in the form dialog, mints the key, says the name), `rename-portfolio` (says the name
  again; the last said stands, as for any fact), and `delete-portfolio` (asks to confirm, then removes everything said of it, after
  which it is not shown; undoable). The first start has none: the Portfolio context says so and offers `new-portfolio`, and the
  first `buy` offers to make one.
- **The buy and sell dialog has a Portfolio field**, defaulting to the selected tag (a row's own portfolio, in the Portfolio context).
- The tags of the Portfolio context are the portfolios by name (the order of their keys means nothing).

## The screen: Tab switches the context

Tab is not bound in stocks, so it can carry this. A **context** is a view of the store with its own tags, columns and model:
`Markets` (today's) and `Portfolio`. Tab goes to the next context and Shift+Tab to the previous (commands `next-context`,
`previous-context`; the palette lists them), so a third context later (a watchlist, a chart) is one more entry and no new key.

- **The table and the tags stay**: a context swaps the tags (presets, or the portfolios there are) and the model
  (`Quotes`, or a new `Positions`) and the columns; the period tags stay and apply to both. Each context keeps its own selected tag,
  sort and scroll position while the program runs.
- **What is built.** `Positions` (`src/Positions.{h,cpp}`) over `portfolio::value`; `Browser` has `Context::Markets` and `Context::Portfolio`,
  `next-context` and `previous-context` (Tab and Shift+Tab in both profiles; a preset command from Portfolio goes to Markets
  first), a `View` saved for each context (tag, sort, first row, selected row; put back with `Table::scrollToPosition()`, which cellui
  gained), the portfolio tags rebuilt by name when the store changes, and a line over the table where there is nothing to show (no
  portfolio: "new-portfolio"; none held). A portfolio with no `currency` fact is valued in the currency of its first position. The
  Value heading names the currency and has a star where the total is incomplete; a position with no rate shows its value with
  its own currency's code. Part of the cellui work of step 6 came early: `TableModel::buttonShown()` and `scrollToPosition()`.
- **`Positions`** is a table model like `Quotes`, with the columns: icon, symbol, name, quantity, average cost, price, value,
  weight, P&L, P&L %, change and trace. The history loader is told the edges of the positions on screen, as for the rows of a
  preset.
- **Entering a transaction** is done with **buttons at the end of the row, beside the info button** (`kRowActions` in
  `Browser.cpp`: one more entry for each, with a glyph chosen as the info glyph is), in both contexts: a click with the mouse, and from
  the keyboard the table's horizontal cursor (cellui#61) moves onto a button and Enter presses it. They run the commands `buy` and
  `sell` on the row the button is on (`action_row_`), so both are in the command palette too. They have **no key of their own**.
  **A button shows only on a row that can be bought** (`tradable`: its market is a real venue or XOFF, not XXXX), so an index, a
  spot price, a currency pair or an interest rate has the info button alone. A row in the Portfolio context is always tradable. The
  commands refuse such a row with a message when run from the palette, since the palette does not look at the row. Each
  opens a **modal dialog** with the fields quantity, price (the last price offered), date (today offered), fee and note, a live line that shows the
  total, and OK and Cancel. A prompt line is not enough for this much data. cellui has no such dialog (only the attribute
  viewer's modal), so a **form dialog is added to cellui first**, general enough to be used again (labelled fields with
  validation, Tab between fields, Enter accepts, Esc cancels), and stocks uses it. It is a separate repository, so that is its
  own change, merged before stocks depends on it.
- **What is built (step 6).** cellui's `FormModal` (a modal form of text, number and date fields with checks, a live summary, a check of the
  fields together, OK and Cancel; works the same in the terminal and in the GTK window, tested in both), and `TableModel::buttonShown()`
  for the per-row buttons. In `Browser` (`src/BrowserPortfolio.cpp`): `buy` and `sell` (glyphs ⊕ U+2295 and ⊖ U+2296, Mathematical Operators
  that the monospace fonts have, one cell wide), shown on a row by `portfolio::tradable` and guarded by the same function when run
  from the palette (the status line says the row cannot be bought); the form has the portfolio (by name; the shown one, else the last
  traded in, else the first), quantity, price (the last), date (today), fee and note, a live total, and for a sell an error as the quantity is
  typed if it would sell more than is held; the first buy with no portfolio offers to make one; `new-portfolio` (a name, unique, and a currency
  the store knows), `rename-portfolio`, `delete-portfolio` (confirms; says how many transactions go) and `undo`/`redo` (a stack in memory of
  pairs of store writes, so a trade, a make, a rename and a delete are each one step; a new change ends the redo stack). Choices where the
  plan was open: a portfolio is picked in the form by typing its name, not from a list; the form takes a comma as a decimal point; a transaction
  id is made of the time and random bits (below 2^62), so two sells of one day apply in the order they were typed.
- **Undo and redo are the standard ones** (`undo`, `redo`: taking back and taking up again the last change the user made to her portfolios): undo is
  `C-x u` and `C-/` in the terminal keymap and `Ctrl+Z` in the desktop keymap; redo is `C-M-_` in the terminal (as Emacs has it)
  and `Ctrl+Y` on the desktop. A deleted or edited transaction, or a deleted portfolio, can be undone too, so undo works on the user's changes, not on
  the last row added. The stack is in memory, for the session: each entry is the inverse of a write, so nothing in the store or the file is special for it. A transactions view to list and edit them comes later.
- **Keys are bound per profile** (`KeymapProfile::TERMINAL` and `DESKTOP`, as `Browser::bindKeys` does), never one set for both:
  `next-context` and `previous-context` are Tab and Shift+Tab on both; undo and redo as above. `buy` and `sell` have no key (the
  buttons and the palette reach them). `assert(keymap.allBoundIn(...))` stays true.
- **The info line's help** says "Tab: switch context" and not "switch focus".

An alternative that was considered and left: portfolios as tags among the presets, with no context. It keeps one view, but the
columns of a portfolio (quantity, cost, P&L) are not those of a market, and the preset tags would mix the program's selections with the
user's own data.

## The server

Not built, and not needed for any step below, but the model leaves room for it (`server.md` says only that the user's own list is
"kept per user once there is one").

- **Market data and portfolios differ.** Market data is shared, public and rebuildable, and the server is its source of truth. A
  portfolio is private, per user and authenticated, and the server would be a backup and a sync point for it, never the only copy
  and never readable by other users.
- **The same store, per user.** The server's change feed (`/v1/changes?since=VERSION`) would carry a user's nodes, facts and events as
  it carries the rest, scoped to that user, so the portfolio code needs nothing a remote store does not already give. This is why
  there is no journal table of our own.
- **Merging two devices** is a union: keys are minted, so nothing clashes; a transaction has its id; a fact is the last said (the
  rule the data model already has), and a delete beats an edit. This is the whole policy until it is shown to be too crude.
- **Not decided**: the user's identity and authentication (a server question).

## Steps

1. **Node-valued facts** (done; `data-model.md`, "Links"): a value that is a node, so that `currency` can be an edge to a
   currency node; its own change, before this one. The attribute node kind (kind 6) is separate and does not block this plan.
2. (Done.) `src/portfolio/Portfolio.{h,cpp}`: `Transaction`, `derivePositions`, `value`, `tradable`, `rate`, `latestPrice`, with tests (`tests/PortfolioTests.cpp`): buys and sells, average
   cost, a sell of more than is held (refused), a position closed and opened again, a missing rate.
3. (Done.) The model: the portfolio node kind (5) and its minted keys, the position edge and the transaction events in `Store`/`MemoryStore`
   (writes, reads, a snapshot of them), with tests in `tests/ModelTests.cpp` and `NodeKeyTests.cpp`: a transaction on XXXX is refused (an
   index, a rate), one on XOFF and on a real market is kept, a portfolio that shows only when
   something is said of it, a name alone, positions and no name, a delete that makes it vanish, several portfolios, the same
   transactions under two datasets (holdings from the chosen one only).
4. (Done.) The portfolio file: a second `FactStore` routed by node kind, with tests on a file in a temporary directory: round trip, another
   schema moved aside and not lost, a damaged file moved aside, nothing of a portfolio in `facts.sqlite`.
5. (Done.) `Positions` model and the context switch in `Browser` (`next-context`, `previous-context`, keys, the help line),
   with `BrowserTests` for the switch, and for each context keeping its state.
6. (Done.) The form dialog in cellui; then the `buy` and `sell` buttons and commands, `new-portfolio`, `rename-portfolio`, `delete-portfolio`, `undo` and
   `redo`, and the status line's message for each.
7. (Done.) The keys, bound for each profile in `Browser::bindKeys` (`C-/` is sent by a terminal as `C-_`, so that is bound to undo too). 8. (Done.) README: the keys and the flags.

## Later, not planned in detail

Realized P&L and dividends (from the corporate actions the store already holds), FIFO lots, CSV import of a broker's export, a view over
every portfolio added up in one currency, allocation by kind and country, a benchmark to compare the portfolio with, a transactions view.

## Open

What was open when it was built, and what was picked (the simplest option):

- Whether the undo stack should survive a restart: **no**, it is for the session (the stack is of closures over store writes).
- Which price edge values a position when two datasets have the instrument on the market: **the latest bar**, ties to the lowest key, in
  the currency the position was bought in (`portfolio::latestPrice`).
- Whether a position in an edge that no source loads any more is shown from its last stored price or marked stale: **not decided, and
  not handled**: the store does not keep bars between runs, so until a source has loaded the edge a position has no price, its
  value is empty and the total is marked incomplete; after that its last bar stands, however old. Marking it stale is later work.
- Whether `next-preset` and `previous-preset` get keys now that Tab is spoken for: **no**, they stay unbound (they step through the tags of
  whichever context is shown).
- Whether the table can show a button on some rows only: **it could not**, so `TableModel::buttonShown()` was added to cellui.
- The glyphs of the buy and sell buttons: **⊕ and ⊖** (U+2295, U+2296), one cell wide in the monospace fonts of terminals and the window.
  Not checked in a real terminal or on a real desktop font here (see the report in the commit messages): the info glyph was chosen the same way.
- Where the average cost leaves the plan open: fees of buys are in it; the ledger of a portfolio is the user's own dataset unless a
  `ledger` fact says another; a portfolio is picked in the form by typing its name.
