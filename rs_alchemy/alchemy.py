"""High alchemy economics: join prices to alch values and compute profit.

Casting High Level Alchemy on an item consumes one nature rune and pays out
60% of the item's shop value, so per cast:

    profit = high_alch_value - grand_exchange_price - nature_rune_price

`roi` is that profit over the capital risked (item + rune).  `profit_per_limit`
scales it by the 4-hour Grand Exchange buy limit, which is what actually caps
how much money an alch flip can make.
"""

import pandas as pd

NATURE_RUNE_ID = 561

# name -> length (None = every datapoint we have), longest first.  Names are
# valid Python identifiers because they become HDF5 node names.
WINDOWS = {
    'all': None,
    'last_1y': pd.Timedelta(days=365),
    'last_6m': pd.Timedelta(days=182),
    'last_3m': pd.Timedelta(days=91),
    'last_1m': pd.Timedelta(days=30),
    'last_1w': pd.Timedelta(days=7),
    'last_1d': pd.Timedelta(days=1),
}

ALCH_COLUMNS = [
    'item_id',
    'name',
    'timestamp',
    'price',
    'volume',
    'highalch',
    'lowalch',
    'limit',
    'members',
    'nature_price',
    'alch_profit',
    'alch_roi',
    'profit_per_limit',
]


def nature_rune_series(history, nature_id=NATURE_RUNE_ID):
    """Daily nature rune price, indexed by timestamp."""
    runes = history[history['item_id'] == nature_id]
    if runes.empty:
        return pd.Series(dtype='float64', name='nature_price')
    series = runes.set_index('timestamp')['price'].sort_index()
    series = series[~series.index.duplicated(keep='last')]
    series.name = 'nature_price'
    return series


def build_alchemy_table(history, items, nature_id=NATURE_RUNE_ID, nature_price=None):
    """Long frame of per-item, per-day alchemy economics.

    Parameters
    ----------
    history : DataFrame from `rs_alchemy.history.load_history`
    items : DataFrame from `rs_alchemy.items.load_items`
    nature_price : float or None
        Flat nature rune price.  If None, the rune's own daily history is used
        (as-of joined, so a day with no rune datapoint takes the last known one).
    """
    if history.empty:
        return pd.DataFrame(columns=ALCH_COLUMNS)

    frame = history.copy()

    if nature_price is None:
        runes = nature_rune_series(history, nature_id)
        if runes.empty:
            raise ValueError(
                f'no history for nature runes (item {nature_id}); '
                'pass nature_price=<gp> to use a flat price instead'
            )
        frame = frame.sort_values('timestamp')
        frame = pd.merge_asof(
            frame,
            runes.reset_index().rename(columns={'timestamp': 'timestamp'}),
            on='timestamp',
            direction='backward',
        )
        # Days before the rune's first datapoint fall back to its earliest price.
        frame['nature_price'] = frame['nature_price'].fillna(runes.iloc[0])
    else:
        frame['nature_price'] = float(nature_price)

    meta = items[['name', 'highalch', 'lowalch', 'limit', 'members']]
    frame = frame.join(meta, on='item_id')

    frame['highalch'] = pd.to_numeric(frame['highalch'], errors='coerce')
    frame['alch_profit'] = frame['highalch'] - frame['price'] - frame['nature_price']
    cost = frame['price'] + frame['nature_price']
    frame['alch_roi'] = frame['alch_profit'] / cost.where(cost > 0)
    frame['profit_per_limit'] = frame['alch_profit'] * pd.to_numeric(
        frame['limit'], errors='coerce'
    )

    frame = frame.reindex(columns=ALCH_COLUMNS)
    return frame.sort_values(['item_id', 'timestamp']).reset_index(drop=True)


def latest_snapshot(alch):
    """Most recent row per item, ranked by profit -- the alchemy page's table."""
    if alch.empty:
        return alch
    latest = alch.sort_values('timestamp').groupby('item_id', as_index=False).last()
    return latest.sort_values('alch_profit', ascending=False).reset_index(drop=True)


def window_slice(alch, length, end=None):
    """Rows within `length` of `end` (default: newest timestamp in the frame).

    `length=None` means the whole history.
    """
    if alch.empty or length is None:
        return alch
    end = end or alch['timestamp'].max()
    return alch[alch['timestamp'] > end - length]


def summarize_window(alch, length, end=None):
    """Per-item summary over one window: price/profit stats and traded volume."""
    chunk = window_slice(alch, length, end)
    if chunk.empty:
        return pd.DataFrame()

    grouped = chunk.groupby('item_id')
    summary = pd.DataFrame(
        {
            'name': grouped['name'].last(),
            'highalch': grouped['highalch'].last(),
            'limit': grouped['limit'].last(),
            'members': grouped['members'].last(),
            'n_days': grouped['price'].size(),
            'price_first': grouped['price'].first(),
            'price_last': grouped['price'].last(),
            'price_min': grouped['price'].min(),
            'price_max': grouped['price'].max(),
            'price_mean': grouped['price'].mean(),
            'volume_mean': grouped['volume'].mean(),
            'volume_total': grouped['volume'].sum(min_count=1),
            'profit_last': grouped['alch_profit'].last(),
            'profit_mean': grouped['alch_profit'].mean(),
            'profit_max': grouped['alch_profit'].max(),
            'roi_mean': grouped['alch_roi'].mean(),
            'days_profitable': grouped['alch_profit'].apply(lambda s: int((s > 0).sum())),
        }
    )
    summary['price_change'] = summary['price_last'] - summary['price_first']
    summary['price_change_frac'] = summary['price_change'] / summary['price_first'].where(
        summary['price_first'] > 0
    )
    return summary.sort_values('profit_last', ascending=False)


def summarize_windows(alch, windows=None, end=None):
    """{window name: summary frame} for every window in `windows`."""
    windows = windows or WINDOWS
    end = end or (alch['timestamp'].max() if not alch.empty else None)
    return {name: summarize_window(alch, length, end) for name, length in windows.items()}
