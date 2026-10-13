"""Offline, low-bandwidth, and removable-media sync (issue #504).

Drives tools/forkmesh_offline_sync.py against real Git repositories and a
fault-injecting HTTP host: repeated disconnects, corrupted bytes, crashes
between a chunk and its checkpoint, data budgets, schedules, cancellation,
filtered and metadata-only replicas, incremental exports, reconciliation
conflicts, rollback, telemetry, and the documentation and gate wiring.
"""

import collections
import hashlib
import http.server
import importlib.util
import json
import os
import random
import re
import subprocess
import sys
import threading
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
TOOL_PATH = ROOT / "tools" / "forkmesh_offline_sync.py"
DOC = ROOT / "www" / "docs" / "offline-sync.md"
sys.path.insert(0, str(ROOT / "app" / "src"))

import availability_proof  # noqa: E402


def _load_tool():
    spec = importlib.util.spec_from_file_location("forkmesh_offline_sync", TOOL_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


tool = _load_tool()

SEED = bytes(range(32))
PUBLIC = tool.ed25519_public_key(SEED)
PUBLIC_B64 = tool._b64encode(PUBLIC)
CHUNK = 8192
GIT_ENV = {
    **os.environ,
    "GIT_AUTHOR_NAME": "Offline Test", "GIT_AUTHOR_EMAIL": "offline@example.invalid",
    "GIT_COMMITTER_NAME": "Offline Test", "GIT_COMMITTER_EMAIL": "offline@example.invalid",
    "GIT_CONFIG_NOSYSTEM": "1",
}


def run_git(cwd, *args, stdin=None):
    completed = subprocess.run(
        ["git", "-c", "commit.gpgsign=false", "-c", "tag.gpgsign=false",
         "-c", "init.defaultBranch=main", *args],
        cwd=cwd, env=GIT_ENV, capture_output=True, input=stdin, check=False,
    )
    assert completed.returncode == 0, completed.stderr.decode()
    return completed.stdout.decode().strip()


def commit(repo, files, message):
    for name, content in files.items():
        path = repo / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        run_git(repo, "add", name)
    run_git(repo, "commit", "-q", "-m", message)
    return run_git(repo, "rev-parse", "HEAD")


def refs_of(repo):
    listed = run_git(repo, "for-each-ref", "--format=%(refname) %(objectname)")
    return dict(line.split(" ", 1) for line in listed.splitlines()
                if not line.startswith("refs/forkmesh-offline/"))


def make_source(tmp_path):
    repo = tmp_path / "source"
    repo.mkdir()
    run_git(repo, "init", "-q", "-b", "main")
    rng = random.Random(504)
    commit(repo, {
        ".forkmesh/issues/open/1/issue-1.json": b'{"number":1,"title":"Works offline"}\n',
        "docs/guide.md": b"# Guide\n",
        "assets/big.bin": rng.randbytes(120_000),
    }, "initial")
    run_git(repo, "tag", "-a", "v1.0", "-m", "release")
    run_git(repo, "switch", "-q", "-c", "feature")
    commit(repo, {"docs/feature.md": b"feature\n"}, "feature work")
    run_git(repo, "switch", "-q", "--orphan", "forkmesh/pulls")
    commit(repo, {".forkmesh/pulls/1.json": b'{"number":1,"state":"open"}\n'}, "pull ledger")
    run_git(repo, "switch", "-q", "main")
    return repo


def export(repo, media, since=None, **selection):
    return tool.export_media(
        repo, media, SEED, selection=tool.normalize_selection(**selection),
        since=since, chunk_size=CHUNK, repository="alice/offline", now_ms=1_800_000_000_000)


def bare(tmp_path, name="replica.git"):
    path = tmp_path / name
    run_git(tmp_path, "init", "-q", "--bare", str(path))
    return path


def cli(*args):
    return subprocess.run(
        [sys.executable, str(TOOL_PATH), *map(str, args)],
        capture_output=True, text=True, env=GIT_ENV, check=False, timeout=120,
    )


class MediaHost:
    """Static host for one media directory with injectable link faults."""

    def __init__(self, root):
        self.root = Path(root)
        self.served = collections.Counter()
        self.requests = collections.Counter()
        self.drop_after = None
        self.drops_left = 0
        self.corrupt_at = None
        self.corrupt_times = 0
        self.ignore_range = False
        self.lock = threading.Lock()
        host = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *args):
                pass

            def do_GET(self):
                host._serve(self)

        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()
        self.url = "http://127.0.0.1:%d/media/" % self.httpd.server_address[1]

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()

    def _serve(self, handler):
        name = handler.path.rsplit("/", 1)[-1]
        path = self.root / name
        if not path.is_file():
            handler.send_error(404)
            return
        data = path.read_bytes()
        start, end, status = 0, len(data) - 1, 200
        requested = handler.headers.get("Range")
        if requested and not self.ignore_range:
            match = re.match(r"bytes=(\d+)-(\d+)$", requested)
            start, end, status = int(match[1]), min(int(match[2]), len(data) - 1), 206
        body = bytearray(data[start:end + 1])
        with self.lock:
            self.requests[name] += 1
            if self.corrupt_times and self.corrupt_at is not None and start <= self.corrupt_at <= end:
                body[self.corrupt_at - start] ^= 0xFF
                self.corrupt_times -= 1
            if (name == tool.BUNDLE_FILE and self.drops_left and self.drop_after is not None
                    and len(body) > self.drop_after):
                self.drops_left -= 1
                body = body[:self.drop_after]
        handler.send_response(status)
        handler.send_header("Content-Length", str(end - start + 1))
        if status == 206:
            handler.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, len(data)))
        handler.send_header("Connection", "close")
        handler.end_headers()
        try:
            handler.wfile.write(bytes(body))
            handler.wfile.flush()
        except OSError:
            pass
        with self.lock:
            self.served[name] += len(body)
        handler.close_connection = True


@pytest.fixture
def source(tmp_path):
    return make_source(tmp_path)


@pytest.fixture
def media(tmp_path, source):
    path = tmp_path / "media"
    export(source, path)
    return path


@pytest.fixture
def host(media):
    served = MediaHost(media)
    yield served
    served.close()


def bundle_size(media):
    return json.loads((media / tool.MANIFEST_FILE).read_text())["manifest"]["bundle"]["size"]


def transfer(source, dest, **options):
    options.setdefault("trusted_keys", {PUBLIC})
    options.setdefault("allow_unpinned", False)
    options.setdefault("sleep", lambda seconds: None)
    return tool.Transfer(source, dest, **options)


# --- Provenance ----------------------------------------------------------------

def test_pure_python_signing_matches_rfc8032_vector(monkeypatch):
    monkeypatch.setitem(sys.modules, "cryptography.hazmat.primitives.asymmetric.ed25519", None)
    secret = bytes.fromhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")
    assert tool.ed25519_public_key(secret).hex() == (
        "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a")
    assert tool.ed25519_sign(secret, b"").hex() == (
        "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065"
        "224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b")


def test_export_import_round_trips_refs_and_collaboration_history(tmp_path, source, media):
    verified = cli("verify", media, "--trusted-key", PUBLIC_B64, "--json")
    assert verified.returncode == 0, verified.stderr
    report = json.loads(verified.stdout)
    assert report["provenance"]["trusted"] and report["provenance"]["signatureValid"]
    assert report["replica"]["scope"] == "full"
    assert report["replica"]["claimsFullAvailability"] is True

    replica = bare(tmp_path)
    imported = cli("import", media, replica, "--trusted-key", PUBLIC_B64, "--json")
    assert imported.returncode == 0, imported.stderr
    result = json.loads(imported.stdout)
    assert result["provenance"]["publicKey"] == PUBLIC_B64
    assert result["decisions"]["create"] == len(refs_of(source))
    assert refs_of(replica) == refs_of(source)
    for spec in ("main:.forkmesh/issues/open/1/issue-1.json",
                 "forkmesh/pulls:.forkmesh/pulls/1.json", "main:assets/big.bin"):
        assert run_git(replica, "rev-parse", spec) == run_git(source, "rev-parse", spec)
    assert run_git(replica, "cat-file", "-t", "refs/tags/v1.0") == "tag"
    assert result["replica"] == {**tool.replica_scope(tool.normalize_selection()), "missingObjects": 0}

    journal = json.loads(Path(result["journal"]).read_text())
    assert journal["manifestSha256"] == result["provenance"]["manifestSha256"]
    assert journal["signer"]["trusted"] is True and journal["state"] == "applied"


def test_untrusted_or_tampered_exports_change_nothing(tmp_path, media):
    replica = bare(tmp_path)
    other = tool._b64encode(tool.ed25519_public_key(bytes(32)))
    assert cli("verify", media, "--trusted-key", other).returncode == 3
    assert cli("import", media, replica).returncode == 3  # neither pinned nor --allow-unpinned

    envelope = json.loads((media / tool.MANIFEST_FILE).read_text())
    forged = json.loads(json.dumps(envelope))
    forged["manifest"]["refs"]["refs/heads/main"] = "0" * 40
    forged["manifestSha256"] = hashlib.sha256(tool.canonical_json(forged["manifest"])).hexdigest()
    _, _, problems = tool.verify_envelope(forged, {PUBLIC})
    assert problems == ["the signature does not verify"]

    overclaim = json.loads(json.dumps(envelope["manifest"]))
    overclaim["selection"]["metadataOnly"] = True
    _, _, problems = tool.verify_envelope(tool.sign_manifest(overclaim, SEED), {PUBLIC})
    assert "the replica advertisement does not match the selection" in problems

    bundle = media / tool.BUNDLE_FILE
    data = bytearray(bundle.read_bytes())
    data[CHUNK * 2 + 5] ^= 0x01
    bundle.write_bytes(bytes(data))
    failed = cli("import", media, replica, "--trusted-key", PUBLIC_B64)
    assert failed.returncode == 3
    assert "chunk 2 is corrupt" in failed.stderr
    assert refs_of(replica) == {}


# --- Resumable transfer under faults -----------------------------------------

def test_fetch_resumes_after_repeated_disconnects_without_duplicate_transfer(tmp_path, source, media, host):
    host.drop_after, host.drops_left = 3000, 15
    dest = tmp_path / "dest"
    result = transfer(tool.HttpSource(host.url), dest, retries=3).run()

    size = bundle_size(media)
    assert result["state"] == "complete", result
    assert result["counters"]["disconnects"] == 15
    assert host.drops_left == 0
    assert host.served[tool.BUNDLE_FILE] == size  # not one byte sent twice
    assert host.requests[tool.MANIFEST_FILE] == 1
    assert result["counters"]["bytesTransferred"] == size
    assert result["counters"]["integrityFailures"] == 0
    assert (dest / tool.BUNDLE_FILE).read_bytes() == (media / tool.BUNDLE_FILE).read_bytes()

    replica = bare(tmp_path)
    tool.import_media(dest, replica, trusted_keys={PUBLIC})
    assert refs_of(replica) == refs_of(source)


def test_fetch_resumes_across_restarts_and_data_budgets(tmp_path, media, host):
    host.drop_after, host.drops_left = 5000, 6
    dest = tmp_path / "dest"
    size = bundle_size(media)
    runs = []
    for _ in range(40):
        result = transfer(tool.HttpSource(host.url), dest, budget=20_000, retries=2).run()
        runs.append(result)
        if result["state"] == "complete":
            break
        assert result["state"] == "paused"
        assert result["reason"] in ("budget_exhausted", "link_down")
        on_disk = tool.estimate(dest)
        assert on_disk["bytesOnDisk"] + on_disk["remainingBytes"] == size
    assert runs[-1]["state"] == "complete"
    assert len(runs) >= 3
    assert host.served[tool.BUNDLE_FILE] == size
    assert host.requests[tool.MANIFEST_FILE] == 1
    counters = runs[-1]["counters"]
    assert counters["sessions"] == len(runs)
    assert counters["resumes"] == len(runs) - 1
    assert counters["bytesTransferred"] == size
    assert counters["bytesResumed"] > 0


def test_lost_checkpoint_and_corrupt_chunk_on_disk_refetch_only_that_chunk(tmp_path, media, host):
    dest = tmp_path / "dest"
    paused = transfer(tool.HttpSource(host.url), dest, budget=CHUNK * 3).run()
    assert paused["reason"] == "budget_exhausted"
    checkpoint_path = dest / tool.TRANSFER_DIR / "checkpoint.json"
    checkpoint = json.loads(checkpoint_path.read_text())
    assert checkpoint["verifiedChunks"] == [0, 1, 2]
    checkpoint["verifiedChunks"] = []  # a crash between the rename and the checkpoint write
    checkpoint_path.write_text(json.dumps(checkpoint))
    damaged = dest / tool.TRANSFER_DIR / "chunks" / "00000001.done"
    damaged.write_bytes(b"\0" * CHUNK)

    result = transfer(tool.HttpSource(host.url), dest).run()
    assert result["state"] == "complete"
    assert result["counters"]["integrityFailures"] == 1
    assert host.served[tool.BUNDLE_FILE] == bundle_size(media) + CHUNK
    assert (dest / tool.BUNDLE_FILE).read_bytes() == (media / tool.BUNDLE_FILE).read_bytes()


def test_corrupted_link_bytes_are_refetched_and_never_assembled(tmp_path, media, host):
    host.corrupt_at, host.corrupt_times = CHUNK * 4 + 17, 1
    dest = tmp_path / "dest"
    result = transfer(tool.HttpSource(host.url), dest).run()
    assert result["state"] == "complete"
    assert result["counters"]["integrityFailures"] == 1
    assert result["counters"]["retransferredBytes"] == CHUNK
    assert host.served[tool.BUNDLE_FILE] == bundle_size(media) + CHUNK
    assert (dest / tool.BUNDLE_FILE).read_bytes() == (media / tool.BUNDLE_FILE).read_bytes()

    host.corrupt_times = 99
    again = tmp_path / "again"
    failed = transfer(tool.HttpSource(host.url), again).run()
    assert (failed["state"], failed["reason"]) == ("failed", "integrity")
    assert failed["counters"]["integrityFailures"] == tool.CHUNK_INTEGRITY_ATTEMPTS
    assert not (again / tool.BUNDLE_FILE).exists()
    assert not (again / tool.MANIFEST_FILE).exists()


def test_link_that_stays_down_pauses_with_checkpoint(tmp_path, media, host):
    host.drop_after, host.drops_left = 0, 10_000
    dest = tmp_path / "dest"
    result = transfer(tool.HttpSource(host.url), dest, retries=3).run()
    assert (result["state"], result["reason"]) == ("paused", "link_down")
    assert result["counters"]["disconnects"] == 4
    assert (dest / tool.TRANSFER_DIR / "checkpoint.json").exists()
    host.drops_left = 0
    assert transfer(tool.HttpSource(host.url), dest).run()["state"] == "complete"
    assert host.served[tool.BUNDLE_FILE] == bundle_size(media)


def test_server_that_ignores_range_is_refused(tmp_path, host):
    host.ignore_range = True
    with pytest.raises(tool.VerificationError, match="Range"):
        transfer(tool.HttpSource(host.url), tmp_path / "dest").run()


def test_cancellation_stops_safely_and_resumes(tmp_path, media):
    dest = tmp_path / "dest"
    polls = [0]

    def stop_after_a_while():
        polls[0] += 1
        return polls[0] > 6

    paused = transfer(tool.LocalSource(media), dest, should_stop=stop_after_a_while).run()
    assert (paused["state"], paused["reason"]) == ("paused", "cancelled")
    assert 0 < paused["estimate"]["bytesOnDisk"] < bundle_size(media)
    done = transfer(tool.LocalSource(media), dest).run()
    assert done["state"] == "complete"
    assert done["counters"]["bytesTransferred"] == bundle_size(media)


def test_schedule_window_pauses_before_any_transfer(tmp_path, host):
    import datetime

    noon = datetime.datetime(2026, 9, 14, 12, 0)
    windows = [tool.parse_window("22:00-06:00")]
    result = transfer(tool.HttpSource(host.url), tmp_path / "dest", windows=windows, now=lambda: noon).run()
    assert (result["state"], result["reason"]) == ("paused", "outside_schedule")
    assert result["nextWindowAt"] == "2026-09-14T22:00:00"
    assert sum(host.requests.values()) == 0

    assert tool.window_allows(windows, datetime.datetime(2026, 9, 14, 23, 30))
    assert tool.window_allows(windows, datetime.datetime(2026, 9, 15, 5, 59))
    assert not tool.window_allows(windows, datetime.datetime(2026, 9, 15, 6, 0))
    for bad in ("25:00-01:00", "9-5", ""):
        with pytest.raises(tool.UsageError):
            tool.parse_window(bad)


def test_rate_limit_paces_the_transfer(tmp_path, media):
    clock = [0.0]

    def sleep(seconds):
        clock[0] += seconds

    rate = 40_000
    result = transfer(tool.LocalSource(media), tmp_path / "dest", rate=rate,
                      clock=lambda: clock[0], sleep=sleep).run()
    assert result["state"] == "complete"
    expected = bundle_size(media) / rate
    assert expected - 0.5 <= clock[0] <= expected + 0.5


def test_sizes_parse_in_binary_units():
    assert tool.parse_size("512k") == 512 * 1024
    assert tool.parse_size("10M") == 10 * 1024 * 1024
    assert tool.parse_size("1GiB") == 1024 ** 3
    assert tool.parse_size("1234") == 1234
    with pytest.raises(tool.UsageError):
        tool.parse_size("ten")


# --- Filtered, metadata-only, and deferred-object replicas ---------------------

@pytest.mark.parametrize("selection,scope,limitations,omits", [
    ({"metadata_only": True}, "metadata-only", ["metadata_only"], True),
    ({"paths": ["docs"]}, "filtered", ["path_filter"], True),
    ({"defer_blobs_over": 50_000}, "filtered", ["deferred_large_objects"], True),
    ({"refs": ["refs/heads/main", "refs/heads/forkmesh/*"]}, "filtered", ["branch_filter"], False),
])
def test_limited_replicas_advertise_limitations_and_never_claim_full(
        tmp_path, source, selection, scope, limitations, omits):
    media = tmp_path / "limited"
    manifest = export(source, media, **selection)["manifest"]
    assert manifest["replica"]["scope"] == scope
    assert manifest["replica"]["limitations"] == limitations
    assert manifest["replica"]["claimsFullAvailability"] is False
    assert (manifest["omitted"]["bytes"] >= 120_000) is omits

    replica = bare(tmp_path)
    refused = cli("import", media, replica, "--trusted-key", PUBLIC_B64)
    assert refused.returncode == 3 and "--allow-limited" in refused.stderr
    assert refs_of(replica) == {}

    report = tool.import_media(media, replica, trusted_keys={PUBLIC}, allow_limited=True)
    advertised = json.loads(cli("replica", replica, "--json").stdout)
    for claim in (report["replica"], advertised):
        assert claim["scope"] != "full"
        assert claim["claimsFullAvailability"] is False
        assert set(limitations) <= set(claim["limitations"])
    # Collaboration history travels in every mode.
    assert run_git(replica, "cat-file", "-p", "main:.forkmesh/issues/open/1/issue-1.json").startswith("{")
    if omits:
        assert report["deferredObjects"] > 0
        assert "missing_objects" in advertised["limitations"]

    # A copy that declares this scope never counts as verified.
    row = {"node": "offline-host", "owner": "offline-host", "status": "online", "integrity": "ok",
           "behind": False, "endpointFresh": True, "endpointHealthy": True, "endpointIntegrity": "ok",
           "checkedAt": 1_800_000_000_000, "replicaScope": advertised["scope"]}
    proof = availability_proof.evaluate_mirrors_payload(
        {"generatedAt": 1_800_000_000_000, "mirrors": [row, dict(row, node="second", owner="second")]})
    assert proof["proof"] == "none"
    assert {copy["class"] for copy in proof["copies"]} == {"observed"}
    assert all("limited_replica" in copy["reasons"] for copy in proof["copies"])


def test_complete_full_import_clears_earlier_limitations(tmp_path, source):
    replica = bare(tmp_path)
    tool.import_media(export(source, tmp_path / "meta", metadata_only=True) and tmp_path / "meta",
                      replica, trusted_keys={PUBLIC}, allow_limited=True)
    assert tool.replica_state(replica)["scope"] == "metadata-only"
    tool.import_media(export(source, tmp_path / "full") and tmp_path / "full",
                      replica, trusted_keys={PUBLIC})
    state = tool.replica_state(replica)
    assert state["scope"] == "full" and state["missingObjects"] == 0
    assert refs_of(replica) == refs_of(source)


def test_incremental_export_requires_its_prerequisites(tmp_path, source):
    first = tmp_path / "first"
    second = tmp_path / "second"
    full = export(source, first)["manifest"]
    commit(source, {"docs/offline.md": b"changed on the other side\n"}, "offline edit")
    incremental = export(source, second, since=first)["manifest"]
    assert incremental["prerequisites"]
    assert incremental["previousExport"] == hashlib.sha256(tool.canonical_json(full)).hexdigest()
    assert incremental["bundle"]["size"] < full["bundle"]["size"] // 10
    assert export(source, tmp_path / "third", since=second) is None

    replica = bare(tmp_path)
    with pytest.raises(tool.VerificationError, match="prerequisite"):
        tool.import_media(second, replica, trusted_keys={PUBLIC})
    assert refs_of(replica) == {}
    tool.import_media(first, replica, trusted_keys={PUBLIC})
    report = tool.import_media(second, replica, trusted_keys={PUBLIC})
    assert report["decisions"]["fast_forward"] == 1
    assert refs_of(replica) == refs_of(source)


# --- Reconciliation and rollback ----------------------------------------------

def test_reconnect_reconciles_conflicts_aside_and_rollback_restores(tmp_path, source):
    replica = bare(tmp_path)
    tool.import_media(export(source, tmp_path / "first") and tmp_path / "first",
                      replica, trusted_keys={PUBLIC})

    # Offline on the replica side: a local commit on feature.
    work = tmp_path / "work"
    run_git(tmp_path, "clone", "-q", str(replica), str(work))
    run_git(work, "switch", "-q", "feature")
    commit(work, {"docs/replica.md": b"replica side\n"}, "replica edit")
    run_git(work, "push", "-q", "origin", "feature")
    # Meanwhile on the source side: main advances, feature diverges, a branch appears.
    commit(source, {"docs/main.md": b"source main\n"}, "source main")
    run_git(source, "switch", "-q", "feature")
    commit(source, {"docs/source.md": b"source side\n"}, "source edit")
    run_git(source, "switch", "-q", "-c", "topic")
    commit(source, {"docs/topic.md": b"topic\n"}, "topic")
    run_git(source, "switch", "-q", "main")
    run_git(source, "tag", "-f", "-a", "v1.0", "-m", "moved")

    before = refs_of(replica)
    second = tmp_path / "second"
    export(source, second)
    completed = cli("import", second, replica, "--trusted-key", PUBLIC_B64, "--json")
    assert completed.returncode == 5, completed.stderr
    report = json.loads(completed.stdout)
    decisions = {item["ref"]: item["decision"] for item in report["plan"]}
    assert decisions == {
        "refs/heads/feature": "conflict",
        "refs/heads/forkmesh/pulls": "unchanged",
        "refs/heads/main": "fast_forward",
        "refs/heads/topic": "create",
        "refs/tags/v1.0": "conflict",
    }
    after = refs_of(replica)
    assert after["refs/heads/feature"] == before["refs/heads/feature"]
    assert after["refs/tags/v1.0"] == before["refs/tags/v1.0"]
    assert after["refs/heads/main"] == refs_of(source)["refs/heads/main"]
    kept = next(item["keptAt"] for item in report["plan"] if item["ref"] == "refs/heads/feature")
    assert run_git(replica, "rev-parse", kept) == refs_of(source)["refs/heads/feature"]

    rolled = cli("rollback", replica, report["importId"], "--json")
    assert rolled.returncode == 0, rolled.stderr
    assert refs_of(replica) == before
    assert run_git(replica, "for-each-ref", "refs/forkmesh-offline/") == ""
    assert cli("rollback", replica, report["importId"]).returncode == 0  # idempotent

    # Refs that move after an import make rollback refuse rather than clobber.
    again = tool.import_media(second, replica, trusted_keys={PUBLIC}, now_ms=1_900_000_000_000)
    run_git(work, "fetch", "-q", "origin")
    run_git(work, "switch", "-q", "-C", "main", "origin/main")
    commit(work, {"docs/after.md": b"after import\n"}, "after import")
    run_git(work, "push", "-q", "origin", "main")
    moved = refs_of(replica)
    with pytest.raises(tool.VerificationError, match="nothing was rolled back"):
        tool.rollback_import(replica, again["importId"])
    assert refs_of(replica) == moved


def test_checked_out_branch_is_never_moved_under_a_working_tree(tmp_path, source):
    work = tmp_path / "work"
    run_git(tmp_path, "clone", "-q", str(source), str(work))
    commit(source, {"docs/new.md": b"new\n"}, "new on main")
    media = tmp_path / "media"
    export(source, media)
    report = tool.import_media(media, work, trusted_keys={PUBLIC})
    item = next(i for i in report["plan"] if i["ref"] == "refs/heads/main")
    assert item["decision"] == "checked_out"
    assert run_git(work, "rev-parse", "HEAD") != refs_of(source)["refs/heads/main"]
    assert run_git(work, "rev-parse", item["keptAt"]) == refs_of(source)["refs/heads/main"]


# --- CLI, telemetry, documentation, and gates ---------------------------------

def test_cli_exit_codes_and_count_only_telemetry(tmp_path, media, host):
    telemetry = tmp_path / "telemetry.jsonl"
    dest = tmp_path / "dest"
    paused = cli("fetch", host.url, dest, "--budget", "10k", "--telemetry", telemetry, "--json")
    assert paused.returncode == 4, paused.stderr
    assert json.loads(paused.stdout)["reason"] == "budget_exhausted"
    status = cli("status", dest, "--json")
    assert status.returncode == 0 and json.loads(status.stdout)["remainingBytes"] > 0
    done = cli("fetch", host.url, dest, "--telemetry", telemetry, "--trusted-key", PUBLIC_B64)
    assert done.returncode == 0, done.stderr
    replica = bare(tmp_path)
    assert cli("import", dest, replica, "--trusted-key", PUBLIC_B64,
               "--telemetry", telemetry).returncode == 0
    assert cli("fetch", host.url, tmp_path / "x", "--rate", "fast").returncode == 2

    records = [json.loads(line) for line in telemetry.read_text().splitlines()]
    assert [r["event"] for r in records] == ["fetch", "fetch", "import"]
    assert records[1]["outcome"] == "complete"
    assert records[1]["bytesTransferred"] > 0 and records[1]["resumes"] == 1
    text = telemetry.read_text()
    for private in ("alice", "alice/offline", "127.0.0.1", PUBLIC_B64, "refs/"):
        assert private not in text


def test_documentation_covers_the_contract():
    doc = DOC.read_text(encoding="utf-8")
    for command in ("keygen", "export", "verify", "fetch", "status", "import", "rollback", "replica"):
        assert "forkmesh_offline_sync.py %s" % command in doc, command
    for vocabulary in tool.LIMITATIONS + tool.PAUSE_REASONS + tool.DECISIONS + tool.SCOPES:
        assert "`%s`" % vocabulary in doc, vocabulary
    for code in ("0", "2", "3", "4", "5"):
        assert re.search(r"^\| `%s` \|" % code, doc, re.M), code
    assert "limited_replica" in (ROOT / "www" / "docs" / "availability-proof.md").read_text()
    assert "(offline-sync.md)" in (ROOT / "www" / "docs" / "README.md").read_text()


def test_quality_gate_and_platform_matrix_run_this_suite():
    config = json.loads((ROOT / "www" / "docs" / "engineering" / "quality-gates.json").read_text())
    gate = next(g for g in config["gates"] if g["id"] == "offline-sync-conformance")
    assert "app/tests/test_offline_sync.py" in gate["command"]
    assert "offline-sync-conformance" in config["profiles"]["core"]
    assert "offline-sync-conformance" in config["profiles"]["release"]
    workflow = (ROOT / ".github" / "workflows" / "collaboration-conformance.yml").read_text()
    assert "app/tests/test_offline_sync.py" in workflow
    assert "windows-latest" in workflow and "macos-latest" in workflow
