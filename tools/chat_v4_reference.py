#!/usr/bin/env python3
import copy
import hashlib
import hmac
import json
import os
import re
import subprocess
import sys
from pathlib import Path

from cryptography.exceptions import InvalidTag

sys.path.insert(0, str(Path(__file__).resolve().parent))

import chat_v3_reference as v3  # noqa: E402
from chat_v3_reference import (  # noqa: E402
    DAY_MS, MAX_RECIPIENTS, MLKEM_CT_BYTES, PREKEY_LIFE_MS, REPLAY_WINDOW, aes_open, aes_seal, b64, b64u,
    ed_sign, ed_verify, hkdf, id16, mlkem_decaps, mlkem_encaps, sha256, unb64, unb64u, x25519, x25519_pub)

VERSION = 4
MAX_SKIP = 256
SKIPPED_KEPT = 512
SKIPPED_LIFE_MS = 8 * DAY_MS
SESSIONS_KEPT = 4
RESET_TYPE = "e2ee-reset"
VECTORS_PATH = Path(__file__).resolve().parents[1] / "conformance" / "chat" / "v4" / "vectors.json"
KID_RE = re.compile(r"^[A-Za-z0-9_-]{16}$")
KEY32_RE = re.compile(r"^[A-Za-z0-9_-]{43}$")


class NoSession(Exception):
    pass


class Replay(Exception):
    pass


def hmac256(key, data):
    return hmac.new(key, data, hashlib.sha256).digest()


def kdf_init(room, sender, recipient, spk_id, sid, ss1, ss2):
    salt = sha256(("ForkMesh chat e2ee salt v4:" + room).encode())
    info = "\n".join(["forkmesh-chat-e2ee-init-v4", room, sender, recipient, spk_id, sid]).encode()
    out = hkdf(ss1 + ss2, salt, info, 64)
    return out[:32], out[32:]


def kdf_rk(rk, dh_out, ss):
    out = hkdf(dh_out + ss, rk, b"forkmesh-chat-e2ee-ratchet-v4", 64)
    return out[:32], out[32:]


def kdf_ck(ck):
    return hmac256(ck, b"\x01"), hmac256(ck, b"\x02")


def session_id(dh_pub, ct):
    return id16(dh_pub + ct)


def content_aad(room, sender, ctr, ts):
    return "\n".join(["forkmesh-chat-e2ee-content-v4", room, sender, str(ctr), str(ts)]).encode()


def entry_line(entry):
    return "|".join([entry["kid"], entry["sid"], entry["spk"], b64u(sha256(unb64(entry["ct"]))), entry["dh"],
                     str(entry["pn"]), str(entry["n"]), entry["nonce"], entry["tag"], entry["key"]])


def wrap_aad(room, sender, entry, body_nonce, body_tag):
    return "\n".join(["forkmesh-chat-e2ee-wrap-v4", room, sender, entry["kid"], entry["sid"], entry["spk"],
                      b64u(sha256(unb64(entry["ct"]))), entry["dh"], str(entry["pn"]), str(entry["n"]),
                      body_nonce, body_tag]).encode()


def sealed_message(room, frame):
    head = "\n".join([
        "forkmesh-chat-e2ee-sealed-v4", room, frame["kid"], frame["idk"], str(frame["ctr"]), str(frame["ts"]),
        frame["nonce"], frame["tag"], b64u(sha256(unb64(frame["body"])))])
    return (head + "".join("\n" + entry_line(k) for k in frame["keys"])).encode()


def new_x25519():
    priv = os.urandom(32)
    return {"priv": b64(priv), "pub": b64u(x25519_pub(priv))}


def _count(value):
    return isinstance(value, int) and not isinstance(value, bool) and 0 <= value < 2 ** 31


def valid_entry(entry):
    if not isinstance(entry, dict):
        return False
    try:
        if not all(isinstance(entry.get(k), str) for k in ("kid", "sid", "spk", "ct", "dh", "nonce", "tag", "key")):
            return False
        if not (KID_RE.match(entry["kid"]) and KID_RE.match(entry["sid"]) and KID_RE.match(entry["spk"])):
            return False
        if not KEY32_RE.match(entry["dh"]) or not (_count(entry["pn"]) and _count(entry["n"])):
            return False
        return (len(unb64(entry["ct"])) == MLKEM_CT_BYTES and len(unb64(entry["nonce"])) == 12
                and len(unb64(entry["tag"])) == 16 and len(unb64(entry["key"])) == 32)
    except (KeyError, ValueError):
        return False


class Device(v3.Device):
    def __init__(self, ik_seed):
        super().__init__(ik_seed)
        self.ratchets = {}

    def peer_state(self, room, kid):
        return self.ratchets.get(room, {}).get(kid) or {"current": None, "sessions": []}

    def export_ratchets(self, room):
        return {"v": VERSION, "peers": copy.deepcopy(self.ratchets.get(room, {}))}

    def import_ratchets(self, room, stored):
        self.ratchets[room] = copy.deepcopy(stored["peers"])

    def reset(self, room, kid):
        self.ratchets.get(room, {}).pop(kid, None)

    def _store_peer(self, room, kid, peer, now):
        peer["sessions"].sort(key=lambda s: s["used"], reverse=True)
        current = next((s for s in peer["sessions"] if s["sid"] == peer["current"]), None)
        kept = [current] if current else []
        kept += [s for s in peer["sessions"] if s is not current][:SESSIONS_KEPT - len(kept)]
        for session in kept:
            session["skipped"] = [k for k in session["skipped"] if k["ts"] >= now - SKIPPED_LIFE_MS][-SKIPPED_KEPT:]
        peer["sessions"] = kept
        self.ratchets.setdefault(room, {})[kid] = peer

    def _prekey(self, spk_id):
        return next((p for p in self.prekeys if p["id"] == spk_id), None)

    def _skip(self, session, until, now):
        if until < session["nr"]:
            raise Replay()
        if until - session["nr"] > MAX_SKIP:
            raise NoSession()
        ck = unb64(session["ckr"])
        while session["nr"] < until:
            mk, ck = kdf_ck(ck)
            session["skipped"].append({"dh": session["dhr"], "n": session["nr"], "mk": b64(mk), "ts": now})
            session["nr"] += 1
        session["ckr"] = b64(ck)

    def _receive(self, session, entry, now):
        for i, skipped in enumerate(session["skipped"]):
            if skipped["dh"] == entry["dh"] and skipped["n"] == entry["n"]:
                del session["skipped"][i]
                return unb64(skipped["mk"])
        if entry["dh"] != session["dhr"]:
            if session["dhs"] is None:
                raise NoSession()
            if session["ckr"] is not None:
                self._skip(session, entry["pn"], now)
            spk = self._prekey(entry["spk"])
            if spk is None:
                raise NoSession()
            ss = mlkem_decaps(spk["pq_seed"], unb64(entry["ct"]))
            dh_out = x25519(unb64(session["dhs"]["priv"]), unb64u(entry["dh"]))
            rk, ckr = kdf_rk(unb64(session["rk"]), dh_out, ss)
            session.update(rk=b64(rk), ckr=b64(ckr), nr=0, dhr=entry["dh"], pn=session["ns"], ns=0, cks=None,
                           sq=None)
        if session["ckr"] is None:
            raise NoSession()
        self._skip(session, entry["n"], now)
        mk, ckr = kdf_ck(unb64(session["ckr"]))
        session["ckr"] = b64(ckr)
        session["nr"] += 1
        return mk

    def _open_entry(self, room, sender, entry, now):
        peer = copy.deepcopy(self.peer_state(room, sender))
        session = next((s for s in peer["sessions"] if s["sid"] == entry["sid"]), None)
        if session is None:
            dh, ct = unb64u(entry["dh"]), unb64(entry["ct"])
            if entry["sid"] != session_id(dh, ct) or entry["pn"] != 0:
                raise NoSession()
            spk = self._prekey(entry["spk"])
            if spk is None:
                raise NoSession()
            ss1 = x25519(spk["x_priv"], dh)
            ss2 = mlkem_decaps(spk["pq_seed"], ct)
            rk, ck = kdf_init(room, sender, self.kid, spk["id"], entry["sid"], ss1, ss2)
            session = {"sid": entry["sid"], "rk": b64(rk), "dhs": None, "dhr": entry["dh"], "cks": None, "ns": 0,
                       "pn": 0, "sq": None, "ckr": b64(ck), "nr": 0, "skipped": [], "created": now, "used": now}
            peer["sessions"].append(session)
        mk = self._receive(session, entry, now)
        session["used"] = now
        peer["current"] = session["sid"]
        return mk, peer

    def _send_entry(self, room, recipient, spk, now, peers):
        peer = peers.setdefault(recipient, copy.deepcopy(self.peer_state(room, recipient)))
        session = next((s for s in peer["sessions"] if s["sid"] == peer["current"]), None)
        if session is not None and session["cks"] is None and session["dhr"] is None:
            session = None
        if session is None:
            eph = new_x25519()
            ss1 = x25519(unb64(eph["priv"]), unb64u(spk["x"]))
            ct, ss2 = mlkem_encaps(unb64u(spk["pq"]))
            sid = session_id(unb64u(eph["pub"]), ct)
            rk, ck = kdf_init(room, self.kid, recipient, spk["id"], sid, ss1, ss2)
            session = {"sid": sid, "rk": b64(rk), "dhs": eph, "dhr": None, "cks": b64(ck), "ns": 0, "pn": 0,
                       "sq": {"spk": spk["id"], "ct": b64(ct)}, "ckr": None, "nr": 0, "skipped": [],
                       "created": now, "used": now}
            peer["sessions"].append(session)
            peer["current"] = sid
        elif session["cks"] is None:
            dhs = new_x25519()
            ct, ss = mlkem_encaps(unb64u(spk["pq"]))
            rk, cks = kdf_rk(unb64(session["rk"]), x25519(unb64(dhs["priv"]), unb64u(session["dhr"])), ss)
            session.update(rk=b64(rk), cks=b64(cks), dhs=dhs, sq={"spk": spk["id"], "ct": b64(ct)})
        mk, cks = kdf_ck(unb64(session["cks"]))
        entry = {"kid": recipient, "sid": session["sid"], "spk": session["sq"]["spk"], "ct": session["sq"]["ct"],
                 "dh": session["dhs"]["pub"], "pn": session["pn"], "n": session["ns"]}
        session["cks"] = b64(cks)
        session["ns"] += 1
        session["used"] = now
        return entry, mk

    def seal4(self, room, scope, plain, now, only=None):
        ctr = self.next_counter(room)
        ck = os.urandom(32)
        nonce, body, tag = aes_seal(ck, json.dumps(plain, separators=(",", ":")).encode(),
                                    content_aad(room, self.kid, ctr, now))
        targets = {}
        for record in self.records(scope).values():
            if record["state"] != "approved" or record["kid"] == self.kid:
                continue
            if only is not None and record["kid"] not in only:
                continue
            if record["spk"]["exp"] > now:
                targets[record["kid"]] = record["spk"]
        if len(targets) > MAX_RECIPIENTS:
            raise ValueError("too many recipients")
        frame = {"type": "sealed", "v": VERSION, "kid": self.kid, "idk": b64u(self.idk), "ctr": ctr, "ts": now,
                 "nonce": b64(nonce), "tag": b64(tag), "body": b64(body), "keys": []}
        peers = {}
        for recipient in sorted(targets):
            entry, mk = self._send_entry(room, recipient, targets[recipient], now, peers)
            knonce, wrapped, ktag = aes_seal(mk, ck, wrap_aad(room, self.kid, entry, frame["nonce"], frame["tag"]))
            entry.update(nonce=b64(knonce), tag=b64(ktag), key=b64(wrapped))
            frame["keys"].append(entry)
        for recipient, peer in peers.items():
            self._store_peer(room, recipient, peer, now)
        frame["sig"] = b64u(ed_sign(self.ik_seed, sealed_message(room, frame)))
        return frame

    def open4(self, room, scope, frame, now, commit=True):
        try:
            if frame.get("type") != "sealed" or frame.get("v") != VERSION:
                return ("invalid",)
            kid, idk = frame["kid"], frame["idk"]
            idk_bytes = unb64u(idk)
            keys = frame["keys"]
            if len(idk_bytes) != 32 or kid != id16(idk_bytes) or not isinstance(keys, list):
                return ("invalid",)
            kids = [k.get("kid") if isinstance(k, dict) else None for k in keys]
            if len(keys) > MAX_RECIPIENTS or not all(valid_entry(k) for k in keys):
                return ("invalid",)
            if kids != sorted(kids) or len(set(kids)) != len(kids) or kid in kids:
                return ("invalid",)
            ctr, ts = frame["ctr"], frame["ts"]
            if not _count(ctr) or ctr < 1 or not isinstance(ts, int) or isinstance(ts, bool):
                return ("invalid",)
            if kid == self.kid:
                return ("own",)
            record = self.records(scope).get(kid)
            if record is None or record["state"] != "approved":
                if commit:
                    self.held.setdefault(room, []).append(frame)
                    self.held[room] = self.held[room][-64:]
                return ("held", kid)
            if record["idk"] != idk:
                return ("invalid",)
            if not ed_verify(idk_bytes, unb64u(frame["sig"]), sealed_message(room, frame)):
                return ("invalid",)
            seen = self.seen.setdefault((room, kid), {"max": 0, "set": set()})
            if ctr <= seen["max"] - REPLAY_WINDOW or ctr in seen["set"]:
                return ("replay",)
            if not now - PREKEY_LIFE_MS <= ts <= now + DAY_MS:
                return ("invalid",)
            entry = next((k for k in keys if k["kid"] == self.kid), None)
            if entry is None:
                return ("notForMe",)
            try:
                mk, peer = self._open_entry(room, kid, entry, now)
                ck = aes_open(mk, unb64(entry["nonce"]), unb64(entry["key"]), unb64(entry["tag"]),
                              wrap_aad(room, kid, entry, frame["nonce"], frame["tag"]))
            except Replay:
                return ("replay",)
            except (NoSession, InvalidTag, ValueError, subprocess.CalledProcessError):
                return ("noSession", kid)
            clear = aes_open(ck, unb64(frame["nonce"]), unb64(frame["body"]), unb64(frame["tag"]),
                             content_aad(room, kid, ctr, ts))
            plain = json.loads(clear)
            if not isinstance(plain, dict):
                return ("invalid",)
            if "senderId" in plain and plain["senderId"] != record["node"]:
                return ("invalid",)
            if commit:
                self._store_peer(room, kid, peer, now)
                seen["set"].add(ctr)
                seen["max"] = max(seen["max"], ctr)
                seen["set"] = {c for c in seen["set"] if c > seen["max"] - REPLAY_WINDOW}
            return ("opened", plain, kid, record["acct"])
        except (KeyError, TypeError, ValueError, InvalidTag, json.JSONDecodeError):
            return ("invalid",)


def _seed(label, length=32):
    return hashlib.sha512(("forkmesh-chat-v4-vector:" + label).encode()).digest()[:length]


def _mlkem_seed(label):
    return hashlib.sha512(("forkmesh-chat-v4-vector-mlkem:" + label).encode()).digest()


def _kdf_vectors():
    room = "chat-direct:0123456789abcdef0123456789abcdef:v1"
    ss1, ss2, rk, dh_out, ss, ck = (_seed(n) for n in ("ss1", "ss2", "rk", "dh-out", "ss", "ck"))
    sid = session_id(_seed("dh"), _seed("ct", 64) * 17)
    init = kdf_init(room, "AAAAAAAAAAAAAAAA", "BBBBBBBBBBBBBBBB", "CCCCCCCCCCCCCCCC", sid, ss1, ss2)
    step = kdf_rk(rk, dh_out, ss)
    chain = kdf_ck(ck)
    return {
        "sessionId": {"dh": b64u(_seed("dh")), "ct": b64(_seed("ct", 64) * 17), "sid": sid},
        "init": {"room": room, "sender": "AAAAAAAAAAAAAAAA", "recipient": "BBBBBBBBBBBBBBBB",
                 "spk": "CCCCCCCCCCCCCCCC", "sid": sid, "ss1": b64(ss1), "ss2": b64(ss2),
                 "rk": b64(init[0]), "ck": b64(init[1])},
        "rootStep": {"rk": b64(rk), "dhOut": b64(dh_out), "ss": b64(ss), "nextRk": b64(step[0]), "ck": b64(step[1])},
        "chainStep": {"ck": b64(ck), "mk": b64(chain[0]), "nextCk": b64(chain[1])},
    }


def _secrets(device):
    return {"identitySeed": b64(device.ik_seed), "idk": b64u(device.idk), "kid": device.kid,
            "prekeys": [{"id": p["id"], "xPriv": b64(p["x_priv"]), "x": b64u(p["x"]), "pqSeed": b64(p["pq_seed"]),
                         "pq": b64u(p["pq"]), "ts": p["ts"], "exp": p["exp"]} for p in device.prekeys]}


def _expect(result, name, kind):
    if result[0] != kind:
        raise AssertionError("%s gave %s" % (name, result))
    return result


def build_vectors():
    now = 1_790_000_000_000
    room = "chat-direct:0123456789abcdef0123456789abcdef:v1"
    scope = "direct:0123456789abcdef0123456789abcdef"
    participants = ["alice", "bob"]
    nodes = {"alice": "node-alice-phone", "bob": "node-bob-phone"}
    alice = Device(_seed("alice"))
    alice.add_prekey(_seed("alice-spk-x"), _mlkem_seed("alice-spk"), now - DAY_MS // 2)
    bob = Device(_seed("bob"))
    bob.add_prekey(_seed("bob-spk-x"), _mlkem_seed("bob-spk"), now - DAY_MS // 3)
    bundles = {"alice": alice.bundle(room, "alice", nodes["alice"]), "bob": bob.bundle(room, "bob", nodes["bob"])}
    for device in (alice, bob):
        for bundle in bundles.values():
            if device.accept_bundle(scope, "strict", room, bundle, now, participants) != "approved":
                raise AssertionError("bundle not approved")
    devices = {"alice": _secrets(alice), "bob": _secrets(bob)}

    def chat(sender, mid, text, at):
        return {"type": "chat", "id": mid, "senderId": nodes[sender], "channel": scope, "text": text, "ts": at}

    m1 = alice.seal4(room, scope, chat("alice", "m1", "Hello Bob ❤️", now), now)
    m2 = alice.seal4(room, scope, chat("alice", "m2", "second, same chain", now + 1000), now + 1000)
    empty = alice.seal4(room, scope, chat("alice", "e1", "to nobody yet", now + 1500), now + 1500, only=[])
    alice_after_m2 = alice.export_ratchets(room)

    _expect(bob.open4(room, scope, m2, now + 2000), "m2", "opened")
    _expect(bob.open4(room, scope, m1, now + 2000), "m1", "opened")
    bob_after_m1 = bob.export_ratchets(room)
    r1 = bob.seal4(room, scope, chat("bob", "r1", "hi Alice", now + 3000), now + 3000)
    bob_after_r1 = bob.export_ratchets(room)

    _expect(alice.open4(room, scope, r1, now + 4000), "r1", "opened")
    m3 = alice.seal4(room, scope, chat("alice", "m3", "third, new chain", now + 5000), now + 5000)
    m4 = alice.seal4(room, scope, chat("alice", "m4", "fourth", now + 6000), now + 6000)

    tampered = json.loads(json.dumps(m3))
    tampered["keys"][0]["n"] = 7
    forged = alice.seal4(room, scope, dict(chat("alice", "m5", "I am Bob", now + 7000), senderId=nodes["bob"]), now + 7000)

    check = Device(bob.ik_seed)
    check.prekeys = [dict(p) for p in bob.prekeys]
    for bundle in bundles.values():
        check.accept_bundle(scope, "strict", room, bundle, now, participants)
    check.import_ratchets(room, bob_after_r1)
    _expect(check.open4(room, scope, m4, now + 8000), "m4", "opened")
    _expect(check.open4(room, scope, m3, now + 8000), "m3", "opened")
    _expect(check.open4(room, scope, m3, now + 8000), "m3 again", "replay")
    _expect(check.open4(room, scope, tampered, now + 8000), "tampered", "invalid")
    _expect(check.open4(room, scope, forged, now + 8000), "forged", "invalid")
    check.seen.clear()
    _expect(check.open4(room, scope, m1, now + 8000), "m1 after its key is gone", "replay")
    fresh = Device(bob.ik_seed)
    fresh.prekeys = [dict(p) for p in bob.prekeys]
    for bundle in bundles.values():
        fresh.accept_bundle(scope, "strict", room, bundle, now, participants)
    _expect(fresh.open4(room, scope, m4, now + 8000), "m4 without a session", "noSession")
    _expect(fresh.open4(room, scope, empty, now + 8000), "empty", "notForMe")
    _expect(alice.open4(room, scope, m1, now + 8000), "own", "own")

    def entry(frame):
        return frame["keys"][0]

    session_after_m1 = bob_after_m1["peers"][alice.kid]["sessions"][0]
    return {
        "version": VERSION,
        "now": now,
        "room": room,
        "scope": scope,
        "participants": participants,
        "nodes": nodes,
        "devices": devices,
        "bundles": bundles,
        "kdf": _kdf_vectors(),
        "frames": {"m1": m1, "m2": m2, "empty": empty, "r1": r1, "m3": m3, "m4": m4, "tampered": tampered,
                   "forgedSenderId": forged},
        "plain": {
            "m1": chat("alice", "m1", "Hello Bob ❤️", now),
            "m2": chat("alice", "m2", "second, same chain", now + 1000),
            "r1": chat("bob", "r1", "hi Alice", now + 3000),
            "m3": chat("alice", "m3", "third, new chain", now + 5000),
            "m4": chat("alice", "m4", "fourth", now + 6000),
        },
        "headers": {name: {k: entry(f)[k] for k in ("sid", "spk", "dh", "pn", "n")}
                    for name, f in (("m1", m1), ("m2", m2), ("r1", r1), ("m3", m3), ("m4", m4))},
        "states": {"aliceAfterM2": alice_after_m2, "bobAfterM1": bob_after_m1, "bobAfterR1": bob_after_r1},
        "bobAfterM1": {"rk": session_after_m1["rk"], "ckr": session_after_m1["ckr"], "nr": session_after_m1["nr"],
                       "skipped": len(session_after_m1["skipped"])},
        "sequence": [
            {"device": "bob", "state": None, "frame": "m2", "at": now + 2000, "expect": "opened", "plain": "m2"},
            {"device": "bob", "frame": "m1", "at": now + 2000, "expect": "opened", "plain": "m1"},
            {"device": "alice", "state": "aliceAfterM2", "frame": "r1", "at": now + 4000, "expect": "opened",
             "plain": "r1"},
            {"device": "alice", "frame": "m1", "at": now + 4000, "expect": "own"},
            {"device": "bob", "state": "bobAfterR1", "frame": "m4", "at": now + 8000, "expect": "opened",
             "plain": "m4"},
            {"device": "bob", "frame": "m3", "at": now + 8000, "expect": "opened", "plain": "m3"},
            {"device": "bob", "frame": "m3", "at": now + 8000, "expect": "replay"},
            {"device": "bob", "frame": "tampered", "at": now + 8000, "expect": "invalid"},
            {"device": "bob", "frame": "forgedSenderId", "at": now + 8000, "expect": "invalid"},
            {"device": "bob", "frame": "m1", "at": now + 8000, "clearSeen": True, "expect": "replay"},
            {"device": "bob", "state": None, "frame": "m4", "at": now + 8000, "expect": "noSession"},
            {"device": "bob", "frame": "empty", "at": now + 8000, "expect": "notForMe"},
        ],
    }


def main(argv):
    if argv[1:] == ["vectors"]:
        VECTORS_PATH.parent.mkdir(parents=True, exist_ok=True)
        VECTORS_PATH.write_text(json.dumps(build_vectors(), indent=1, sort_keys=True, ensure_ascii=False) + "\n")
        print(VECTORS_PATH)
        return 0
    print("usage: chat_v4_reference.py vectors", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
