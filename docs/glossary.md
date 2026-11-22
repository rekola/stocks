# Glossary

Terms with a specific meaning in this project.

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
What a node's facts are kept under in the facts file, since the ids of a run
mean nothing in the next: an instrument's ISIN (its symbol where it has none), a
market's MIC (else the id a source gives it, else its name), a dataset's name.
When a stronger one is learned the facts move to it.

**Updated**
When a source last said a fact, whether or not the value was the same as
before: every write sets it. It is not when the value last changed, which is not
known (the data is loaded too seldom). A fact said again with the same value
moves `updated` and nothing else: no new version of the store, no redraw. It is
written to the file with the next batch and not on its own. A time in
milliseconds, UTC, taken by the store where the write happens.
