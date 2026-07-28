"""Scraper for the RuneScape 3 alchemy price data behind
https://prices.runescape.wiki/rs/alchemy

Public entry points:
    rs_alchemy.api.Client        -- cached, rate-limited HTTP client
    rs_alchemy.items.load_items  -- item catalogue (name, high alch value, limit)
    rs_alchemy.history.load_history -- daily price/volume history per item
    rs_alchemy.alchemy.build_alchemy_table -- profit table
    rs_alchemy.store.write_hdf5  -- dump every frame to a single .h5
"""

__version__ = '0.1.0'
