"""The official rules of the ForkMesh September Revenue Contest.

/hackathon/rules is the document that applies when any other page disagrees,
so every term that decides who gets paid has to be stated on it, and nothing
may be left pending on a page that names cash amounts.
"""

import re
from pathlib import Path

RULES = (Path(__file__).resolve().parents[1] / "public" / "hackathon"
         / "rules.html").read_text(encoding="utf-8")
TEXT = " ".join(re.sub(r"<[^>]+>", "", RULES).lower().split())


def test_rules_state_the_contest_window_in_utc():
    assert "15 september 2026, 00:00 utc" in TEXT
    assert "15 october 2026, 23:59 utc" in TEXT


def test_rules_state_the_commission_and_the_prizes():
    assert "50% of each eligible customer’s first month" in TEXT
    for plan, commission in (("$5", "$2.50"), ("$10", "$5"), ("$50", "$25"),
                             ("$100", "$50")):
        assert f"{plan} plan: {commission}" in TEXT
    assert "$25 plan" not in TEXT
    assert "the 50% commission rate did not change" in TEXT
    assert "Last updated 6 October 2026" in RULES
    assert "not number of signups" in TEXT
    for place, prize in (("1st", "$100"), ("2nd", "$50"), ("3rd", "$25")):
        assert f"{place}: {prize}" in TEXT
    assert "prizes are paid in addition to commissions" in TEXT


def test_rules_state_the_terms_that_keep_the_contest_fair():
    assert "maximum of 10 companies" in TEXT
    assert "posted publicly before outreach" in TEXT
    assert "expire after 7 days" in TEXT
    assert "no self-referrals" in TEXT
    assert "refunds, chargebacks" in TEXT
    assert "30 days" in TEXT


def test_rules_leave_nothing_pending():
    assert "to be confirmed" not in RULES
    assert 'class="tbd"' not in RULES
