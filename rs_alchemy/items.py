"""The item catalogue: id, name, shop value, high/low alch value, buy limit.

The alchemy page needs a high-alch value per item, which the exchange history
API does not carry.  Weird Gloop publish an item dump that does:

    https://chisel.weirdgloop.org/gazproj/gazbot/rs_dump.json

The parser below is deliberately tolerant -- it accepts a dict-of-dicts or a
list-of-dicts and normalises whatever key spellings it finds -- so a change in
the dump's shape (or pointing `--items-url` at a different mirror) does not
require code changes.  `--items-file` reads the same JSON off disk.
"""

import json

import pandas as pd

from . import api

ITEMS_URL = 'https://chisel.weirdgloop.org/gazproj/gazbot/rs_dump.json'

# canonical column -> accepted spellings in the source JSON
_FIELD_ALIASES = {
    'item_id': ('id', 'item_id', 'itemId'),
    'name': ('name', 'item_name'),
    'value': ('value', 'store_price', 'price'),
    'highalch': ('highalch', 'highalchvalue', 'high_alch', 'highAlch', 'ha'),
    'lowalch': ('lowalch', 'lowalchvalue', 'low_alch', 'lowAlch', 'la'),
    'limit': ('limit', 'buy_limit', 'buylimit', 'ge_limit'),
    'members': ('members', 'isMembers', 'member'),
    'examine': ('examine', 'description'),
}

COLUMNS = list(_FIELD_ALIASES)


def _pick(record, aliases):
    for alias in aliases:
        if alias in record and record[alias] is not None:
            return record[alias]
    return None


def _as_bool(value):
    if isinstance(value, str):
        return value.strip().lower() in ('true', 'yes', '1')
    return bool(value)


def parse_items(raw):
    """Normalise a raw item dump into a DataFrame indexed by item_id."""
    if isinstance(raw, dict):
        # Either {"<id or name>": {...}} or {"items": [...]}.
        for key in ('items', 'data', 'item'):
            if key in raw and isinstance(raw[key], (list, dict)):
                raw = raw[key]
                break
    records = list(raw.values()) if isinstance(raw, dict) else list(raw)

    rows = []
    for record in records:
        if not isinstance(record, dict):
            continue
        row = {name: _pick(record, aliases) for name, aliases in _FIELD_ALIASES.items()}
        if row['item_id'] is None or row['name'] is None:
            continue
        rows.append(row)

    if not rows:
        raise ValueError('no usable item records found in the item dump')

    items = pd.DataFrame(rows, columns=COLUMNS)
    items['item_id'] = pd.to_numeric(items['item_id'], errors='coerce').astype('Int64')
    items = items.dropna(subset=['item_id'])

    for column in ('value', 'highalch', 'lowalch', 'limit'):
        items[column] = pd.to_numeric(items[column], errors='coerce').astype('Int64')

    # High/low alchemy pay 60%/40% of the shop value, so fill any gaps.
    value = items['value'].astype('Float64')
    items['highalch'] = items['highalch'].fillna((value * 0.6).round().astype('Int64'))
    items['lowalch'] = items['lowalch'].fillna((value * 0.4).round().astype('Int64'))

    items['members'] = items['members'].map(_as_bool)
    items['name'] = items['name'].astype(str)
    items['examine'] = items['examine'].fillna('').astype(str)

    items = items.drop_duplicates(subset='item_id').set_index('item_id').sort_index()
    return items


def load_items(client=None, url=ITEMS_URL, path=None):
    """Fetch (or read) and parse the item catalogue."""
    if path:
        with open(path) as f:
            return parse_items(json.load(f))
    client = client or api.Client()
    return parse_items(client.get_json(url))


def tradeable_alchables(items, min_highalch=1):
    """Items worth pulling history for: they alch for something and can be traded.

    Untradeable items have no Grand Exchange history, so asking for it just
    burns requests.  We cannot tell tradeability from the dump directly, so the
    filter is on alch value only; items with no history are dropped later.
    """
    keep = items['highalch'].fillna(0) >= min_highalch
    return items[keep]
