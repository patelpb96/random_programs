"""Offline tests: synthetic API payloads, no network.

    python -m pytest rs_alchemy/tests -q
"""

import json
import os
import sys

import pandas as pd
import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from rs_alchemy import alchemy, history, items as items_module, store  # noqa: E402

ITEM_DUMP = {
    'Rune platebody': {
        'id': 1127,
        'name': 'Rune platebody',
        'value': 65000,
        'highalch': 39000,
        'lowalch': 26000,
        'limit': 10,
        'members': False,
        'examine': 'Provides excellent protection.',
    },
    'Nature rune': {
        'id': 561,
        'name': 'Nature rune',
        'value': 180,
        'limit': 25000,
        'members': 'false',
    },
    'Broken item': {'name': 'no id here'},
}


def _series(start_price, n_days=400, step=1, base_ms=1_600_000_000_000):
    day = 86_400_000
    return [
        {'timestamp': base_ms + i * day, 'price': start_price + i * step, 'volume': 100 + i}
        for i in range(n_days)
    ]


def test_parse_items_normalises_and_fills_alch_values():
    items = items_module.parse_items(ITEM_DUMP)

    assert list(items.index) == [561, 1127]
    assert items.loc[1127, 'highalch'] == 39000
    # Nature rune has no explicit alch values -> filled from 60%/40% of value.
    assert items.loc[561, 'highalch'] == 108
    assert items.loc[561, 'lowalch'] == 72
    assert items.loc[561, 'members'] is False or not items.loc[561, 'members']
    assert items.loc[1127, 'limit'] == 10


def test_parse_items_rejects_an_empty_dump():
    with pytest.raises(ValueError):
        items_module.parse_items([{'no': 'id'}])


def test_parse_history_epoch_ms_and_iso():
    frame = history.parse_history({'Rune platebody': _series(38000, n_days=3)}, 1127)
    assert list(frame.columns) == history.HISTORY_COLUMNS
    assert len(frame) == 3
    assert frame['item_id'].unique().tolist() == [1127]
    assert frame['timestamp'].is_monotonic_increasing

    iso = {'x': [{'timestamp': '2024-01-01T00:00:00Z', 'price': 5}]}
    assert history.parse_history(iso, 2)['timestamp'].iloc[0] == pd.Timestamp('2024-01-01')


def test_parse_history_handles_empty_payloads():
    assert history.parse_history({}, 1).empty
    assert history.parse_history({'x': []}, 1).empty
    assert history.parse_history({'x': [{'nope': 1}]}, 1).empty


def test_trim_to_window_keeps_only_the_last_year():
    frame = history.parse_history({'x': _series(100, n_days=400)}, 1127)
    trimmed = history.trim_to_window(frame, 365)
    span = trimmed['timestamp'].max() - trimmed['timestamp'].min()
    assert span <= pd.Timedelta(days=365)
    assert len(trimmed) == 366


def test_choose_filter():
    assert history.choose_filter(30) == 'last90d'
    assert history.choose_filter(365) == 'all'


def _fixture_frames():
    items = items_module.parse_items(ITEM_DUMP)
    prices = pd.concat(
        [
            history.parse_history({'a': _series(38000, n_days=400, step=5)}, 1127),
            history.parse_history({'b': _series(200, n_days=400, step=0)}, 561),
        ],
        ignore_index=True,
    )
    prices = prices.sort_values(['item_id', 'timestamp']).reset_index(drop=True)
    return items, prices


def test_build_alchemy_table_profit_math():
    items, prices = _fixture_frames()
    alch = alchemy.build_alchemy_table(prices, items)

    row = alch[(alch['item_id'] == 1127)].iloc[0]
    assert row['nature_price'] == 200
    assert row['alch_profit'] == 39000 - 38000 - 200
    assert row['alch_roi'] == pytest.approx(800 / (38000 + 200))
    assert row['profit_per_limit'] == 800 * 10
    assert row['name'] == 'Rune platebody'

    # Rising item price eats the margin, so late rows are unprofitable.
    assert alch[alch['item_id'] == 1127]['alch_profit'].iloc[-1] < 0


def test_build_alchemy_table_flat_nature_price():
    items, prices = _fixture_frames()
    alch = alchemy.build_alchemy_table(prices, items, nature_price=250)
    assert (alch['nature_price'] == 250).all()


def test_build_alchemy_table_requires_a_rune_price():
    items, prices = _fixture_frames()
    with pytest.raises(ValueError):
        alchemy.build_alchemy_table(prices[prices['item_id'] == 1127], items)


def test_summarize_windows_span_from_a_year_to_a_day():
    items, prices = _fixture_frames()
    alch = alchemy.build_alchemy_table(prices, items)
    summaries = alchemy.summarize_windows(alch)

    assert set(summaries) == set(alchemy.WINDOWS)
    assert summaries['last_1y'].loc[1127, 'n_days'] == 365
    assert summaries['last_1w'].loc[1127, 'n_days'] == 7
    assert summaries['last_1d'].loc[1127, 'n_days'] == 1
    assert summaries['last_1y'].loc[1127, 'price_max'] > summaries['last_1y'].loc[1127, 'price_min']
    # the margin closes partway through the year as the item's price climbs
    assert 0 < summaries['last_1y'].loc[1127, 'days_profitable'] < 365


def test_latest_snapshot_is_one_row_per_item_ranked_by_profit():
    items, prices = _fixture_frames()
    alch = alchemy.build_alchemy_table(prices, items)
    latest = alchemy.latest_snapshot(alch)

    assert len(latest) == alch['item_id'].nunique()
    assert latest['alch_profit'].is_monotonic_decreasing


def test_write_and_read_hdf5_round_trip(tmp_path):
    items, prices = _fixture_frames()
    alch = alchemy.build_alchemy_table(prices, items)
    path = str(tmp_path / 'out.h5')

    store.write_hdf5(
        path,
        items=items,
        history=prices,
        alchemy=alch,
        latest=alchemy.latest_snapshot(alch),
        summaries=alchemy.summarize_windows(alch),
        wide={'price': history.to_wide(prices, 'price')},
        attrs={'days': 365},
    )

    frames = store.read_hdf5(path)
    assert {'items', 'history', 'alchemy', 'latest', 'wide/price'} <= set(frames)
    assert len(frames['history']) == len(prices)
    assert frames['summary/last_1d'].loc[1127, 'n_days'] == 1

    # data columns make partial reads work
    runes = pd.read_hdf(path, 'alchemy', where='item_id == 561')
    assert (runes['item_id'] == 561).all()

    with pd.HDFStore(path, mode='r') as handle:
        assert handle.get_storer('items').attrs.scrape['days'] == 365


def test_cli_end_to_end_against_a_fake_api(tmp_path, monkeypatch):
    """Run the whole pipeline with the network stubbed out."""
    from rs_alchemy import api, cli

    def fake_get_json(self, url, params=None, use_cache=True):
        if 'exchange/history' in url:
            item_id = int(url.rsplit('id=', 1)[1])
            start = 200 if item_id == 561 else 38000
            step = 0 if item_id == 561 else 5
            return {str(item_id): _series(start, n_days=400, step=step)}
        return ITEM_DUMP

    monkeypatch.setattr(api.Client, 'get_json', fake_get_json)

    out = str(tmp_path / 'cli.h5')
    assert cli.main(['--out', out, '--cache-dir', str(tmp_path / 'cache')]) == 0

    frames = store.read_hdf5(out)
    assert set(frames['history']['item_id']) == {561, 1127}
    assert len(frames['history']) == 800  # full history, not trimmed
    assert frames['summary/all'].loc[1127, 'n_days'] == 400
    assert frames['summary/last_1y'].loc[1127, 'n_days'] == 365
    assert list(frames['wide/price'].columns) == ['561', '1127']

    # --days trims to the requested window
    out_year = str(tmp_path / 'year.h5')
    assert cli.main(['--out', out_year, '--days', '30', '--cache-dir', str(tmp_path / 'c2')]) == 0
    trimmed = store.read_hdf5(out_year)['history']
    assert trimmed['timestamp'].max() - trimmed['timestamp'].min() <= pd.Timedelta(days=30)


def test_export_site_data_bundle(tmp_path):
    """The JSON the static site reads: manifest, tables, per-item series."""
    from rs_alchemy import export

    items, prices = _fixture_frames()
    alch = alchemy.build_alchemy_table(prices, items)
    out = tmp_path / 'data'

    manifest = export.export_site_data(
        str(out),
        latest=alchemy.latest_snapshot(alch),
        summaries=alchemy.summarize_windows(alch),
        history=prices,
        max_series_points=100,
        sample=True,
    )

    assert manifest['sample'] is True
    assert manifest['n_items'] == 2
    assert set(manifest['windows']) == set(alchemy.WINDOWS)
    assert sorted(manifest['series_ids']) == [561, 1127]

    latest = json.loads((out / 'latest.json').read_text())
    assert latest['columns'][:2] == ['item_id', 'name']
    assert len(latest['rows']) == 2
    assert all(len(row) == len(latest['columns']) for row in latest['rows'])

    summary = json.loads((out / 'summary_last_1w.json').read_text())
    assert summary['columns'][0] == 'item_id'

    series = json.loads((out / 'series' / '1127.json').read_text())
    assert len(series['t']) == len(series['p']) == len(series['v']) <= 100
    assert series['t'] == sorted(series['t'])


def test_export_json_is_free_of_nan_and_numpy(tmp_path):
    """NaN is not valid JSON -- everything missing must land as null."""
    from rs_alchemy import export

    frame = pd.DataFrame(
        {
            'item_id': [1, 2],
            'name': ['a', 'b'],
            'price': [1.5, float('nan')],
            'members': [True, False],
            'timestamp': pd.to_datetime(['2024-01-01', '2024-01-02']),
        }
    )
    payload = export.frame_to_columnar(frame)
    text = json.dumps(payload)

    assert 'NaN' not in text
    assert payload['rows'][1][2] is None
    assert payload['rows'][0][4] == '2024-01-01'
    assert json.loads(text) == payload
