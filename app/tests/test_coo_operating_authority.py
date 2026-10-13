#!/usr/bin/env python3
"""Contracts for the COO's autonomous operating-decision authority."""

import json
from pathlib import Path


PROJECT = Path(__file__).resolve().parents[2]
COO_CONFIG = PROJECT / ".forkmesh" / "teammates" / "coo.json"
PROMPT_BUILDER = PROJECT / "desktop" / "src" / "MainWindowTeammates.cpp"


def _load_without_duplicate_keys(path):
    def unique_object(pairs):
        result = {}
        for key, value in pairs:
            assert key not in result, f"duplicate JSON key {key!r} in {path}"
            result[key] = value
        return result

    return json.loads(
        path.read_text(encoding="utf-8"),
        object_pairs_hook=unique_object,
    )


def test_coo_decides_routine_operational_tradeoffs_without_owner_input():
    coo = _load_without_duplicate_keys(COO_CONFIG)

    assert coo["id"] == "coo"
    assert coo["enabled"] is True
    assert "Make routine operational decisions yourself" in coo["instructions"]
    assert "without asking the owner or CEO to choose" in coo["instructions"]
    assert "genuinely unavailable material fact" in coo["instructions"]
    assert "Never merge, release, spend funds" in coo["instructions"]


def test_prompt_builder_reinforces_coo_operational_decision_authority():
    prompt_builder = PROMPT_BUILDER.read_text(encoding="utf-8")

    assert 'teammate.id == QLatin1String("coo")' in prompt_builder
    assert "COO decision authority" in prompt_builder
    assert "not use a user-input request to resolve a routine operational " in prompt_builder
    assert "choice; record the decision and rationale" in prompt_builder
    assert "one evidence-backed recommendation" in prompt_builder
