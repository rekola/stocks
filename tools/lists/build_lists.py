#!/usr/bin/env python3
"""Builds data/lists/nasdaq-helsinki.csv and currencies.csv: the instruments Yahoo is asked for (it has no discovery), each checked
against its chart endpoint. The stocks carry their ISIN from Nordnet's stock list. indices.csv is kept by hand. There are no commodities: Yahoo's are futures contracts,
not the commodity's price (and in other units: copper is dollars a pound).

usage: build_lists.py [OUTPUT_DIR]      (default: data/lists in this script's repository)

The candidates are listed below; a candidate Yahoo does not know, or has no recent price for, is left out and reported, so the files hold only
tickers that answered. The lists are not meant to be complete. One request per candidate, paused between them: run it
when the lists change, not on every build.
"""
import csv
import json
import os
import sys
import time
import urllib.request

HOSTS = ["query1.finance.yahoo.com", "query2.finance.yahoo.com"]
PAUSE = 0.25

NORDNET_SYMBOL = {"NDA-FI": "NDA FI"}

# ---- Nasdaq Helsinki: Yahoo tickers (the Helsinki exchange suffix is .HE) ----
HELSINKI = """
NOKIA FORTUM SAMPO KNEBV UPM NESTE STERV WRT1V ELISA ORNBV ORNAV NDA-FI METSO OUT1V KESKOB KESKOA KCR TIETO TYRES
VALMT HUH1V KALMAR FSKRS KEMIRA QTCOM REG1V SANOMA TELIA1 YIT ASPO CAPMAN CTY1S DIGIA EVLI FIA1S GOFORE INDERES KAMUX
MANTA MEKKO OLVAS PUUILO PON1V RAIVV RAUTE REMEDY SSABAH SSABBH VAIAS VIK1V WUF1V ADMCM AKTIA ALMA BITTI BIOBV DETEC
EEZY ELEAV ENENTO ETTE FODELIA GLA1V HARVIA HONBS MUSTI OMASP ORTHEX PIHLIS QPR1V ROBIT SCANFL SPINN SRV1V TAALA
TOKMAN TRH1V TULAV ATRAV AIFORIA APETIT CTH1V LUMI MARAS NANOFH SAGCV SUY1V TEM1V TTALO VINCIT
HIAB PNA1V VERK TNOM NXTMH SIILI KREATE AFAGR SOLTQ INVEST LOIHDE ALISA
""".split()

# ---- Currencies: the pairs, base first, in market convention ----
CURRENCY_NAMES = {
    "USD": "US dollar", "EUR": "Euro", "GBP": "British pound", "JPY": "Japanese yen", "CHF": "Swiss franc",
    "AUD": "Australian dollar", "NZD": "New Zealand dollar", "CAD": "Canadian dollar", "SEK": "Swedish krona",
    "NOK": "Norwegian krone", "DKK": "Danish krone", "PLN": "Polish zloty", "CZK": "Czech koruna",
    "HUF": "Hungarian forint", "RON": "Romanian leu", "TRY": "Turkish lira", "ZAR": "South African rand",
    "MXN": "Mexican peso", "BRL": "Brazilian real", "INR": "Indian rupee", "CNY": "Chinese yuan",
    "HKD": "Hong Kong dollar", "SGD": "Singapore dollar", "KRW": "South Korean won", "THB": "Thai baht",
    "ILS": "Israeli shekel", "AED": "UAE dirham", "SAR": "Saudi riyal", "IDR": "Indonesian rupiah",
    "PHP": "Philippine peso", "MYR": "Malaysian ringgit", "TWD": "New Taiwan dollar", "CLP": "Chilean peso",
    "COP": "Colombian peso", "ARS": "Argentine peso", "ISK": "Icelandic krona", "BGN": "Bulgarian lev",
}
MAJORS = ["EUR", "GBP", "AUD", "NZD", "USD", "CAD", "CHF", "JPY", "SEK", "NOK", "DKK"]  # base before quote
OTHERS = ["PLN", "CZK", "HUF", "RON", "TRY", "ZAR", "MXN", "BRL", "INR", "CNY", "HKD", "SGD", "KRW", "THB", "ILS",
          "AED", "SAR", "IDR", "PHP", "MYR", "TWD", "CLP", "COP", "ARS", "ISK", "BGN"]


def nordnet_isins(market_id):
    """symbol -> ISIN of the stocks Nordnet lists on one market (Yahoo gives no ISIN, so the data does)."""
    base = "https://www.nordnet.fi/api/2/instrument_search/query/stocklist?sort_order=asc&sort_attribute=symbol&limit=1000"
    headers = {"Accept": "application/json", "Origin": "https://www.nordnet.fi", "Referer": "https://www.nordnet.fi/",
               "Client-Id": "NEXT"}
    out, offset = {}, 0
    while True:
        with urllib.request.urlopen(urllib.request.Request(base + "&offset=%d" % offset, headers=headers), timeout=60) as r:
            doc = json.load(r)
        for row in doc["results"]:
            if row["market_info"]["market_id"] == market_id and row["instrument_info"].get("isin"):
                out[row["instrument_info"]["symbol"]] = row["instrument_info"]["isin"]
        offset += len(doc["results"])
        if not doc["results"] or offset >= doc["total_hits"]:
            return out
        time.sleep(0.5)


def currency_pairs():
    pairs = [(a, b) for i, a in enumerate(MAJORS) for b in MAJORS[i + 1:]]
    pairs += [("USD", o) for o in OTHERS]
    pairs += [("EUR", o) for o in OTHERS if o in ("PLN", "CZK", "HUF", "RON", "TRY", "ZAR", "CNY", "BGN", "ISK")]
    return pairs


def chart(ticker):
    """The chart meta of `ticker` (five daily bars), or None when Yahoo has nothing for it."""
    for host in HOSTS:
        url = "https://%s/v8/finance/chart/%s?range=5d&interval=1d" % (host, ticker)
        try:
            request = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0 (compatible; stocks)"})
            with urllib.request.urlopen(request, timeout=20) as r:
                doc = json.load(r)
        except Exception:
            continue
        result = (doc.get("chart") or {}).get("result")
        if result and result[0].get("timestamp") and any(c is not None for c in result[0]["indicators"]["quote"][0]["close"]):
            return result[0]["meta"]
        return None
    return None


def fresh(meta, days=14):
    """Whether the last price is recent: a delisted or dormant ticker still answers, with old data."""
    t = meta.get("regularMarketTime")
    return t is None or time.time() - t < days * 86400


def write(path, market, rows, mic=None):
    """A file of `market` (its name, for rows with no code), or, with `mic` (a code for every row, or None), of rows that
    each say their market by its ISO 10383 code."""
    with open(path, "w", newline="", encoding="utf-8") as f:
        if mic is None:
            f.write("# market: %s\n" % market)
            f.write("# kind,base,quote,name,yahoo,isin: what one unit of `base` costs in `quote`; `yahoo` is Yahoo's ticker\n")
        else:
            f.write("# %s\n" % market)
            f.write("# kind,base,quote,name,yahoo,isin,mic: what one unit of `base` costs in `quote`; `yahoo` is Yahoo's ticker; "
                    "`mic` is the ISO 10383 code of the market it is listed on\n")
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["kind", "base", "quote", "name", "yahoo", "isin"] + ([] if mic is None else ["mic"]))
        w.writerows(rows if mic is None else [r + [mic] for r in rows])


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "..", "data", "lists")
    os.makedirs(out, exist_ok=True)
    missing = []

    isins = nordnet_isins(24)  # Nasdaq Helsinki
    rows = []
    seen = set()
    for t in HELSINKI:
        ticker = t + ".HE"
        if ticker in seen:
            continue
        seen.add(ticker)
        meta = chart(ticker)
        time.sleep(PAUSE)
        if not meta or meta.get("exchangeName") != "HEL" or not fresh(meta):
            missing.append(ticker)
            continue
        name = (meta.get("longName") or meta.get("shortName") or t).strip()
        # The base is the symbol Nordnet calls it on this market (the Yahoo adapter finds the instrument by it): the same,
        # but for the few that Yahoo spells with a hyphen where Nordnet has a space.
        isin = isins.get(NORDNET_SYMBOL.get(t, t))
        if not isin:
            missing.append(ticker + " (no ISIN at Nordnet)")
            continue
        rows.append(["company", NORDNET_SYMBOL.get(t, t), meta.get("currency", "EUR"), name, ticker, isin])
    rows.sort(key=lambda r: r[1])
    write(os.path.join(out, "nasdaq-helsinki.csv"), "The Nasdaq Helsinki shares: tools/lists/build_lists.py", rows, mic="XHEL")
    print("Nasdaq Helsinki: %d of %d" % (len(rows), len(seen)), flush=True)

    rows = []
    rows = []
    rows = []
    pairs = currency_pairs()
    for base, quote in pairs:
        ticker = base + quote + "=X"
        meta = chart(ticker)
        time.sleep(PAUSE)
        if not meta or not fresh(meta):
            missing.append(ticker)
            continue
        rows.append(["currency", base, quote, CURRENCY_NAMES[base], ticker, ""])
    write(os.path.join(out, "currencies.csv"), "Currencies", rows)
    print("Currencies: %d of %d" % (len(rows), len(pairs)), flush=True)

    print("left out (not on Yahoo, or no recent price) (%d): %s" % (len(missing), " ".join(missing)))


if __name__ == "__main__":
    main()
