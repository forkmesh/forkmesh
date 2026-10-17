#!/usr/bin/env python3
import base64
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from cryptography.exceptions import InvalidSignature, InvalidTag

DAY_MS = 24 * 60 * 60 * 1000
PREKEY_LIFE_MS = 8 * DAY_MS
MAX_RECIPIENTS = 64
REPLAY_WINDOW = 1024
MLKEM_PUB_BYTES = 1184
MLKEM_CT_BYTES = 1088
MLKEM_SPKI_PREFIX = bytes.fromhex("308204b2300b0609608648016503040402038204a100")
VECTORS_PATH = Path(__file__).resolve().parents[1] / "conformance" / "chat" / "v3" / "vectors.json"


def b64u(data):
    return base64.urlsafe_b64encode(data).decode().rstrip("=")


def unb64u(text):
    return base64.urlsafe_b64decode(text + "=" * (-len(text) % 4))


def b64(data):
    return base64.b64encode(data).decode()


def unb64(text):
    return base64.b64decode(text, validate=True)


def sha256(data):
    return hashlib.sha256(data).digest()


def id16(data):
    return b64u(sha256(data))[:16]


def hkdf(ikm, salt, info, length=32):
    return HKDF(algorithm=hashes.SHA256(), length=length, salt=salt, info=info).derive(ikm)


def aes_seal(key, plain, aad, nonce=None):
    nonce = nonce if nonce is not None else os.urandom(12)
    out = AESGCM(key).encrypt(nonce, plain, aad)
    return nonce, out[:-16], out[-16:]


def aes_open(key, nonce, cipher, tag, aad):
    return AESGCM(key).decrypt(nonce, cipher + tag, aad)


def x25519_pub(priv):
    return X25519PrivateKey.from_private_bytes(priv).public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw)


def x25519(priv, pub):
    shared = X25519PrivateKey.from_private_bytes(priv).exchange(X25519PublicKey.from_public_bytes(pub))
    if shared == bytes(32):
        raise ValueError("all-zero X25519 shared secret")
    return shared


def ed_pub(seed):
    return Ed25519PrivateKey.from_private_bytes(seed).public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw)


def ed_sign(seed, message):
    return Ed25519PrivateKey.from_private_bytes(seed).sign(message)


def ed_verify(pub, signature, message):
    try:
        Ed25519PublicKey.from_public_bytes(pub).verify(signature, message)
        return True
    except (InvalidSignature, ValueError):
        return False


def _openssl(args, cwd):
    subprocess.run(["openssl", *args], cwd=cwd, check=True, capture_output=True)


def mlkem_pub(seed):
    with tempfile.TemporaryDirectory() as work:
        _openssl(["genpkey", "-algorithm", "ML-KEM-768", "-pkeyopt", "hexseed:" + seed.hex(), "-out", "k.pem"], work)
        _openssl(["pkey", "-in", "k.pem", "-pubout", "-outform", "DER", "-out", "p.der"], work)
        der = Path(work, "p.der").read_bytes()
    if len(der) != len(MLKEM_SPKI_PREFIX) + MLKEM_PUB_BYTES or not der.startswith(MLKEM_SPKI_PREFIX):
        raise ValueError("unexpected ML-KEM public key encoding")
    return der[len(MLKEM_SPKI_PREFIX):]


def mlkem_encaps(pub):
    with tempfile.TemporaryDirectory() as work:
        Path(work, "p.der").write_bytes(MLKEM_SPKI_PREFIX + pub)
        _openssl(["pkey", "-pubin", "-inform", "DER", "-in", "p.der", "-out", "p.pem"], work)
        _openssl(["pkeyutl", "-encap", "-inkey", "p.pem", "-pubin", "-out", "ct.bin", "-secret", "ss.bin"], work)
        return Path(work, "ct.bin").read_bytes(), Path(work, "ss.bin").read_bytes()


def mlkem_decaps(seed, ct):
    with tempfile.TemporaryDirectory() as work:
        _openssl(["genpkey", "-algorithm", "ML-KEM-768", "-pkeyopt", "hexseed:" + seed.hex(), "-out", "k.pem"], work)
        Path(work, "ct.bin").write_bytes(ct)
        _openssl(["pkeyutl", "-decap", "-inkey", "k.pem", "-in", "ct.bin", "-secret", "ss.bin"], work)
        return Path(work, "ss.bin").read_bytes()


def prekey(x_priv, pq_seed, ts):
    x = x25519_pub(x_priv)
    pq = mlkem_pub(pq_seed)
    return {"id": id16(x + pq), "x": x, "pq": pq, "ts": ts, "exp": ts + PREKEY_LIFE_MS,
            "x_priv": x_priv, "pq_seed": pq_seed}


def public_prekey(spk):
    return {"id": spk["id"], "x": b64u(spk["x"]), "pq": b64u(spk["pq"]), "ts": spk["ts"], "exp": spk["exp"]}


def bundle_message(room, kid, idk, acct, node, spk):
    return "\n".join([
        "forkmesh-chat-e2ee-bundle-v3", room, kid, idk, acct, node, spk["id"], spk["x"], spk["pq"],
        str(spk["ts"]), str(spk["exp"])]).encode()


def valid_acct(acct):
    return isinstance(acct, str) and 1 <= len(acct) <= 128 and acct.isprintable() and "\n" not in acct


def verify_bundle(bundle, room, now):
    try:
        if bundle.get("type") != "e2ee-key" or bundle.get("v") != 3:
            return False
        kid, idk, acct, node, spk = bundle["kid"], bundle["idk"], bundle["acct"], bundle["node"], bundle["spk"]
        idk_bytes, x, pq = unb64u(idk), unb64u(spk["x"]), unb64u(spk["pq"])
        if len(idk_bytes) != 32 or len(x) != 32 or len(pq) != MLKEM_PUB_BYTES:
            return False
        if kid != id16(idk_bytes) or spk["id"] != id16(x + pq) or not valid_acct(acct) or not valid_acct(node):
            return False
        ts, exp = spk["ts"], spk["exp"]
        if not (isinstance(ts, int) and isinstance(exp, int)) or not (0 < exp - ts <= 9 * DAY_MS):
            return False
        if ts > now + DAY_MS or exp <= now:
            return False
        return ed_verify(idk_bytes, unb64u(bundle["sig"]), bundle_message(room, kid, idk, acct, node, spk))
    except (KeyError, TypeError, ValueError):
        return False


def kek(room, sender, recipient, spk_id, eph_pub, ct, ss1, ss2):
    salt = sha256(("ForkMesh chat e2ee salt v3:" + room).encode())
    info = "\n".join(["forkmesh-chat-e2ee-kek-v3", room, sender, recipient, spk_id, b64u(eph_pub), b64u(sha256(ct))])
    return hkdf(ss1 + ss2, salt, info.encode())


def content_aad(room, sender, ctr, ts):
    return "\n".join(["forkmesh-chat-e2ee-content-v3", room, sender, str(ctr), str(ts)]).encode()


def wrap_aad(room, sender, recipient, nonce, tag):
    return "\n".join(["forkmesh-chat-e2ee-wrap-v3", room, sender, recipient, nonce, tag]).encode()


def sealed_message(room, frame):
    head = "\n".join([
        "forkmesh-chat-e2ee-sealed-v3", room, frame["kid"], frame["idk"], str(frame["ctr"]), str(frame["ts"]),
        frame["nonce"], frame["tag"], b64u(sha256(unb64(frame["body"])))])
    tail = "".join(
        "\n" + "|".join([k["kid"], k["spk"], k["eph"], b64u(sha256(unb64(k["ct"]))), k["nonce"], k["tag"], k["key"]])
        for k in frame["keys"])
    return (head + tail).encode()


def safety_number(scope, idks):
    keys = b"".join(sorted(idks))
    h = hashlib.sha512(("forkmesh-safety-v3\n" + scope + "\n").encode() + keys).digest()
    for _ in range(5199):
        h = hashlib.sha512(h + keys).digest()
    digits = "".join("%05d" % (int.from_bytes(h[i:i + 5], "big") % 100000) for i in range(0, 60, 5))
    return digits, "forkmesh-safety:3:%s:%s" % (scope, b64u(h[:32]))


class Device:
    def __init__(self, ik_seed):
        self.ik_seed = ik_seed
        self.idk = ed_pub(ik_seed)
        self.kid = id16(self.idk)
        self.prekeys = []
        self.counters = {}
        self.trust = {}
        self.seen = {}
        self.held = {}

    def add_prekey(self, x_priv, pq_seed, ts):
        spk = prekey(x_priv, pq_seed, ts)
        self.prekeys.append(spk)
        self.prekeys = self.prekeys[-10:]
        return spk

    def newest_prekey(self):
        return max(self.prekeys, key=lambda p: p["ts"])

    def wipe_expired(self, now):
        self.prekeys = [p for p in self.prekeys if now <= p["exp"]]

    def bundle(self, room, acct, node):
        spk = public_prekey(self.newest_prekey())
        idk = b64u(self.idk)
        sig = ed_sign(self.ik_seed, bundle_message(room, self.kid, idk, acct, node, spk))
        return {"type": "e2ee-key", "v": 3, "kid": self.kid, "idk": idk, "acct": acct, "node": node, "spk": spk,
                "sig": b64u(sig)}

    def records(self, scope):
        return self.trust.setdefault(scope, {})

    def accept_bundle(self, scope, policy, room, bundle, now, participants=None):
        if not verify_bundle(bundle, room, now):
            return None
        if participants is not None and bundle["acct"] not in participants:
            return None
        records = self.records(scope)
        known = records.get(bundle["kid"])
        if known:
            if known["idk"] != bundle["idk"] or known["acct"] != bundle["acct"] or known["node"] != bundle["node"]:
                return None
            if bundle["spk"]["ts"] > known["spk"]["ts"]:
                known["spk"] = bundle["spk"]
            return known["state"]
        state = "approved"
        if policy == "strict" and bundle["kid"] != self.kid:
            if any(r["acct"] == bundle["acct"] and r["state"] == "approved" for r in records.values()):
                state = "pending"
        records[bundle["kid"]] = {"kid": bundle["kid"], "idk": bundle["idk"], "acct": bundle["acct"],
                                  "node": bundle["node"], "spk": bundle["spk"], "state": state, "firstSeen": now,
                                  "verified": False}
        return state

    def approve(self, scope, kid):
        self.records(scope)[kid]["state"] = "approved"

    def block(self, scope, kid):
        self.records(scope)[kid]["state"] = "blocked"

    def next_counter(self, room):
        self.counters[room] = self.counters.get(room, 0) + 1
        return self.counters[room]

    def seal(self, room, scope, plain, now, only=None):
        ctr = self.next_counter(room)
        ck = os.urandom(32)
        nonce, body, tag = aes_seal(ck, json.dumps(plain, separators=(",", ":")).encode(), content_aad(room, self.kid, ctr, now))
        own = public_prekey(self.newest_prekey())
        targets = {self.kid: own}
        for record in self.records(scope).values():
            if record["state"] != "approved" or record["kid"] == self.kid:
                continue
            if only is not None and record["kid"] not in only:
                continue
            if record["spk"]["exp"] > now:
                targets[record["kid"]] = record["spk"]
        if len(targets) > MAX_RECIPIENTS:
            raise ValueError("too many recipients")
        frame = {"type": "sealed", "v": 3, "kid": self.kid, "idk": b64u(self.idk), "ctr": ctr, "ts": now,
                 "nonce": b64(nonce), "tag": b64(tag), "body": b64(body), "keys": []}
        for recipient in sorted(targets):
            spk = targets[recipient]
            eph = X25519PrivateKey.generate()
            eph_priv = eph.private_bytes(serialization.Encoding.Raw, serialization.PrivateFormat.Raw,
                                         serialization.NoEncryption())
            eph_pub = x25519_pub(eph_priv)
            ss1 = x25519(eph_priv, unb64u(spk["x"]))
            ct, ss2 = mlkem_encaps(unb64u(spk["pq"]))
            key = kek(room, self.kid, recipient, spk["id"], eph_pub, ct, ss1, ss2)
            knonce, wrapped, ktag = aes_seal(key, ck, wrap_aad(room, self.kid, recipient, frame["nonce"], frame["tag"]))
            frame["keys"].append({"kid": recipient, "spk": spk["id"], "eph": b64u(eph_pub), "ct": b64(ct),
                                  "nonce": b64(knonce), "tag": b64(ktag), "key": b64(wrapped)})
        frame["sig"] = b64u(ed_sign(self.ik_seed, sealed_message(room, frame)))
        return frame

    def open(self, room, scope, frame, now):
        try:
            if frame.get("type") != "sealed" or frame.get("v") != 3:
                return ("invalid",)
            kid, idk = frame["kid"], frame["idk"]
            idk_bytes = unb64u(idk)
            keys = frame["keys"]
            kids = [k["kid"] for k in keys]
            if len(idk_bytes) != 32 or kid != id16(idk_bytes) or not 1 <= len(keys) <= MAX_RECIPIENTS:
                return ("invalid",)
            if kids != sorted(kids) or len(set(kids)) != len(kids):
                return ("invalid",)
            ctr, ts = frame["ctr"], frame["ts"]
            if not isinstance(ctr, int) or ctr < 1 or not isinstance(ts, int):
                return ("invalid",)
            record = self.records(scope).get(kid)
            if kid == self.kid:
                record = {"idk": b64u(self.idk), "state": "approved", "acct": None, "node": None}
            if record is None or record["state"] != "approved":
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
            spk = next((p for p in self.prekeys if p["id"] == entry["spk"]), None)
            if spk is None:
                return ("notForMe",)
            ct = unb64(entry["ct"])
            eph_pub = unb64u(entry["eph"])
            if len(ct) != MLKEM_CT_BYTES or len(eph_pub) != 32:
                return ("invalid",)
            ss1 = x25519(spk["x_priv"], eph_pub)
            ss2 = mlkem_decaps(spk["pq_seed"], ct)
            key = kek(room, kid, self.kid, spk["id"], eph_pub, ct, ss1, ss2)
            ck = aes_open(key, unb64(entry["nonce"]), unb64(entry["key"]), unb64(entry["tag"]),
                          wrap_aad(room, kid, self.kid, frame["nonce"], frame["tag"]))
            clear = aes_open(ck, unb64(frame["nonce"]), unb64(frame["body"]), unb64(frame["tag"]),
                             content_aad(room, kid, ctr, ts))
            plain = json.loads(clear)
            if not isinstance(plain, dict):
                return ("invalid",)
            if record["node"] is not None and "senderId" in plain and plain["senderId"] != record["node"]:
                return ("invalid",)
            seen["set"].add(ctr)
            seen["max"] = max(seen["max"], ctr)
            seen["set"] = {c for c in seen["set"] if c > seen["max"] - REPLAY_WINDOW}
            return ("opened", plain, kid, record["acct"])
        except (KeyError, TypeError, ValueError, InvalidTag, json.JSONDecodeError, subprocess.CalledProcessError):
            return ("invalid",)


def link_transcript(i_hello, r_hello):
    text = "\n".join(["forkmesh-mesh-link-v3", i_hello["idk"], i_hello["eph"], i_hello["nonce"],
                      r_hello["idk"], r_hello["eph"], r_hello["nonce"]])
    return sha256(text.encode())


def link_auth_message(role, transcript):
    return ("forkmesh-mesh-link-auth-v3\n%s\n%s" % (role, b64u(transcript))).encode()


def link_keys(own_eph_priv, peer_eph_pub, transcript):
    ss = x25519(own_eph_priv, peer_eph_pub)
    return (hkdf(ss, transcript, b"forkmesh-mesh-link-v3 i2r"), hkdf(ss, transcript, b"forkmesh-mesh-link-v3 r2i"))


def link_nonce(counter):
    return bytes(4) + counter.to_bytes(8, "big")


def link_seal(key, counter, frame):
    _, cipher, tag = aes_seal(key, json.dumps(frame, separators=(",", ":")).encode(), b"forkmesh-mesh-link-v3",
                              link_nonce(counter))
    return cipher + tag


def link_open(key, counter, data):
    return json.loads(aes_open(key, link_nonce(counter), data[:-16], data[-16:], b"forkmesh-mesh-link-v3"))


def _seed(label, length=32):
    return hashlib.sha512(("forkmesh-chat-v3-vector:" + label).encode()).digest()[:length]


def _mlkem_seed(label):
    return hashlib.sha512(("forkmesh-chat-v3-vector-mlkem:" + label).encode()).digest()


def _secrets(device):
    return {"identitySeed": b64(device.ik_seed), "idk": b64u(device.idk), "kid": device.kid,
            "prekeys": [{"id": p["id"], "xPriv": b64(p["x_priv"]), "x": b64u(p["x"]), "pqSeed": b64(p["pq_seed"]),
                         "pq": b64u(p["pq"]), "ts": p["ts"], "exp": p["exp"]} for p in device.prekeys]}


def build_vectors():
    now = 1_790_000_000_000
    room = "chat-direct:0123456789abcdef0123456789abcdef:v1"
    scope = "direct:0123456789abcdef0123456789abcdef"
    alice = Device(_seed("alice"))
    alice.add_prekey(_seed("alice-spk-x"), _mlkem_seed("alice-spk"), now - DAY_MS // 2)
    alice2 = Device(_seed("alice-phone-2"))
    alice2.add_prekey(_seed("alice2-spk-x"), _mlkem_seed("alice2-spk"), now - DAY_MS // 4)
    bob = Device(_seed("bob"))
    bob.add_prekey(_seed("bob-spk-old-x"), _mlkem_seed("bob-spk-old"), now - 3 * DAY_MS)
    bob.add_prekey(_seed("bob-spk-x"), _mlkem_seed("bob-spk"), now - DAY_MS // 3)

    participants = ["alice", "bob"]
    nodes = {"alice": "node-alice-phone", "alice2": "node-alice-tablet", "bob": "node-bob-phone"}
    alice_bundle = alice.bundle(room, "alice", nodes["alice"])
    alice2_bundle = alice2.bundle(room, "alice", nodes["alice2"])
    bob_bundle = bob.bundle(room, "bob", nodes["bob"])
    injected = Device(_seed("server-injected"))
    injected.add_prekey(_seed("injected-spk-x"), _mlkem_seed("injected-spk"), now - DAY_MS // 5)
    injected_outsider = injected.bundle(room, "mallory", "node-mallory")
    injected_as_bob = injected.bundle(room, "bob", "node-bob-new")
    for device in (alice, bob):
        for bundle in (alice_bundle, bob_bundle):
            device.accept_bundle(scope, "strict", room, bundle, now, participants)
    pending_state = bob.accept_bundle(scope, "strict", room, alice2_bundle, now, participants)

    old_spk = public_prekey(bob.prekeys[0])
    bob_old_bundle = dict(bob_bundle)
    bob_old_bundle["spk"] = old_spk
    bob_old_bundle["sig"] = b64u(ed_sign(bob.ik_seed, bundle_message(room, bob.kid, b64u(bob.idk), "bob", nodes["bob"], old_spk)))
    alice.records(scope)[bob.kid]["spk"] = old_spk

    plain = {"type": "chat", "id": "m1", "senderId": nodes["alice"], "channel": scope, "text": "Hello Bob ❤️"}
    sealed = alice.seal(room, scope, plain, now)
    alice.records(scope)[bob.kid]["spk"] = bob_bundle["spk"]
    sealed_new = alice.seal(room, scope, dict(plain, id="m2", text="second"), now + 1000)

    tampered = json.loads(json.dumps(sealed))
    body = bytearray(unb64(tampered["body"]))
    body[0] ^= 1
    tampered["body"] = b64(bytes(body))

    forged = alice.seal(room, scope, dict(plain, id="m3", senderId=nodes["bob"], text="I am Bob"), now + 2000)
    alice2.accept_bundle(scope, "strict", room, bob_bundle, now, participants)
    from_pending = alice2.seal(room, scope, dict(plain, id="m4", senderId=nodes["alice2"], text="from the second phone"),
                               now + 3000)
    from_pending_keys = [k["kid"] for k in from_pending["keys"]]

    bob_check = Device(bob.ik_seed)
    bob_check.prekeys = [dict(p) for p in bob.prekeys]
    for bundle in (alice_bundle, bob_bundle):
        bob_check.accept_bundle(scope, "strict", room, bundle, now, participants)
    bob_check.accept_bundle(scope, "strict", room, alice2_bundle, now, participants)
    if bob_check.accept_bundle(scope, "strict", room, injected_outsider, now, participants) is not None:
        raise AssertionError("an outsider bundle was accepted")
    if bob_check.accept_bundle(scope, "strict", room, injected_as_bob, now, participants) != "pending":
        raise AssertionError("a new device for bob was not held")
    results = {
        "first": bob_check.open(room, scope, sealed, now),
        "replay": bob_check.open(room, scope, sealed, now),
        "second": bob_check.open(room, scope, sealed_new, now + 1000),
        "tampered": bob_check.open(room, scope, tampered, now),
        "forged": bob_check.open(room, scope, forged, now + 2000),
        "pending": bob_check.open(room, scope, from_pending, now + 3000),
    }
    bob_check.approve(scope, alice2.kid)
    results["approved"] = bob_check.open(room, scope, from_pending, now + 3000)
    for name, expected in {"first": "opened", "replay": "replay", "second": "opened", "tampered": "invalid",
                           "forged": "invalid", "pending": "held", "approved": "opened"}.items():
        if results[name][0] != expected:
            raise AssertionError("%s gave %s" % (name, results[name]))

    eph_priv = _seed("kek-eph")
    spk = bob.newest_prekey()
    ct, ss2 = mlkem_encaps(spk["pq"])
    ss1 = x25519(eph_priv, spk["x"])
    kek_vector = {"room": room, "sender": alice.kid, "recipient": bob.kid, "spkId": spk["id"],
                  "ephPriv": b64(eph_priv), "ephPub": b64u(x25519_pub(eph_priv)), "ct": b64(ct),
                  "ss1": b64(ss1), "ss2": b64(ss2), "ss2FromDecaps": b64(mlkem_decaps(spk["pq_seed"], ct)),
                  "kek": b64(kek(room, alice.kid, bob.kid, spk["id"], x25519_pub(eph_priv), ct, ss1, ss2))}
    if kek_vector["ss2"] != kek_vector["ss2FromDecaps"]:
        raise AssertionError("ML-KEM decapsulation mismatch")

    digits, qr = safety_number(scope, [alice.idk, bob.idk])
    digits3, qr3 = safety_number(scope, [alice.idk, alice2.idk, bob.idk])

    i_eph, r_eph = _seed("link-i-eph"), _seed("link-r-eph")
    i_hello = {"t": "fm-link-hello", "v": 3, "idk": b64u(alice.idk), "eph": b64u(x25519_pub(i_eph)),
               "nonce": b64u(_seed("link-i-nonce", 16))}
    r_hello = {"t": "fm-link-hello", "v": 3, "idk": b64u(bob.idk), "eph": b64u(x25519_pub(r_eph)),
               "nonce": b64u(_seed("link-r-nonce", 16))}
    transcript = link_transcript(i_hello, r_hello)
    k_i2r, k_r2i = link_keys(i_eph, x25519_pub(r_eph), transcript)
    if (k_i2r, k_r2i) != link_keys(r_eph, x25519_pub(i_eph), transcript):
        raise AssertionError("link keys disagree")
    link_frame = {"type": "hello", "senderId": nodes["alice"]}

    return {
        "version": 3,
        "now": now,
        "room": room,
        "scope": scope,
        "participants": participants,
        "nodes": nodes,
        "devices": {"alice": _secrets(alice), "alice2": _secrets(alice2), "bob": _secrets(bob)},
        "bundles": {
            "alice": alice_bundle, "alice2": alice2_bundle, "bob": bob_bundle, "bobOld": bob_old_bundle,
            "invalid": {
                "wrongAcct": dict(alice_bundle, acct="mallory"),
                "wrongKid": dict(alice_bundle, kid=bob.kid),
                "wrongRoom": alice.bundle("another-room", "alice", nodes["alice"]),
                "wrongNode": dict(alice_bundle, node="node-mallory"),
            },
            "injectedOutsider": injected_outsider,
            "injectedAsBob": injected_as_bob,
        },
        "bundleChecks": {"expiredAt": alice_bundle["spk"]["exp"] + 1, "pendingStateForAlice2AtBob": pending_state},
        "kek": kek_vector,
        "sealed": {
            "first": {"frame": sealed, "expect": "opened", "plain": plain, "senderKid": alice.kid,
                      "note": "sealed to Bob's older prekey, which Bob still keeps"},
            "replay": {"frame": sealed, "expect": "replay", "after": "first"},
            "second": {"frame": sealed_new, "expect": "opened", "plain": dict(plain, id="m2", text="second")},
            "tampered": {"frame": tampered, "expect": "invalid"},
            "forgedSenderId": {"frame": forged, "expect": "invalid"},
            "fromPendingDevice": {"frame": from_pending, "expect": "held", "keys": from_pending_keys,
                                  "afterApproval": "opened"},
        },
        "afterBobWipesOldPrekey": {"frame": "first", "expect": "notForMe", "now": bob.prekeys[0]["exp"] + 1},
        "safety": {
            "twoDevices": {"scope": scope, "idks": [b64u(alice.idk), b64u(bob.idk)], "digits": digits, "qr": qr},
            "threeDevices": {"scope": scope, "idks": [b64u(alice.idk), b64u(alice2.idk), b64u(bob.idk)],
                             "digits": digits3, "qr": qr3},
        },
        "link": {
            "initiatorIdentitySeed": b64(alice.ik_seed), "responderIdentitySeed": b64(bob.ik_seed),
            "initiatorEphPriv": b64(i_eph), "responderEphPriv": b64(r_eph),
            "initiatorHello": i_hello, "responderHello": r_hello, "transcript": b64u(transcript),
            "initiatorAuthSig": b64u(ed_sign(alice.ik_seed, link_auth_message("i", transcript))),
            "responderAuthSig": b64u(ed_sign(bob.ik_seed, link_auth_message("r", transcript))),
            "keyI2R": b64(k_i2r), "keyR2I": b64(k_r2i),
            "frame0": {"plain": link_frame, "counter": 0, "data": b64(link_seal(k_i2r, 0, link_frame))},
            "frame1": {"plain": link_frame, "counter": 1, "data": b64(link_seal(k_i2r, 1, link_frame))},
        },
    }


def main(argv):
    if argv[1:] == ["vectors"]:
        VECTORS_PATH.parent.mkdir(parents=True, exist_ok=True)
        VECTORS_PATH.write_text(json.dumps(build_vectors(), indent=1, sort_keys=True) + "\n")
        print(VECTORS_PATH)
        return 0
    print("usage: chat_v3_reference.py vectors", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
