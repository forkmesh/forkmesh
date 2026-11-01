"""Release manifest + content-addressed blob helpers for the ForkMesh relay.

Pure, stdlib-only helpers for the signed-release flow (issue #304): tag/asset
name validation, the content-addressed blob path layout, the canonical signable
release-manifest body, the release signing message, `SHASUMS256.txt` generation,
and semver ordering for the `latest` alias.

Split out of ``entry.py`` — like ``urls.py`` and ``solana.py`` these depend only
on ``re``/stdlib and hold no runtime state or js imports, so the module loads
standalone (the test suite parses it the same way it parses ``entry.py``). The
one async orchestrator, ``verify_release_manifest``, stays in ``entry.py``
because it reaches back into that module's Ed25519/sha256 crypto helpers; it
imports ``release_manifest_content`` + ``release_signing_message`` from here, so
the dependency stays one-directional (``entry`` imports from here, never the
reverse).
"""

import re
from urllib.parse import quote

RELEASE_TAG_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._+-]{0,127}$")
# Lowercase hex sha256 — the content address of a blob and the integrity anchor.
SHA256_HEX_RE = re.compile(r"^[0-9a-f]{64}$")
GIT_COMMIT_RE = re.compile(r"^(?:[0-9a-f]{40}|[0-9a-f]{64})$")
# Recognise vMAJOR.MINOR.PATCH[-prerelease] for `latest` ordering.
RELEASE_SEMVER_RE = re.compile(r"^v?(\d+)\.(\d+)\.(\d+)(?:[-.](.+))?$")


def valid_release_tag(value):
    value = (value or "").strip()
    return (bool(value) and ".." not in value and
            bool(RELEASE_TAG_RE.match(value)))


def valid_asset_name(value):
    # An asset's download filename. Must be one path segment with no traversal,
    # separators, or control characters so it can never escape the release dir.
    value = value or ""
    if not (1 <= len(value) <= 255) or value in (".", ".."):
        return False
    if "/" in value or "\\" in value or "\x00" in value:
        return False
    return not any(ord(ch) < 0x20 for ch in value)


def valid_sha256_hex(value):
    return bool(SHA256_HEX_RE.match((value or "").strip().lower()))


def release_download_content_disposition(value):
    """Build a safe attachment header for a manifest-selected asset name.

    Release bytes remain addressed and verified by SHA-256.  The filename is
    presentation metadata from the signed release manifest, so keep it out of
    the mirror request signature and apply it only at the public edge.  The
    ASCII fallback supports older clients while ``filename*`` preserves an
    otherwise valid Unicode asset name.
    """
    name = value or ""
    if not valid_asset_name(name):
        return ""
    fallback = re.sub(r"[^A-Za-z0-9._ -]+", "_", name).strip(" .")
    fallback = (fallback or "download")[:180]
    encoded = quote(name, safe="!#$&+-.^_`|~")
    return (
        'attachment; filename="' + fallback.replace('"', "_") + '"; '
        "filename*=UTF-8''" + encoded
    )


def valid_git_commit(value):
    return bool(GIT_COMMIT_RE.match((value or "").strip().lower()))


def cas_blob_relpath(sha256_hex):
    # Where a blob's bytes live in the per-node content-addressed store, relative
    # to .forkmesh/release-blobs/. Fan out by the first byte to keep directories
    # shallow. Returns None for a non-sha256 input so callers reject it.
    h = (sha256_hex or "").strip().lower()
    if not SHA256_HEX_RE.match(h):
        return None
    return "sha256/%s/%s/data" % (h[:2], h)


def release_asset_line(asset):
    # One deterministic line per asset for the signable manifest. The fields are
    # the asset's IMMUTABLE identity (name + the exact bytes it resolves to);
    # mutable presentation (download_count) and release notes are intentionally
    # absent so editing them never invalidates the signature.
    return "\x00".join([
        asset.get("name", "") or "",
        (asset.get("blob_sha256", "") or "").lower(),
        str(int(asset.get("size", 0) or 0)),
        asset.get("content_type", "") or "",
        asset.get("os", "") or "",
        asset.get("arch", "") or "",
        asset.get("label", "") or "",
    ])


def release_manifest_content(manifest):
    # The canonical, signable body of a release. Binds the repo, the tag, the
    # commit the tag pointed to at finalize (so a later force-push can't silently
    # redefine the release), the independently-recorded commit embedded in the
    # artifact, and the full asset set sorted by line for order independence.
    # name/body/prerelease are NOT included: they are the editable metadata of
    # an otherwise immutable release.
    repo = manifest.get("repo", "") or ""
    tag = manifest.get("tag", "") or ""
    tag_commit = manifest.get("tag_commit", "") or ""
    build_commit = manifest.get("build_commit", "") or ""
    assets = manifest.get("assets") or []
    asset_lines = sorted(release_asset_line(a) for a in assets)
    header_fields = [repo, tag, tag_commit]
    if build_commit:
        header_fields.append(build_commit)
    header_fields.append(str(len(asset_lines)))
    header = "\x00".join(header_fields)
    return "\n".join([header] + asset_lines)


def release_signing_message(repo, tag, author, ts, content_hash):
    # The exact bytes signed/verified for a release manifest, matching the
    # "forkmesh-<thing>-v1\n…\n<sha256 of content>" form used by issue/PR events.
    return (
        "forkmesh-release-v1\n" + repo + "\n" + tag + "\n" + author + "\n" +
        str(ts) + "\n" + content_hash
    ).encode()


def generate_shasums(assets):
    # A `sha256sum -c`-compatible SHASUMS256.txt ("<hash>  <name>", two spaces =
    # text mode), sorted by name. Derived from the asset content addresses, so it
    # is covered by the manifest signature and can't be tampered independently.
    lines = []
    for asset in sorted(assets, key=lambda a: a.get("name", "") or ""):
        digest = (asset.get("blob_sha256", "") or "").lower()
        name = asset.get("name", "") or ""
        if SHA256_HEX_RE.match(digest) and name:
            lines.append("%s  %s" % (digest, name))
    return "".join(line + "\n" for line in lines)


def release_semver_key(tag):
    # Orderable key for `latest` resolution. A final release sorts ABOVE any
    # prerelease of the same x.y.z. Returns None for non-semver tags (skipped).
    match = RELEASE_SEMVER_RE.match((tag or "").strip())
    if not match:
        return None
    major, minor, patch = (
        int(match.group(1)), int(match.group(2)), int(match.group(3)))
    pre = match.group(4)
    pre_rank = (1,) if pre is None else (0, pre)
    return (major, minor, patch, pre_rank)


def resolve_latest_release(releases):
    # The release the `…/releases/latest/<asset>` alias points to: the highest
    # semver among published, non-draft, non-prerelease releases. Computed (never
    # a committed pointer) so it can't drift from the actual release set.
    best = None
    best_key = None
    for release in releases:
        if release.get("draft"):
            continue
        if (release.get("state", "published") or "published") != "published":
            continue
        if release.get("prerelease"):
            continue
        key = release_semver_key(release.get("tag", ""))
        if key is None:
            continue
        if best_key is None or key > best_key:
            best_key, best = key, release
    return best


def asset_upload_decision(release_state, existing_asset, new_sha256):
    # Resolve a same-name (re-)upload (issue #304 §7). On a draft, replacing an
    # asset is allowed; on a published/yanked (immutable) release, only a
    # byte-identical re-upload is a no-op — a different hash is a conflict.
    new_sha = (new_sha256 or "").lower()
    if existing_asset is None:
        return "create"
    existing_sha = (existing_asset.get("blob_sha256", "") or "").lower()
    if release_state == "draft":
        return "noop" if existing_sha == new_sha else "replace"
    return "noop" if existing_sha == new_sha else "conflict"


APT_REPOSITORY = "forkmesh/forkmesh"
APT_METADATA_DIR = ".forkmesh/releases/apt"
APT_METADATA_RE = re.compile(
    r"^/apt/(dists/stable/(?:InRelease|Release|Release\.gpg"
    r"|main/binary-[a-z0-9]{1,16}/Packages)"
    r"|forkmesh-archive-keyring\.asc|forkmesh\.sources)$")
APT_POOL_RE = re.compile(
    r"^/apt/pool/sha256/([0-9a-f]{64})/[A-Za-z0-9._+~%-]{1,255}\.deb$")


def apt_redirect_target(path):
    match = APT_POOL_RE.match(path or "")
    if match:
        return "/api/repo/%s/releases/blob/sha256/%s" % (
            APT_REPOSITORY, match.group(1))
    match = APT_METADATA_RE.match(path or "")
    if match:
        return "/api/repo/%s/raw?path=%s&ref=main" % (
            APT_REPOSITORY,
            quote(APT_METADATA_DIR + "/" + match.group(1), safe=""))
    return ""
