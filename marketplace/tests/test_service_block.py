#!/usr/bin/env python3

import json
import shutil
import sys
from pathlib import Path

import pytest

TOOLS = Path(__file__).resolve().parents[1] / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from marketplace_lib import MarketplaceError, load_manifest  # noqa: E402

EXAMPLE = Path(__file__).resolve().parents[2] / "desktop" / "core" / "examples" / "service-plugins" / "repo-pulse"
SCHEMA = Path(__file__).resolve().parents[1] / "manifest.schema.json"


@pytest.fixture()
def package(tmp_path):
    target = tmp_path / "repo-pulse"
    shutil.copytree(EXAMPLE, target)
    return target


def rewrite(package, change):
    path = package / "manifest.json"
    manifest = json.loads(path.read_text(encoding="utf-8"))
    change(manifest)
    path.write_text(json.dumps(manifest), encoding="utf-8")
    return path


def test_example_service_package_is_valid(package):
    manifest = load_manifest(package / "manifest.json")
    assert manifest["service"]["start"] == "boot"
    assert set(manifest["service"]["remoteMethods"]) <= set(manifest["service"]["methods"])


def test_schema_describes_the_service_block():
    schema = json.loads(SCHEMA.read_text(encoding="utf-8"))
    service = schema["properties"]["service"]
    assert service["additionalProperties"] is False
    assert service["properties"]["api"] == {"const": 1}


@pytest.mark.parametrize(
    "change",
    [
        lambda m: m["service"].update(api=2),
        lambda m: m["service"].update(exec="../escape.py"),
        lambda m: m["service"].update(exec="content/service/missing.py"),
        lambda m: m["service"].update(interpreter="perl"),
        lambda m: m["service"].update(start="always"),
        lambda m: m["service"].update(methods=[]),
        lambda m: m["service"].update(remoteMethods=["watch", "delete"]),
        lambda m: m["service"].update(remoteEvents=["secret"]),
        lambda m: m["service"].update(memoryMB=8192),
        lambda m: m["service"].update(shell=True),
        lambda m: m["permissions"].append({"id": "service.root", "label": "Run as root", "risk": "high"}),
    ],
)
def test_invalid_service_blocks_are_rejected(package, change):
    with pytest.raises(MarketplaceError):
        load_manifest(rewrite(package, change))


def test_packages_without_a_service_are_unaffected(package):
    path = rewrite(package, lambda m: m.pop("service"))
    assert "service" not in load_manifest(path)
