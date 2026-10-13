#!/usr/bin/env python3
"""Contracts for the retired anonymous Market ratings endpoint.

Ratings now belong to signed-in, email-verified accounts
(app/src/marketplace_reviews.py, test_marketplace_reviews.py). The anonymous
per-browser table keeps its historical rows readable, but it takes no writes.
"""

import re
from pathlib import Path


APP = Path(__file__).resolve().parents[1]
ENTRY = APP / "src" / "entry.py"
RUNTIME = APP / "src" / "app_runtime.py"
MARKET_PAGE = APP.parent / "www" / "public" / "market.html"

ENTRY_SOURCE = ENTRY.read_text(encoding="utf-8")
RUNTIME_SOURCE = RUNTIME.read_text(encoding="utf-8")
MARKET_SOURCE = MARKET_PAGE.read_text(encoding="utf-8")


def _handler_source():
    start = ENTRY_SOURCE.index("async def package_ratings_handler(")
    return ENTRY_SOURCE[start:ENTRY_SOURCE.index("\nasync def ", start + 10)]


def test_anonymous_rating_writes_are_retired():
    handler = _handler_source()
    assert "status=410" in handler
    assert '"account_rating_required"' in handler
    assert "INSERT INTO package_ratings" not in ENTRY_SOURCE
    assert "_package_rating_event" not in ENTRY_SOURCE
    assert "MAX_PACKAGE_RATING_ROWS" not in ENTRY_SOURCE


def test_legacy_aggregates_stay_readable():
    handler = _handler_source()
    assert 'if method == "GET":' in handler
    assert "await package_rating_counts(env)" in handler
    assert '"/api/market/ratings", "/api/market/ratings/"' in RUNTIME_SOURCE
    assert "return await package_ratings_handler(self.env, request)" in RUNTIME_SOURCE


def test_market_page_reads_account_ratings_and_posts_no_anonymous_votes():
    assert "data-market-rating" in MARKET_SOURCE
    assert 'fetch("/api/market/reviews/summary"' in MARKET_SOURCE
    assert not re.search(r'fetch\("/api/market/ratings"', MARKET_SOURCE)
    assert "forkmesh.market.rating.run" not in MARKET_SOURCE
