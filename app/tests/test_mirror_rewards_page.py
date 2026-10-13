from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
APP = ROOT / "app" / "public"


def read(path):
    return path.read_text(encoding="utf-8")


def assert_kit_hero(html, name):
    assert '<body class="kit-page">' in html, name
    assert 'class="kit-badge"' in html, name
    assert 'class="kit-title"' in html, name
    assert 'class="eyebrow"' not in html, name


def test_mirror_rewards_page_says_the_program_is_not_live():
    html = read(APP / "mirror-payouts.html")
    assert_kit_hero(html, "mirror-payouts.html")
    assert '<span class="payouts-state">Not live yet</span>' in html
    assert "<strong>Mirror rewards are not active yet.</strong>" in html
    assert "auth-card" not in html
    assert "auth-page-theme.css" not in html
    assert '<div data-forkmesh-footer="standard"></div>' in html
    for hook in (
        'id="mp-node"',
        'id="calc-nodes"',
        'id="calc-reward"',
        'id="pool-network"',
        'id="pool-balance"',
        'id="pool-balance-checked"',
        'id="pool-transfers"',
        'id="pool-balance-refresh"',
    ):
        assert hook in html, hook
