#!/usr/bin/env bash
# Publish prebuilt release binaries WITHOUT committing them into git (issue #304;
# see www/docs/design/release-binary-publishing.md).
#
# For each binary you pass, this:
#   1. computes its sha256 (its content address),
#   2. copies the bytes into the node's content-addressed release store (the CAS,
#      co-located with the served mirror — NEVER committed to git), and
#   3. records the asset in the channel's release metadata:
#        .forkmesh/releases/<channel>/SHASUMS256.txt   (sha256sum -c compatible)
#        .forkmesh/releases/<channel>/release.json     (authenticated manifest body)
#        .forkmesh/releases/<channel>/release.json.sig (Ed25519 detached signature)
#
# Commit the .forkmesh/releases/<channel>/ metadata changes (small, text) the usual way;
# install.sh authenticates release.json against a separately trusted key or
# controller digest, verifies that it binds SHASUMS256.txt, then downloads each
# binary from the content-addressed endpoint and verifies the SHA-256.
#
# Usage:
#   tools/forkmesh-release-publish.sh [--channel latest] [--tag vX.Y.Z] \
#       [--tag-commit GIT_COMMIT] [--build-commit GIT_COMMIT] \
#       [--cas-dir DIR] [--repo owner/repo] [--signing-key PRIVATE_KEY.pem] \
#       BINARY [BINARY ...]
#
#   --channel   Release channel directory under .forkmesh/releases/ (default: latest, or
#               $RELEASE_CHANNEL).
#   --tag       Immutable git tag this release is cut from (default: $FORKMESH_TAG
#               or the current `git describe --tags`).
#   --tag-commit  Release-cut commit --tag peels to now, at signing (default:
#                 $FORKMESH_TAG_COMMIT, else `git rev-list -n1 <tag>`). The
#                 tag later moves to the publish commit; see
#                 .forkmesh/releases/README.md for the definition.
#   --build-commit
#                 Exact code-source commit embedded in the binaries (default:
#                 $FORKMESH_BUILD_COMMIT, else the newest commit outside
#                 .forkmesh/releases/). This is deliberately separate from the
#                 immutable tag target so same-semver rebuild provenance does
#                 not redefine an existing tag.
#   --cas-dir   Where to write blob bytes — point this at the serving node's
#               <mirror>/forkmesh-releases directory. Defaults to
#               $FORKMESH_RELEASE_CAS, else .forkmesh/release-blobs (the node that
#               serves the repo must read its CAS from this path).
#   --repo      owner/repo for the manifest (default: $FORKMESH_REPO,
#               else forkmesh/forkmesh).
#   --signing-key
#               Ed25519 private key in PEM form. Defaults to
#               $FORKMESH_RELEASE_SIGNING_KEY. Publishing fails closed when it
#               is absent: release metadata is executable supply-chain input,
#               not an optional authenticity hint.
#
# Each BINARY should already be named the way install.sh resolves per platform:
#   forkmesh-<os>-<arch>[.exe]   (os: linux|macos|windows, arch: x86_64|arm64)
set -euo pipefail

channel="${RELEASE_CHANNEL:-latest}"
tag="${FORKMESH_TAG:-}"
cas_dir="${FORKMESH_RELEASE_CAS:-}"
repo="${FORKMESH_REPO:-forkmesh/forkmesh}"
tag_commit="${FORKMESH_TAG_COMMIT:-}"
build_commit="${FORKMESH_BUILD_COMMIT:-}"
signing_key="${FORKMESH_RELEASE_SIGNING_KEY:-}"
bins=()

while [ $# -gt 0 ]; do
  case "$1" in
    --channel) channel="$2"; shift 2 ;;
    --tag)     tag="$2"; shift 2 ;;
    --tag-commit) tag_commit="$2"; shift 2 ;;
    --build-commit) build_commit="$2"; shift 2 ;;
    --cas-dir) cas_dir="$2"; shift 2 ;;
    --repo)    repo="$2"; shift 2 ;;
    --signing-key) signing_key="$2"; shift 2 ;;
    -h|--help) sed -n '2,40p' "$0"; exit 0 ;;
    --) shift; while [ $# -gt 0 ]; do bins+=("$1"); shift; done ;;
    -*) echo "Unknown option: $1" >&2; exit 2 ;;
    *)  bins+=("$1"); shift ;;
  esac
done

[ "${#bins[@]}" -gt 0 ] || { echo "Error: no binaries given." >&2; exit 2; }

if [ -z "$tag" ]; then
  tag="$(
    git describe --tags --exact-match 2>/dev/null ||
      git describe --tags --abbrev=0 2>/dev/null ||
      echo ""
  )"
fi
[ -n "$tag" ] || {
  echo "Error: --tag must name an existing release tag." >&2
  exit 2
}
[ -n "$cas_dir" ] || cas_dir=".forkmesh/release-blobs"

resolved_tag_commit=""
if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  resolved_tag_commit="$(
    git rev-parse --verify "refs/tags/${tag}^{commit}" 2>/dev/null || true
  )"
  if [ -z "$resolved_tag_commit" ]; then
    echo "Error: release tag '$tag' does not resolve through refs/tags/ to a commit." >&2
    exit 2
  fi
  resolved_tag_commit="$(
    printf '%s' "$resolved_tag_commit" | tr 'A-F' 'a-f'
  )"
  if [ -n "$tag_commit" ] &&
     [ "$(printf '%s' "$tag_commit" | tr 'A-F' 'a-f')" != "$resolved_tag_commit" ]; then
    echo "Error: --tag-commit does not match the peeled target of refs/tags/$tag." >&2
    exit 2
  fi
  tag_commit="$resolved_tag_commit"
fi
[ -n "$build_commit" ] ||
  build_commit="$(git log -1 --format=%H -- . \
    ':(exclude).forkmesh/releases/**' 2>/dev/null || echo "")"
tag_commit="$(printf '%s' "$tag_commit" | tr 'A-F' 'a-f')"
build_commit="$(printf '%s' "$build_commit" | tr 'A-F' 'a-f')"
if ! printf '%s' "$tag_commit" |
    grep -Eq '^([0-9a-f]{40}|[0-9a-f]{64})$'; then
  echo "Error: --tag-commit must identify the tag's exact 40- or 64-hex Git commit." >&2
  exit 2
fi
if ! printf '%s' "$build_commit" |
    grep -Eq '^([0-9a-f]{40}|[0-9a-f]{64})$'; then
  echo "Error: --build-commit must identify the binary's exact 40- or 64-hex Git commit." >&2
  exit 2
fi
[ -n "$signing_key" ] || {
  echo "Error: --signing-key (or FORKMESH_RELEASE_SIGNING_KEY) is required." >&2
  exit 2
}
[ -f "$signing_key" ] && [ ! -L "$signing_key" ] || {
  echo "Error: release signing key must be a non-symlink regular file." >&2
  exit 2
}
signing_key_uid="$(
  stat -c '%u' "$signing_key" 2>/dev/null ||
    stat -f '%u' "$signing_key" 2>/dev/null || true
)"
[ "$signing_key_uid" = "$(id -u)" ] || {
  echo "Error: release signing key must be owned by the publishing user." >&2
  exit 2
}
signing_key_mode="$(
  stat -c '%a' "$signing_key" 2>/dev/null ||
    stat -f '%Lp' "$signing_key" 2>/dev/null || true
)"
case "$signing_key_mode" in
  *00) ;;
  *) echo "Error: release signing key must not be group/world accessible (mode 0600 or stricter)." >&2
     exit 2 ;;
esac
command -v openssl >/dev/null 2>&1 || {
  echo "Error: OpenSSL is required to sign release metadata." >&2
  exit 2
}
meta_dir=".forkmesh/releases/${channel}"
mkdir -p "$meta_dir" "$cas_dir"

# Hash with the shell alone: `awk` is an /etc/alternatives symlink on Debian and
# is not guaranteed to resolve inside a minimal sandbox, so the digest of a
# just-built release must not depend on it.
sha256_of() {
  local line
  if command -v sha256sum >/dev/null 2>&1; then line="$(sha256sum "$1")"
  elif command -v shasum  >/dev/null 2>&1; then line="$(shasum -a 256 "$1")"
  else echo "Error: need sha256sum or shasum." >&2; exit 1; fi
  printf '%s' "${line%% *}"
}

# Derive os/arch from bare binaries and their native-installer suffixes.
asset_os=""; asset_arch=""
detect_os_arch() {
  local n="$1"
  n="${n%.zsync}"
  n="${n%.AppImage}"
  n="${n%.apk}"
  n="${n%.dmg}"
  n="${n%-setup.exe}"
  n="${n%.exe}"
  asset_arch="${n##*-}"; n="${n%-*}"; asset_os="${n##*-}"
}

# Escape a value for embedding in a JSON string. Filenames/tags are operator
# supplied, but one containing a double quote or backslash would otherwise
# corrupt (or inject fields into) the emitted manifest JSON.
json_escape() {
  local s="$1"
  s="${s//\\/\\\\}"    # backslash first so later escapes aren't re-escaped
  s="${s//\"/\\\"}"    # double quote
  s="${s//$'\n'/\\n}"
  s="${s//$'\r'/\\r}"
  s="${s//$'\t'/\\t}"
  printf '%s' "$s"
}

sums_file="${meta_dir}/SHASUMS256.txt"
records_file="${sums_file}.records.tmp"
: > "$records_file"

# A release is built by native runners, one operating system at a time. Preserve
# assets already published for this exact tag/provenance so the next runner adds
# its platform rather than replacing the others. A different tag starts clean;
# conflicting provenance for the same tag fails closed.
if [ -f "${meta_dir}/release.json" ]; then
  python3 - "${meta_dir}/release.json" "$repo" "$tag" \
    "$tag_commit" "$build_commit" >>"$records_file" <<'PY'
import json
import re
import sys

path, repo, tag, tag_commit, build_commit = sys.argv[1:]
try:
    manifest = json.load(open(path, encoding="utf-8"))
except Exception:
    raise SystemExit("Error: existing release.json is not valid JSON")
if manifest.get("tag") != tag:
    raise SystemExit(0)
expected = {
    "schema": "forkmesh-release-v2",
    "repo": repo,
    "tag_commit": tag_commit,
}
if any(manifest.get(key) != value for key, value in expected.items()):
    raise SystemExit(
        "Error: existing assets for this tag have conflicting release provenance"
    )
if manifest.get("build_commit") != build_commit:
    # An explicit same-version rebuild advances artifact provenance. Mixing its
    # bytes with the old build would be invalid, so begin a fresh candidate.
    raise SystemExit(0)
for asset in manifest.get("assets", []):
    values = (
        str(asset.get("blob_sha256", "")),
        str(asset.get("name", "")),
        str(asset.get("size", "")),
        str(asset.get("os", "")),
        str(asset.get("arch", "")),
    )
    if (
        not re.fullmatch(r"[0-9a-f]{64}", values[0])
        or not values[1]
        or any("\t" in value or "\n" in value or "\r" in value for value in values)
        or not values[2].isdigit()
        or int(values[2]) <= 0
    ):
        raise SystemExit("Error: existing release.json contains an invalid asset")
    print("\t".join(values))
PY
fi

for bin in "${bins[@]}"; do
  [ -f "$bin" ] || { echo "Error: not a file: $bin" >&2; exit 1; }
  name="$(basename "$bin")"
  hash="$(sha256_of "$bin")"
  size="$(wc -c < "$bin" | tr -d ' ')"
  detect_os_arch "$name"

  # Content-addressed store: sha256/<aa>/<full>/data. Idempotent — re-publishing
  # identical bytes is a no-op (the blob already exists under its hash).
  blob="${cas_dir}/sha256/${hash:0:2}/${hash}/data"
  mkdir -p "$(dirname "$blob")"
  [ -f "$blob" ] || cp "$bin" "$blob"

  printf '%s\t%s\t%s\t%s\t%s\n' \
    "$hash" "$name" "$size" "$asset_os" "$asset_arch" >> "$records_file"
  echo "Staged $name ($size bytes) -> ${blob}"
done

# The last record for a name is the freshly built replacement. Emit stable,
# filename-sorted checksum and JSON files from that merged set.
python3 - "$records_file" "$sums_file" "${sums_file}.assets.tmp" <<'PY'
import json
import sys

records, sums, assets_path = sys.argv[1:]
assets = {}
with open(records, encoding="utf-8") as handle:
    for line in handle:
        digest, name, size, os_name, arch = line.rstrip("\n").split("\t")
        assets[name] = {
            "name": name,
            "blob_sha256": digest,
            "size": int(size),
            "os": os_name,
            "arch": arch,
        }
ordered = [assets[name] for name in sorted(assets)]
with open(sums, "w", encoding="utf-8") as handle:
    for asset in ordered:
        handle.write(f'{asset["blob_sha256"]}  {asset["name"]}\n')
with open(assets_path, "w", encoding="utf-8") as handle:
    json.dump(ordered, handle, separators=(",", ":"))
PY
assets_json="$(cat "${sums_file}.assets.tmp")"
rm -f "$records_file" "${sums_file}.assets.tmp"
sums_hash="$(sha256_of "$sums_file")"

# release.json: the manifest install.sh / the release page can read, and the
# exact body signed below. The checksum-list hash prevents an attacker from
# mixing an authenticated manifest with a modified SHASUMS256.txt.
{
  printf '{\n'
  printf '  "schema": "forkmesh-release-v2",\n'
  printf '  "repo": "%s",\n' "$(json_escape "$repo")"
  printf '  "tag": "%s",\n' "$(json_escape "$tag")"
  printf '  "tag_commit": "%s",\n' "$(json_escape "$tag_commit")"
  printf '  "build_commit": "%s",\n' "$(json_escape "$build_commit")"
  printf '  "channel": "%s",\n' "$(json_escape "$channel")"
  printf '  "created_at": %s,\n' "$(date +%s)000"
  printf '  "checksums_sha256": "%s",\n' "$sums_hash"
  printf '  "assets": %s\n' "$assets_json"
  printf '}\n'
} > "${meta_dir}/release.json"

# Raw 64-byte Ed25519 detached signature. Keeping the signature detached means
# the exact release.json bytes are independently hashable/controller-pinnable.
signature="${meta_dir}/release.json.sig"
if ! openssl pkeyutl -sign -rawin -inkey "$signing_key" \
     -in "${meta_dir}/release.json" -out "${signature}.tmp" 2>/dev/null; then
  rm -f "${signature}.tmp"
  echo "Error: could not Ed25519-sign release.json with the configured key." >&2
  exit 1
fi
[ "$(wc -c < "${signature}.tmp" | tr -d ' ')" = "64" ] || {
  rm -f "${signature}.tmp"
  echo "Error: release signer did not produce a 64-byte Ed25519 signature." >&2
  exit 1
}
mv -f "${signature}.tmp" "$signature"

echo
echo "Wrote ${meta_dir}/SHASUMS256.txt, release.json, and release.json.sig"
echo "Blobs staged in: ${cas_dir} (NOT committed; served by the node over the relay)"
echo "Next: commit the .forkmesh/releases/${channel}/ metadata, then publish it."
