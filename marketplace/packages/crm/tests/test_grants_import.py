import json
import subprocess
import sys
from pathlib import Path
from urllib.parse import parse_qs, urlparse

from test_crm import Runtime, call

REPO = Path(__file__).resolve().parents[4]
GRANTS = REPO / "docs" / "finance" / "grants"
TOOL = REPO / "tools" / "grants_build.py"


def test_the_grants_files_are_built_from_the_json():
    result = subprocess.run([sys.executable, str(TOOL), "--check"],
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    grants = json.loads((GRANTS / "grants.json").read_text(encoding="utf-8"))
    assert len(grants) == 100
    assert len({grant["id"] for grant in grants}) == 100


def test_every_ranked_grant_has_a_one_click_chatgpt_draft():
    ranked = (GRANTS / "ranked.md").read_text(encoding="utf-8")
    links = [line.split("(", 1)[1].split(")", 1)[0]
             for line in ranked.splitlines()
             if line.startswith("**[Draft this application in ChatGPT](")]
    assert len(links) == 100
    for link in links:
        url = urlparse(link)
        assert (url.scheme, url.netloc, url.path) == ("https", "chatgpt.com", "/")
        prompt = parse_qs(url.query)["q"][0]
        assert prompt.startswith("You are helping the ForkMesh team write a complete draft")
        assert "## About us: ForkMesh" in prompt
        assert "What the application asks for:" in prompt
        assert len(link) < 24000


def test_the_grants_csv_imports_into_a_grants_pipeline_list():
    runtime = Runtime()
    call(runtime.use("PUT", body={"flows": [
        {"id": "customers", "name": "Customers", "stages": ["lead", "won"]},
    ]}), "/api/crm/acme/settings")
    imported = call(runtime.use("POST", "alice", {
        "fileName": "forkmesh-grants.csv",
        "csv": (GRANTS / "forkmesh-grants.csv").read_text(encoding="utf-8"),
    }), "/api/crm/acme/contacts/import")["data"]
    assert (imported["created"], imported["skipped"]) == (100, 0)
    assert imported["view"]["flow"] == "grants"
    data = call(runtime.use("GET"), "/api/crm/acme")["data"]
    assert "grants" in [flow["id"] for flow in data["settings"]["flows"]]
    contacts = data["contacts"]
    assert {(contact["flow"], contact["stage"]) for contact in contacts} == {
        ("grants", "researching")}
    first = next(contact for contact in contacts if contact["fields"].get("rank") == "1")
    assert first["company"] and first["website"].startswith("https://")
    assert first["value"] > 0 and first["nextStep"]
    assert "How to apply:" in first["notes"]
    assert "What the application asks for:" in first["notes"]
    assert {"fit_score", "award", "deadline", "apply_at"} <= set(first["fields"])
