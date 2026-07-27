"""Export the scraped frames as JSON for the static site in `docs/`.

The site is plain HTML/JS with no build step, so the format is deliberately
dumb: one manifest plus one file per table, each stored column-oriented

    {"columns": ["item_id", "name", ...], "rows": [[2, "Cannonball", ...], ...]}

which is roughly half the bytes of a list of objects and loads in one pass.

Per-item price series are written individually under `series/` and fetched
lazily when a row is opened, so the initial page load stays small.
"""

import datetime
import json
import math
import os
import shutil

import pandas as pd

# columns the site shows for the "latest" snapshot, in display order
LATEST_COLUMNS = [
    'item_id',
    'name',
    'timestamp',
    'price',
    'volume',
    'highalch',
    'limit',
    'members',
    'nature_price',
    'alch_profit',
    'alch_roi',
    'profit_per_limit',
]


def _clean(value):
    """JSON-safe scalar: NaN/NaT/pd.NA -> None, numpy scalars -> python."""
    if value is None or value is pd.NaT or value is pd.NA:
        return None
    if isinstance(value, float) and math.isnan(value):
        return None
    if isinstance(value, (pd.Timestamp, datetime.datetime, datetime.date)):
        return str(pd.Timestamp(value).date())
    if hasattr(value, 'item'):  # numpy scalar
        try:
            value = value.item()
        except (ValueError, AttributeError):
            return str(value)
    if isinstance(value, float):
        if math.isnan(value) or math.isinf(value):
            return None
        return round(value, 6)
    if isinstance(value, (int, str, bool)):
        return value
    return str(value)


def frame_to_columnar(frame, index_name=None, columns=None):
    """{'columns': [...], 'rows': [[...], ...]} for one DataFrame."""
    if frame is None or len(frame) == 0:
        return {'columns': [], 'rows': []}

    frame = frame.copy()
    if index_name and frame.index.name == index_name:
        frame = frame.reset_index()
    elif index_name and index_name not in frame.columns:
        frame = frame.rename_axis(index_name).reset_index()

    if columns:
        frame = frame.reindex(columns=[c for c in columns if c in frame.columns])

    names = list(frame.columns)
    rows = [[_clean(v) for v in record] for record in frame.itertuples(index=False, name=None)]
    return {'columns': names, 'rows': rows}


def _write_json(path, payload):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w') as f:
        json.dump(payload, f, separators=(',', ':'))
    return os.path.getsize(path)


def export_series(history, out_dir, max_points=1500):
    """One file per item: {"t": [days since epoch], "p": [price], "v": [volume]}.

    Long histories are thinned to `max_points` evenly spaced samples, which is
    far more resolution than a 900px-wide chart can show anyway.
    """
    series_dir = os.path.join(out_dir, 'series')
    shutil.rmtree(series_dir, ignore_errors=True)
    os.makedirs(series_dir, exist_ok=True)

    written = []
    epoch = pd.Timestamp('1970-01-01')

    for item_id, group in history.groupby('item_id'):
        group = group.sort_values('timestamp')
        if max_points and len(group) > max_points:
            step = math.ceil(len(group) / max_points)
            group = group.iloc[::step]
        days = ((group['timestamp'] - epoch).dt.total_seconds() // 86400).astype('int64')
        payload = {
            't': days.tolist(),
            'p': [_clean(v) for v in group['price']],
            'v': [_clean(v) for v in group['volume']],
        }
        _write_json(os.path.join(series_dir, f'{int(item_id)}.json'), payload)
        written.append(int(item_id))

    return written


def export_site_data(
    out_dir,
    latest=None,
    summaries=None,
    history=None,
    with_series=True,
    max_series_points=1500,
    attrs=None,
    sample=False,
):
    """Write the whole JSON bundle the site reads.  Returns the manifest."""
    summaries = summaries or {}
    os.makedirs(out_dir, exist_ok=True)

    files = {}
    sizes = {}

    if latest is not None and len(latest):
        payload = frame_to_columnar(latest, columns=LATEST_COLUMNS)
        sizes['latest.json'] = _write_json(os.path.join(out_dir, 'latest.json'), payload)
        files['latest'] = 'latest.json'

    summary_files = {}
    for name, frame in summaries.items():
        if frame is None or len(frame) == 0:
            continue
        payload = frame_to_columnar(frame, index_name='item_id')
        filename = f'summary_{name}.json'
        sizes[filename] = _write_json(os.path.join(out_dir, filename), payload)
        summary_files[name] = filename
    if summary_files:
        files['summaries'] = summary_files

    series_ids = []
    if with_series and history is not None and len(history):
        series_ids = export_series(history, out_dir, max_series_points)
        files['series'] = 'series/{item_id}.json'

    span = {}
    if history is not None and len(history):
        span = {
            'first_timestamp': str(pd.Timestamp(history['timestamp'].min()).date()),
            'last_timestamp': str(pd.Timestamp(history['timestamp'].max()).date()),
            'n_datapoints': int(len(history)),
        }

    manifest = {
        'generated_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds'),
        'source': 'https://prices.runescape.wiki/rs/alchemy',
        'sample': bool(sample),
        'n_items': int(len(latest)) if latest is not None else 0,
        'windows': list(summary_files),
        'series_ids': series_ids,
        'files': files,
        'bytes': sizes,
    }
    manifest.update(span)
    manifest.update(attrs or {})

    _write_json(os.path.join(out_dir, 'manifest.json'), manifest)
    return manifest
