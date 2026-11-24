# Portfolios

Status: plan, nothing built.

A portfolio is what the user holds: positions in edges she has bought, valued at the prices the store already has. It is the
user's own data, not a fact of any node (see "the user's own preset" in [data-model.md](data-model.md)), so it lives in a file
of her own and not in `facts.sqlite`.

## Decided

- **Transactions are the truth; positions are derived.** The file keeps what happened (buy, sell, later dividend and fee) and a
  position is computed from it. Keeping only a quantity and a cost loses the history, and realized P&L, FIFO and a corrected
  mistake all need the history.
- **A position refers to an edge by its four keys** (`EdgeKey`), which are the same in every run, so the file survives a restart.
  The price is therefore in the edge's quote currency, as everywhere else.
- **Cost basis is the average cost** at first. FIFO and lots are later work, and are only a different derivation from the same
  transactions.
- **Greenfield** (CLAUDE.md): no migration. But this file is the only copy of what the user typed, unlike `facts.sqlite`, which is
  a cache the sources fill again. So a file of another schema is **moved aside** (`.old-<time>`, as a damaged one is moved to `.bad-<time>`)
  and an empty one started, saying so once; it is never dropped in place. The user can still read the old file, and nothing is migrated.
- **Two files, not one.** The facts file is public data that can be rebuilt, the portfolio file is private data that cannot, and they
  differ in backup, deletion, privacy and sync (see "The server"). They share no table and no schema version.
- **Ids are UUIDs**, not autoincrement integers, for portfolios and transactions, so that two devices can add entries without
  clashing (see "The server").

## The file

`portfolios.sqlite` beside `facts.sqlite` in the user's data directory; `--portfolios FILE` and `--no-portfolios`, as for facts.
Behind an interface (`Ledger`: `load()`, `apply(operation)`), with `SqliteLedger` in no header, like `FactStore`. Writes are rare and
by hand, so they are synchronous (no writer thread). Tables: `portfolios(id, name, currency)` and
`transactions(id, portfolio, time, base, quote, market, dataset, kind, quantity, price, fee, fee_currency, note)` hold the current
state, and `journal(seq, id, device, time, op, subject, before, after)` is an **append-only log of operations** (add, edit and delete
of a portfolio or a transaction, with what it was and what it became). The schema version is in `PRAGMA user_version`.

- **Every change is one operation** written with the state change in one transaction. Nothing changes the tables without a journal
  entry.
- **Undo and redo** work on the operations of the session (the stack is in memory and not kept between runs). Undo does not erase
  the journal: it appends the inverse operation, which is what makes the journal mergeable later.
- **Deleting** a portfolio or a transaction is an operation like the others, so it is undone like them (a deleted portfolio comes back
  with its transactions).

## Valuation

- **A position**: quantity, average cost, last price (from `lastCandles` of the daily series, as a row of the table does), value,
  P&L (absolute and per cent), weight in the portfolio, and the change and trace over the shared period.
- **Currency.** A portfolio has a currency (an instrument, usually EUR). A position's value is converted by the latest price of the
  edge `quote -> currency` in the store, or through a second edge when there is no direct one. Where there is no such edge the position is
  shown in its own currency, left out of the total, and the total is marked incomplete. No rate is invented.
- **A pure function** `derivePositions(transactions)` and `value(positions, snapshot, currency)` with unit tests; nothing in them
  touches the UI or the disk.

## Several portfolios

A user has any number of portfolios (a pension account, a trading account, a "watch and wait" one).

- **A transaction belongs to exactly one portfolio.** A position is per portfolio and edge.
- **"All" is a view, not a portfolio.** It is the last tag of the Portfolio context and adds the portfolios up: where one edge is held
  in two, the quantities are summed and the cost is the quantity-weighted average. It cannot be bought or sold in; `buy` and `sell`
  there ask which portfolio.
- **Currency.** Each portfolio has its own currency. "All" converts through one **base currency** of the program (default EUR; a setting,
  not a fact of any portfolio), and is marked incomplete in the same way as any total when a rate is missing.
- **Commands**: `new-portfolio`, `rename-portfolio`, `delete-portfolio` (each asks in the form dialog; a delete asks to confirm and is
  undoable). The first start has no portfolio: the Portfolio context says so and offers `new-portfolio`, and the first `buy` offers to make one.
- **The buy and sell dialog has a Portfolio field**, defaulting to the selected tag, or to the one last used where "All" is selected.
- The tags of the Portfolio context are the portfolios in the order they were made, then "All". Renaming does not change the order.

## The screen: Tab switches the context

Tab is not bound in stocks, so it can carry this. A **context** is a view of the store with its own tags, columns and model:
`Markets` (today's) and `Portfolio`. Tab goes to the next context and Shift+Tab to the previous (commands `next-context`,
`previous-context`; the palette lists them), so a third context later (a watchlist, a chart) is one more entry and no new key.

- **The table and the tags stay**: a context swaps the tags (presets, or the user's portfolios and an "All") and the model
  (`Quotes`, or a new `Positions`) and the columns; the period tags stay and apply to both. Each context keeps its own selected tag,
  sort and scroll position while the program runs.
- **`Positions`** is a table model like `Quotes`, with the columns: icon, symbol, name, quantity, average cost, price, value,
  weight, P&L, P&L %, change and trace. The history loader is told the edges of the positions on screen, as for the rows of a
  preset.
- **Entering a transaction** is a command on the selected row in either context (`buy`, `sell`) that opens a **modal dialog**
  with the fields quantity, price (the last price offered), date (today offered), fee and note, a live line that shows the
  total, and OK and Cancel. A prompt line is not enough for this much data. cellui has no such dialog (only the attribute
  viewer's modal), so a **form dialog is added to cellui first**, general enough to be used again (labelled fields with
  validation, Tab between fields, Enter accepts, Esc cancels), and stocks uses it. It is a separate repository, so that is its
  own change, merged before stocks depends on it.
- **Undo and redo are the standard ones** (`undo`, `redo`: taking back and taking up again the last change to the ledger): undo is
  `C-x u` and `C-/` in the terminal keymap and `Ctrl+Z` in the desktop keymap; redo is `C-M-_` in the terminal (as Emacs has it)
  and `Ctrl+Y` on the desktop. A deleted or edited transaction can be undone too, so undo works on changes to the ledger, not on
  the last row added. A transactions view to list and edit them comes later.
- **Keys are bound per profile** (`KeymapProfile::TERMINAL` and `DESKTOP`, as `Browser::bindKeys` does), never one set for both:
  `next-context` and `previous-context` are Tab and Shift+Tab on both; `buy` and `sell` are `b` and `s` in the terminal and
  `Ctrl+B` and `Ctrl+S` unless the desktop keymap uses them (to be checked); undo and redo as above. `assert(keymap.allBoundIn(...))` stays true.
- **The info line's help** says "Tab: switch context" and not "switch focus".

An alternative that was considered and left: portfolios as tags among the presets, with no context. It keeps one view, but the
columns of a portfolio (quantity, cost, P&L) are not those of a market, and the preset tags would mix the program's selections with the
user's own data.

## The server

Not built, and not needed for any step below, but the design leaves room for it (`server.md` says only that the user's own list is
"kept per user once there is one").

- **Market data and portfolios are different.** Market data is shared, public and rebuildable, and the server is its source of truth.
  A portfolio is private, per user and authenticated, and the server would be a backup and a sync point for it, never the only copy
  and never readable by other users.
- **A `RemoteLedger`** would implement `Ledger` as `RemoteStore` implements `Store`: the same operations, sent to and fetched from the
  server per user.
- **Merging is a union of journals.** Operations have a UUID and a device, so two devices that worked offline give two logs whose union,
  in the order of time then device, is the state. An edit of one transaction on two devices is the one conflict: the later operation
  stands, and the earlier one stays in the journal. A delete wins over an edit. This is the whole policy until it is shown to be too crude.
- **Not decided**: the user's identity and authentication (a server question), and whether the journal is sent whole or from a mark.

## Steps

1. `src/portfolio/`: `Transaction`, `derivePositions`, `value`, with tests (`tests/PortfolioTests.cpp`): buys and sells, average
   cost, a sell of more than is held (refused), a position closed and opened again, a missing rate.
2. `Ledger` and `SqliteLedger`, with tests on a file in a temporary directory: round trip, the journal written with every change,
   undo and redo (including a deleted portfolio), another schema moved aside and not lost, a damaged file moved aside, several portfolios.
3. `Positions` model and the context switch in `Browser` (`next-context`, `previous-context`, keys, the help line), with
   `BrowserTests` for the switch, for each context keeping its state and for the "All" view.
4. The form dialog in cellui; then `buy` and `sell` on a row, `new-portfolio`, `rename-portfolio`, `delete-portfolio`, `undo` and
   `redo`, and the status line's message for each.
5. README: the keys and the flags.

## Later, not planned in detail

Realized P&L and dividends (from the corporate actions the store already holds), FIFO lots, CSV import of a broker's export, allocation
by kind and country, a benchmark to compare the portfolio with, a transactions view.

## Open

- The base currency of "All": a setting kept where (the portfolio file, or a settings file of its own).
- Whether the undo stack should survive a restart (not at first).
- Whether a position in an edge that no source loads any more is shown from its last stored price or marked stale.
- The keys for `buy` and `sell` on the desktop, and whether `next-preset` and `previous-preset` (unbound today) get keys of their own now
  that Tab is spoken for.
