"""HTTP plumbing: polite rate limiting, retries, and an on-disk response cache.

The alchemy page at https://prices.runescape.wiki/rs/alchemy is a client-side
app; the numbers it renders come from the Weird Gloop exchange API (the same
group that hosts the wiki).  Endpoints used here:

    GET {API_BASE}/exchange/history/rs/latest?id=2|6|8|...
        -> {"2": {"id": "2", "timestamp": "...", "price": 123, "volume": 45}, ...}

    GET {API_BASE}/exchange/history/rs/last90d?id=2
    GET {API_BASE}/exchange/history/rs/sample?id=2
    GET {API_BASE}/exchange/history/rs/all?id=2
        -> {"<item key>": [{"timestamp": <ms>, "price": 123, "volume": 45}, ...]}

If those paths ever move, `rs_alchemy.discover` re-reads them out of the live
page's JS bundle, and every entry point takes an `api_base` override.
"""

import json
import os
import random
import threading
import time
import urllib.parse

import requests

API_BASE = 'https://api.weirdgloop.org'
PAGE_URL = 'https://prices.runescape.wiki/rs/alchemy'

# The wiki asks for a descriptive User-Agent so they can contact you if a
# script misbehaves.  Override with the RS_ALCHEMY_USER_AGENT env var.
DEFAULT_USER_AGENT = os.environ.get(
    'RS_ALCHEMY_USER_AGENT',
    'rs_alchemy scraper - github.com/patelpb96/GizmoElementTracers - contact via github',
)


class ApiError(RuntimeError):
    """Raised when an endpoint keeps failing after all retries."""


class Client:
    """Small requests wrapper: rate limit + retry + optional disk cache.

    Parameters
    ----------
    cache_dir : str or None
        Directory for cached JSON responses.  None disables caching.
    min_interval : float
        Minimum seconds between requests (shared across threads).
    max_retries : int
        Attempts per URL before giving up.
    timeout : float
        Per-request socket timeout, seconds.
    """

    def __init__(
        self,
        cache_dir='cache',
        min_interval=0.35,
        max_retries=5,
        timeout=30.0,
        user_agent=DEFAULT_USER_AGENT,
        session=None,
    ):
        self.cache_dir = cache_dir
        self.min_interval = min_interval
        self.max_retries = max_retries
        self.timeout = timeout
        self.session = session or requests.Session()
        self.session.headers.update({'User-Agent': user_agent, 'Accept': 'application/json'})

        self._lock = threading.Lock()
        self._next_allowed = 0.0

        if cache_dir:
            os.makedirs(cache_dir, exist_ok=True)

    # -- cache ------------------------------------------------------------

    def _cache_path(self, url, params):
        key = url
        if params:
            key += '?' + urllib.parse.urlencode(sorted(params.items()))
        safe = ''.join(c if c.isalnum() or c in '-_.' else '_' for c in key)
        # Keep the tail: it carries the item id, which is the part that varies.
        return os.path.join(self.cache_dir, safe[-180:] + '.json')

    # -- rate limit -------------------------------------------------------

    def _throttle(self):
        with self._lock:
            now = time.monotonic()
            wait = self._next_allowed - now
            if wait > 0:
                time.sleep(wait)
                now = time.monotonic()
            self._next_allowed = now + self.min_interval

    # -- fetch ------------------------------------------------------------

    def get_json(self, url, params=None, use_cache=True):
        """GET `url` and parse JSON, with cache/retry/backoff."""
        path = self._cache_path(url, params) if (self.cache_dir and use_cache) else None

        if path and os.path.exists(path):
            with open(path) as f:
                try:
                    return json.load(f)
                except ValueError:
                    os.remove(path)  # truncated write from an interrupted run

        data = self._get_json_uncached(url, params)

        if path:
            tmp = path + '.tmp'
            with open(tmp, 'w') as f:
                json.dump(data, f)
            os.replace(tmp, path)

        return data

    def get_text(self, url, params=None):
        """GET `url` as text (used for the HTML page and its JS bundles)."""
        last_error = None
        for attempt in range(self.max_retries):
            self._throttle()
            try:
                response = self.session.get(url, params=params, timeout=self.timeout)
                if response.status_code == 429 or response.status_code >= 500:
                    raise requests.HTTPError(f'{response.status_code} from {url}')
                response.raise_for_status()
                return response.text
            except (requests.RequestException, ValueError) as error:
                last_error = error
                self._backoff(attempt)
        raise ApiError(f'GET {url} failed after {self.max_retries} attempts: {last_error}')

    def _get_json_uncached(self, url, params):
        last_error = None
        for attempt in range(self.max_retries):
            self._throttle()
            try:
                response = self.session.get(url, params=params, timeout=self.timeout)
                if response.status_code == 429 or response.status_code >= 500:
                    raise requests.HTTPError(f'{response.status_code} from {url}')
                response.raise_for_status()
                return response.json()
            except (requests.RequestException, ValueError) as error:
                last_error = error
                self._backoff(attempt)
        raise ApiError(f'GET {url} failed after {self.max_retries} attempts: {last_error}')

    def _backoff(self, attempt):
        delay = min(2.0**attempt, 30.0) * (0.5 + random.random())
        time.sleep(delay)
