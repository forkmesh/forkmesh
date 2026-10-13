"""Sealed start and the unseal socket (``FORKMESH_VAULT=1``).

With the vault on, a node boots **sealed**: its environment holds no
application secret except the bootstrap credentials rqlite and the mesh need
(``vault_crypto.BOOTSTRAP_NAMES``), and ``DATA_KEY`` is nowhere on the node.
While sealed:

* every request except ``/health`` and ``/readyz`` answers
  ``503 {"error": "sealed"}`` (``/readyz`` reports the reason ``sealed``);
* cron and queue deliveries do not run;
* the Durable Object cluster keeps its membership view but stops
  heart-beating and never hosts objects (``ClusterDurableHost.set_available``).

The operator's desktop unseals it through a unix socket (default
``/run/forkmesh-next/unseal.sock``, mode 0600, owner forkmesh-app; root may
connect, any other peer uid is refused). One command per connection, one
JSON line back::

    unseal\\n<DATA_KEY>\\n   derive the keys, check key_id + __vault_check__
                            against a linearizable read of host_secrets,
                            decrypt every row onto env (DATA_KEY included)
    seal\\n                  drop every vault value and DATA_KEY again
    seal-restart\\n          seal, reply, overwrite the dropped values' bytes
                            (best effort) and exit; systemd restarts the
                            app, which comes back sealed (the IDS lockdown
                            and ``next_vault.py seal --restart`` use it)
    reload\\n                re-read host_secrets (after next_vault.py sync)
    status\\n                sealed?, key_id, secret names (never values)
    ops <action> <account>\\n run forkmesh_host.ops in this process, so no
                            second process ever holds the key

A wrong key leaves the node sealed; after 5 failed unseals within 60 s
further attempts are refused until the window passes.

Peer unseal (``FORKMESH_VAULT_PEER_UNSEAL=1``, cluster mode): a sealed node
asks its core peers (static ``FORKMESH_MESH_PEERS`` map, rendezvous order)
over the WireGuard mesh for the key, at start and every 15 s while sealed,
and unseals with it only after the same ``__vault_check__`` verification.
A peer answers ``POST /_vault/peer-unseal`` only when it is unsealed, the
request carries a valid mesh HMAC, it comes from the requester's own
WireGuard address, and the requester has a fresh ``sealed`` row in
``cluster_members``; the key travels sealed to the requester's ephemeral
X25519 key (vault_crypto.peer_seal). Rate-limited, and every answer is
logged (node, address, time; never the key). Only a restart of every node
at once needs the operator's desktop.

The key is kept only as ``env.DATA_KEY`` (plus the app's own derived
CryptoKey caches); nothing here logs or stores it. Python strings cannot be
wiped, so ``seal`` removes references and clears the app's key caches but
cannot guarantee the bytes are overwritten: see docs/VAULT.md. For that,
``seal-restart`` ends the process (its memory goes back to the kernel, which
zeroes pages before reuse).

IDS quarantine (quarantine.py, docs/IDS.md): a quarantined node refuses
every unseal, and its peers refuse it peer unseal. After the operator's
release, peers keep refusing until the desktop has unsealed it once through
this socket (which stamps the release row).
"""

from __future__ import annotations

import asyncio
import collections
import json
import os
import socket
import stat
import struct
import sys
import time
from pathlib import Path

from . import vault_crypto
from .vault_crypto import VaultError, VaultKey

ENV_FLAG = "FORKMESH_VAULT"
SOCKET_ENV = "FORKMESH_VAULT_SOCKET"
DEFAULT_SOCKET = "/run/forkmesh-next/unseal.sock"
MAX_FAILURES = 5
FAILURE_WINDOW = 60.0
LINE_LIMIT = 4096
READ_TIMEOUT = 10.0
SELECT_ROWS = ("SELECT name, nonce, ciphertext, key_id FROM host_secrets "
               "ORDER BY name")
PEER_ENV_FLAG = "FORKMESH_VAULT_PEER_UNSEAL"
PEERS_ENV = "FORKMESH_MESH_PEERS"
PEER_PATH = "/_vault/peer-unseal"
PEER_RETRY_SECONDS = 15.0
PEER_FIRST_DELAY = 3.0
PEER_TIMEOUT = 5.0
PEER_MAX_PER_NODE = 3          # served per requester per minute
PEER_MAX_TOTAL = 20            # served per minute overall
RESTART_DELAY = 0.2            # seal-restart: reply first, then exit
# entry.py caches of DATA_KEY-derived material (keyed by the secret).
_ENTRY_KEY_CACHES = ("_data_key_cache", "_hmac_key_cache", "_room_key_cache")
_MISSING = object()


def enabled(environ) -> bool:
    return str((environ or {}).get(ENV_FLAG) or "") == "1"


def parse_peers(text):
    """``n1=http://10.77.0.1:8792,n2=...`` -> {node: base url}."""
    peers = {}
    for item in str(text or "").split(","):
        name, sep, url = item.partition("=")
        name, url = name.strip(), url.strip().rstrip("/")
        if sep and name and url.startswith("http://"):
            peers[name] = url
    return peers


def _default_http_client():
    import httpx
    return httpx.AsyncClient(timeout=PEER_TIMEOUT)


def peer_order(requester, nodes):
    """Rendezvous order of the peers a sealed node asks, highest first."""
    import hashlib
    return sorted(nodes, key=lambda node: hashlib.sha256(
        ("vault:peer-unseal:%s:%s" % (requester, node)).encode()).digest(),
        reverse=True)


def harden_process():
    """Best effort: no core dumps, and not dumpable (Linux), so another
    process of the same uid cannot ptrace us or read /proc/<pid>/mem."""
    try:
        import resource
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    except (ImportError, OSError, ValueError):
        pass
    if not sys.platform.startswith("linux"):
        return False
    try:
        import ctypes
        libc = ctypes.CDLL(None, use_errno=True)
        PR_SET_DUMPABLE = 4
        return libc.prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) == 0
    except (OSError, AttributeError):
        return False


def socket_request(lines, path=None, timeout=30.0):
    """Client side: send ``lines`` (str) to the unseal socket, return the
    JSON reply. Used by ``forkmesh_host ops`` under the vault."""
    path = path or os.environ.get(SOCKET_ENV) or DEFAULT_SOCKET
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(timeout)
        client.connect(path)
        client.sendall("".join(line + "\n" for line in lines).encode())
        client.shutdown(socket.SHUT_WR)
        data = b""
        while not data.endswith(b"\n"):
            chunk = client.recv(65536)
            if not chunk:
                break
            data += chunk
    return json.loads(data.decode("utf-8") or "{}")


def _error(code, detail=None, **extra):
    body = {"ok": False, "error": code}
    if detail:
        body["detail"] = detail
    body.update(extra)
    return body


def scrub(value):
    """Best effort: overwrite the bytes of a dropped secret in place (CPython
    compact ASCII str, bytes, bytearray). Only for values nothing will use
    again: ``seal-restart`` calls it just before the process exits."""
    try:
        import ctypes
        if isinstance(value, bytearray):
            for index in range(len(value)):
                value[index] = 0
            return True
        if isinstance(value, str):
            if len(value) < 8 or not value.isascii() or \
                    sys.implementation.name != "cpython":
                return False
        elif not isinstance(value, bytes) or len(value) < 8:
            return False
        offset = sys.getsizeof(value) - len(value) - 1
        if offset <= 0:
            return False
        ctypes.memset(id(value) + offset, 0x2a, len(value))
        return True
    except Exception:
        return False


def _error_code(error: VaultError):
    text = str(error)
    head = text.split(":", 1)[0]
    if head in ("wrong_key", "vault_empty"):
        return head
    if "key_id mismatch" in text or "failed authentication" in text:
        return "vault_tampered"
    return "vault_error"


class Vault:
    def __init__(self, runtime, socket_path=None, clock=time.monotonic):
        self.runtime = runtime
        environ = getattr(runtime.config, "environ", None) or os.environ
        self.socket_path = str(socket_path or environ.get(SOCKET_ENV)
                               or DEFAULT_SOCKET)
        self.clock = clock
        self.sealed = True
        self.key_id = None
        self.unsealed_at = None
        self.names: list = []
        self._shadowed: dict = {}
        self._failures = collections.deque()
        self._lock = None
        self._server = None
        self.stats = {"unseals": 0, "failures": 0, "rate_limited": 0,
                      "reloads": 0, "seals": 0, "peer_served": 0,
                      "peer_refused": 0, "peer_unseals": 0}
        # Peer unseal (cluster mode only): see serve_peer / peer_loop.
        durable = getattr(runtime, "durable", None)
        self.membership = getattr(durable, "membership", None)
        self.node_id = getattr(durable, "node_id", None)
        self.peers = parse_peers(environ.get(PEERS_ENV))
        self.peer_enabled = (str(environ.get(PEER_ENV_FLAG) or "") == "1"
                             and self.membership is not None
                             and bool(self.peers))
        self._mesh_secret = getattr(getattr(durable, "config", None),
                                    "secret", None)
        self._peer_served = collections.deque()
        self.http_client_factory = _default_http_client
        self._peer_task = None
        self._exit_after_reply = False
        self._dropped: list = []
        self.exit = os._exit
        if self.peer_enabled:
            durable.peer_unseal_handler = self.serve_peer

    # -- state ----------------------------------------------------------
    def status(self):
        body = {"ok": True, "sealed": self.sealed, "keyId": self.key_id,
                "secrets": list(self.names), "unsealedAt": self.unsealed_at,
                "stats": dict(self.stats)}
        quarantine = self._quarantine()
        if quarantine is not None:
            body["quarantined"] = quarantine.self_quarantined
            body["needsDesktopUnseal"] = quarantine.self_needs_reunseal
        return body

    def _quarantine(self):
        view = getattr(self.membership, "quarantine", None)
        return view if getattr(view, "enabled", False) else None

    def _lock_(self):
        if self._lock is None:
            self._lock = asyncio.Lock()
        return self._lock

    def _rate_limited(self):
        now = self.clock()
        while self._failures and now - self._failures[0] > FAILURE_WINDOW:
            self._failures.popleft()
        if len(self._failures) >= MAX_FAILURES:
            return max(1, int(FAILURE_WINDOW - (now - self._failures[0])) + 1)
        return 0

    def _fail(self):
        self._failures.append(self.clock())
        self.stats["failures"] += 1

    async def _read_rows(self):
        db = self.runtime.env.DB
        session = getattr(db, "withSession", None)
        target = session("first-primary") if callable(session) else db
        result = await target.prepare(SELECT_ROWS).all()
        rows = getattr(result, "results", None)
        if rows is None and isinstance(result, dict):
            rows = result.get("results")
        return [dict(row) for row in rows or []]

    def _apply(self, values, data_key):
        env = self.runtime.env
        wanted = dict(values)
        wanted[vault_crypto.KEY_NAME] = data_key
        for name in list(self._shadowed):
            if name not in wanted:
                self._restore(name)
        for name, value in wanted.items():
            if name not in self._shadowed:
                self._shadowed[name] = getattr(env, name, _MISSING)
            setattr(env, name, value)
        self.names = sorted(values)

    def _restore(self, name):
        env = self.runtime.env
        original = self._shadowed.pop(name, _MISSING)
        if original is _MISSING:
            if name in vars(env):
                delattr(env, name)
        else:
            setattr(env, name, original)

    def _clear_app_caches(self):
        entry = getattr(self.runtime, "entry", None)
        for name in _ENTRY_KEY_CACHES:
            cache = getattr(entry, name, None)
            if isinstance(cache, dict):
                for key in list(cache):
                    cache[key] = None
        scoped = getattr(entry, "_scoped_key_cache", None)
        if isinstance(scoped, dict):
            scoped.clear()

    def _set_available(self, available):
        setter = getattr(self.runtime, "set_available", None)
        if setter is not None:
            setter(available)

    # -- commands -------------------------------------------------------
    async def unseal(self, key_text, source="socket"):
        # Keys offered by a peer are checked exactly the same way, but a bad
        # one does not count toward the socket's failure limit (a broken
        # peer must not lock the operator out).
        fail = self._fail if source == "socket" else (lambda: None)
        quarantine = self._quarantine()
        if quarantine is not None and quarantine.self_quarantined:
            print("vault: unseal refused (quarantined, from %s)" % source,
                  flush=True)
            return _error("quarantined", "the IDS quarantined this node "
                          "(docs/IDS.md); release it with tools/next_ids.py")
        reunseal = quarantine is not None and quarantine.self_needs_reunseal
        if reunseal and source != "socket":
            return _error("desktop_unseal_required")
        async with self._lock_():
            wait = self._rate_limited() if source == "socket" else 0
            if wait:
                self.stats["rate_limited"] += 1
                return _error("rate_limited", retryAfter=wait)
            try:
                key = VaultKey(key_text)
            except VaultError as error:
                fail()
                return _error("bad_key", str(error))
            if not self.sealed:
                if key.key_id == self.key_id:
                    return dict(self.status(), already=True)
                fail()
                return _error("wrong_key", "the node is unsealed with "
                              "another key; seal it first")
            try:
                rows = await self._read_rows()
            except Exception as error:
                return _error("db_unavailable", type(error).__name__)
            try:
                values = vault_crypto.open_rows(key, rows)
            except VaultError as error:
                fail()
                print("vault: unseal refused (%s, from %s)"
                      % (_error_code(error), source), flush=True)
                return _error(_error_code(error), str(error))
            self._apply(values, vault_crypto.check_key_text(key_text))
            self.key_id = key.key_id
            self.unsealed_at = int(time.time())
            self.sealed = False
            self._failures.clear()
            self.stats["unseals"] += 1
            self._set_available(True)
            if source != "socket":
                self.stats["peer_unseals"] += 1
            print("vault: unsealed (%d secrets, key_id %s, from %s)"
                  % (len(values), key.key_id, source), flush=True)
        if reunseal:
            try:
                await quarantine.mark_reunsealed()
            except Exception as error:
                print("vault: could not stamp the release row: %s"
                      % type(error).__name__, flush=True)
        return self.status()

    async def reload(self):
        async with self._lock_():
            if self.sealed:
                return _error("sealed")
            try:
                key = VaultKey(getattr(self.runtime.env,
                                       vault_crypto.KEY_NAME))
                rows = await self._read_rows()
                values = vault_crypto.open_rows(key, rows)
            except VaultError as error:
                return _error(_error_code(error), str(error))
            except Exception as error:
                return _error("db_unavailable", type(error).__name__)
            self._apply(values, getattr(self.runtime.env,
                                        vault_crypto.KEY_NAME))
            self.stats["reloads"] += 1
            print("vault: reloaded (%d secrets)" % len(values), flush=True)
            return self.status()

    async def seal(self, keep_dropped=False):
        async with self._lock_():
            was_sealed = self.sealed
            self.sealed = True
            self._set_available(False)
            env = self.runtime.env
            if keep_dropped:
                # seal-restart overwrites these once the reply is out.
                self._dropped = [getattr(env, name, None)
                                 for name in self._shadowed]
            for name in list(self._shadowed):
                self._restore(name)
            self._clear_app_caches()
            self.names = []
            self.key_id = None
            self.unsealed_at = None
            if not was_sealed:
                self.stats["seals"] += 1
                print("vault: sealed", flush=True)
            return self.status()

    async def ops(self, action, account):
        if self.sealed:
            return _error("sealed")
        from . import ops as _ops
        command = _ops.COMMANDS.get(action)
        if command is None:
            return _error("unknown_ops_action", action)
        scope = getattr(self.runtime, "_db_scope", None)
        try:
            if scope is not None:
                with scope():
                    return await command(self.runtime, account)
            return await command(self.runtime, account)
        except Exception as error:
            return _error("ops_failed", type(error).__name__)

    # -- peer unseal --------------------------------------------------------
    def _peer_rate_limited(self, node):
        now = self.clock()
        while self._peer_served and now - self._peer_served[0][0] > 60:
            self._peer_served.popleft()
        mine = sum(1 for _, who in self._peer_served if who == node)
        return mine >= PEER_MAX_PER_NODE or \
            len(self._peer_served) >= PEER_MAX_TOTAL

    def _refuse(self, status, code, node, client_ip):
        self.stats["peer_refused"] += 1
        print("vault: peer unseal refused for %s from %s (%s)"
              % (node or "?", client_ip or "?", code), flush=True)
        return status, _error(code)

    async def serve_peer(self, client_ip, body):
        """Mesh ``POST /_vault/peer-unseal`` (already HMAC-verified with
        FORKMESH_MESH_SECRET by the mesh listener). Answers only an
        unsealed host's peers: the requester must be another core node of
        the static FORKMESH_MESH_PEERS map, connecting from that node's
        WireGuard address, with a fresh 'sealed' row in cluster_members.
        The key goes back sealed to the requester's ephemeral X25519 key."""
        try:
            request = json.loads(body or b"{}")
            node = str(request.get("node") or "")
            pub = str(request.get("pub") or "")
            nonce = str(request.get("nonce") or "")
        except (ValueError, AttributeError):
            return self._refuse(400, "bad_request", None, client_ip)
        if self.sealed:
            return 503, _error("sealed")
        replica = None
        if node and node != self.node_id and node not in self.peers:
            replica = await self._replica_peer(node, client_ip)
            if replica is None:
                return self._refuse(403, "unknown_peer", node, client_ip)
        elif not node or node == self.node_id:
            return self._refuse(403, "unknown_peer", node, client_ip)
        quarantine = self._quarantine()
        if quarantine is not None and quarantine.peer_unseal_blocked(node):
            return self._refuse(403, "quarantined", node, client_ip)
        import urllib.parse
        expected = (replica["wg_ip"] if replica is not None else
                    urllib.parse.urlsplit(self.peers[node]).hostname)
        if not client_ip or client_ip != expected:
            return self._refuse(403, "source_mismatch", node, client_ip)
        if not (8 <= len(nonce) <= 64) or len(pub) > 64:
            return self._refuse(400, "bad_request", node, client_ip)
        from . import cluster as _cluster
        row = (self.membership.members or {}).get(node) or {}
        fresh = int(row.get("heartbeat_ms") or 0) >= \
            _cluster.now_ms() - _cluster.STALE_MS
        if replica is None and (not fresh or row.get("status") != "sealed"):
            return self._refuse(403, "not_a_sealed_member", node, client_ip)
        if self._peer_rate_limited(node):
            return self._refuse(429, "rate_limited", node, client_ip)
        try:
            reply = vault_crypto.peer_seal(
                getattr(self.runtime.env, vault_crypto.KEY_NAME), node,
                self.node_id, pub, nonce)
        except (VaultError, AttributeError):
            return self._refuse(400, "bad_request", node, client_ip)
        self._peer_served.append((self.clock(), node))
        self.stats["peer_served"] += 1
        print("vault: peer unseal served to %s (%s) at %d"
              % (node, client_ip, int(time.time())), flush=True)
        return 200, dict(reply, ok=True, keyId=self.key_id)

    async def _replica_peer(self, node, client_ip):
        import re as _re
        if not _re.fullmatch(r"a[1-9]", str(node or "")) or not client_ip:
            return None
        membership = getattr(self, "membership", None)
        rqlite = getattr(membership, "rqlite", None)
        if rqlite is None:
            return None
        try:
            rows = await rqlite.query(
                "SELECT name, wg_ip, state, created_by FROM autoscale_nodes "
                "WHERE name=?", node, level="strong")
        except Exception:
            return None
        row = dict(rows[0]) if rows else None
        if (row is None or row.get("created_by") != "autoscaler"
                or row.get("state") not in ("joining", "serving")
                or row.get("wg_ip") != "10.77.0.%d" % (20 + int(node[1:]))
                or row.get("wg_ip") != client_ip):
            return None
        return row

    async def _ask_peer(self, client, node):
        from . import cluster as _cluster
        private, public = vault_crypto.peer_keypair()
        nonce = os.urandom(16).hex()
        body = json.dumps({"v": 1, "node": self.node_id, "nonce": nonce,
                           "pub": vault_crypto._b64(public)}).encode()
        response = await client.post(
            self.peers[node] + PEER_PATH, content=body, headers={
                "content-type": "application/json",
                _cluster.AUTH_HEADER: _cluster.sign(
                    self._mesh_secret, "POST", PEER_PATH, body)})
        if response.status_code != 200:
            try:
                reason = response.json().get("error")
            except ValueError:
                reason = None
            return None, "%s %s" % (response.status_code, reason or "")
        try:
            key = vault_crypto.peer_open(private, public, self.node_id, node,
                                         nonce, response.json())
        except (VaultError, ValueError) as error:
            return None, str(error)
        return key, None

    async def try_peers(self):
        """Ask the peers in rendezvous order; unseal with the first key
        that passes the vault check. Returns True once unsealed."""
        quarantine = self._quarantine()
        if quarantine is not None and (quarantine.self_quarantined
                                       or quarantine.self_needs_reunseal):
            return False
        nodes = peer_order(self.node_id, [n for n in self.peers
                                          if n != self.node_id])
        async with self.http_client_factory() as client:
            for node in nodes:
                if not self.sealed:
                    return True
                try:
                    key, reason = await self._ask_peer(client, node)
                except Exception as error:
                    key, reason = None, type(error).__name__
                if key is None:
                    continue
                result = await self.unseal(key, source="peer " + node)
                del key
                if result.get("ok"):
                    return True
                print("vault: key from peer %s rejected (%s)"
                      % (node, result.get("error")), flush=True)
        return not self.sealed

    async def peer_loop(self):
        await asyncio.sleep(PEER_FIRST_DELAY)
        while True:
            if self.sealed:
                try:
                    await self.try_peers()
                except Exception as error:
                    print("vault: peer unseal round failed: %r"
                          % (type(error).__name__,), flush=True)
            await asyncio.sleep(PEER_RETRY_SECONDS)

    # -- socket -----------------------------------------------------------
    @staticmethod
    def _peer_allowed(writer):
        sock = writer.get_extra_info("socket")
        option = getattr(socket, "SO_PEERCRED", None)
        if sock is None or option is None:
            return True
        try:
            raw = sock.getsockopt(socket.SOL_SOCKET, option,
                                  struct.calcsize("3i"))
            _pid, uid, _gid = struct.unpack("3i", raw)
        except OSError:
            return False
        return uid in (0, os.getuid())

    async def _dispatch(self, reader):
        line = await asyncio.wait_for(reader.readline(), READ_TIMEOUT)
        command = line.decode("utf-8", "replace").strip()
        if command == "unseal":
            key_line = await asyncio.wait_for(reader.readline(), READ_TIMEOUT)
            try:
                key_text = key_line.decode("utf-8").rstrip("\r\n")
            except UnicodeDecodeError:
                return _error("bad_key", "not utf-8")
            finally:
                del key_line
            return await self.unseal(key_text)
        if command == "seal":
            return await self.seal()
        if command == "seal-restart":
            reply = await self.seal(keep_dropped=True)
            self._exit_after_reply = True
            print("vault: sealed; restarting the process to release its "
                  "memory", flush=True)
            return dict(reply, restarting=True)
        if command == "reload":
            return await self.reload()
        if command == "status":
            return self.status()
        parts = command.split()
        if parts and parts[0] == "ops":
            if len(parts) != 3:
                return _error("usage", "ops <action> <account>")
            return await self.ops(parts[1], parts[2])
        return _error("unknown_command")

    async def _handle(self, reader, writer):
        try:
            if not self._peer_allowed(writer):
                reply = _error("forbidden")
            else:
                try:
                    reply = await self._dispatch(reader)
                except (ValueError, asyncio.LimitOverrunError):
                    reply = _error("line_too_long")
                except asyncio.TimeoutError:
                    reply = _error("timeout")
            writer.write(json.dumps(reply, sort_keys=True).encode() + b"\n")
            await writer.drain()
        except (ConnectionError, OSError):
            pass
        finally:
            writer.close()
            if self._exit_after_reply:
                await self._restart()

    async def seal_and_exit(self):
        """Seal, then end the process like ``seal-restart`` (the IDS
        quarantine of this node, runtime._on_quarantine). Only when a key is
        in memory: a sealed process has nothing to release, and restarting
        it again would loop."""
        if self.sealed:
            return await self.seal()
        reply = await self.seal(keep_dropped=True)
        print("vault: sealed; restarting the process to release its memory",
              flush=True)
        asyncio.get_running_loop().create_task(self._restart())
        return reply

    async def _restart(self):
        """seal-restart: overwrite the dropped values, then leave. The unit
        restarts the app (Restart=always), which comes back sealed."""
        await asyncio.sleep(RESTART_DELAY)
        dropped, self._dropped = self._dropped, []
        for value in dropped:
            scrub(value)
        del dropped
        self.exit(0)

    async def serve(self):
        path = Path(self.socket_path)
        try:
            if stat.S_ISSOCK(path.lstat().st_mode):
                path.unlink()
        except FileNotFoundError:
            pass
        old = os.umask(0o177)
        try:
            self._server = await asyncio.start_unix_server(
                self._handle, path=str(path), limit=LINE_LIMIT)
        finally:
            os.umask(old)
        os.chmod(path, 0o600)
        print("vault: sealed; unseal socket %s%s" % (
            path, "; asking peers %s" % ",".join(sorted(self.peers))
            if self.peer_enabled else ""), flush=True)
        if self.peer_enabled and self._peer_task is None:
            self._peer_task = asyncio.get_running_loop().create_task(
                self.peer_loop())
        return self._server

    async def close(self):
        if self._peer_task is not None:
            self._peer_task.cancel()
            self._peer_task = None
        server, self._server = self._server, None
        if server is None:
            return
        server.close()
        try:
            await asyncio.wait_for(server.wait_closed(), 2)
        except (asyncio.TimeoutError, Exception):
            pass
        try:
            path = Path(self.socket_path)
            if stat.S_ISSOCK(path.lstat().st_mode):
                path.unlink()
        except OSError:
            pass
