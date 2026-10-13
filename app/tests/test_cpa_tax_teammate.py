#!/usr/bin/env python3
"""Contracts for the Delaware corporate-tax readiness teammate."""

import json
from pathlib import Path


PROJECT = Path(__file__).resolve().parents[2]
CPA_CONFIG = PROJECT / ".forkmesh" / "teammates" / "cpa.json"


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


def test_cpa_is_a_delaware_corporate_tax_readiness_teammate():
    cpa = _load_without_duplicate_keys(CPA_CONFIG)

    assert cpa["id"] == "cpa"
    assert cpa["name"] == "CPA"
    assert cpa["enabled"] is True
    assert cpa["focus"] == "Delaware Corporate Tax Readiness"
    assert cpa["schedule"]["mode"] == "on-demand"
    assert cpa["schedule"]["active"] is False
    assert {"read-repositories", "external-tools", "manage-issues"} == set(
        cpa["permissions"]
    )
    assert "Delaware Franchise Tax Readiness" in cpa["skills"]
    assert "Federal Corporate Tax Preparation" in cpa["skills"]


def test_cpa_requires_primary_sources_and_professional_review():
    cpa = _load_without_duplicate_keys(CPA_CONFIG)
    instructions = cpa["instructions"]

    assert "IRS and Delaware government websites" in instructions
    assert "not a licensed CPA, tax attorney, authorized tax representative" in instructions
    assert "Do not sign, e-file, submit, amend, or pay" in instructions
    assert "qualified licensed tax professional" in instructions
    assert "do not request or expose secrets" in instructions
