"""Write every frame into one HDF5 file (and read it back).

Layout of the output file:

    /items                 item catalogue, indexed by item_id
    /history               long frame: item_id, timestamp, price, volume
    /alchemy               history joined to alch values, with profit and roi
    /latest                newest row per item, ranked by profit
    /wide/price            timestamp x item_id price matrix
    /wide/volume           timestamp x item_id volume matrix
    /summary/<window>      per-item stats for 1y, 6m, 3m, 1m, 1w, 1d

Frames are stored in `table` format with `item_id` and `timestamp` as data
columns, so you can query slices without loading everything:

    pd.read_hdf('alchemy.h5', 'alchemy', where='item_id == 561')
"""

import datetime

import pandas as pd

DEFAULT_PATH = 'rs_alchemy.h5'
_MIN_ITEMSIZE = {'name': 128, 'examine': 512}


def _hdf_safe(frame):
    """Cast pandas extension dtypes that PyTables cannot store natively."""
    out = frame.copy()
    for column in out.columns:
        dtype = out[column].dtype
        if isinstance(dtype, pd.BooleanDtype):
            out[column] = out[column].fillna(False).astype(bool)
        elif isinstance(dtype, (pd.Int64Dtype, pd.Int32Dtype, pd.Float64Dtype)):
            out[column] = out[column].astype('float64')
        elif isinstance(dtype, pd.StringDtype):
            out[column] = out[column].fillna('').astype(object)
    if isinstance(out.index.dtype, (pd.Int64Dtype, pd.Int32Dtype)):
        out.index = out.index.astype('int64')
    return out


def _itemsize(frame):
    return {k: v for k, v in _MIN_ITEMSIZE.items() if k in frame.columns}


def write_hdf5(
    path=DEFAULT_PATH,
    items=None,
    history=None,
    alchemy=None,
    latest=None,
    summaries=None,
    wide=None,
    attrs=None,
    complevel=5,
):
    """Write the whole scrape to `path`, overwriting any existing file."""
    summaries = summaries or {}
    wide = wide or {}

    written = []

    with pd.HDFStore(path, mode='w', complevel=complevel, complib='blosc') as store:
        def put(key, frame, data_columns=None):
            if frame is None or len(frame) == 0:
                return
            frame = _hdf_safe(frame)
            store.put(
                key,
                frame,
                format='table',
                data_columns=data_columns or True,
                min_itemsize=_itemsize(frame) or None,
            )
            written.append(key)

        put('items', items, data_columns=['name', 'highalch', 'members'])
        put('history', history, data_columns=['item_id', 'timestamp'])
        put('alchemy', alchemy, data_columns=['item_id', 'timestamp', 'alch_profit'])
        put('latest', latest, data_columns=['item_id', 'name', 'alch_profit'])

        for name, frame in summaries.items():
            put(f'summary/{name}', frame, data_columns=['name', 'profit_last'])

        # Wide matrices have one column per item id -- too many for a table.
        for name, frame in wide.items():
            if frame is not None and len(frame):
                store.put(f'wide/{name}', _hdf_safe(frame), format='fixed')

        metadata = {
            'created_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
            'source': 'https://prices.runescape.wiki/rs/alchemy',
        }
        metadata.update(attrs or {})
        if written:
            # Attach the run's provenance to the first table we wrote; read it
            # back with store.get_storer(key).attrs.scrape
            store.get_storer(written[0]).attrs.scrape = metadata

    return path


def read_hdf5(path=DEFAULT_PATH):
    """Read every key back into a {key: DataFrame} dict."""
    with pd.HDFStore(path, mode='r') as store:
        return {key.lstrip('/'): store[key] for key in store.keys()}


def describe(path=DEFAULT_PATH):
    """One line per key: shape and column names, for a quick sanity check."""
    lines = []
    with pd.HDFStore(path, mode='r') as store:
        for key in store.keys():
            frame = store[key]
            lines.append(f'{key:24s} {frame.shape[0]:>8,} x {frame.shape[1]:<4} ')
    return '\n'.join(lines)
