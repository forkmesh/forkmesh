#!/usr/bin/env python3
"""Contracts for the Genie special agent that the desktop spider's Reply sends to."""

import json
import re
from pathlib import Path

from repo_workflows import as_teammate


PROJECT = Path(__file__).resolve().parents[2]
CONFIG = PROJECT / ".forkmesh" / "workflows" / "genie.json"
TEAMMATE_STORE = PROJECT / "desktop" / "src" / "TeammateStore.cpp"
GENIE_VITALS = PROJECT / "desktop" / "src" / "GenieVitals.h"
GENIE_WINDOW = PROJECT / "desktop" / "src" / "MainWindowGenie.cpp"


def _config():
    return as_teammate(json.loads(CONFIG.read_text(encoding="utf-8")))


def _known_permissions():
    source = TEAMMATE_STORE.read_text(encoding="utf-8")
    body = re.search(r"QStringList knownPermissions\(\)\s*\{(.*?)\}", source, re.DOTALL)
    assert body, "knownPermissions() not found in TeammateStore.cpp"
    return set(re.findall(r'QStringLiteral\("([^"]+)"\)', body.group(1)))


def test_genie_is_on_and_runs_on_a_low_cost_model():
    genie = _config()
    assert genie["id"] == "genie"
    assert genie["enabled"] is True
    assert genie["lowCostModel"] is True
    assert genie["schedule"]["mode"] == "on-demand"
    assert genie["workflow"] == ""


def test_genie_id_matches_the_spider():
    header = GENIE_VITALS.read_text(encoding="utf-8")
    assert 'teammateId = "genie"' in header
    assert "replyToGenieSpider" in GENIE_WINDOW.read_text(encoding="utf-8")


def test_genie_reads_the_crm_but_cannot_change_code_or_run_unattended():
    permissions = set(_config()["permissions"])
    assert permissions <= _known_permissions()
    assert permissions == {"read-repositories", "external-tools"}


def test_genie_teaches_without_changing_the_crm():
    instructions = _config()["instructions"]
    assert "forkmesh-genie" in instructions
    assert "crm_get_contact" in instructions
    assert "never change the CRM" in instructions


def test_genie_checks_the_messages_the_operator_commented_on():
    instructions = _config()["instructions"]
    assert '"checked"' in instructions
    header = GENIE_VITALS.read_text(encoding="utf-8")
    assert "struct GenieSaidLog" in header
    assert "recordGenieSaid" in GENIE_WINDOW.read_text(encoding="utf-8")
