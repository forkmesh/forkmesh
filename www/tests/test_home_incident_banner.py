"""Regression contract for the banner-free static homepage."""

from pathlib import Path


PAGE = (
    Path(__file__).resolve().parents[1] / "public" / "index.html"
).read_text(encoding="utf-8")


def test_homepage_has_no_incident_banner_or_availability_probe():
    assert "app-incident-banner" not in PAGE
    assert "data-app-incident" not in PAGE
    assert "/api/home-availability" not in PAGE
    assert "has-app-incident" not in PAGE
