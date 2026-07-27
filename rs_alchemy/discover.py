"""Read the live alchemy page and pull the API endpoints out of its JS bundle.

The page is a single-page app, so there is no server-rendered table to scrape;
the useful thing the HTML gives us is the set of URLs the app itself calls.
This module exists as a fallback (and as a diagnostic) in case the hard-coded
defaults in `rs_alchemy.api` drift.

    python -m rs_alchemy.discover
"""

import re
import urllib.parse

from . import api

# Any absolute URL, plus fetch()/axios() string literals that look like paths.
_URL_RE = re.compile(r'https?://[A-Za-z0-9._~:/?#\[\]@!$&\'()*+,;=%-]+')
_SCRIPT_RE = re.compile(r'<script[^>]+src=["\']([^"\']+)["\']', re.IGNORECASE)
_LINK_RE = re.compile(r'<link[^>]+href=["\']([^"\']+\.js)["\']', re.IGNORECASE)

INTERESTING = ('exchange', 'history', 'alchem', 'api', 'moduledata', 'chisel', 'weirdgloop')


def fetch_page_scripts(client=None, page_url=api.PAGE_URL):
    """Return {script_url: source} for every JS asset the page loads."""
    client = client or api.Client()
    html = client.get_text(page_url)

    sources = {page_url: html}
    for match in set(_SCRIPT_RE.findall(html)) | set(_LINK_RE.findall(html)):
        url = urllib.parse.urljoin(page_url, match)
        try:
            sources[url] = client.get_text(url)
        except api.ApiError:
            continue  # a missing chunk is not fatal; we still have the others
    return sources


def discover_endpoints(client=None, page_url=api.PAGE_URL):
    """Return the sorted set of candidate data URLs referenced by the page."""
    found = set()
    for source in fetch_page_scripts(client, page_url).values():
        for url in _URL_RE.findall(source):
            url = url.rstrip('\\"\'`,;)')
            if any(token in url.lower() for token in INTERESTING):
                found.add(url)
    return sorted(found)


def discover_api_base(client=None, page_url=api.PAGE_URL, default=api.API_BASE):
    """Best guess at the exchange-API origin the page talks to."""
    for url in discover_endpoints(client, page_url):
        if '/exchange/history/' in url:
            parts = urllib.parse.urlsplit(url)
            return f'{parts.scheme}://{parts.netloc}'
    return default


if __name__ == '__main__':
    for endpoint in discover_endpoints():
        print(endpoint)
