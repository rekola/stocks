# Glossary

Terms with a specific meaning in this project.

**Dataset code**
What names a dataset: one to ten letters or digits that its adapter defines and
never changes. The name a person reads is a fact of the dataset.

**Fact**
One thing said of a node: a node, a predicate, a value, and when it was
updated. The persisted form of an attribute (a thin edge), kept in
the facts file under the key of its node.

**Facts file**
The SQLite file the facts are kept in between runs, `facts.sqlite` in the user's
data directory. The store reads it at start-up and writes every change to it.
A file that cannot be read (damaged, or of a newer schema) is moved aside and a
new one started.

**Node key**
The number that names a node, the same in every run: one 64-bit number whose top
bits say what kind of node it is. An instrument's is its ISIN packed, a
currency's or unit's its code, a dataset's its code, a market's its MIC (so a
market is never made up: a source that knows a market only by its own id maps it
to a MIC, and what it cannot map it leaves out). The name and the country are
facts of the market.

**XXXX, XOFF**
The two codes for what has no venue. XXXX is "no market": an index, a spot price or
a currency pair, which are not the price of an exchange. XOFF is off exchange: a
fund, priced by its company. A real venue, such as a futures market, has its own MIC.
The facts file keeps a fact under the key of its node.

**Preset**
A selection of edges that the table shows, one row per edge: a market, a set of
markets, a filter (by the kind of the instrument, the country of the market) or
the edges the user follows. Today a preset is a market (by its MIC) and, where the market holds several kinds (XXXX), the kind of the instrument, or All.

**Updated**
When a source last said a fact, whether or not the value was the same as
before: every write sets it. It is not when the value last changed, which is not
known (the data is loaded too seldom). A fact said again with the same value
moves `updated` and nothing else: no new version of the store, no redraw. It is
written to the file with the next batch and not on its own. A time in
milliseconds, UTC, taken by the store where the write happens.
