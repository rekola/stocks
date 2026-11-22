#!/usr/bin/env python3
"""Builds the stock lists of the countries other than Finland (data/lists/<country>.csv): the largest listed companies of each, for
the Yahoo adapter, which has no discovery and needs its instruments listed.

usage: build_global_lists.py [OUTPUT_DIR] [COUNTRY ...]      (default: data/lists, every country below)

Where the instruments come from: a stock list that has an ISIN for every stock and its market value. The largest
N of a country by market value are taken (ordinary shares only: no preferred shares or over-the-counter listings). Each is
found on Yahoo by its **ISIN** (Yahoo's search answers an ISIN with its listings), so that the ticker is certainly the same
company, and the listing on the country's own exchange is chosen (a suffix: .L for London, .PA for Paris, ...). It is then
checked against Yahoo's chart: one with no recent price is left out. Two requests a candidate, paused between them; run it
when the lists change, not on every build. Only what that list has gets an ISIN here: Asia-Pacific and Latin America wait for
another source of ISINs.
"""
import csv
import json
import os
import sys
import time
import urllib.parse
import urllib.request

import build_lists as bl

# country (the list's exchange_country) -> the market's name, how many, the Yahoo suffix of its exchange ("" for the US, whose
# tickers have none and whose exchanges are listed beside).
COUNTRIES = {
    "US": ("US", 300, "", {"NASDAQ", "NYSE", "NYSE American", "NYSEArca", "NASDAQ GS", "NasdaqGS", "NasdaqGM", "NasdaqCM"}),
    "CA": ("Canada", 100, ".TO", None),
    "GB": ("United Kingdom", 150, ".L", None),
    "DE": ("Germany", 100, ".DE", None),
    "FR": ("France", 100, ".PA", None),
    "SE": ("Sweden", 100, ".ST", None),
    "DK": ("Denmark", 50, ".CO", None),
    "NO": ("Norway", 50, ".OL", None),
    "NL": ("Netherlands", 50, ".AS", None),
    "BE": ("Belgium", 30, ".BR", None),
    "ES": ("Spain", 50, ".MC", None),
    "IT": ("Italy", 50, ".MI", None),
    "CH": ("Switzerland", 80, ".SW", None),
    "AT": ("Austria", 25, ".VI", None),
    "PT": ("Portugal", 20, ".LS", None),
    "IE": ("Ireland", 20, ".IR", None),
}
SHARE_TYPES = {"ESH", "ESHMTF"}  # ordinary shares, on an exchange or a multilateral facility
PAUSE = 0.3


def listed_stocks():
    """Every stock of the list with an ISIN and a market value: country -> [(market value, isin, name, symbol)]."""
    base = "https://www.nordnet.fi/api/2/instrument_search/query/stocklist?sort_order=asc&sort_attribute=symbol&limit=1000"
    headers = {"Accept": "application/json", "Origin": "https://www.nordnet.fi", "Referer": "https://www.nordnet.fi/",
               "Client-Id": "NEXT"}
    by, offset = {}, 0
    while True:
        with urllib.request.urlopen(urllib.request.Request(base + "&offset=%d" % offset, headers=headers), timeout=90) as r:
            doc = json.load(r)
        for row in doc["results"]:
            info, company = row["instrument_info"], row.get("company_info") or {}
            country = (row.get("exchange_info") or {}).get("exchange_country")
            cap = company.get("market_cap")
            if country in COUNTRIES and info.get("isin") and cap and info.get("instrument_type") in SHARE_TYPES:
                by.setdefault(country, {})[info["isin"]] = (cap, info["isin"], (info.get("long_name") or info["name"]).strip(), info["symbol"])
        offset += len(doc["results"])
        if not doc["results"] or offset >= doc["total_hits"]:
            return {c: sorted(v.values(), reverse=True) for c, v in by.items()}
        time.sleep(0.5)


def search(isin):
    """The Yahoo listings of an ISIN: [(symbol, exchange)]."""
    url = "https://query2.finance.yahoo.com/v1/finance/search?" + urllib.parse.urlencode(
        {"q": isin, "quotesCount": 10, "newsCount": 0, "listsCount": 0})
    for attempt in range(2):
        try:
            request = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0 (compatible; stocks)"})
            with urllib.request.urlopen(request, timeout=20) as r:
                doc = json.load(r)
            break
        except urllib.error.HTTPError as e:
            if e.code == 429 and attempt == 0:  # asked too fast: wait and ask once more
                print("  (rate limited, waiting)", flush=True)
                time.sleep(30)
                continue
            return []
        except Exception:
            return []
    return [(q["symbol"], q.get("exchDisp") or q.get("exchange") or "") for q in doc.get("quotes", []) if q.get("quoteType") == "EQUITY"]


def ticker_of(listings, suffix, exchanges):
    for symbol, exchange in listings:
        if suffix and symbol.endswith(suffix):
            return symbol
        if not suffix and "." not in symbol and exchange in exchanges:
            return symbol
    return None


def build(country, stocks, out):
    market, wanted, suffix, exchanges = COUNTRIES[country]
    rows, seen, tried = [], set(), 0
    for cap, isin, name, symbol in stocks:
        if len(rows) >= wanted or tried >= wanted * 3:  # a bound: the rest of a long list is not wanted
            break
        tried += 1
        if tried % 50 == 0:
            print("  %s: %d found of %d tried" % (market, len(rows), tried), flush=True)
        listings = search(isin)
        time.sleep(PAUSE)
        ticker = ticker_of(listings, suffix, exchanges)
        if not ticker or ticker in seen:
            continue
        meta = bl.chart(ticker)
        time.sleep(PAUSE)
        if not meta or not bl.fresh(meta):
            continue
        seen.add(ticker)
        rows.append(["company", ticker, meta.get("currency") or "USD", name, ticker, isin])
    rows.sort(key=lambda r: r[1])
    slug = market.lower().replace(" ", "-")
    bl.write(os.path.join(out, slug + ".csv"), market, rows)
    print("%s: %d of %d tried (wanted %d)" % (market, len(rows), tried, wanted), flush=True)


def main():
    args = sys.argv[1:]
    out = args.pop(0) if args and os.path.isdir(args[0]) else os.path.join(os.path.dirname(__file__), "..", "..", "data", "lists")
    wanted = [c for c in args] or list(COUNTRIES)
    stocks = listed_stocks()
    print("listed: %s" % {c: len(stocks.get(c, [])) for c in wanted}, flush=True)
    for country in wanted:
        build(country, stocks.get(country, []), out)


if __name__ == "__main__":
    main()
