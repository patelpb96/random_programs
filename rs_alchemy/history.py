"""Daily Grand Exchange price/volume history for a set of items.

The exchange API serves one item per request, so this module runs a small
thread pool over the item list, sharing the client's rate limiter and disk
cache.  Re-running after an interruption is cheap: cached items are read back
off disk instead of being re-fetched.

Timestamps come back as epoch milliseconds (UTC) and are stored tz-naive UTC,
because tz-aware columns round-trip awkwardly through HDF5.
"""

import concurrent.futures as futures
import sys

import pandas as pd

from . import api

# API path segment -> how far back it reaches
FILTERS = {
    'latest': 0,
    'last90d': 90,
    'sample': None,  # full history, decimated
    'all': None,  # full history, every point
}

HISTORY_COLUMNS = ['item_id', 'timestamp', 'price', 'volume']


def choose_filter(days):
    """Cheapest endpoint that still covers `days` of history (None = all of it)."""
    if days is not None and days <= FILTERS['last90d']:
        return 'last90d'
    return 'all'


def history_url(item_id, filter_name, api_base=api.API_BASE, game='rs'):
    return f'{api_base}/exchange/history/{game}/{filter_name}?id={item_id}'


def parse_history(raw, item_id):
    """Turn one API response into a tidy frame; empty frame if there is none."""
    series = None
    if isinstance(raw, dict):
        for value in raw.values():
            if isinstance(value, list):
                series = value
                break
    elif isinstance(raw, list):
        series = raw

    if not series:
        return pd.DataFrame(columns=HISTORY_COLUMNS)

    frame = pd.DataFrame(series)
    if 'timestamp' not in frame.columns or 'price' not in frame.columns:
        return pd.DataFrame(columns=HISTORY_COLUMNS)

    # The API sends epoch milliseconds; some endpoints send ISO strings.
    stamps = frame['timestamp']
    if pd.api.types.is_numeric_dtype(stamps):
        frame['timestamp'] = pd.to_datetime(stamps, unit='ms', utc=True).dt.tz_localize(None)
    else:
        frame['timestamp'] = pd.to_datetime(stamps, utc=True, format='mixed').dt.tz_localize(None)

    frame['item_id'] = int(item_id)
    frame['price'] = pd.to_numeric(frame['price'], errors='coerce')
    if 'volume' in frame.columns:
        frame['volume'] = pd.to_numeric(frame['volume'], errors='coerce')
    else:
        frame['volume'] = pd.NA

    frame = frame[HISTORY_COLUMNS].dropna(subset=['timestamp', 'price'])
    return frame.sort_values('timestamp').reset_index(drop=True)


def fetch_item_history(item_id, client, days=None, filter_name=None, api_base=api.API_BASE):
    """History for a single item, trimmed to the last `days`."""
    filter_name = filter_name or choose_filter(days)
    raw = client.get_json(history_url(item_id, filter_name, api_base))
    frame = parse_history(raw, item_id)
    return trim_to_window(frame, days)


def trim_to_window(frame, days, end=None):
    """Keep only rows within `days` of `end` (default: the frame's last point)."""
    if days is None or frame.empty:
        return frame
    end = end or frame['timestamp'].max()
    start = end - pd.Timedelta(days=days)
    return frame[frame['timestamp'] >= start].reset_index(drop=True)


def load_history(
    item_ids,
    client=None,
    days=None,
    filter_name=None,
    api_base=api.API_BASE,
    max_workers=4,
    progress=True,
):
    """Fetch history for many items and concatenate into one long frame.

    Returns
    -------
    frame : DataFrame with columns [item_id, timestamp, price, volume]
    failed : dict of {item_id: error string} for items that never came back
    """
    client = client or api.Client()
    item_ids = [int(i) for i in item_ids]
    frames = []
    failed = {}

    def work(item_id):
        return item_id, fetch_item_history(item_id, client, days, filter_name, api_base)

    with futures.ThreadPoolExecutor(max_workers=max_workers) as pool:
        pending = [pool.submit(work, item_id) for item_id in item_ids]
        for count, future in enumerate(futures.as_completed(pending), start=1):
            try:
                item_id, frame = future.result()
                if not frame.empty:
                    frames.append(frame)
            except (api.ApiError, ValueError) as error:
                failed[item_id] = str(error)
            if progress and (count % 25 == 0 or count == len(pending)):
                print(
                    f'  history {count}/{len(pending)} items'
                    f' ({len(frames)} with data, {len(failed)} failed)',
                    file=sys.stderr,
                )

    if not frames:
        return pd.DataFrame(columns=HISTORY_COLUMNS), failed

    history = pd.concat(frames, ignore_index=True)
    history = history.sort_values(['item_id', 'timestamp']).reset_index(drop=True)
    return history, failed


def to_wide(history, column='price'):
    """Pivot the long frame to timestamp x item_id, one cell per item per day."""
    if history.empty:
        return pd.DataFrame()
    wide = history.pivot_table(index='timestamp', columns='item_id', values=column, aggfunc='last')
    wide.columns = [str(c) for c in wide.columns]  # HDF5 wants string column names
    return wide.sort_index()
