#!/usr/bin/env bash
# Generate a Sparkle/WinSparkle appcast from a channel's release.json (issue #370).
# The appcast is the in-app update manifest: Sparkle 2 (macOS) and WinSparkle
# (Windows) poll it, verify each enclosure's EdDSA signature, and offer the
# update. Deriving it from release.json keeps a single source of truth for
# "what is the latest release" — see /docs#installers-updates.
#
# Emits: .forkmesh/releases/<channel>/appcast.xml
#
# Usage:
#   tools/package/generate-appcast.sh [--channel latest] [--host https://forkmesh.com]
#
# Signing env (optional):
#   FORKMESH_SPARKLE_ED_KEY   base64 Ed25519 private key. When set, each
#                             installer enclosure gets an EdDSA `sparkle:edSignature`
#                             over its bytes (reuses the node release key, per #370).
#                             When absent, the appcast ships without signatures
#                             and Sparkle is expected to run with signature
#                             enforcement disabled until a key is wired in.
#
# release.json is the source of truth for asset sha256/size; the enclosure URL is
# the relay's content-addressed blob endpoint the mirror already serves.
set -euo pipefail

channel="${RELEASE_CHANNEL:-latest}"
host="${FORKMESH_HOST:-https://forkmesh.com}"
while [ $# -gt 0 ]; do
  case "$1" in
    --channel) channel="$2"; shift 2 ;;
    --host)    host="$2"; shift 2 ;;
    -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
    *) echo "Unknown option: $1" >&2; exit 2 ;;
  esac
done

meta=".forkmesh/releases/${channel}/release.json"
[ -f "$meta" ] || { echo "Error: no manifest at $meta" >&2; exit 1; }
out=".forkmesh/releases/${channel}/appcast.xml"

log() { echo "[appcast] $*" >&2; }
host="${host%/}"

# Parse release.json with python (already required across the toolchain) and emit
# the appcast. Only the platform *installers* (AppImage/dmg/setup.exe) are
# advertised for in-app updates; the bare binaries stay install.sh-only.
FORKMESH_APPCAST_HOST="$host" \
FORKMESH_APPCAST_CHANNEL="$channel" \
python3 - "$meta" "$out" <<'PY'
import json, os, sys, hashlib, base64, subprocess, datetime, xml.sax.saxutils as x
from urllib.parse import quote

meta_path, out_path = sys.argv[1], sys.argv[2]
host = os.environ["FORKMESH_APPCAST_HOST"]
channel = os.environ["FORKMESH_APPCAST_CHANNEL"]
m = json.load(open(meta_path))
manifest_repo = str(m.get("repo", "")).strip()
repo_parts = manifest_repo.split("/")
if len(repo_parts) != 2 or not all(repo_parts):
    raise SystemExit("release manifest repo must be owner/repo for appcast downloads")
repo_owner, repo_name = repo_parts

tag = m.get("tag", "")
version = tag[1:] if tag.startswith("v") else (tag or "0.0.0")
created = int(m.get("created_at", 0)) // 1000 or int(datetime.datetime.now().timestamp())
pubdate = datetime.datetime.fromtimestamp(created, datetime.timezone.utc).strftime("%a, %d %b %Y %H:%M:%S +0000")

ed_key = os.environ.get("FORKMESH_SPARKLE_ED_KEY", "").strip()
signer = None
if ed_key:
    try:
        from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
        signer = Ed25519PrivateKey.from_private_bytes(base64.b64decode(ed_key))
    except Exception as e:
        sys.stderr.write(f"[appcast] EdDSA key present but unusable ({e}); shipping unsigned\n")

INSTALLER_SUFFIXES = (".AppImage", ".dmg", "-setup.exe")
def is_installer(name):
    return any(name.endswith(s) for s in INSTALLER_SUFFIXES)

def blob_url(sha, name):
    return (
        f"{host}/api/repo/{quote(repo_owner, safe='')}/"
        f"{quote(repo_name, safe='')}/releases/blob/sha256/{sha}"
        f"?filename={quote(name, safe='')}"
    )

def find_blob(sha):
    # Best-effort local blob lookup for signing; the served bytes are identical.
    for root in (os.environ.get("FORKMESH_RELEASE_CAS", ""), ".forkmesh/release-blobs"):
        if not root:
            continue
        p = os.path.join(root, "sha256", sha[:2], sha, "data")
        if os.path.isfile(p):
            return p
    return None

items = []
for a in m.get("assets", []):
    name = a.get("name", "")
    if not is_installer(name):
        continue
    sha = a.get("blob_sha256", "")
    size = a.get("size", 0)
    os_name = a.get("os", "")
    ed_sig_attr = ""
    if signer:
        blob = find_blob(sha)
        if blob:
            with open(blob, "rb") as f:
                sig = base64.b64encode(signer.sign(f.read())).decode()
            ed_sig_attr = f' sparkle:edSignature="{x.quoteattr(sig)[1:-1]}"'
        else:
            sys.stderr.write(f"[appcast] blob for {name} not found locally; enclosure left unsigned\n")
    title = x.escape(f"ForkMesh {version} ({os_name})")
    url = x.escape(blob_url(sha, name))
    items.append(f"""    <item>
      <title>{title}</title>
      <pubDate>{pubdate}</pubDate>
      <sparkle:version>{x.escape(version)}</sparkle:version>
      <sparkle:shortVersionString>{x.escape(version)}</sparkle:shortVersionString>
      <enclosure url="{url}" sparkle:version="{x.escape(version)}" length="{size}" type="application/octet-stream"{ed_sig_attr} />
    </item>""")

doc = f"""<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle" xmlns:dc="http://purl.org/dc/elements/1.1/">
  <channel>
    <title>ForkMesh ({x.escape(channel)})</title>
    <description>ForkMesh desktop client updates</description>
    <language>en</language>
{chr(10).join(items)}
  </channel>
</rss>
"""
with open(out_path, "w") as f:
    f.write(doc)
sys.stderr.write(f"[appcast] wrote {out_path} with {len(items)} installer item(s)\n")
PY

log "done: $out"
echo "$out"
