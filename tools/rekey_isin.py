#!/usr/bin/env python3
"""Moves the transactions of a portfolio file from one ISIN to another: for an instrument whose ISIN changed (the gold bar and the gold coin, which
had ISINs hashed from their symbols before src/source/KnownIsins.h numbered them).

    tools/rekey_isin.py ~/.local/share/stocks/portfolios.sqlite OLD NEW [OLD NEW ...]
    tools/rekey_isin.py --selftest

Close stocks first (the file is open while it runs). A copy of the file is made beside it (`.before-rekey-<time>`) before anything is changed. The
program itself migrates nothing (CLAUDE.md: nothing is kept compatible); this is for the one file whose owner typed its transactions in by hand.

Known changes: the gold bar ZZ5US2XRP188 -> ZZ0000000016, the gold coin ZZBQ4H41SRQ4 -> ZZ0000000024, the platinum bar ZZ2QMOY3WUP7 -> ZZ0000000032.
"""
import json
import shutil
import sqlite3
import sys
import tempfile
import time
from pathlib import Path


def key(isin):
    """The key of an instrument (src/model/NodeKey.cpp): its country letters and its nine-character number, without the check digit, plus one."""
    country = (ord(isin[0]) - 65) * 26 + (ord(isin[1]) - 65)
    return country * 36**9 + int(isin[2:11], 36) + 1


def rekey(path, pairs):
    """Changes the instrument of every transaction in `path` from the first ISIN of a pair to the second; returns how many were changed."""
    shutil.copy(path, f"{path}.before-rekey-{int(time.time())}")
    db = sqlite3.connect(path)
    changed = 0
    for old, new in pairs:
        for portfolio, dataset, id_, body in db.execute("SELECT portfolio, dataset, id, body FROM events").fetchall():
            fields = json.loads(body)
            if fields.get("instrument") == key(old):
                fields["instrument"] = key(new)
                db.execute("UPDATE events SET body = ? WHERE portfolio = ? AND dataset = ? AND id = ?",
                           (json.dumps(fields, separators=(",", ":")), portfolio, dataset, id_))
                changed += 1
    db.commit()
    db.close()
    return changed


def selftest():
    # The keys the program makes (src/model/NodeKey.cpp), as the tests of it print them.
    assert key("ZZ5US2XRP188") == 68569488353198205 and key("ZZBQ4H41SRQ4") == 68586050170286055
    assert key("ZZ0000000016") == 68552970751180802 and key("ZZ0000000024") == 68552970751180803
    with tempfile.TemporaryDirectory() as directory:
        path = str(Path(directory) / "portfolios.sqlite")
        db = sqlite3.connect(path)
        db.execute("CREATE TABLE events (portfolio INTEGER NOT NULL, dataset INTEGER NOT NULL, id INTEGER NOT NULL, time INTEGER NOT NULL, "
                   "body TEXT NOT NULL, PRIMARY KEY (portfolio, dataset, id)) WITHOUT ROWID")
        rows = [(1, key("ZZ5US2XRP188"), 50), (2, key("ZZBQ4H41SRQ4"), 2), (3, key("FI0009000681"), 10)]
        for id_, instrument, quantity in rows:
            body = json.dumps({"side": "buy", "instrument": instrument, "market": 7, "quote": 8, "quantity": quantity, "price": 3.5, "fee": 0, "note": "ä"})
            db.execute("INSERT INTO events VALUES (1, 2, ?, 100, ?)", (id_, body))
        db.commit()
        db.close()
        assert rekey(path, [("ZZ5US2XRP188", "ZZ0000000016"), ("ZZBQ4H41SRQ4", "ZZ0000000024")]) == 2
        assert len(list(Path(directory).glob("portfolios.sqlite.before-rekey-*"))) == 1  # the copy
        db = sqlite3.connect(path)
        found = {id_: json.loads(body) for id_, body in db.execute("SELECT id, body FROM events")}
        assert found[1]["instrument"] == key("ZZ0000000016") and found[2]["instrument"] == key("ZZ0000000024")
        assert found[3]["instrument"] == key("FI0009000681")  # another instrument: left alone
        assert found[1]["quantity"] == 50 and found[1]["note"] == "ä" and found[1]["market"] == 7  # nothing else changed
        db.close()
        assert rekey(path, [("ZZ5US2XRP188", "ZZ0000000016")]) == 0  # nothing left to move
    print("rekey_isin: ok")


if __name__ == "__main__":
    if sys.argv[1:] == ["--selftest"]:
        selftest()
    elif len(sys.argv) >= 4 and len(sys.argv) % 2 == 0:
        pairs = list(zip(sys.argv[2::2], sys.argv[3::2]))
        print(rekey(sys.argv[1], pairs), "transaction(s) moved")
    else:
        sys.exit(__doc__)
