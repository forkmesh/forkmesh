#!/usr/bin/env python3
"""Cross-surface contract for the canonical product promise and proof path."""

from html import unescape
from pathlib import Path
import re

from _dashboard_bundle import assembled_dashboard_js


ROOT = Path(__file__).resolve().parents[2]
APP_PUBLIC = ROOT / "app" / "public"
WWW_PUBLIC = ROOT / "www" / "public"
PROMISE = (
    "ForkMesh lets developers collaborate on, mirror, and preserve source-code "
    "repositories across independent hosts—without relying on any single hosting provider."
)


def _visible_text(path: Path) -> str:
    source = path.read_text(encoding="utf-8")
    text = re.sub(r"<[^>]+>", " ", source)
    return " ".join(unescape(text).split())


def test_primary_surfaces_publish_the_same_product_promise():
    surfaces = {
        "homepage": WWW_PUBLIC / "index.html",
        "README": ROOT / "README.md",
        "onboarding": WWW_PUBLIC / "docs" / "onboarding" / "index.html",
        "press": WWW_PUBLIC / "press.html",
        "about": WWW_PUBLIC / "about.html",
        "proof documentation": WWW_PUBLIC / "docs" / "product-promise" / "index.html",
    }
    for label, path in surfaces.items():
        assert PROMISE in _visible_text(path), "%s changed the approved sentence" % label

    # Repository pages already carry the repository's own About copy; repeating
    # the platform promise there crowds out repository actions and metadata.
    assert PROMISE not in assembled_dashboard_js()
    assert PROMISE in (ROOT / ".forkmesh" / "info.json").read_text(encoding="utf-8")


def test_every_named_surface_links_to_the_working_proof_path():
    for relative in (
        "index.html",
        "press.html",
        "docs/onboarding/index.html",
    ):
        html = (WWW_PUBLIC / relative).read_text(encoding="utf-8")
        assert 'href="/docs/product-promise"' in html, relative

    dashboard = assembled_dashboard_js()
    assert 'data-repo-product-promise' not in dashboard
    assert 'recordProductAction?.("repository-clone-copied")' in dashboard

    docs_index = (WWW_PUBLIC / "docs" / "index.html").read_text(encoding="utf-8")
    source_index = (ROOT / "www" / "docs" / "README.md").read_text(encoding="utf-8")
    assert 'href="/docs/product-promise"' in docs_index
    assert "[Product promise](product-promise.md)" in source_index


def test_proof_uses_real_actions_and_documents_failure_and_recovery():
    html = _visible_text(WWW_PUBLIC / "docs" / "product-promise" / "index.html")
    markdown = (ROOT / "www" / "docs" / "product-promise.md").read_text(encoding="utf-8")

    for marker in (
        "01 / COLLABORATE",
        "02 / MIRROR",
        "03 / TAKE SOURCE OFFLINE",
        "04 / CLONE AGAIN",
        "Stop forkmesh-mirror-node on the managed source host",
        "git clone https://forkmesh.com/OWNER/REPOSITORY",
        "desktop remains online as a client and is not part of the serving path",
        "restart forkmesh-mirror-node",
        "signed latest release manifest is authoritative",
        "Linux x86_64, macOS (Apple silicon and Intel), and Windows x86_64",
        "Linux headless",
    ):
        assert marker in html

    for marker in (
        "No eligible mirror",
        "integrity-mismatched mirror",
        "All serving hosts offline",
        "Private repository",
        "Rollback is explicit",
        "home-proof-opened",
        "proof-clone-copied",
        "repository-clone-copied",
    ):
        assert marker in markdown

    public_html = (WWW_PUBLIC / "docs" / "product-promise" / "index.html").read_text(encoding="utf-8")
    for action in (
        'href="https://app.forkmesh.com/forkmesh/forkmesh/issues"',
        'href="https://app.forkmesh.com/dashboard/network"',
        'href="https://app.forkmesh.com/forkmesh/forkmesh/tree/main/www/docs/operations/desktop-control-node.md"',
        'href="https://app.forkmesh.com/forkmesh/forkmesh/tree/main/www/docs/operations/mirror-node.md"',
    ):
        assert action in public_html


def test_comprehension_gate_and_linked_evidence_are_explicit():
    guide = (ROOT / "www" / "docs" / "product-promise.md").read_text(encoding="utf-8")
    normalized = " ".join(guide.split())
    for marker in (
        "every five-person cohort",
        "maintainers, contributors, and operators",
        "A cohort passes at 4/5 or better",
        "Do not mark this gate complete with blank",
        "app/tests/test_installer_release.py",
        "app/tests/test_mirror_serving.py",
        "app/tests/test_https_mirror_routing_integration.py",
        "app/tests/test_mirror_service_recovery.py",
        "app/tests/test_product_promise.py",
        "app/tests/test_first_party_analytics.py",
    ):
        assert marker in normalized

    for relative in re.findall(r"`(app/tests/[^`]+\.py)`", guide):
        assert (ROOT / relative).is_file(), relative


def test_superseded_primary_headlines_do_not_return():
    primary = "\n".join(
        (WWW_PUBLIC / relative).read_text(encoding="utf-8")
        for relative in ("index.html", "press.html")
    )
    for old_positioning in (
        "Protect the code that matters from a single-host failure",
        "Coding Reimagined",
        "For Modern Teams",
        "local-first source-code collaboration network",
    ):
        assert old_positioning not in primary


def test_homepage_claim_audit_keeps_routing_and_custody_bounded():
    homepage = (WWW_PUBLIC / "index.html").read_text(encoding="utf-8")

    for supported in (
        "Health-aware routing",
        "network distance is not promised",
        "Operator-held custody",
        "The relay is not the canonical Git store",
    ):
        assert supported in homepage

    for superseded in (
        "Nearest-mirror routing",
        "Self-custodial, always",
        "ForkMesh never holds the only copy of your code",
        "65+ merged open-source contributions",
    ):
        assert superseded not in homepage
