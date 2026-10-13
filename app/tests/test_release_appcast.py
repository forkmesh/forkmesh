import json
import os
import subprocess
import xml.etree.ElementTree as ET
from pathlib import Path
from urllib.parse import parse_qs, urlparse


ROOT = Path(__file__).resolve().parents[2]
GENERATOR = ROOT / "tools" / "package" / "generate-appcast.sh"


def _generate_appcast(tmp_path, manifest):
    release_dir = tmp_path / ".forkmesh" / "releases" / "latest"
    release_dir.mkdir(parents=True)
    (release_dir / "release.json").write_text(
        json.dumps(manifest), encoding="utf-8"
    )
    env = os.environ.copy()
    env.pop("FORKMESH_SPARKLE_ED_KEY", None)
    env.pop("FORKMESH_RELEASE_CAS", None)
    result = subprocess.run(
        [
            "bash",
            str(GENERATOR),
            "--channel",
            "latest",
            "--host",
            "https://downloads.example/",
        ],
        cwd=tmp_path,
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )
    return result, release_dir / "appcast.xml"


def test_appcast_enclosure_uses_canonical_blob_route_and_asset_filename(tmp_path):
    digest = "a" * 64
    result, appcast = _generate_appcast(
        tmp_path,
        {
            "repo": "acme/widgets",
            "tag": "v1.2.3",
            "created_at": 1_700_000_000_000,
            "assets": [
                {
                    "name": "ForkMesh Pro & Tools.dmg",
                    "blob_sha256": digest,
                    "size": 1234,
                    "os": "macos",
                },
                {
                    "name": "forkmesh-macos-arm64",
                    "blob_sha256": "b" * 64,
                    "size": 5678,
                    "os": "macos",
                },
            ],
        },
    )

    assert result.returncode == 0, result.stderr
    root = ET.parse(appcast).getroot()
    enclosures = root.findall(".//enclosure")
    assert len(enclosures) == 1
    url = enclosures[0].attrib["url"]
    parsed = urlparse(url)
    assert parsed.path == (
        "/api/repo/acme/widgets/releases/blob/sha256/" + digest
    )
    assert parse_qs(parsed.query) == {"filename": ["ForkMesh Pro & Tools.dmg"]}
    assert url.endswith("?filename=ForkMesh%20Pro%20%26%20Tools.dmg")


def test_appcast_rejects_a_manifest_without_a_download_repository(tmp_path):
    result, appcast = _generate_appcast(
        tmp_path,
        {
            "repo": "missing-repository-separator",
            "tag": "v1.2.3",
            "assets": [],
        },
    )

    assert result.returncode != 0
    assert "release manifest repo must be owner/repo" in result.stderr
    assert not appcast.exists()
