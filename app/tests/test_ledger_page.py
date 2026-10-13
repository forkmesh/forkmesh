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


def test_ledger_opens_on_the_page_kit():
    assert_kit_hero(read(APP / "ledger.html"), "ledger.html")


def test_ledger_rows_are_compact_and_open_a_details_panel():
    html = read(APP / "ledger.html")
    script = read(APP / "ledger.js")
    assert html.index("data-ledger-wallets") < html.index("data-ledger-expenses-total")
    assert 'grid-template-areas: "date record status amount receipt toggle";' in html
    assert "ledger-integrity-dot" not in html
    assert "ledger-kicker" not in html
    assert 'id="how-to-read"' in html
    for marker in (
        "function makeDetails(entry, detailsId, noteShown)",
        'toggle.setAttribute("aria-controls", detailsId);',
        "details.hidden = !open;",
        'row.classList.toggle("is-open", open);',
        "function makeParties(entry)",
        "function makeSubjects(entry)",
        'if (event.target.closest("a, button")) return;',
    ):
        assert marker in script, marker
    assert "function makeLinks(entry)" not in script


def test_expected_expenses_have_a_tab_and_a_reminder_outside_the_totals():
    html = read(APP / "ledger.html")
    script = read(APP / "ledger.js")
    assert 'data-ledger-view="expected"' in html
    assert "data-ledger-expected-reminder hidden" in html
    assert "data-ledger-expected-list" in html
    for marker in (
        '"expected-expense": "Expected expense"',
        "function renderExpectedReminder()",
        "payload.expectedExpenses",
        'row.dataset.expectedExpense = String(isExpectedExpense(entry));',
    ):
        assert marker in script, marker
    expense_rule = script[script.index("function isAccountingExpense"):]
    assert "expected-expense" not in expense_rule[:expense_rule.index("}\n")]
