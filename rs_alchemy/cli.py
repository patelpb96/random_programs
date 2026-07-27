"""Command line driver.

    python -m rs_alchemy --out rs_alchemy.h5

Fetches the item catalogue, pulls the full daily Grand Exchange history for
every item (the same data behind /rs/all-items), computes high-alchemy profit
(/rs/alchemy), summarises every timeframe from the whole history down to the
last day, and writes the lot to one HDF5 file.

Use --days to trim to a shorter window and --alchable-only to skip items that
cannot be alched.
"""

import argparse
import sys

import pandas as pd

from . import alchemy, api, discover, export, history, items as items_module, store


def build_parser():
    parser = argparse.ArgumentParser(prog='rs_alchemy', description=__doc__)
    parser.add_argument('--out', default=store.DEFAULT_PATH, help='output HDF5 path')
    parser.add_argument(
        '--days',
        type=int,
        help='trim history to the last N days (default: keep everything the API has)',
    )
    parser.add_argument(
        '--filter',
        dest='filter_name',
        choices=sorted(history.FILTERS),
        help='force an API history endpoint instead of choosing one from --days',
    )
    parser.add_argument('--api-base', default=api.API_BASE, help='exchange API origin')
    parser.add_argument('--items-url', default=items_module.ITEMS_URL, help='item dump URL')
    parser.add_argument('--items-file', help='read the item dump from disk instead of the network')
    parser.add_argument('--cache-dir', default='cache', help='response cache directory')
    parser.add_argument('--no-cache', action='store_true', help='disable the response cache')
    parser.add_argument('--workers', type=int, default=4, help='concurrent history requests')
    parser.add_argument(
        '--min-interval',
        type=float,
        default=0.35,
        help='minimum seconds between requests (be kind to the wiki)',
    )
    parser.add_argument(
        '--min-alch', type=int, default=0, help='skip items whose high alch value is below this'
    )
    parser.add_argument(
        '--alchable-only',
        action='store_true',
        help='only fetch items with a nonzero high alch value (implies --min-alch 1)',
    )
    parser.add_argument(
        '--max-items', type=int, help='only fetch this many items (useful for a smoke test)'
    )
    parser.add_argument('--item-ids', help='comma-separated item ids to fetch instead of all')
    parser.add_argument(
        '--nature-price',
        type=float,
        help='flat nature rune price; default is the rune\'s own daily history',
    )
    parser.add_argument(
        '--export-dir',
        help='also write the JSON bundle the docs/ site reads (e.g. docs/data)',
    )
    parser.add_argument(
        '--no-series',
        action='store_true',
        help='skip the per-item series files when exporting (much smaller, no charts)',
    )
    parser.add_argument(
        '--max-series-points',
        type=int,
        default=1500,
        help='thin each exported series to at most this many points',
    )
    parser.add_argument(
        '--discover',
        action='store_true',
        help='print the data URLs referenced by the live alchemy page, then exit',
    )
    parser.add_argument('--user-agent', default=api.DEFAULT_USER_AGENT)
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)

    client = api.Client(
        cache_dir=None if args.no_cache else args.cache_dir,
        min_interval=args.min_interval,
        user_agent=args.user_agent,
    )

    if args.discover:
        for url in discover.discover_endpoints(client):
            print(url)
        return 0

    print('fetching item catalogue ...', file=sys.stderr)
    catalogue = items_module.load_items(client, url=args.items_url, path=args.items_file)
    print(f'  {len(catalogue):,} items', file=sys.stderr)

    if args.item_ids:
        wanted = [int(part) for part in args.item_ids.split(',') if part.strip()]
        selected = catalogue.reindex([i for i in wanted if i in catalogue.index])
    else:
        min_alch = max(args.min_alch, 1) if args.alchable_only else args.min_alch
        selected = items_module.tradeable_alchables(catalogue, min_alch)
        if args.max_items:
            selected = selected.head(args.max_items)

    item_ids = list(selected.index)
    if alchemy.NATURE_RUNE_ID not in item_ids and args.nature_price is None:
        item_ids.append(alchemy.NATURE_RUNE_ID)  # needed for the per-cast rune cost

    span = f'{args.days}d' if args.days else 'full'
    print(f'fetching {span} history for {len(item_ids):,} items ...', file=sys.stderr)
    prices, failed = history.load_history(
        item_ids,
        client=client,
        days=args.days,
        filter_name=args.filter_name,
        api_base=args.api_base,
        max_workers=args.workers,
    )
    if prices.empty:
        print('no price history returned -- check --api-base or run --discover', file=sys.stderr)
        return 1
    print(
        f'  {len(prices):,} datapoints for {prices["item_id"].nunique():,} items'
        f' ({len(failed)} items failed)',
        file=sys.stderr,
    )

    alch = alchemy.build_alchemy_table(
        prices, catalogue, nature_price=args.nature_price
    )
    latest = alchemy.latest_snapshot(alch)
    summaries = alchemy.summarize_windows(alch)
    wide = {
        'price': history.to_wide(prices, 'price'),
        'volume': history.to_wide(prices, 'volume'),
    }

    store.write_hdf5(
        args.out,
        items=catalogue,
        history=prices,
        alchemy=alch,
        latest=latest,
        summaries=summaries,
        wide=wide,
        attrs={
            'days': args.days if args.days else 'all',
            'api_base': args.api_base,
            'items_url': args.items_url,
            'n_items_requested': len(item_ids),
            'n_items_with_data': int(prices['item_id'].nunique()),
            'n_failed': len(failed),
            'first_timestamp': str(prices['timestamp'].min()),
            'last_timestamp': str(prices['timestamp'].max()),
        },
    )

    if args.export_dir:
        manifest = export.export_site_data(
            args.export_dir,
            latest=latest,
            summaries=summaries,
            history=prices,
            with_series=not args.no_series,
            max_series_points=args.max_series_points,
            attrs={'days': args.days if args.days else 'all'},
        )
        total = sum(manifest['bytes'].values())
        print(
            f'exported {len(manifest["bytes"])} JSON tables'
            f' ({total / 1e6:.1f} MB) to {args.export_dir}',
            file=sys.stderr,
        )

    print(f'\nwrote {args.out}', file=sys.stderr)
    print(store.describe(args.out), file=sys.stderr)

    with pd.option_context('display.width', 140, 'display.max_columns', 20):
        print('\ntop 15 by current alch profit:')
        print(
            latest.head(15)[
                ['item_id', 'name', 'price', 'highalch', 'alch_profit', 'alch_roi', 'volume']
            ].to_string(index=False)
        )
    return 0


if __name__ == '__main__':
    sys.exit(main())
