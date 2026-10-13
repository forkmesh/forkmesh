#!/usr/bin/env python3
"""Release-publishing integrity helpers (issue #304).

These pin the pure (stdlib-only) spine of the release-binary publishing system
designed in docs/design/release-binary-publishing.md:

  * the canonical, signable manifest body — if this drifts, every published
    release's Ed25519 signature would stop verifying across the mesh;
  * the content-addressed blob path layout;
  * sha256sum-compatible checksum generation;
  * `latest` semver resolution;
  * the same-name re-upload decision (§7 edge cases).

Like test_repo_state.py, helpers are extracted by AST so the JS runtime /
WebCrypto bindings are never imported. The pure release spine now lives in
releases.py (route regexes in urls.py); both are parsed alongside entry.py.
"""

import ast
import hashlib
from pathlib import Path


ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
# Route regexes (RELEASE_BLOB_RE, REPO_RELEASE_DOWNLOADS_RE) live in urls.py now.
URLS = Path(__file__).resolve().parents[1] / "src" / "urls.py"
# The pure release helpers + their regexes live in releases.py now.
RELEASES = Path(__file__).resolve().parents[1] / "src" / "releases.py"


def _load(*names):
    """Load named module-level functions and Assign(constants) from the sources."""
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    urls_tree = ast.parse(URLS.read_text(encoding="utf-8"), filename=str(URLS))
    releases_tree = ast.parse(
        RELEASES.read_text(encoding="utf-8"), filename=str(RELEASES))
    wanted = set(names)
    body = []
    selected = set()
    for node in list(urls_tree.body) + list(releases_tree.body) + list(tree.body):
        if isinstance(node, ast.FunctionDef) and node.name in wanted:
            body.append(node)
            selected.add(node.name)
        elif isinstance(node, ast.Assign):
            targets = {t.id for t in node.targets if isinstance(t, ast.Name)}
            matched = (targets & wanted) - selected
            if matched:
                body.append(node)
                selected.update(matched)
    module = ast.fix_missing_locations(ast.Module(body=body, type_ignores=[]))
    namespace = {
        "re": __import__("re"),
        "quote": __import__("urllib.parse", fromlist=["quote"]).quote,
    }
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


NS = _load(
    "RELEASE_TAG_RE", "SHA256_HEX_RE", "GIT_COMMIT_RE",
    "RELEASE_SEMVER_RE", "RELEASE_BLOB_RE",
    "REPO_RELEASE_DOWNLOADS_RE",
    "valid_release_tag", "valid_asset_name", "valid_sha256_hex",
    "release_download_content_disposition",
    "valid_git_commit",
    "cas_blob_relpath", "release_asset_line", "release_manifest_content",
    "release_signing_message", "generate_shasums", "release_semver_key",
    "resolve_latest_release", "asset_upload_decision",
)
valid_release_tag = NS["valid_release_tag"]
valid_asset_name = NS["valid_asset_name"]
valid_sha256_hex = NS["valid_sha256_hex"]
release_download_content_disposition = NS[
    "release_download_content_disposition"]
valid_git_commit = NS["valid_git_commit"]
cas_blob_relpath = NS["cas_blob_relpath"]
release_manifest_content = NS["release_manifest_content"]
release_signing_message = NS["release_signing_message"]
generate_shasums = NS["generate_shasums"]
release_semver_key = NS["release_semver_key"]
resolve_latest_release = NS["resolve_latest_release"]
asset_upload_decision = NS["asset_upload_decision"]
RELEASE_BLOB_RE = NS["RELEASE_BLOB_RE"]
REPO_RELEASE_DOWNLOADS_RE = NS["REPO_RELEASE_DOWNLOADS_RE"]


H1 = "a" * 64
H2 = "b" * 64
H3 = "c" * 64


# --- name / tag / hash validation -----------------------------------------

def test_valid_release_tag():
    assert valid_release_tag("v1.2.3")
    assert valid_release_tag("2024.06.01")
    assert valid_release_tag("v1.0.0-rc.1+build5")
    assert not valid_release_tag("")
    assert not valid_release_tag("../etc")
    assert not valid_release_tag("v1/2")
    assert not valid_release_tag("-leading-dash")
    assert not valid_release_tag("a" * 200)


def test_valid_asset_name_rejects_traversal():
    assert valid_asset_name("forkmesh-linux-x86_64")
    assert valid_asset_name("SHASUMS256.txt")
    assert not valid_asset_name("")
    assert not valid_asset_name(".")
    assert not valid_asset_name("..")
    assert not valid_asset_name("a/b")
    assert not valid_asset_name("a\\b")
    assert not valid_asset_name("with\x00nul")
    assert not valid_asset_name("with\nnewline")


def test_valid_sha256_hex():
    assert valid_sha256_hex(H1)
    assert valid_sha256_hex(H1.upper())  # normalised to lowercase
    assert not valid_sha256_hex("xyz")
    assert not valid_sha256_hex("a" * 63)


def test_release_download_content_disposition_preserves_manifest_filename():
    assert release_download_content_disposition(
        "forkmesh-linux-x86_64.AppDir.tar.gz"
    ) == (
        'attachment; filename="forkmesh-linux-x86_64.AppDir.tar.gz"; '
        "filename*=UTF-8''forkmesh-linux-x86_64.AppDir.tar.gz"
    )
    assert release_download_content_disposition("ForkMesh café.zip") == (
        'attachment; filename="ForkMesh caf_.zip"; '
        "filename*=UTF-8''ForkMesh%20caf%C3%A9.zip"
    )


def test_release_download_content_disposition_rejects_header_injection():
    assert release_download_content_disposition("../secret") == ""
    assert release_download_content_disposition("asset.zip\r\nX-Evil: yes") == ""


def test_valid_git_commit():
    assert valid_git_commit("a" * 40)
    assert valid_git_commit("B" * 64)
    assert not valid_git_commit("a" * 39)
    assert not valid_git_commit("not-a-commit")


# --- content-addressed storage layout -------------------------------------

def test_cas_blob_relpath_layout():
    assert cas_blob_relpath(H1) == "sha256/aa/%s/data" % H1
    # Uppercase input is normalised so a blob is stored once regardless of case.
    assert cas_blob_relpath(H1.upper()) == cas_blob_relpath(H1)
    assert cas_blob_relpath("not-a-hash") is None
    assert cas_blob_relpath("") is None


# --- canonical signable manifest ------------------------------------------

def _manifest():
    return {
        "repo": "alice/widget",
        "tag": "v1.2.3",
        "tag_commit": "deadbeef" * 5,
        "build_commit": "feedface" * 5,
        "name": "Widget 1.2.3",          # editable — must NOT affect content
        "body": "release notes here",    # editable — must NOT affect content
        "prerelease": False,             # editable — must NOT affect content
        "assets": [
            {"name": "widget-linux", "blob_sha256": H1, "size": 100,
             "os": "linux", "arch": "x86_64"},
            {"name": "widget-macos", "blob_sha256": H2, "size": 200,
             "os": "macos", "arch": "arm64"},
        ],
    }


def test_manifest_content_is_order_independent():
    m1 = _manifest()
    m2 = _manifest()
    m2["assets"] = list(reversed(m2["assets"]))
    assert release_manifest_content(m1) == release_manifest_content(m2)


def test_manifest_content_ignores_editable_metadata():
    base = release_manifest_content(_manifest())
    edited = _manifest()
    edited["name"] = "Totally different title"
    edited["body"] = "rewritten notes"
    edited["prerelease"] = True
    # Editing presentation metadata must not change the signed body, so the
    # existing signature stays valid (design §5: immutable release, editable meta).
    assert release_manifest_content(edited) == base


def test_manifest_content_changes_when_bytes_change():
    base = release_manifest_content(_manifest())
    tampered = _manifest()
    tampered["assets"][0]["blob_sha256"] = H3  # swap an artifact's bytes
    assert release_manifest_content(tampered) != base

    retagged = _manifest()
    retagged["tag_commit"] = "f" * 40  # force-moved tag → different signed body
    assert release_manifest_content(retagged) != base

    rebuilt = _manifest()
    rebuilt["build_commit"] = "0" * 40
    assert release_manifest_content(rebuilt) != base


def test_legacy_manifest_canonical_bytes_remain_compatible():
    legacy = _manifest()
    legacy.pop("build_commit")
    content = release_manifest_content(legacy)
    assert content.startswith(
        "alice/widget\x00v1.2.3\x00%s\x002\n" % ("deadbeef" * 5)
    )


def test_signing_message_is_stable():
    content_hash = hashlib.sha256(
        release_manifest_content(_manifest()).encode()).hexdigest()
    msg = release_signing_message("alice/widget", "v1.2.3", "PUBKEY", 1700, content_hash)
    assert msg == (
        b"forkmesh-release-v1\nalice/widget\nv1.2.3\nPUBKEY\n1700\n"
        + content_hash.encode()
    )


# --- checksum generation ---------------------------------------------------

def test_generate_shasums_format_and_order():
    assets = [
        {"name": "widget-macos", "blob_sha256": H2},
        {"name": "widget-linux", "blob_sha256": H1},
        {"name": "incomplete", "blob_sha256": ""},   # skipped (no bytes yet)
    ]
    out = generate_shasums(assets)
    assert out == "%s  widget-linux\n%s  widget-macos\n" % (H1, H2)
    assert generate_shasums([]) == ""


# --- `latest` resolution ---------------------------------------------------

def test_release_semver_key_orders_prerelease_below_final():
    assert release_semver_key("v1.2.3") > release_semver_key("v1.2.3-rc1")
    assert release_semver_key("v1.2.10") > release_semver_key("v1.2.9")
    assert release_semver_key("v2.0.0") > release_semver_key("v1.99.99")
    assert release_semver_key("not-semver") is None


def test_resolve_latest_skips_draft_prerelease_yanked():
    releases = [
        {"tag": "v1.0.0", "state": "published"},
        {"tag": "v1.2.0", "draft": True},                       # not published
        {"tag": "v2.0.0-rc1", "prerelease": True},              # prerelease
        {"tag": "v1.9.0", "state": "yanked"},                   # withdrawn
        {"tag": "v1.5.0", "state": "published"},
    ]
    latest = resolve_latest_release(releases)
    assert latest["tag"] == "v1.5.0"

    assert resolve_latest_release([]) is None
    assert resolve_latest_release([{"tag": "v3.0.0-beta", "prerelease": True}]) is None


# --- download route --------------------------------------------------------

def test_release_blob_route():
    m = RELEASE_BLOB_RE.match("/api/repo/alice/widget/releases/blob/sha256/" + H1)
    assert m is not None
    assert m.group(1) == "alice"
    assert m.group(2) == "widget"
    assert m.group(3) == H1
    # Reject non-sha256 and traversal attempts in the hash segment.
    assert RELEASE_BLOB_RE.match("/api/repo/a/b/releases/blob/sha256/short") is None
    assert RELEASE_BLOB_RE.match(
        "/api/repo/a/b/releases/blob/sha256/" + "A" * 64) is None  # must be lowercase
    assert RELEASE_BLOB_RE.match("/api/repo/a/b/releases/blob/md5/" + H1) is None


def test_release_downloads_route():
    m = REPO_RELEASE_DOWNLOADS_RE.match("/api/repo/alice/widget/releases/downloads")
    assert m is not None
    assert m.group(1) == "alice"
    assert m.group(2) == "widget"
    assert REPO_RELEASE_DOWNLOADS_RE.match(
        "/api/repo/alice/widget/releases/blob/sha256/" + H1) is None


# --- same-name re-upload (§7) ---------------------------------------------

def test_asset_upload_decision():
    existing = {"blob_sha256": H1}
    # New name on a draft → create.
    assert asset_upload_decision("draft", None, H1) == "create"
    # Same name, same bytes → idempotent no-op in any state.
    assert asset_upload_decision("draft", existing, H1) == "noop"
    assert asset_upload_decision("published", existing, H1) == "noop"
    # Same name, different bytes: allowed on a draft, rejected once published.
    assert asset_upload_decision("draft", existing, H2) == "replace"
    assert asset_upload_decision("published", existing, H2) == "conflict"
    assert asset_upload_decision("yanked", existing, H2) == "conflict"
