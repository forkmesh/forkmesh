import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

import catalog


def _clone_url(url):
    record = catalog.safe_catalog_record(
        {"owner": "alice", "name": "repo", "maintainer": "PK", "cloneUrl": url})
    return record["cloneUrl"]


def test_catalog_keeps_only_remote_clone_urls():
    for url in ("https://app.forkmesh.com/alice/repo.git", "ssh://git@host/x.git",
                "git@github.com:alice/repo.git", "git://host/repo"):
        assert _clone_url(url) == url
    for url in ("/home/alice/projects/secret", "file:///home/alice/secret",
                "../secret", "C:\\Users\\alice\\secret", "ext::sh -c id",
                "-uupload-pack=touch x"):
        assert _clone_url(url) == "", url
