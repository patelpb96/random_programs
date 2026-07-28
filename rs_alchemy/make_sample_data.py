"""Generate a synthetic dataset and run it through the real export path.

This exists so the static site has something to render before anyone runs the
scraper for real, and so the export code is exercised end to end without
touching the network.  The manifest it writes is flagged `sample: true`, which
makes the page show a "SAMPLE DATA" banner -- these are made-up numbers, not
Grand Exchange prices.

    python -m rs_alchemy.make_sample_data --out ../patelpb96.github.io/public/rs-alchemy/data
"""

import argparse
import random

import pandas as pd

from . import alchemy, export, items as items_module

MATERIALS = ['Bronze', 'Iron', 'Steel', 'Mithril', 'Adamant', 'Rune', 'Dragon']
FORMS = [
    'platebody',
    'platelegs',
    'plateskirt',
    'full helm',
    'med helm',
    'kiteshield',
    'sq shield',
    'longsword',
    'battleaxe',
    'scimitar',
    'warhammer',
    'dagger',
    'mace',
    'claws',
    'halberd',
    'boots',
    'gauntlets',
    'chainbody',
]
EXTRAS = [
    'Nature rune',
    'Cannonball',
    'Yew logs',
    'Magic logs',
    'Shark',
    'Rune essence',
    'Dragon bones',
    'Ranarr weed',
    'Snapdragon',
    'Air battlestaff',
    'Water battlestaff',
    'Amulet of glory',
    'Ring of life',
    'Rune arrow',
    'Onyx bolt tips',
    'Saradomin brew',
    'Super restore',
    'Prayer potion',
    'Blue dragonhide body',
    'Green dragonhide chaps',
    'Yew shortbow',
    'Magic longbow',
    'Mystic robe top',
    'Infinity boots',
    'Bandos chestplate',
    'Armadyl helmet',
]


def build_items(n_items, seed):
    rng = random.Random(seed)
    records = [
        {
            'id': 561,
            'name': 'Nature rune',
            'value': 180,
            'limit': 25000,
            'members': False,
            'examine': 'Used for Alchemy and Enchanting spells.',
        }
    ]

    item_id = 1000
    for material_index, material in enumerate(MATERIALS):
        for form in FORMS:
            item_id += rng.randint(2, 9)
            value = int(200 * (2.4**material_index) * rng.uniform(0.7, 1.5))
            records.append(
                {
                    'id': item_id,
                    'name': f'{material} {form}',
                    'value': value,
                    'highalch': int(value * 0.6),
                    'lowalch': int(value * 0.4),
                    'limit': rng.choice([10, 25, 100, 250]),
                    'members': material_index >= 4,
                    'examine': f'A {form} made of {material.lower()}.',
                }
            )
    for name in EXTRAS[1:]:
        item_id += rng.randint(2, 9)
        value = rng.randint(50, 4000)
        records.append(
            {
                'id': item_id,
                'name': name,
                'value': value,
                'highalch': int(value * 0.6),
                'limit': 10000,
                'members': rng.random() < 0.5,
            }
        )

    return items_module.parse_items(records[:n_items] if n_items else records)


def build_history(catalogue, days, seed):
    """A random walk per item, anchored near its alch value so some items alch."""
    rng = random.Random(seed + 1)
    end = pd.Timestamp.now('UTC').normalize().tz_localize(None)
    dates = pd.date_range(end - pd.Timedelta(days=days - 1), end, freq='D')

    frames = []
    for item_id, item in catalogue.iterrows():
        highalch = float(item['highalch'] or 100)
        price = highalch * rng.uniform(0.72, 1.15)
        drift = rng.uniform(-0.0009, 0.0012)
        vol = rng.uniform(0.008, 0.05)
        base_volume = rng.choice([12, 90, 400, 2500, 20000])

        prices, volumes = [], []
        for _ in dates:
            price = max(1.0, price * (1 + drift + rng.gauss(0, vol)))
            prices.append(round(price))
            volumes.append(max(0, int(rng.gauss(base_volume, base_volume * 0.35))))

        frames.append(
            pd.DataFrame(
                {
                    'item_id': int(item_id),
                    'timestamp': dates,
                    'price': prices,
                    'volume': volumes,
                }
            )
        )

    history = pd.concat(frames, ignore_index=True)
    return history.sort_values(['item_id', 'timestamp']).reset_index(drop=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', required=True, help='directory to write the JSON bundle into')
    parser.add_argument('--days', type=int, default=420)
    parser.add_argument('--items', type=int, default=0, help='0 = all generated items')
    parser.add_argument('--seed', type=int, default=7)
    args = parser.parse_args(argv)

    catalogue = build_items(args.items, args.seed)
    history = build_history(catalogue, args.days, args.seed)
    alch = alchemy.build_alchemy_table(history, catalogue)

    manifest = export.export_site_data(
        args.out,
        latest=alchemy.latest_snapshot(alch),
        summaries=alchemy.summarize_windows(alch),
        history=history,
        attrs={'days': args.days, 'note': 'synthetic data for layout testing'},
        sample=True,
    )
    print(
        f'wrote {manifest["n_items"]} items x {args.days} days to {args.out}'
        f' ({sum(manifest["bytes"].values()) / 1e6:.2f} MB)'
    )
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
