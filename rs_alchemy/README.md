# rs_alchemy

Scraper for the RuneScape 3 Grand Exchange data behind
[prices.runescape.wiki/rs/alchemy](https://prices.runescape.wiki/rs/alchemy) and
[/rs/all-items](https://prices.runescape.wiki/rs/all-items): pulls the full daily
price history for every item, computes high-alchemy profit, and writes
everything to pandas DataFrames plus a single HDF5 file.

## Install

```sh
pip install pandas tables requests   # `tables` is PyTables, for HDF5
```

## Run

```sh
# from the repository root
python -m rs_alchemy --out rs_alchemy.h5           # every item, every datapoint
python -m rs_alchemy --days 365                    # trim to the last year
python -m rs_alchemy --alchable-only               # skip items that cannot be alched
python -m rs_alchemy --max-items 20 --days 90      # quick smoke test
python -m rs_alchemy --discover                    # print the URLs the live page calls
```

A full run is one request per item (~4k items). The client rate-limits itself to
`--min-interval` seconds between requests (0.35s default, so roughly 25 minutes)
and caches every response under `--cache-dir`, so an interrupted run resumes
almost instantly. Set a contactable `--user-agent` (or `RS_ALCHEMY_USER_AGENT`) —
the wiki asks for one.

## Where the data comes from

The page is a client-side app, so there is no server-rendered table to parse. The
numbers come from the Weird Gloop exchange API:

| what | endpoint |
| --- | --- |
| full daily history | `GET {api_base}/exchange/history/rs/all?id=<id>` |
| last 90 days | `GET {api_base}/exchange/history/rs/last90d?id=<id>` |
| latest tick | `GET {api_base}/exchange/history/rs/latest?id=<id>` |
| item catalogue (name, high alch value, buy limit) | `chisel.weirdgloop.org/gazproj/gazbot/rs_dump.json` |

Both are overridable (`--api-base`, `--items-url`, `--items-file`), and
`--discover` re-reads the endpoints out of the live page's JS bundle if they ever
move. The item-dump parser accepts a dict-of-dicts or list-of-dicts and
normalises key spellings, so a schema tweak upstream does not break the run.

## Output

`--out` writes one HDF5 file:

| key | contents |
| --- | --- |
| `/items` | item catalogue indexed by `item_id`: name, value, highalch, lowalch, limit, members |
| `/history` | long frame: `item_id, timestamp, price, volume` |
| `/alchemy` | history joined to alch values: `nature_price, alch_profit, alch_roi, profit_per_limit` |
| `/latest` | newest row per item, ranked by profit — the alchemy page's table |
| `/wide/price`, `/wide/volume` | `timestamp x item_id` matrices |
| `/summary/all`, `/summary/last_1y` … `/summary/last_1d` | per-item stats per timeframe |

Tables are stored in PyTables `table` format with `item_id` and `timestamp` as
data columns, so you can slice without loading everything:

```python
import pandas as pd
from rs_alchemy import store

frames = store.read_hdf5('rs_alchemy.h5')       # everything, as a dict of frames
best = frames['summary/last_1m'].nlargest(20, 'profit_mean')

runes = pd.read_hdf('rs_alchemy.h5', 'alchemy', where='item_id == 561')   # one item
good = pd.read_hdf('rs_alchemy.h5', 'alchemy', where='alch_profit > 500')
```

Provenance for a run (row counts, date range, endpoints used) is on the first
table's attributes:

```python
with pd.HDFStore('rs_alchemy.h5') as h:
    print(h.get_storer('items').attrs.scrape)
```

## Feeding the web dashboard

`--export-dir` writes the JSON bundle the static site reads, alongside the HDF5:

```sh
python -m rs_alchemy --export-dir ../patelpb96.github.io/public/rs-alchemy/data
python -m rs_alchemy --export-dir <dir> --no-series      # tables only, no charts
```

| file | contents |
| --- | --- |
| `manifest.json` | provenance, window list, which files exist, `sample` flag |
| `latest.json` | newest row per item |
| `summary_<window>.json` | per-item stats for `all`, `last_1y` … `last_1d` |
| `series/<item_id>.json` | `{t: days-since-epoch, p: price, v: volume}`, thinned to `--max-series-points` |

Tables are column-oriented (`{"columns": [...], "rows": [[...]]}`) to roughly
halve the bytes. The site lives in
[`patelpb96.github.io/public/rs-alchemy/`](https://patelpb96.github.io/rs-alchemy/):
sortable on multiple keys at once, numeric range filters, per-item price /
volume / profit charts.

To regenerate the synthetic dataset the site ships with (no network needed):

```sh
python -m rs_alchemy.make_sample_data --out <dir>
```

## Library use

```python
from rs_alchemy import alchemy, api, history, items, store

client = api.Client(cache_dir='cache')
catalogue = items.load_items(client)
prices, failed = history.load_history(catalogue.index, client=client)   # all history
alch = alchemy.build_alchemy_table(prices, catalogue)
summaries = alchemy.summarize_windows(alch)
store.write_hdf5('rs_alchemy.h5', items=catalogue, history=prices, alchemy=alch,
                 latest=alchemy.latest_snapshot(alch), summaries=summaries)
```

## The profit model

Per High Level Alchemy cast:

```
profit           = highalch - grand_exchange_price - nature_rune_price
roi              = profit / (grand_exchange_price + nature_rune_price)
profit_per_limit = profit * 4h_buy_limit
```

The nature rune price is item 561's own daily history, as-of joined to each row,
so historical profit uses the rune price of that day. `--nature-price` pins it to
a flat value instead.

## Tests

Offline, with synthetic API payloads (no network):

```sh
python -m pytest rs_alchemy/tests -q
```
