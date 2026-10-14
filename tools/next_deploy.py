#!/usr/bin/env python3
"""Build and roll out a ForkMesh Next app release to the core nodes.

    next_deploy.py build     [--execute]            # local build -> tarball
    next_deploy.py upload    [--execute] [nN ...]   # to releases/<rev> on nodes
    next_deploy.py migrate   [--execute]            # rqlite_migrate via any healthy node
    next_deploy.py restart   [--execute] [nN ...]   # rolling, one node at a time
    next_deploy.py all       [--execute]            # build, upload, migrate, restart
    next_deploy.py rollback  nN [--execute]         # relink the previous release

**Dry-run is the default**: without ``--execute`` every step is printed.

A release is ``/opt/forkmesh-next/app/releases/<rev>`` holding the built app
(``app/host``, ``app/src_build``, ``app/dist``, ``app/wrangler.toml``), the
built World (``world/dist``, api-origin meta injected; the app host answers
world.next ``/health/mobile`` from these files), a ``.venv`` installed offline from the bundled wheelhouse,
the Caddy route snippets for that release, ``BUILD_REV`` and
``release.env``. ``current`` is a symlink; the previous target is kept in
``/opt/forkmesh-next/app/previous``.

Each node restart is gated (over SSH, on the node) on
``127.0.0.1:8080/readyz`` and ``/api/version`` reporting this BUILD_REV, then
on Caddy serving ``https://nN.<domain>/readyz`` on loopback (up to 30 s, and
30 s more after one Caddy restart); a node that fails the gate is relinked to
its previous release and the rollout stops.
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tarfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import forkmesh_safe_delete  # noqa: E402
import next_common as common  # noqa: E402
from next_common import GuardError, ToolError  # noqa: E402

REPO = common.REPO
RELEASES = "/opt/forkmesh-next/app/releases"
CURRENT = "/opt/forkmesh-next/app/current"
PREVIOUS = "/opt/forkmesh-next/app/previous"
VENVS = "/opt/forkmesh-next/app/venvs"
WHEELHOUSES = "/opt/forkmesh-next/app/wheelhouses"
KEEP_RELEASES = 5
PRECOMPRESS_SUFFIXES = {".html", ".js", ".mjs", ".css", ".json", ".svg", ".xml",
                        ".txt", ".map", ".webmanifest", ".wasm"}
PRECOMPRESS_MIN_BYTES = 1024
WHEEL_PLATFORMS = ("manylinux_2_28_x86_64", "manylinux_2_17_x86_64",
                   "manylinux2014_x86_64", "any")
REV_RE = __import__("re").compile(r"^[0-9a-f]{12}$")


def dist_dir() -> Path:
    configured = os.environ.get("FMNEXT_DIST_DIR")
    if configured:
        return Path(configured)
    return common.data_dir() / "dist"


def git_rev(repo=REPO, allow_dirty=False) -> str:
    rev = subprocess.run(["git", "rev-parse", "HEAD"], cwd=repo, check=True,
                         capture_output=True, text=True).stdout.strip()[:12]
    dirty = subprocess.run(["git", "status", "--porcelain"], cwd=repo, check=True,
                           capture_output=True, text=True).stdout.strip()
    if dirty and not allow_dirty:
        raise ToolError("working tree is dirty; commit first or pass --allow-dirty")
    return rev


# --------------------------------------------------------------------------
# build (local)


def app_build_steps():
    """The app's [build] steps (app/host/forkmesh_host/serve.py BUILD_STEPS)
    plus the World site build (world/wrangler.toml [build])."""
    return [
        (REPO / "app", ["tools/build_dashboard_assets.py"]),
        (REPO / "app", ["tools/build_worker_python.py"]),
        (REPO / "app", ["tools/build_site_assets.py", "app"]),
        (REPO / "world", ["../app/tools/build_site_assets.py", "world"]),
    ]


def inject_world_meta(index: Path, api_origin: str, rev: str,
                      version: str = "", built_at: int = 0):
    html = index.read_text("utf-8")
    meta = ('<meta name="forkmesh-api-origin" content="%s">'
            '<meta name="forkmesh-world-deploy" content="%s">'
            '<meta name="forkmesh-world-version" content="%s">'
            '<meta name="forkmesh-world-built" content="%d">'
            '<link rel="preconnect" href="%s" crossorigin>' % (
                api_origin, rev, version, int(built_at), api_origin))
    if 'name="forkmesh-api-origin"' in html:
        return
    marker = html.lower().find("<head")
    if marker < 0:
        raise ToolError("world index.html has no <head>")
    close = html.index(">", marker) + 1
    index.write_text(html[:close] + meta + html[close:], "utf-8")


NODE_BROTLI = (
    "const fs=require('fs'),zlib=require('zlib');"
    "for(const file of JSON.parse(fs.readFileSync(0,'utf8'))){"
    "const data=fs.readFileSync(file);"
    "const packed=zlib.brotliCompressSync(data,{params:{"
    "[zlib.constants.BROTLI_PARAM_QUALITY]:11,"
    "[zlib.constants.BROTLI_PARAM_SIZE_HINT]:data.length}});"
    "if(packed.length<data.length)fs.writeFileSync(file+'.br',packed);}"
)


def node_binary():
    found = shutil.which("node")
    if found:
        return found
    for candidate in sorted(
            (Path.home() / ".nvm" / "versions" / "node").glob("v*/bin/node"),
            reverse=True):
        return str(candidate)
    return None


def node_brotli(paths, run=subprocess.run) -> bool:
    node = node_binary()
    if not node or not paths:
        return False
    completed = run([node, "-e", NODE_BROTLI],
                    input=json.dumps([str(path) for path in paths]),
                    capture_output=True, text=True, timeout=900)
    return completed.returncode == 0


def precompress(root: Path, run=subprocess.run) -> int:
    try:
        import brotli  # type: ignore
    except ImportError:
        brotli = None
    count = 0
    pending_brotli = []
    for path in sorted(root.rglob("*")):
        if not path.is_file() or path.suffix not in PRECOMPRESS_SUFFIXES:
            continue
        data = path.read_bytes()
        if len(data) < PRECOMPRESS_MIN_BYTES:
            continue
        packed = gzip.compress(data, compresslevel=9, mtime=0)
        if len(packed) < len(data):
            path.with_name(path.name + ".gz").write_bytes(packed)
            count += 1
        if brotli is not None:
            packed = brotli.compress(data, quality=11)
            if len(packed) < len(data):
                path.with_name(path.name + ".br").write_bytes(packed)
        else:
            pending_brotli.append(path)
    node_brotli(pending_brotli, run=run)
    return count


def copy_tree(source: Path, target: Path):
    shutil.copytree(source, target, ignore=shutil.ignore_patterns(
        "__pycache__", "*.pyc", "tests", "tests_rqlite", ".pytest_cache"))


def stage_release(rev, stage: Path, fleet, run=subprocess.run):
    import caddy_routes
    import render
    if stage.exists():
        raise ToolError("%s exists; remove it first" % stage)
    for cwd, step in app_build_steps():
        run([sys.executable, *step], cwd=cwd, check=True)
    app = stage / "app"
    app.mkdir(parents=True)
    copy_tree(REPO / "app" / "host", app / "host")
    copy_tree(REPO / "app" / "src_build", app / "src_build")
    shutil.copytree(REPO / "app" / "dist", app / "dist")
    shutil.copy2(REPO / "app" / "wrangler.toml", app / "wrangler.toml")
    world = stage / "world" / "dist"
    shutil.copytree(REPO / "world" / "dist", world)
    inject_world_meta(world / "world" / "index.html",
                      "https://%s" % render.served_app_host(fleet), rev,
                      common.release_version(), int(time.time()))
    precompress(app / "dist")
    precompress(world)
    caddy = stage / "caddy"
    caddy.mkdir()
    (caddy / "app-routes.caddy").write_text(caddy_routes.generate(
        "app", REPO, dist=app / "dist", root=CURRENT + "/app/dist"))
    (caddy / "world-routes.caddy").write_text(caddy_routes.generate(
        "world", REPO, dist=world, root=CURRENT + "/world/dist",
        app_root=CURRENT + "/app/dist", app_dist=app / "dist"))
    (stage / "WHEELHOUSE").write_text(cached_wheelhouse(run).name + "\n")
    stage_join_tree(stage)
    (stage / "BUILD_REV").write_text(rev + "\n")
    # The app validates DEPLOY_FINGERPRINT as 64 lowercase hex (public
    # snapshot rows are keyed on it): the same input digest deploy.sh stamps.
    fingerprint = deploy_fingerprint()
    (stage / "release.env").write_text(
        "FORKMESH_VAR_BUILD_REV=%s\nFORKMESH_VAR_DEPLOY_FINGERPRINT=%s\n"
        "FORKMESH_VAR_APP_VERSION=%s\n" % (
            rev, fingerprint, common.release_version()))


def wheelhouse_key(requirements=None) -> str:
    requirements = Path(requirements or REPO / "app/host/requirements.txt")
    material = "3.13\ncp\n%s\n%s" % ("\n".join(WHEEL_PLATFORMS),
                                     requirements.read_text("utf-8"))
    return hashlib.sha256(material.encode("utf-8")).hexdigest()[:16]


WHEELHOUSE_TARBALL_LOCK = threading.Lock()


def wheelhouse_tarball(wheels: Path) -> Path:
    tarball = wheels.parent / (wheels.name + ".tar")
    with WHEELHOUSE_TARBALL_LOCK:
        if not tarball.is_file():
            tmp = tarball.with_name(".%s.%d.%d" % (tarball.name, os.getpid(),
                                                   threading.get_ident()))
            with tarfile.open(tmp, "w") as archive:
                for wheel in sorted(wheels.glob("*.whl")):
                    archive.add(wheel, arcname=wheel.name)
            os.replace(tmp, tarball)
    return tarball


def wheelhouse_script(key):
    if not re.fullmatch(r"[0-9a-f]{16}", key):
        raise GuardError("bad wheelhouse key %r" % key)
    return """
install -d -m 0755 {root}
cd {root}
if [ -f {key}/.complete ]; then echo 'wheelhouse {key} present'; rm -f .incoming-{key}.tar; exit 0; fi
if [ ! -f .incoming-{key}.tar ]; then echo need; exit 0; fi
rm -rf .tmp-{key}
install -d -m 0755 .tmp-{key}
tar -xf .incoming-{key}.tar -C .tmp-{key}
chmod -R go-w,a+rX .tmp-{key}
touch .tmp-{key}/.complete
rm -rf {key}
mv -T .tmp-{key} {key}
rm -f .incoming-{key}.tar
echo 'wheelhouse {key} installed'
""".format(root=WHEELHOUSES, key=key)


def cached_wheelhouse(run=subprocess.run) -> Path:
    import tempfile
    requirements = REPO / "app/host/requirements.txt"
    cache = dist_dir() / "wheelhouse"
    target = cache / wheelhouse_key(requirements)
    if target.is_dir() and any(target.glob("*.whl")):
        return target
    cache.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".download-", dir=cache) as tmp:
        download = Path(tmp) / "wheels"
        command = [sys.executable, "-m", "pip", "download", "--quiet", "--dest",
                   str(download), "--only-binary=:all:", "--python-version", "3.13",
                   "--implementation", "cp", "-r", str(requirements)]
        for platform in WHEEL_PLATFORMS:
            command += ["--platform", platform]
        run(command, check=True)
        if target.is_dir():
            forkmesh_safe_delete.remove_tree(target, cache)
        download.rename(target)
    return target


JOIN_TOOLS = ("next_fleet.py", "next_common.py", "next_deploy.py",
              "forkmesh_paths.py")
JOIN_DEPLOY = ("core", "runner")


def public_fleet_state():
    state = common.load_json(common.config_dir() / "fleet-state.json", {}) or {}
    return {key: dict(state.get(key) or {}) for key in ("wg_pubkeys", "ids_pubkeys")}


def public_fleet_instances():
    data = common.load_json(common.config_dir() / "fleet-instances.json", {}) or {}
    return {label: {key: info[key] for key in ("ip", "ipv6") if info.get(key)}
            for label, info in sorted(data.items()) if isinstance(info, dict)}


def stage_join_tree(stage, state=None, instances=None):
    deploy = stage / "deploy"
    deploy.mkdir(parents=True, exist_ok=True)
    for name in JOIN_DEPLOY:
        shutil.copytree(REPO / "deploy" / name, deploy / name,
                        ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    shutil.copy2(REPO / "deploy" / "fleet.toml", deploy / "fleet.toml")
    (deploy / "fleet-state.public.json").write_text(json.dumps(
        public_fleet_state() if state is None else state, indent=2,
        sort_keys=True) + "\n")
    (deploy / "fleet-instances.public.json").write_text(json.dumps(
        public_fleet_instances() if instances is None else instances, indent=2,
        sort_keys=True) + "\n")
    (stage / "world").mkdir(exist_ok=True)
    shutil.copy2(REPO / "world" / "wrangler.toml", stage / "world" / "wrangler.toml")
    tools = stage / "tools"
    tools.mkdir(exist_ok=True)
    for name in JOIN_TOOLS:
        shutil.copy2(REPO / "tools" / name, tools / name)


def deploy_fingerprint() -> str:
    sys.path.insert(0, str(REPO / "app" / "tools"))
    try:
        import deploy_targets
    finally:
        sys.path.pop(0)
    value = deploy_targets.fingerprint("app")
    if not (len(value) == 64 and all(c in "0123456789abcdef" for c in value)):
        raise RuntimeError("deploy fingerprint is not 64 hex: %r" % value)
    return value


def pack(stage: Path, tarball: Path) -> str:
    with tarfile.open(tarball, "w:gz") as archive:
        archive.add(stage, arcname=".")
    return hashlib.sha256(tarball.read_bytes()).hexdigest()


def sha_path(tarball) -> Path:
    tarball = Path(tarball)
    return tarball.with_name(tarball.name[:-len(".tar.gz")] + ".sha256")


DIST_RELEASE_RE = re.compile(r"^fmnext-[0-9a-f]{12}\.tar\.gz$")


def prune_dist(out, keep=(), keep_n=KEEP_RELEASES):
    out = Path(out)
    tarballs = []
    for entry in out.iterdir() if out.is_dir() else ():
        if DIST_RELEASE_RE.match(entry.name) and entry.is_file() and not entry.is_symlink():
            tarballs.append((entry.stat().st_mtime, entry))
    tarballs.sort(reverse=True)
    removed = []
    for _, tarball in tarballs[keep_n:]:
        if tarball.name in keep:
            continue
        for path in (tarball, sha_path(tarball)):
            try:
                path.unlink()
            except FileNotFoundError:
                pass
        removed.append(tarball.name)
    return removed


def build(fleet, runner, allow_dirty=False, run=subprocess.run):
    import tempfile
    rev = git_rev(allow_dirty=allow_dirty)
    out = dist_dir()
    tarball = out / ("fmnext-%s.tar.gz" % rev)

    def action():
        out.mkdir(parents=True, exist_ok=True)
        # A fresh mkdtemp directory, removed by TemporaryDirectory itself.
        with tempfile.TemporaryDirectory(prefix="stage-", dir=out) as tmp:
            stage = Path(tmp) / "release"
            stage_release(rev, stage, fleet, run)
            digest = pack(stage, tarball)
        sha_path(tarball).write_text(digest + "\n")
        prune_dist(out, keep={tarball.name})
        return digest
    runner.step("build %s: app build steps, world build, precompress, Caddy routes, "
                "wheelhouse -> %s" % (rev, tarball), action)
    return rev, tarball


# --------------------------------------------------------------------------
# remote steps


def upload_script(rev, sha256):
    if not REV_RE.match(rev):
        raise GuardError("bad release id %r" % rev)
    return """
cd {releases}
if [ -s {rev}/BUILD_REV ]; then
  if [ -f .incoming-{rev}.tar.gz ] && [ ! -f {rev}.tar.gz ]; then
    echo '{sha}  .incoming-{rev}.tar.gz' | sha256sum -c - && mv -f .incoming-{rev}.tar.gz {rev}.tar.gz \
      && chmod 0644 {rev}.tar.gz && echo '{sha}  {rev}.tar.gz' > {rev}.tar.gz.sha256 && chmod 0644 {rev}.tar.gz.sha256
  fi
  rm -f .incoming-{rev}.tar.gz
  echo 'release {rev} already present'; exit 0
fi
echo '{sha}  .incoming-{rev}.tar.gz' | sha256sum -c -
rm -rf .tmp-{rev}
install -d -m 0755 .tmp-{rev}
tar -xzf .incoming-{rev}.tar.gz -C .tmp-{rev}
wheels={wheelhouses}/$(cat .tmp-{rev}/WHEELHOUSE)
[ -f "$wheels/.complete" ] || {{ echo "wheelhouse $wheels missing" >&2; exit 4; }}
req=$( (cat .tmp-{rev}/app/host/requirements.txt; ls -1 "$wheels" | grep -v '^\\.'; python3 -V) | sha256sum | cut -c1-16)
venv={venvs}/$req
if [ ! -f "$venv/.complete" ]; then
  rm -rf "$venv"
  install -d -m 0755 {venvs}
  python3 -m venv "$venv"
  "$venv/bin/pip" install --quiet --no-index --find-links "$wheels" \\
    -r .tmp-{rev}/app/host/requirements.txt
  chown -R root:root "$venv"
  chmod -R go-w,a+rX "$venv"
  touch "$venv/.complete"
  echo "venv $req built"
else
  echo "venv $req reused"
fi
ln -sfn "$venv" .tmp-{rev}/.venv
"$venv/bin/python" -m compileall -q -o 2 .tmp-{rev}/app/host .tmp-{rev}/app/src_build
chown -R root:root .tmp-{rev}
chmod -R go-w,a+rX .tmp-{rev}
mv -T .tmp-{rev} {rev}
mv -f .incoming-{rev}.tar.gz {rev}.tar.gz
chmod 0644 {rev}.tar.gz
echo '{sha}  {rev}.tar.gz' > {rev}.tar.gz.sha256
chmod 0644 {rev}.tar.gz.sha256
ls -1t *.tar.gz 2>/dev/null | tail -n +4 | while read old; do rm -f -- "$old" "$old.sha256"; done
keep=" {rev} $(basename "$(readlink -f {current})") $(basename "$(cat {previous} 2>/dev/null)") "
{{ ls -1dt -- */ 2>/dev/null || true; }} | sed 's#/$##' | {{ grep -E '^[0-9a-f]{{12}}$' || true; }} | tail -n +{keep_n} | while read old; do
  case "$keep" in *" $old "*) continue ;; esac
  rm -rf -- "$old"
done
""".format(releases=RELEASES, rev=rev, sha=sha256, venvs=VENVS, wheelhouses=WHEELHOUSES,
           current=CURRENT, previous=PREVIOUS, keep_n=KEEP_RELEASES + 1)


# Vault on (docs/VAULT.md): DATA_KEY arrives as the first stdin line, is kept
# in an unexported shell variable (printf is a bash builtin: never an argv or
# the environment) and is handed to the restarted app's unseal socket.
VAULT_READ_KEY = """
IFS= read -r fmnext_key || true
[ -n "$fmnext_key" ] || { echo "vault: no DATA_KEY on stdin" >&2; exit 5; }
unseal_app() {
  printf '%s\\n' "$fmnext_key" | /usr/local/sbin/fmnext-unseal --wait 60 >/dev/null \\
    || echo "vault: unseal after restart failed" >&2
}
"""


ACTIVATION_LOCK = "/run/fmnext-activate.lock"
ACTIVATION_GUARD = """
set +e +o pipefail
trap '' HUP PIPE
exec 9>{lock}
flock -w 900 9 || {{ echo "another activation holds {lock}" >&2; exit 5; }}
""".format(lock=ACTIVATION_LOCK)
STATE_SCRIPT = ACTIVATION_GUARD + """
current=$(cat {current}/BUILD_REV 2>/dev/null | tr -d ' \\n')
running=$(curl -fsS --max-time 5 http://127.0.0.1:8080/api/version 2>/dev/null |
  python3 -c 'import json,sys; print(json.load(sys.stdin).get("rev") or "")' 2>/dev/null)
app=$(systemctl is-active forkmesh-app.service 2>/dev/null)
python3 -c 'import json,sys; print(json.dumps(dict(zip(("current","running","app"),sys.argv[1:]))))' \
  "$current" "$running" "$app"
""".format(current=CURRENT)
HOST_RE = __import__("re").compile(
    r"^(?=.{1,253}$)[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?"
    r"(?:\.[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?)+$")
CADDY_WAIT = 30
SETTLE_SECONDS = 3


def node_host(fleet, name):
    if str(name).startswith("a"):
        import render
        return render.served_app_host(fleet)
    return "%s.%s" % (name, fleet["domain"])


def serving_nodes(fleet):
    return list(fleet.get("nodes") or []) + list(fleet.get("app_nodes") or [])


def caddy_functions(host, wait=CADDY_WAIT, max_time=3):
    if not HOST_RE.match(str(host)):
        raise GuardError("bad node host name %r" % host)
    return """
caddy_serving() {{
  for i in $(seq 1 {wait}); do
    if systemctl is-active --quiet caddy &&
       curl -ks -o /dev/null --max-time {max_time} --resolve {host}:443:127.0.0.1 https://{host}/readyz
    then return 0; fi
    sleep 1
  done
  return 1
}}
caddy_ready() {{
  for i in $(seq 1 {wait}); do
    if systemctl is-active --quiet caddy &&
       curl -kfs -o /dev/null --max-time 3 --resolve {host}:443:127.0.0.1 https://{host}/readyz
    then return 0; fi
    sleep 1
  done
  return 1
}}
reload_caddy() {{
  if ! caddy validate --config /etc/caddy/Caddyfile --adapter caddyfile >/dev/null 2>&1; then
    echo "caddy: the new routes do not validate; the running config stays" >&2
    return 0
  fi
  timeout 60 systemctl reload caddy || echo "caddy: reload failed or timed out" >&2
  if ! caddy_serving; then
    echo "caddy: not serving {wait} s after the reload; restarting it" >&2
    systemctl restart caddy || echo "caddy: restart failed" >&2
    caddy_serving || echo "caddy: still not serving after a restart" >&2
  fi
  return 0
}}
caddy_gate() {{
  caddy_ready && return 0
  echo "caddy: {host}/readyz did not answer within {wait} s; restarting caddy" >&2
  systemctl restart caddy || echo "caddy: restart failed" >&2
  caddy_ready
}}
""".format(host=host, wait=int(wait), max_time=int(max_time))


def activate_script(rev, host, timeout=90, vault=False):
    if not REV_RE.match(rev):
        raise GuardError("bad release id %r" % rev)
    return (VAULT_READ_KEY if vault else "unseal_app() { :; }\n") + \
        ACTIVATION_GUARD + caddy_functions(host) + """
set -u
new={releases}/{rev}
[ -s "$new/BUILD_REV" ] || {{ echo "release {rev} is not uploaded" >&2; exit 3; }}
prev=$(readlink {current} || true)
if [ -n "$prev" ] && [ "$prev" != "$new" ]; then echo "$prev" >{previous}; fi
switch_to() {{
  if ! {{ ln -sfn "$1" {current}.new && mv -T {current}.new {current}; }}; then
    echo "could not point {current} at $1" >&2
    return 1
  fi
  install -m 0644 "$1/caddy/app-routes.caddy" /etc/forkmesh-next/caddy/app-routes.caddy \
    || echo "could not install the app routes of $1" >&2
  install -m 0644 "$1/caddy/world-routes.caddy" /etc/forkmesh-next/caddy/world-routes.caddy \
    || echo "could not install the world routes of $1" >&2
  reload_caddy
  systemctl restart forkmesh-app.service || echo "forkmesh-app restart failed" >&2
  unseal_app
  return 0
}}
gate() {{
  want=$(cat "$1/BUILD_REV")
  for i in $(seq 1 {timeout}); do
    if curl -fsS --max-time 3 http://127.0.0.1:8080/readyz >/dev/null 2>&1 &&
       curl -fsS --max-time 3 http://127.0.0.1:8080/api/version 2>/dev/null |
         python3 -c 'import json,sys; sys.exit(0 if json.load(sys.stdin).get("rev")==sys.argv[1] else 1)' "$want"
    then caddy_gate; return; fi
    sleep 1
  done
  return 1
}}
if switch_to "$new" && gate "$new"; then unset fmnext_key; echo "activated {rev}"; exit 0; fi
echo "release {rev} failed its gate; rolling back to ${{prev:-nothing}}" >&2
if [ -n "$prev" ] && [ "$prev" != "$new" ]; then
  switch_to "$prev"
  gate "$prev" && echo "rolled back to $prev" >&2
fi
unset fmnext_key
exit 4
""".format(releases=RELEASES, current=CURRENT, previous=PREVIOUS, rev=rev,
           timeout=int(timeout))


def rollback_script(host):
    return ACTIVATION_GUARD + caddy_functions(host) + """
set -u
prev=$(cat {previous} 2>/dev/null || true)
case "$prev" in {releases}/*) ;; *) echo 'no previous release recorded' >&2; exit 3;; esac
[ -s "$prev/BUILD_REV" ] || {{ echo "$prev is gone" >&2; exit 3; }}
cur=$(readlink {current} || true)
ln -sfn "$prev" {current}.new && mv -T {current}.new {current}
[ -n "$cur" ] && echo "$cur" >{previous}
install -m 0644 "$prev/caddy/app-routes.caddy" /etc/forkmesh-next/caddy/app-routes.caddy
install -m 0644 "$prev/caddy/world-routes.caddy" /etc/forkmesh-next/caddy/world-routes.caddy
reload_caddy
systemctl restart forkmesh-app.service || echo "forkmesh-app restart failed" >&2
echo "now on $prev"
""".format(previous=PREVIOUS, releases=RELEASES, current=CURRENT)


def rqlite_status(ssh, ip):
    out = ssh.run(ip, common.rqlite_curl("/status"), timeout=30).stdout
    return json.loads(out)


def rqlite_role(ssh, ip):
    store = rqlite_status(ssh, ip).get("store") or {}
    state = ((store.get("raft") or {}).get("state") or "").lower()
    return "leader" if state == "leader" else (state or "unknown")


def cluster_has_leader(status) -> bool:
    """This rqlite node is a voter of a cluster that has a leader."""
    store = (status or {}).get("store") or {}
    state = ((store.get("raft") or {}).get("state") or "").lower()
    if state == "leader":
        return True
    leader = store.get("leader") or {}
    return state == "follower" and bool(leader.get("node_id")
                                        or leader.get("addr"))


def find_rqlite_node(fleet, ssh):
    """The first core node (fleet order) whose rqlite is healthy -- part of
    a cluster with a leader. Any one will do: rqlite forwards writes and
    strong reads to its leader, so no tool needs to find the leader."""
    for node in fleet.get("nodes") or []:
        try:
            if cluster_has_leader(rqlite_status(ssh, node["public_ipv4"])):
                return node
        except (ToolError, ValueError, subprocess.TimeoutExpired):
            continue
    raise ToolError("no core node has a healthy rqlite (a cluster with a "
                    "leader)")


def find_leader(fleet, ssh):
    """The node holding rqlite's leader (backups read its snapshot)."""
    for node in fleet.get("nodes") or []:
        try:
            if rqlite_role(ssh, node["public_ipv4"]) == "leader":
                return node
        except (ToolError, ValueError, subprocess.TimeoutExpired):
            continue
    raise ToolError("no rqlite leader found on any core node")


def rollout_order(fleet, leader_name=None, rev=None):
    """Fleet order, or with ``rev`` an order rotated per release. Only the
    app restarts (rqlite keeps running), so the rqlite leader's node needs no
    special place in the rollout."""
    del leader_name
    nodes = serving_nodes(fleet)
    if not rev:
        return nodes
    return sorted(nodes, key=lambda n: (hashlib.sha256(
        ("%s:%s" % (rev, n["name"])).encode("utf-8")).hexdigest(), n["name"]))


LOOPBACK_HOSTS = ("127.0.0.1", "::1", "127.0.0.2")
LOOPBACK_CHECK = ("python3 -c 'import socket; s=socket.socket(); s.bind((\"127.0.0.1\",0)); "
                  "s.listen(); socket.create_connection(s.getsockname(), timeout=2)'")


def _family(host):
    return socket.AF_INET6 if ":" in host else socket.AF_INET


def loopback_accepts(host, timeout=1.0):
    try:
        with socket.socket(_family(host)) as listener:
            listener.bind((host, 0))
            listener.listen(1)
            socket.create_connection(listener.getsockname()[:2], timeout=timeout).close()
        return True
    except OSError:
        return False


def loopback_host():
    for host in LOOPBACK_HOSTS:
        if loopback_accepts(host):
            return host
    raise ToolError(
        "this machine drops TCP to its own loopback (tried %s), so no SSH tunnel "
        "to rqlite can be reached. Cause: a local packet filter on this machine "
        "(OpenSnitch runs here), not the fleet. Check: %s. Fix: allow loopback "
        "TCP for this user in OpenSnitch, or remove the deny rule shown by "
        "sudo nft list ruleset, then run the deploy again" % (
            ", ".join(LOOPBACK_HOSTS), LOOPBACK_CHECK))


def free_port(host="127.0.0.1"):
    with socket.socket(_family(host)) as sock:
        sock.bind((host, 0))
        return sock.getsockname()[1]


def open_rqlite_tunnel(ssh, node, wait=True):
    host = loopback_host()
    port = free_port(host)
    tunnel = ssh.tunnel(node["public_ipv4"], port, bind=host)

    def close():
        tunnel.terminate()
        tunnel.wait(timeout=10)
    try:
        if wait and not common.wait_for(lambda: _port_open(port, host), 15, interval=0.3):
            raise tunnel_error(node["name"], tunnel, port, host)
    except BaseException:
        close()
        raise
    return "http://%s:%d" % ("[%s]" % host if ":" in host else host, port), close


def migrate_step(fleet, ssh, run=subprocess.run):
    def action():
        target = find_rqlite_node(fleet, ssh)
        values = common.read_env_file(common.config_dir() / "secrets.env")
        url, close = open_rqlite_tunnel(ssh, target)
        try:
            env = dict(os.environ,
                       RQLITE_USERNAME=(fleet.get("rqlite") or {}).get("user", "fmnext"),
                       RQLITE_PASSWORD=values.get("RQLITE_PASSWORD", ""))
            run([sys.executable, str(REPO / "tools" / "rqlite_migrate.py"),
                 "--url", url], env=env, check=True)
        finally:
            close()
    return ("pick the first core node with a healthy rqlite, tunnel to its "
            "127.0.0.1:4001 from a loopback address this machine accepts and run tools/rqlite_migrate.py (rqlite forwards "
            "the writes; credentials via environment)", action)


def migrate(fleet, ssh, runner, run=subprocess.run):
    runner.step(*migrate_step(fleet, ssh, run))


WORLD_DEPLOY_SQL = (
    "INSERT INTO world_deploy_status(singleton,state,revision,started_at,finished_at) "
    "VALUES(1,?,?,?,?) ON CONFLICT(singleton) DO UPDATE SET state=excluded.state,"
    "revision=excluded.revision,started_at=CASE WHEN excluded.state='deploying' "
    "THEN excluded.started_at ELSE world_deploy_status.started_at END,"
    "finished_at=excluded.finished_at")


def world_deploy_body(state, rev, now_ms):
    started = now_ms if state == "deploying" else 0
    finished = 0 if state == "deploying" else now_ms
    return json.dumps([[WORLD_DEPLOY_SQL, state, rev, started, finished]])


def signal_world_deploy(fleet, ssh, runner, state, rev, clock=None):
    import time

    def action():
        target = find_rqlite_node(fleet, ssh)
        now_ms = int((clock or time.time)() * 1000)
        completed = ssh.run(target["public_ipv4"], common.rqlite_curl(
            "/db/execute", "POST", "-"), input_bytes=world_deploy_body(
                state, rev, now_ms).encode(), check=False, timeout=60)
        try:
            body = json.loads(completed.stdout or b"{}")
            error = ((body.get("results") or [{}])[0]).get("error")
        except ValueError:
            error = "unreadable rqlite answer"
        if completed.returncode != 0 or error:
            raise ToolError(error or "rqlite exited %d" % completed.returncode)
    try:
        runner.step("World deploy notice: %s %s" % (state, rev), action)
    except (ToolError, OSError, subprocess.TimeoutExpired) as error:
        runner.say("World deploy notice not written (%s); the deploy goes on" % error)


def _port_open(port, host="127.0.0.1"):
    try:
        socket.create_connection((host, port), timeout=0.3).close()
        return True
    except OSError:
        return False


SSH_EXIT_FIXES = (
    ("Permission denied", "the node refused the fleet SSH key; check that "
     "~/projects/forkmesh/ssh/fleet/next_fleet_ed25519 is the key in the node's "
     "/root/.ssh/authorized_keys (FMNEXT_SSH_KEY overrides it)"),
    ("Host key verification failed", "the node's host key changed; if the node "
     "was rebuilt, remove its line from ~/projects/forkmesh/config/fleet/known_hosts"),
    ("REMOTE HOST IDENTIFICATION HAS CHANGED", "the node's host key changed; if "
     "the node was rebuilt, remove its line from "
     "~/projects/forkmesh/config/fleet/known_hosts"),
    ("timed out", "the node did not answer on port 22; check that it is up "
     "(python3 tools/next_fleet.py list) and that this machine has network"),
    ("No route to host", "the node did not answer on port 22; check that it is "
     "up (python3 tools/next_fleet.py list) and that this machine has network"),
    ("Connection refused", "sshd on the node is not listening; check the node's "
     "console in Vultr"),
    ("Could not resolve", "DNS failed on this machine; check its network"),
    ("cannot listen", "another process took the local port first; run the "
     "deploy again"),
    ("Address already in use", "another process took the local port first; run "
     "the deploy again"),
)


def tunnel_error(name, tunnel, port, host="127.0.0.1"):
    code = tunnel.poll()
    address = "[%s]:%d" % (host, port) if ":" in host else "%s:%d" % (host, port)
    if code is None:
        return ToolError(
            "SSH tunnel to %s is up, but this machine does not accept "
            "connections to its own %s. Cause: a local firewall dropping "
            "loopback TCP (OpenSnitch runs here), not the fleet. Check: %s. "
            "Fix: allow loopback TCP for this user in the firewall, then run "
            "the deploy again" % (name, address, LOOPBACK_CHECK))
    stderr = tunnel.stderr.read() if tunnel.stderr else b""
    tail = (stderr or b"")[-500:].decode("utf-8", "replace").strip()
    message = "SSH tunnel to %s exited %d: %s" % (name, code, tail or "no output")
    for needle, fix in SSH_EXIT_FIXES:
        if needle in tail:
            return ToolError("%s. Fix: %s" % (message.rstrip("."), fix))
    return ToolError(message)


def upload(fleet, rev, tarball, ssh, runner, names=None):
    nodes = [n for n in serving_nodes(fleet)
             if not names or n["name"] in names]
    sha = None
    if runner.execute:
        sha = hashlib.sha256(Path(tarball).read_bytes()).hexdigest()
        recorded = sha_path(tarball)
        if recorded.exists() and recorded.read_text().strip() != sha:
            raise GuardError("%s does not match its .sha256" % Path(tarball).name)

    wheels = cached_wheelhouse() if runner.execute else None

    def one(node):
        def action():
            ip = node["public_ipv4"]
            script = wheelhouse_script(wheels.name)
            if ssh.run(ip, script).stdout.strip().endswith(b"need"):
                ssh.put(ip, wheelhouse_tarball(wheels),
                        "%s/.incoming-%s.tar" % (WHEELHOUSES, wheels.name))
                ssh.run(ip, script)
            ssh.put(ip, tarball, "%s/.incoming-%s.tar.gz" % (RELEASES, rev))
            ssh.run(ip, upload_script(rev, sha))
        runner.step("%s: upload %s to %s/%s, venv from the wheelhouse" % (
            node["name"], Path(tarball).name, RELEASES, rev), action)
    if not runner.execute or len(nodes) < 2:
        for node in nodes:
            one(node)
        return
    from concurrent.futures import ThreadPoolExecutor
    with ThreadPoolExecutor(max_workers=len(nodes)) as pool:
        for future in [pool.submit(one, node) for node in nodes]:
            future.result()


def vault_on(fleet):
    import render
    return render.vault_enabled(fleet)


def vault_key(fleet, runner):
    """DATA_KEY bytes for stdin, or None when the vault is off."""
    if not (runner.execute and vault_on(fleet)):
        return None
    import next_vault
    return (next_vault.load_data_key() + "\n").encode()


def node_state(ssh, ip):
    try:
        completed = ssh.run(ip, STATE_SCRIPT, check=False, timeout=1200)
    except (ToolError, subprocess.TimeoutExpired) as error:
        return {"error": str(error)}
    for line in reversed((completed.stdout or b"").decode(
            "utf-8", "replace").splitlines()):
        if line.startswith("{"):
            try:
                return json.loads(line)
            except ValueError:
                break
    return {"error": "no state (exit %d)" % completed.returncode}


def state_text(state):
    if state.get("error"):
        return state["error"]
    return "current %s, running %s, app %s" % (
        state.get("current") or "?", state.get("running") or "?",
        state.get("app") or "?")


def activated(state, rev):
    return state.get("current") == rev and state.get("running") == rev


def half_activated(state):
    return (not state.get("error") and state.get("current")
            and state.get("current") != state.get("running"))


def activate_node(fleet, node, rev, ssh, runner, vault, key):
    ip = node["public_ipv4"]
    script = activate_script(rev, node_host(fleet, node["name"]), vault=vault)
    for attempt in (1, 2):
        try:
            completed = ssh.run(ip, script, input_bytes=key, check=False,
                                timeout=1200)
            code = completed.returncode
        except (ToolError, subprocess.TimeoutExpired) as error:
            runner.say("%s: activation session ended early (%s); checking the "
                       "node" % (node["name"], error))
            code = None
        state = node_state(ssh, ip)
        runner.say("%s: %s" % (node["name"], state_text(state)))
        if activated(state, rev):
            if code != 0:
                runner.say("%s: the activation finished on the node" %
                           node["name"])
            return True
        if attempt == 1 and (half_activated(state) or code is None):
            runner.say("%s: the running app does not match current; "
                       "activating %s again" % (node["name"], rev))
            continue
        return state
    return state


def restart(fleet, rev, ssh, runner, names=None, parallel=False, settle=None):
    vault = vault_on(fleet)
    key = vault_key(fleet, runner)
    order = [n for n in rollout_order(fleet, rev=rev)
             if not names or n["name"] in names]
    canary, rest = order[:1], order[1:]
    together = parallel and runner.execute and len(rest) > 1
    runner.say("rollout order: %s" % (
        "%s first, then %s together" % (
            canary[0]["name"], ", ".join(n["name"] for n in rest))
        if together else "%s (one node at a time)" % ", ".join(
            n["name"] for n in order)))
    done = []
    import next_ids

    def pause():
        if runner.execute and SETTLE_SECONDS > 0:
            runner.say("waiting %d s for every core to see %s live before the "
                       "next node drains" % (SETTLE_SECONDS, done[-1]))
            (settle or time.sleep)(SETTLE_SECONDS)

    def one(node):
        next_ids.expect_activate(fleet, ssh, runner, node["name"], rev)

        def action():
            result = activate_node(fleet, node, rev, ssh, runner, vault, key)
            if result is not True:
                raise ToolError("%s failed its readiness/version gate (%s); "
                                "stopped after %s" % (
                                    node["name"], state_text(result),
                                    ", ".join(done) or "no node"))
            done.append(node["name"])
        runner.step("%s: switch current -> %s, reload Caddy routes, restart the app, "
                    "%sgate on /readyz + /api/version rev, then Caddy serving "
                    "/readyz (up to %d s, one Caddy restart); then check that "
                    "current and the running app are both %s" % (
                        node["name"], rev,
                        "unseal it (DATA_KEY on SSH stdin), " if vault else "",
                        CADDY_WAIT, rev), action)
    if not together:
        for index, node in enumerate(order):
            if index:
                pause()
            one(node)
        return done
    one(canary[0])
    pause()
    from concurrent.futures import ThreadPoolExecutor
    with ThreadPoolExecutor(max_workers=len(rest)) as pool:
        futures = [pool.submit(one, node) for node in rest]
    errors = [f.exception() for f in futures if f.exception() is not None]
    if errors:
        raise errors[0]
    return done


def main(argv=None, ssh=None, run=subprocess.run) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--fleet")
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("build", "upload", "migrate", "restart", "all"):
        p = sub.add_parser(name)
        p.add_argument("nodes", nargs="*")
        p.add_argument("--rev", help="release id (default: git HEAD, 12 hex)")
        p.add_argument("--allow-dirty", action="store_true")
        common.add_execute_flags(p)
    p = sub.add_parser("rollback")
    p.add_argument("node")
    common.add_execute_flags(p)
    args = parser.parse_args(argv)
    lock = None
    signalled = False
    try:
        runner = common.runner_from_args(args)
        if runner.execute:
            lock = common.DeployLock(what="next_deploy %s" % args.command).acquire()
        fleet = common.load_fleet(args.fleet)
        ssh = ssh or common.Ssh()
        known = {n["name"] for n in serving_nodes(fleet)}
        for name in getattr(args, "nodes", None) or ([args.node] if getattr(
                args, "node", None) else []):
            if name not in known:
                raise GuardError("%r is not a core node in the fleet file" % name)
        if args.command == "rollback":
            node = next(n for n in serving_nodes(fleet)
                        if n["name"] == args.node)
            import next_ids
            next_ids.expect_activate(fleet, ssh, runner, args.node, "rollback")
            runner.step("%s: relink the previous release and restart" % args.node,
                        lambda: ssh.run(node["public_ipv4"], rollback_script(
                            node_host(fleet, args.node))))
            if runner.execute:
                runner.say("%s: %s" % (args.node, state_text(node_state(
                    ssh, node["public_ipv4"]))))
            if vault_on(fleet):
                import next_vault
                return next_vault.unseal(fleet, ssh, runner, [args.node])
            return 0
        if args.command == "build":
            build(fleet, runner, args.allow_dirty, run)
            return 0
        rev = args.rev or git_rev(allow_dirty=True)
        if not REV_RE.match(rev):
            raise GuardError("--rev must be 12 lowercase hex characters")
        tarball = dist_dir() / ("fmnext-%s.tar.gz" % rev)
        if args.command == "all":
            rev, tarball = build(fleet, runner, args.allow_dirty, run)
        whole_fleet = not args.nodes
        if args.command in ("upload", "all"):
            if runner.execute and not tarball.is_file():
                raise ToolError("%s is missing; run build first" % tarball)
            upload(fleet, rev, tarball, ssh, runner, args.nodes)
        if args.command in ("migrate", "all"):
            if whole_fleet:
                signal_world_deploy(fleet, ssh, runner, "deploying", rev)
            signalled = whole_fleet
            migrate(fleet, ssh, runner, run)
        if args.command in ("restart", "all"):
            if whole_fleet and args.command == "restart":
                signal_world_deploy(fleet, ssh, runner, "deploying", rev)
            signalled = whole_fleet
            done = restart(fleet, rev, ssh, runner, args.nodes, parallel=True)
            if runner.execute and set(done) >= known:
                entry = record_release(rev, run=run)
                runner.say("recorded %s in %s (commit %s)" % (
                    rev, common.deploy_manifest_path(),
                    (entry.get("commit") or "unknown")[:12]))
                signal_world_deploy(fleet, ssh, runner, "ready", rev)
            signalled = False
        if not runner.execute:
            runner.say("(dry run: nothing was changed; pass --execute to apply)")
    except (ToolError, OSError, KeyError, subprocess.CalledProcessError) as error:
        print("next_deploy: %s" % error, file=sys.stderr)
        if signalled and runner.execute:
            signal_world_deploy(fleet, ssh, runner, "failed", rev)
        return 2 if isinstance(error, GuardError) else 1
    finally:
        if lock is not None:
            lock.release()
    return 0


def record_release(rev, repo=REPO, run=subprocess.run):
    completed = run(["git", "rev-parse", "--verify", "--quiet", rev + "^{commit}"],
                    cwd=repo, capture_output=True, text=True, check=False)
    commit = completed.stdout.strip() if completed.returncode == 0 else ""
    signature = ""
    fingerprint = ""
    if commit:
        try:
            signature = common.served_signature(commit, repo, run=run)
        except ToolError:
            signature = ""
        head = run(["git", "rev-parse", "HEAD"], cwd=repo, capture_output=True,
                   text=True, check=False).stdout.strip()
        if head == commit:
            try:
                fingerprint = deploy_fingerprint()
            except (RuntimeError, OSError, ImportError):
                fingerprint = ""
    return common.record_deploy(rev, commit=commit, signature=signature,
                                fingerprint=fingerprint, by="next_deploy")


if __name__ == "__main__":
    sys.exit(main())
