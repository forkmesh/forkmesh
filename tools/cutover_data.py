#!/usr/bin/env python3
"""The production cutover's data move (docs/CUTOVER.md, section "Data")."""

from __future__ import annotations

import argparse
import contextlib
import json
import os
import re
import shutil
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE))

PROD_ENV = Path.home() / "projects" / "forkmesh" / "src" / "app" / ".env.production"
D1_DATABASE = "forkmesh"

NEXT_TABLES = (
    "cluster_members", "do_storage", "do_alarms", "host_secrets",
    "runner_nodes", "runner_job_leases", "runner_availability",
    "host_cron_runs", "dns_health_votes", "dns_reconciler_state",
    "cluster_quarantine", "security_incidents", "ids_heartbeats",
    "marketplace_reviews", "marketplace_review_history",
    "marketplace_review_votes", "marketplace_review_reports",
    "marketplace_package_rating_stats", "marketplace_package_uses",
    "hosted_repos", "hosted_repo_copies",
)
NEXT_EPHEMERAL = ("runner_request_nonces", "runner_login_sessions",
                  "runner_login_chunks", "runner_account_requests",
                  "agent_stream_state", "agent_stream_chunks",
                  "agent_stream_watchers")
CORE_NODE_PREFIX = "fmnext-"


class CutoverError(RuntimeError):
    pass


def log(message):
    print(message, flush=True)


def private_file(path: Path):
    path = Path(path)
    if path.exists():
        raise CutoverError("%s already exists" % path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(str(path), os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    os.close(fd)
    return path


def read_env_values(path: Path, names):
    path = Path(path)
    info = path.stat()
    if info.st_mode & 0o077:
        raise CutoverError("%s is readable by group/other; chmod 0600 it" % path)
    values = {}
    for line in path.read_text("utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        key, value = key.strip(), value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "'\"":
            value = value[1:-1]
        if key in names:
            values[key] = value
    missing = [n for n in names if not values.get(n)]
    if missing:
        raise CutoverError("%s has no %s" % (path, ", ".join(missing)))
    return values


def wrangler_bin():
    configured = os.environ.get("FORKMESH_WRANGLER_BIN")
    if configured:
        return [configured]
    found = shutil.which("wrangler")
    if found:
        return [found]
    return ["npx", "--yes", "wrangler@4.136.3"]


def export(out: Path, execute=False, env_file=PROD_ENV, run=subprocess.run):
    out = Path(out)
    command = wrangler_bin() + ["d1", "export", D1_DATABASE, "--remote",
                                "--output", str(out)]
    log("export: %s -> %s (read-only; blocks D1 for its duration)"
        % (" ".join(command[:4] + ["--remote"]), out))
    if not execute:
        log("dry-run: pass --execute")
        return None
    creds = read_env_values(env_file, ("CLOUDFLARE_API_TOKEN",
                                       "CLOUDFLARE_ACCOUNT_ID"))
    env = {k: v for k, v in os.environ.items()
           if k not in ("CLOUDFLARE_API_TOKEN", "CLOUDFLARE_ACCOUNT_ID",
                        "FORKMESH_HOME", "FORKMESH_PROJECTS_DIR")}
    env.update(creds)
    private_file(out)
    out.unlink()
    old_umask = os.umask(0o077)
    started = time.monotonic()
    try:
        result = run(command, env=env, stdout=subprocess.DEVNULL,
                     stderr=subprocess.DEVNULL, timeout=1800, check=False)
    finally:
        os.umask(old_umask)
    seconds = time.monotonic() - started
    if result.returncode != 0 or not out.is_file():
        raise CutoverError("wrangler d1 export failed (exit %s); re-run it by "
                           "hand to see why" % result.returncode)
    os.chmod(out, 0o600)
    log("export: %d bytes in %.1f s" % (out.stat().st_size, seconds))
    return seconds


def dump_to_sqlite(dump: Path, out: Path):
    started = time.monotonic()
    private_file(out)
    conn = sqlite3.connect(str(out))
    try:
        conn.execute("PRAGMA journal_mode=OFF")
        conn.execute("PRAGMA synchronous=OFF")
        conn.executescript(Path(dump).read_text("utf-8"))
        conn.commit()
        check = conn.execute("PRAGMA integrity_check").fetchone()[0]
        users = conn.execute("SELECT COUNT(*) FROM users").fetchone()[0]
    finally:
        conn.close()
    if check != "ok":
        raise CutoverError("integrity_check: %s" % check)
    seconds = time.monotonic() - started
    log("sqlite: %s (%d users) in %.1f s" % (out, users, seconds))
    return seconds


def qi(name):
    return '"' + str(name).replace('"', '""') + '"'


def tables_of(conn):
    return {r[0] for r in conn.execute(
        "SELECT name FROM sqlite_master WHERE type='table'")}


def columns_of(conn, table):
    return [r[1] for r in conn.execute("PRAGMA table_info(%s)" % qi(table))]


def pk_of(conn, table):
    cols = sorted((r[5], r[1]) for r in conn.execute(
        "PRAGMA table_info(%s)" % qi(table)) if r[5])
    return [c for _, c in cols]


def admin_env_path():
    import next_common
    return next_common.config_dir() / "next-admin.env"


def default_accounts():
    path = admin_env_path()
    if not path.is_file():
        return []
    return [read_env_values(path, ("NEXT_ADMIN_USER",))["NEXT_ADMIN_USER"]]


def carry_plan(src, dst, accounts):
    plan = []
    src_tables, dst_tables = tables_of(src), tables_of(dst)
    for table in NEXT_TABLES:
        if table in src_tables and table in dst_tables:
            plan.append((table, "", (), "replace"))
    names = [a.strip().lower() for a in accounts if a.strip()]
    if names:
        marks = ",".join("?" * len(names))
        plan.append(("users", "lower(username) IN (%s)" % marks, tuple(names),
                     "insert"))
        plan.append(("account_devices",
                     "account_bi IN (SELECT user_bi FROM users WHERE "
                     "lower(username) IN (%s))" % marks, tuple(names), "insert"))
    plan.append(("nodes", "name LIKE ?", (CORE_NODE_PREFIX + "%",), "insert"))
    plan.append(("mirror_https_endpoints", "node_name LIKE ?",
                 (CORE_NODE_PREFIX + "%",), "insert"))
    if "hosted_repos" in src_tables:
        plan.append(("repositories",
                     "key_bi IN (SELECT key_bi FROM hosted_repos WHERE deleted_at=0)",
                     (), "insert"))
    return [p for p in plan if p[0] in src_tables and p[0] in dst_tables]


def carry(source: Path, target: Path, accounts, execute=False):
    src = sqlite3.connect("file:%s?mode=ro" % source, uri=True)
    dst = sqlite3.connect(str(target))
    report = []
    try:
        plan = carry_plan(src, dst, accounts)
        dst.execute("BEGIN")
        for table, where, params, mode in plan:
            cols = [c for c in columns_of(src, table) if c in set(columns_of(dst, table))]
            select = "SELECT %s FROM %s%s" % (",".join(qi(c) for c in cols), qi(table),
                                            " WHERE " + where if where else "")
            rows = src.execute(select, params).fetchall()
            if mode == "replace":
                before = dst.execute("SELECT COUNT(*) FROM %s" % qi(table)).fetchone()[0]
                dst.execute("DELETE FROM %s" % qi(table))
            else:
                before = 0
                key = pk_of(dst, table) or cols[:1]
                idx = [cols.index(c) for c in key if c in cols]
                for row in rows:
                    clash = dst.execute(
                        "SELECT 1 FROM %s WHERE %s" % (qi(table), " AND ".join(
                            "%s=?" % qi(key[i]) for i in range(len(idx)))),
                        [row[i] for i in idx]).fetchone()
                    if clash:
                        raise CutoverError(
                            "%s: a carried row's key already exists in the target "
                            "(%s); resolve by hand" % (table, ",".join(key)))
            dst.executemany("INSERT INTO %s (%s) VALUES (%s)" % (
                qi(table), ",".join(qi(c) for c in cols), ",".join("?" * len(cols))),
                rows)
            report.append((table, mode, before, len(rows)))
        fk = dst.execute("PRAGMA foreign_key_check").fetchall()
        if fk:
            raise CutoverError("foreign-key violations after carry: %d" % len(fk))
        if execute:
            dst.commit()
        else:
            dst.rollback()
    except Exception:
        with contextlib.suppress(sqlite3.Error):
            dst.rollback()
        raise
    finally:
        src.close()
        dst.close()
    for table, mode, before, count in report:
        log("carry: %-36s %-7s %5d row(s)%s" % (
            table, mode, count, " (replaced %d)" % before if mode == "replace" else ""))
    if not execute:
        log("dry-run: rolled back; pass --execute")
    others = next_only_accounts(source, target, accounts)
    if others:
        log("carry: NOTE %d other account(s) exist only in the Next copy (signed "
            "up on Next, or deleted in production since the copy); not carried"
            % others)
    return report


def extract(source: Path, out: Path, accounts):
    src = sqlite3.connect("file:%s?mode=ro" % source, uri=True)
    private_file(out)
    dst = sqlite3.connect(str(out))
    counts = {}
    try:
        plan = carry_plan(src, src, accounts)
        needed = {t for t, _w, _p, _m in plan} | {"hosted_repos", "users"}
        for table in sorted(needed & tables_of(src)):
            ddl = src.execute("SELECT sql FROM sqlite_master WHERE type='table' "
                              "AND name=?", (table,)).fetchone()[0]
            dst.execute(ddl)
        for table, where, params, _mode in plan:
            cols = columns_of(src, table)
            rows = src.execute("SELECT %s FROM %s%s" % (
                ",".join(qi(c) for c in cols), qi(table),
                " WHERE " + where if where else ""), params).fetchall()
            dst.executemany("INSERT OR IGNORE INTO %s (%s) VALUES (%s)" % (
                qi(table), ",".join(qi(c) for c in cols),
                ",".join("?" * len(cols))), rows)
            counts[table] = counts.get(table, 0) + len(rows)
        dst.commit()
    finally:
        src.close()
        dst.close()
    log("extract: " + ", ".join("%s=%d" % kv for kv in sorted(counts.items())))
    return counts


def reseal(small: Path, out: Path, work: Path, next_key_path=None, prod_key_env=None,
           out_log=None):
    import next_rekey
    old = next_rekey.read_key(Path(next_key_path or default_key_path()), "next")
    new = next_rekey.read_key_from_env_file(
        prod_key_env or "%s:DATA_KEY" % PROD_ENV, "production")
    summary = next_rekey.rekey(small, out, old, new, work,
                               out=out_log or sys.stdout)
    orphans = summary["blind_indexes"].get("dead_row_strict_pseudonymised", 0)
    if summary["problems"] or orphans:
        raise CutoverError("reseal: problems %d, dead identity rows %d"
                           % (len(summary["problems"]), orphans))
    return summary


def next_only_accounts(source, target, accounts):
    src = sqlite3.connect("file:%s?mode=ro" % source, uri=True)
    try:
        src.execute("ATTACH DATABASE ? AS t", ("file:%s?mode=ro" % target,))
        names = [a.strip().lower() for a in accounts]
        rows = src.execute(
            "SELECT lower(username) FROM main.users WHERE user_bi NOT IN "
            "(SELECT user_bi FROM t.users)").fetchall()
    except sqlite3.Error:
        return 0
    finally:
        src.close()
    return sum(1 for (name,) in rows if name not in names)


def load_key(path):
    import next_rekey
    return next_rekey.read_key(Path(path), "verify")


def default_key_path():
    import next_common
    return next_common.config_dir() / "DATA_KEY"


def verify(path: Path, key_path=None, baseline=None, baseline_key_env=None,
           keyset=None, quiet=False):
    keyset = keyset or load_key(key_path or default_key_path())
    conn = sqlite3.connect("file:%s?mode=ro" % path, uri=True)
    counts = {"users": 0, "users_by_name": 0, "users_by_email": 0,
              "users_without_email": 0, "users_undecryptable": 0,
              "nodes": 0, "nodes_by_name": 0, "nodes_undecryptable": 0}
    problems = []
    try:
        for user_bi, email_bi, data in conn.execute(
                "SELECT user_bi, email_bi, data FROM users"):
            counts["users"] += 1
            plain = keyset.open(data)
            if plain is None:
                counts["users_undecryptable"] += 1
                continue
            rec = json.loads(plain)
            name = str(rec.get("name") or "").strip().lower()
            if name and keyset.bi(name) == user_bi:
                counts["users_by_name"] += 1
            else:
                problems.append("users: a row does not resolve by its name")
            email = str(rec.get("email") or "").strip().lower()
            if not email:
                counts["users_without_email"] += 1
            elif email_bi and keyset.bi(email) == email_bi:
                counts["users_by_email"] += 1
            elif email_bi:
                problems.append("users: a row does not resolve by its e-mail")
            else:
                counts["users_without_email"] += 1
        for node_bi, name, data in conn.execute(
                "SELECT node_bi, name, data FROM nodes"):
            counts["nodes"] += 1
            if keyset.open(data) is None:
                counts["nodes_undecryptable"] += 1
                continue
            if name and keyset.bi(str(name).strip().lower()) == node_bi:
                counts["nodes_by_name"] += 1
            else:
                problems.append("nodes: a row does not resolve by its name")
        integrity = conn.execute("PRAGMA integrity_check").fetchone()[0]
        if integrity != "ok":
            problems.append("integrity_check: %s" % integrity)
    finally:
        conn.close()
    if quiet:
        return counts, problems
    if baseline is not None:
        import next_rekey
        old = next_rekey.read_key_from_env_file(
            baseline_key_env or "%s:DATA_KEY" % PROD_ENV, "baseline")
        base_counts, base_problems = verify(baseline, keyset=old, quiet=True)
        del old
        log("verify: baseline " + ", ".join(
            "%s=%d" % kv for kv in base_counts.items()))
        known = {p: base_problems.count(p) for p in set(base_problems)}
        kept = []
        for problem in sorted(set(problems)):
            extra = problems.count(problem) - known.get(problem, 0)
            if known.get(problem):
                log("NOTE: %s (%d, already in the baseline)"
                    % (problem, known[problem]))
            kept += [problem] * max(extra, 0)
        for name in ("users_by_name", "users_by_email", "nodes_by_name"):
            if counts[name] < base_counts[name]:
                kept.append("%s dropped from %d to %d"
                            % (name, base_counts[name], counts[name]))
        problems = kept
    log("verify: " + ", ".join("%s=%d" % kv for kv in counts.items()))
    for problem in sorted(set(problems)):
        log("PROBLEM: %s (%d)" % (problem, problems.count(problem)))
    log("verify: %s" % ("FAILED" if problems else "OK"))
    return counts, problems


def _free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def sandbox_env(tmp: Path, key_text: str, origin: str):
    env = {k: v for k, v in os.environ.items()
           if k not in ("FORKMESH_HOME", "FORKMESH_PROJECTS_DIR")
           and not k.startswith("FORKMESH_VAR_")
           and not k.startswith("CLOUDFLARE_")}
    home = tmp / "home"
    for sub in ("", ".local/share", ".config", ".cache"):
        (home / sub).mkdir(parents=True, exist_ok=True)
    env.update({
        "HOME": str(home), "XDG_DATA_HOME": str(home / ".local/share"),
        "XDG_CONFIG_HOME": str(home / ".config"), "XDG_CACHE_HOME": str(home / ".cache"),
        "FORKMESH_HOST_DATA_DIR": str(tmp / "data"),
        "FORKMESH_HOST_OFFLINE": "1",
        "FORKMESH_HOST_ASSETS_DIR": str(REPO / "app" / (
            "dist" if (REPO / "app" / "dist" / "signup.html").is_file()
            else "public")),
        "FORKMESH_VAR_DATA_KEY": key_text,
        "FORKMESH_VAR_ADMIN_PASS": "probe-" + os.urandom(16).hex(),
        "FORKMESH_VAR_APP_ORIGIN": origin, "FORKMESH_VAR_API_ORIGIN": origin,
        "FORKMESH_VAR_WORLD_ORIGIN": origin, "FORKMESH_VAR_PUBLIC_BASE_URL": origin,
        "FORKMESH_VAR_CORS_ALLOWED_ORIGINS": origin,
        "FORKMESH_VAR_TURNSTILE_SITE_KEY": "1x00000000000000000000AA",
        "FORKMESH_VAR_TURNSTILE_SECRET_KEY": "1x0000000000000000000000000000000AA",
        "FORKMESH_VAR_TURNSTILE_TEST_MODE": "1",
    })
    return env


@contextlib.contextmanager
def local_app(db_file: Path, key_text: str, work: Path):
    import httpx
    port = _free_port()
    origin = "http://127.0.0.1:%d" % port
    env = sandbox_env(work, key_text, origin)
    log_path = work / "app.log"
    with open(log_path, "w") as log_file:
        process = subprocess.Popen(
            [sys.executable, "-m", "forkmesh_host", "serve", "--port", str(port),
             "--db", "sqlite:%s" % db_file, "--no-cron", "--no-build",
             "--log-level", "warning"],
            cwd=str(REPO / "app" / "host"), env=env, stdout=log_file,
            stderr=subprocess.STDOUT)
        try:
            deadline = time.time() + 90
            while True:
                if process.poll() is not None:
                    raise CutoverError("local app exited; see %s" % log_path)
                try:
                    if httpx.get(origin + "/health", timeout=1).status_code == 200:
                        break
                except httpx.HTTPError:
                    pass
                if time.time() > deadline:
                    raise CutoverError("local app did not start; see %s" % log_path)
                time.sleep(0.2)
            with httpx.Client(base_url=origin, timeout=60,
                              headers={"origin": origin}) as client:
                yield client
        finally:
            process.terminate()
            try:
                process.wait(15)
            except subprocess.TimeoutExpired:
                process.kill()


def read_accounts(path):
    raw = {}
    for line in Path(path).read_text("utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#") and "=" in line:
            key, _, value = line.partition("=")
            raw[key.strip()] = value.strip()
    prefixes = sorted({k[:-len("_USER")] for k in raw if k.endswith("_USER")})
    out = []
    for prefix in prefixes:
        name = raw.get(prefix + "_USER", "")
        password = raw.get(prefix + "_PASSWORD", "")
        if name and password:
            out.append((name, raw.get(prefix + "_EMAIL", ""), password))
    return out


def login_probe(db_file: Path, accounts, key_path=None, work=None, key_text=None):
    key_text = key_text or Path(key_path or default_key_path()).read_text("utf-8").strip()
    results = []
    with tempfile.TemporaryDirectory(prefix="cutover-probe-",
                                     dir=str(work) if work else None) as tmp:
        tmp = Path(tmp)
        copy = tmp / "probe.sqlite3"
        shutil.copyfile(db_file, copy)
        os.chmod(copy, 0o600)
        with local_app(copy, key_text, tmp) as client:
            for name, email, password in accounts:
                for label, ident in (("name", name), ("email", email)):
                    if not ident:
                        continue
                    good = client.post("/api/accounts/login",
                                       json={"nodeName": ident, "password": password})
                    bad = client.post("/api/accounts/login",
                                      json={"nodeName": ident,
                                            "password": password + "-wrong"})
                    ok = (good.status_code == 200 and "sessionToken" in good.json()
                          and bad.status_code in (401, 403))
                    results.append((name, label, good.status_code, bad.status_code, ok))
    for name, label, good, bad, ok in results:
        log("login-probe: %s by %s: right password %d, wrong password %d: %s"
            % (name, label, good, bad, "OK" if ok else "FAILED"))
    return results


def _rqlite_bin():
    configured = os.environ.get("FMNEXT_RQLITED")
    candidates = [configured] if configured else []
    import next_common
    candidates.append(str(next_common.data_dir() / "bin" / "rqlited"))
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return candidate
    found = shutil.which("rqlited")
    if found:
        return found
    raise CutoverError("no rqlited binary (set FMNEXT_RQLITED)")


@contextlib.contextmanager
def local_rqlite(work: Path):
    import urllib.request
    port, raft = _free_port(), _free_port()
    data = work / "rqlite"
    data.mkdir(mode=0o700)
    with open(work / "rqlite.log", "w") as log_file:
        process = subprocess.Popen(
            [_rqlite_bin(), "-node-id", "rehearsal", "-http-addr",
             "127.0.0.1:%d" % port, "-raft-addr", "127.0.0.1:%d" % raft, "-fk",
             str(data)], stdout=log_file, stderr=subprocess.STDOUT)
        url = "http://127.0.0.1:%d" % port
        try:
            deadline = time.time() + 30
            while True:
                try:
                    with urllib.request.urlopen(url + "/readyz", timeout=1) as r:
                        if r.status == 200:
                            break
                except OSError:
                    pass
                if process.poll() is not None or time.time() > deadline:
                    raise CutoverError("local rqlite did not start")
                time.sleep(0.2)
            yield url
        finally:
            process.terminate()
            try:
                process.wait(15)
            except subprocess.TimeoutExpired:
                process.kill()


def checkpoint(path: Path):
    conn = sqlite3.connect(str(path))
    try:
        conn.execute("PRAGMA wal_checkpoint(TRUNCATE)")
        conn.execute("PRAGMA journal_mode=DELETE")
    finally:
        conn.close()


def rqlite_load(url, path: Path):
    import urllib.request
    checkpoint(path)
    request = urllib.request.Request(url + "/db/load", data=Path(path).read_bytes(),
                                     method="POST", headers={
                                         "Content-Type": "application/octet-stream"})
    with urllib.request.urlopen(request, timeout=600) as response:
        body = response.read()
    if b'"error"' in body:
        raise CutoverError("rqlite /db/load failed")


def rqlite_backup(url, out: Path):
    import urllib.request
    private_file(out)
    with urllib.request.urlopen(url + "/db/backup", timeout=600) as response:
        Path(out).write_bytes(response.read())


def seed_probe_account(db_file: Path, key_text: str, work: Path):
    name = "cutoverprobe" + os.urandom(3).hex()
    email = name + "@example.com"
    password = os.urandom(18).hex()
    with tempfile.TemporaryDirectory(prefix="cutover-seed-", dir=str(work)) as tmp:
        with local_app(db_file, key_text, Path(tmp)) as client:
            page = client.get("/signup")
            field = re.search(r"contact_[0-9a-f]{18}", page.text)
            if page.status_code != 200 or not field:
                raise CutoverError("seed: signup page unavailable (%d)"
                                   % page.status_code)
            signup = client.post("/api/accounts/signup", json={
                "nodeName": name, field.group(0): email, "password": password,
                "turnstileToken": "XXXX.DUMMY.TOKEN.XXXX"})
            if signup.status_code != 201:
                raise CutoverError("seed: signup answered %d" % signup.status_code)
    log("seed: signed up a throwaway probe account on the local copy")
    return (name, email, password)


def rehearse_keep_key(work: Path, dump=None, next_backup=None, accounts=None,
                      next_key_path=None, prod_key_env=None, execute_export=False,
                      seed_probe=False):
    import next_rekey
    work = Path(work)
    work.mkdir(parents=True, exist_ok=True, mode=0o700)
    os.chmod(work, 0o700)
    os.umask(0o077)
    timings = {}
    accounts = accounts if accounts is not None else default_accounts()

    def timed(name, fn, *a, **kw):
        started = time.monotonic()
        value = fn(*a, **kw)
        timings[name] = time.monotonic() - started
        log("phase %-10s %6.1f s" % (name, timings[name]))
        return value

    if dump is None:
        if not execute_export:
            raise CutoverError("rehearse without --dump needs --execute-export")
        dump = work / "export.sql"
        timed("export", export, dump, execute=True)
    sqlite_file = work / "export.sqlite3"
    timed("sqlite", dump_to_sqlite, Path(dump), sqlite_file)
    prod = next_rekey.read_key_from_env_file(
        prod_key_env or "%s:DATA_KEY" % PROD_ENV, "production")
    probe = seed_probe_account(sqlite_file, prod._secret, work) if seed_probe else None
    resealed = None
    if next_backup:
        small = work / "next-rows.sqlite3"
        timed("extract", extract, Path(next_backup), small, accounts)
        resealed = work / "next-rows-production-key.sqlite3"
        timed("reseal", reseal, small, resealed, work / "reseal-work",
              next_key_path, prod_key_env, open(work / "reseal.log", "w"))
    final = work / "final.sqlite3"
    with local_rqlite(work) as url:
        timed("load", rqlite_load, url, sqlite_file)
        timed("migrate", subprocess.run, [
            sys.executable, str(HERE / "rqlite_migrate.py"), "--url", url,
            "--app-schema"], check=True, stdout=open(work / "migrate.log", "w"),
            stderr=subprocess.STDOUT)
        timed("backup", rqlite_backup, url, final)
    if resealed:
        timed("carry", carry, resealed, final, accounts, True)
    counts, problems = timed("verify", verify, final, None, sqlite_file,
                             prod_key_env, keyset=prod)
    problems = list(problems) + carried_rows_open(final, resealed, prod)
    reload_dir = work / "reload"
    reload_dir.mkdir(mode=0o700)
    with local_rqlite(reload_dir) as url:
        timed("reload", rqlite_load, url, final)
    probes = [probe] if probe else []
    if next_backup:
        admin = admin_env_path()
        if admin.is_file():
            probes += read_accounts(admin)
    if probes:
        results = timed("login", login_probe, final, probes, None, work, prod._secret)
        if not all(r[-1] for r in results):
            problems.append("login probe failed")
    del prod
    log("rehearsal: %.1f s of data work (%s)" % (sum(timings.values()), ", ".join(
        "%s %.1f" % kv for kv in timings.items())))
    for problem in problems:
        log("PROBLEM: %s" % problem)
    log("rehearsal: %s" % ("FAILED" if problems else "OK"))
    return timings, final, problems


def carried_rows_open(final: Path, resealed, keyset, next_key_path=None):
    if not resealed:
        return []
    import next_rekey
    next_keyset = next_rekey.read_key(Path(next_key_path or default_key_path()), "next")
    src = sqlite3.connect("file:%s?mode=ro" % resealed, uri=True)
    problems, checked, sealed = [], 0, 0
    try:
        for table in sorted(tables_of(src)):
            cols = columns_of(src, table)
            cipher = set(next_rekey.CIPHER_COLUMNS.get(table, ()))
            for row in src.execute("SELECT %s FROM %s" % (
                    ",".join(qi(c) for c in cols), qi(table))):
                for col, value in zip(cols, row):
                    if not isinstance(value, str) or not value:
                        continue
                    checked += 1
                    if next_keyset.open(value) is not None:
                        problems.append("%s.%s still opens under Next's key"
                                        % (table, col))
                    if col in cipher and value[:1] not in "{[":
                        sealed += 1
                        if keyset.open(value) is None:
                            problems.append("%s.%s does not open under "
                                            "production's key" % (table, col))
    finally:
        src.close()
    log("carried: %d value(s) checked, %d registered ciphertext(s) open under "
        "production's key, %d problem(s)" % (checked, sealed, len(problems)))
    return sorted(set(problems))


def rehearse(work: Path, dump=None, next_backup=None, accounts=None,
             key_path=None, old_key_env=None, execute_export=False,
             seed_probe=False):
    import next_rekey
    work = Path(work)
    work.mkdir(parents=True, exist_ok=True, mode=0o700)
    os.chmod(work, 0o700)
    os.umask(0o077)
    timings = {}

    def timed(name, fn, *a, **kw):
        started = time.monotonic()
        value = fn(*a, **kw)
        timings[name] = time.monotonic() - started
        log("phase %-10s %6.1f s" % (name, timings[name]))
        return value

    if dump is None:
        dump = work / "export.sql"
        timed("export", export, dump, execute=execute_export)
        if not execute_export:
            raise CutoverError("rehearse without --dump needs --execute-export")
    sqlite_file = work / "export.sqlite3"
    timed("sqlite", dump_to_sqlite, Path(dump), sqlite_file)
    rekeyed = work / "rekeyed.sqlite3"
    old = next_rekey.read_key_from_env_file(
        old_key_env or "%s:DATA_KEY" % PROD_ENV, "old")
    probe = None
    if seed_probe:
        probe = seed_probe_account(sqlite_file, old._secret, work)
    new = next_rekey.read_key(Path(key_path or default_key_path()), "new")
    timed("rekey", next_rekey.rekey, sqlite_file, rekeyed, old, new,
          work / "rekey-work", out=open(work / "rekey.log", "w"))
    del old
    final = work / "final.sqlite3"
    with local_rqlite(work) as url:
        timed("load", rqlite_load, url, rekeyed)
        timed("migrate", subprocess.run, [
            sys.executable, str(HERE / "rqlite_migrate.py"), "--url", url,
            "--app-schema"], check=True, stdout=open(work / "migrate.log", "w"),
            stderr=subprocess.STDOUT)
        timed("backup", rqlite_backup, url, final)
    if next_backup:
        timed("carry", carry, Path(next_backup), final,
              accounts if accounts is not None else default_accounts(), True)
    counts, problems = timed("verify", verify, final, key_path, sqlite_file,
                             old_key_env)
    reload_dir = work / "reload"
    reload_dir.mkdir(mode=0o700)
    with local_rqlite(reload_dir) as url:
        timed("reload", rqlite_load, url, final)
    if probe or next_backup:
        probes = [probe] if probe else []
        if next_backup:
            admin = admin_env_path()
            if admin.is_file():
                probes += read_accounts(admin)
        results = timed("login", login_probe, final, probes, key_path, work)
        if not results or not all(r[-1] for r in results):
            problems = list(problems) + ["login probe failed"]
    total = sum(timings.values())
    log("rehearsal: %.1f s of data work (%s)" % (total, ", ".join(
        "%s %.1f" % kv for kv in timings.items())))
    return timings, final, problems


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("export")
    p.add_argument("--out", required=True)
    p.add_argument("--env-file", default=str(PROD_ENV))
    p.add_argument("--execute", action="store_true")
    p = sub.add_parser("sqlite")
    p.add_argument("--in", dest="input", required=True)
    p.add_argument("--out", required=True)
    p = sub.add_parser("carry")
    p.add_argument("--from", dest="source", required=True)
    p.add_argument("--into", dest="target", required=True)
    p.add_argument("--account", action="append", default=None)
    p.add_argument("--execute", action="store_true")
    p = sub.add_parser("extract")
    p.add_argument("--from", dest="source", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--account", action="append", default=None)
    p = sub.add_parser("reseal")
    p.add_argument("--in", dest="input", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--next-key", default=None)
    p.add_argument("--production-key-env", default=None, metavar="FILE:NAME")
    p.add_argument("--work", default=None)
    p = sub.add_parser("verify")
    p.add_argument("--in", dest="input", required=True)
    p.add_argument("--key", default=None)
    p.add_argument("--key-env", default=None, metavar="FILE:NAME")
    p.add_argument("--baseline", default=None,
                   help="the export before the re-key (SQLite)")
    p.add_argument("--baseline-key-env", default=None, metavar="FILE:NAME",
                   help="old key for --baseline (default production's "
                        ".env.production:DATA_KEY)")
    p = sub.add_parser("login-probe")
    p.add_argument("--in", dest="input", required=True)
    p.add_argument("--key", default=None)
    p.add_argument("--key-env", default=None, metavar="FILE:NAME")
    p.add_argument("--accounts-env", default=str(admin_env_path()))
    p = sub.add_parser("rehearse")
    p.add_argument("--work", required=True)
    p.add_argument("--dump", default=None, help="an existing export (else export)")
    p.add_argument("--execute-export", action="store_true")
    p.add_argument("--next-backup", default=None,
                   help="restored Next backup (SQLite) to carry rows from")
    p.add_argument("--account", action="append", default=None)
    p.add_argument("--key", default=None, help="Next DATA_KEY file")
    p.add_argument("--old-key-env", default=None, metavar="FILE:NAME")
    p.add_argument("--keep-production-key", action="store_true",
                   help="no re-key: the export stays under production's key; "
                        "Next's carried rows are resealed to it")
    p.add_argument("--seed-probe", action="store_true",
                   help="sign a throwaway account up on the local copy before "
                        "the re-key and log in with it afterwards")
    args = parser.parse_args(argv)
    try:
        if args.command == "export":
            export(Path(args.out), args.execute, Path(args.env_file))
        elif args.command == "sqlite":
            dump_to_sqlite(Path(args.input), Path(args.out))
        elif args.command == "carry":
            accounts = args.account if args.account is not None else default_accounts()
            carry(Path(args.source), Path(args.target), accounts, args.execute)
        elif args.command == "extract":
            extract(Path(args.source), Path(args.out),
                    args.account if args.account is not None else default_accounts())
        elif args.command == "reseal":
            work = Path(args.work) if args.work else Path(args.out).parent / "reseal-work"
            summary = reseal(Path(args.input), Path(args.out), work, args.next_key,
                             args.production_key_env)
            log("reseal: OK, %d ciphertext(s) resealed" %
                summary["totals"].get("cipher_reencrypted", 0))
        elif args.command == "verify":
            keyset = None
            if args.key_env:
                import next_rekey
                keyset = next_rekey.read_key_from_env_file(args.key_env, "verify")
            _counts, problems = verify(
                Path(args.input), args.key,
                Path(args.baseline) if args.baseline else None,
                args.baseline_key_env, keyset=keyset)
            return 1 if problems else 0
        elif args.command == "login-probe":
            key_text = None
            if args.key_env:
                import next_rekey
                key_text = next_rekey.read_key_from_env_file(args.key_env, "probe")._secret
            results = login_probe(Path(args.input), read_accounts(args.accounts_env),
                                  args.key, key_text=key_text)
            return 0 if results and all(r[-1] for r in results) else 1
        elif args.command == "rehearse" and args.keep_production_key:
            _t, _final, problems = rehearse_keep_key(
                Path(args.work), args.dump, args.next_backup, args.account,
                args.key, args.old_key_env, args.execute_export, args.seed_probe)
            return 1 if problems else 0
        elif args.command == "rehearse":
            _t, _final, problems = rehearse(
                Path(args.work), args.dump, args.next_backup, args.account,
                args.key, args.old_key_env, args.execute_export, args.seed_probe)
            return 1 if problems else 0
    except (CutoverError, OSError, sqlite3.Error) as error:
        print("cutover_data: %s" % error, file=sys.stderr)
        return 1
    except Exception as error:
        if type(error).__name__ == "RekeyError":
            print("cutover_data: %s" % error, file=sys.stderr)
            return 1
        raise
    return 0


if __name__ == "__main__":
    sys.exit(main())
