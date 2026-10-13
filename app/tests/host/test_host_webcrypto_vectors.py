"""The host's crypto.subtle matches Node's WebCrypto byte for byte.

Vectors come from app/tests/host/webcrypto_vectors.mjs run under Node at
test time (skipped when node is not installed). Deterministic algorithms are
compared exactly; ECDSA (randomized) is cross-verified in both directions.
"""

from __future__ import annotations

import asyncio
import json
import shutil
import subprocess
from pathlib import Path

import pytest

from forkmesh_host import js
from forkmesh_host.ffi import to_js
from forkmesh_host.js import Object, Uint8Array, crypto

SCRIPT = Path(__file__).with_name("webcrypto_vectors.mjs")
NODE = shutil.which("node")
pytestmark = pytest.mark.skipif(NODE is None, reason="node is not installed")
subtle = crypto.subtle


def _obj(value):
    return to_js(value, dict_converter=Object.fromEntries)


def _u8(hex_text):
    return Uint8Array.new(bytes.fromhex(hex_text))


def _hex(buffer):
    return bytes(Uint8Array.new(buffer).to_py()).hex()


@pytest.fixture(scope="module")
def vectors():
    out = subprocess.run([NODE, str(SCRIPT)], check=True,
                         capture_output=True, text=True, timeout=60)
    return json.loads(out.stdout)


def node_verify(payload):
    out = subprocess.run([NODE, str(SCRIPT), "verify"],
                         input=json.dumps(payload), check=True,
                         capture_output=True, text=True, timeout=60)
    return json.loads(out.stdout)["ok"]


def run(coro):
    return asyncio.run(coro)


def _pattern(n, seed):
    return bytes((i * 7 + seed) & 0xFF for i in range(n))


def test_digests(vectors):
    message = _u8(vectors["message"])
    big = Uint8Array.new(_pattern(200000, 3))
    for name, expected in vectors["digests"].items():
        assert _hex(run(subtle.digest(name, message))) == expected["small"]
        assert _hex(run(subtle.digest(_obj({"name": name}), big))) == \
            expected["big"]


def test_hmac(vectors):
    message = _u8(vectors["message"])
    for hash_name, expected in vectors["hmac"]["sigs"].items():
        key = run(subtle.importKey(
            "raw", _u8(vectors["hmac"]["key"]),
            _obj({"name": "HMAC", "hash": hash_name}), False,
            to_js(["sign", "verify"])))
        sig = run(subtle.sign("HMAC", key, message))
        assert _hex(sig) == expected
        assert run(subtle.verify("HMAC", key, sig, message)) is True


def test_ed25519(vectors):
    v = vectors["ed25519"]
    message = _u8(vectors["message"])
    # The app's shape: importKey("jwk", {kty, crv, d, x}) then sign.
    jwk = _obj({"kty": "OKP", "crv": "Ed25519", "d": v["jwk"]["d"],
                "x": v["jwk"]["x"]})
    private = run(subtle.importKey("jwk", jwk, _obj({"name": "Ed25519"}),
                                   False, to_js(["sign"])))
    assert _hex(run(subtle.sign(_obj({"name": "Ed25519"}), private,
                                message))) == v["sig"]
    from_pkcs8 = run(subtle.importKey("pkcs8", _u8(v["pkcs8"]),
                                      _obj({"name": "Ed25519"}), True,
                                      to_js(["sign"])))
    exported = run(subtle.exportKey("jwk", from_pkcs8))
    assert (exported.d, exported.x) == (v["jwk"]["d"], v["jwk"]["x"])
    public = run(subtle.importKey("raw", _u8(v["raw"]),
                                  _obj({"name": "Ed25519"}), False,
                                  to_js(["verify"])))
    assert run(subtle.verify(_obj({"name": "Ed25519"}), public,
                             _u8(v["sig"]), message)) is True
    assert run(subtle.verify(_obj({"name": "Ed25519"}), public,
                             _u8(v["sig"]), Uint8Array.new(b"x"))) is False
    spki_key = run(subtle.importKey("spki", _u8(v["spki"]),
                                    _obj({"name": "Ed25519"}), True,
                                    to_js(["verify"])))
    assert _hex(run(subtle.exportKey("raw", spki_key))) == v["raw"]
    # generateKey + exportKey("raw"/"jwk") as entry.py uses it.
    pair = run(subtle.generateKey(_obj({"name": "Ed25519"}), True,
                                  to_js(["sign", "verify"])))
    raw = bytes(Uint8Array.new(run(subtle.exportKey(
        "raw", pair.publicKey))).to_py())
    seed = str(getattr(run(subtle.exportKey("jwk", pair.privateKey)), "d"))
    assert len(raw) == 32 and len(seed) == 43
    sig = run(subtle.sign("Ed25519", pair.privateKey, message))
    assert node_verify({"kind": "ed25519", "raw": raw.hex(),
                        "sig": _hex(sig), "message": vectors["message"]})


def test_aes_gcm(vectors):
    v = vectors["aesgcm"]
    key = run(subtle.importKey("raw", _u8(v["key"]),
                               _obj({"name": "AES-GCM"}), False,
                               to_js(["encrypt", "decrypt"])))
    iv = _u8(v["iv"])
    plain = _u8(v["plain"])
    assert _hex(run(subtle.encrypt(
        _obj({"name": "AES-GCM", "iv": iv, "tagLength": 128}), key,
        plain))) == v["cipher"]
    assert _hex(run(subtle.encrypt(
        _obj({"name": "AES-GCM", "iv": iv, "additionalData": _u8(v["aad"])}),
        key, plain))) == v["cipherAad"]
    assert _hex(run(subtle.encrypt(
        _obj({"name": "AES-GCM", "iv": iv, "tagLength": 96}), key,
        plain))) == v["cipher96"]
    decrypted = run(subtle.decrypt(_obj({"name": "AES-GCM", "iv": iv}), key,
                                   _u8(v["cipher"])))
    assert _hex(decrypted) == v["plain"]
    assert _hex(run(subtle.decrypt(
        _obj({"name": "AES-GCM", "iv": iv, "tagLength": 96}), key,
        _u8(v["cipher96"])))) == v["plain"]
    tampered = bytearray(bytes.fromhex(v["cipher"]))
    tampered[0] ^= 1
    with pytest.raises(js.JsException):
        run(subtle.decrypt(_obj({"name": "AES-GCM", "iv": iv}), key,
                           Uint8Array.new(bytes(tampered))))
    big = run(subtle.encrypt(_obj({"name": "AES-GCM", "iv": iv}), key,
                             Uint8Array.new(_pattern(200000, 3))))
    assert _hex(run(subtle.digest("SHA-256", big))) == v["bigCipherSha256"]


def test_pbkdf2(vectors):
    v = vectors["pbkdf2"]
    base = run(subtle.importKey("raw", _u8(v["password"]),
                                _obj({"name": "PBKDF2"}), False,
                                to_js(["deriveBits", "deriveKey"])))
    salt = _u8(v["salt"])
    bits = run(subtle.deriveBits(_obj({
        "name": "PBKDF2", "salt": salt, "iterations": v["iterations"],
        "hash": "SHA-256"}), base, 256))
    assert _hex(bits) == v["bits"]
    bits1 = run(subtle.deriveBits(_obj({
        "name": "PBKDF2", "salt": salt, "iterations": v["iterations"],
        "hash": "SHA-1"}), base, 160))
    assert _hex(bits1) == v["bitsSha1"]
    derived = run(subtle.deriveKey(
        _obj({"name": "PBKDF2", "salt": salt, "iterations": v["iterations"],
              "hash": "SHA-256"}), base,
        _obj({"name": "AES-GCM", "length": 256}), True, to_js(["encrypt"])))
    assert _hex(run(subtle.exportKey("raw", derived))) == v["derivedRaw"]
    iv = _u8(vectors["aesgcm"]["iv"])
    assert _hex(run(subtle.encrypt(_obj({"name": "AES-GCM", "iv": iv}),
                                   derived, _u8(vectors["message"])))) == \
        v["derivedCipher"]


def test_ecdsa_p256(vectors):
    v = vectors["ecdsa"]
    message = _u8(vectors["message"])
    algorithm = _obj({"name": "ECDSA", "namedCurve": "P-256"})
    public = run(subtle.importKey("spki", _u8(v["spki"]), algorithm, True,
                                  to_js(["verify"])))
    sign_alg = _obj({"name": "ECDSA", "hash": {"name": "SHA-256"}})
    assert run(subtle.verify(sign_alg, public, _u8(v["sig"]), message))
    raw_public = run(subtle.importKey("raw", _u8(v["raw"]), algorithm, True,
                                      to_js(["verify"])))
    assert _hex(run(subtle.exportKey("spki", raw_public))) == v["spki"]
    # apns.py imports the provider key as pkcs8 and signs with it.
    private = run(subtle.importKey("pkcs8", _u8(v["pkcs8"]), algorithm,
                                   False, to_js(["sign"])))
    sig = run(subtle.sign(sign_alg, private, message))
    assert len(bytes(sig)) == 64
    assert node_verify({"kind": "ecdsa", "spki": v["spki"],
                        "sig": _hex(sig), "message": vectors["message"]})
    jwk_private = run(subtle.importKey("jwk", _obj(v["jwk"]), algorithm,
                                       True, to_js(["sign"])))
    exported = run(subtle.exportKey("jwk", jwk_private))
    assert (exported.x, exported.y, exported.d) == (
        v["jwk"]["x"], v["jwk"]["y"], v["jwk"]["d"])


def test_rsassa_pkcs1_v1_5(vectors):
    v = vectors["rsa"]
    message = _u8(vectors["message"])
    algorithm = _obj({"name": "RSASSA-PKCS1-v1_5", "hash": "SHA-256"})
    private = run(subtle.importKey("pkcs8", _u8(v["pkcs8"]), algorithm,
                                   False, to_js(["sign"])))
    sig = run(subtle.sign(_obj({"name": "RSASSA-PKCS1-v1_5"}), private,
                          message))
    assert _hex(sig) == v["sig"]
    public = run(subtle.importKey("spki", _u8(v["spki"]), algorithm, False,
                                  to_js(["verify"])))
    assert run(subtle.verify(_obj({"name": "RSASSA-PKCS1-v1_5"}), public,
                             sig, message)) is True
    # fediverse_routes.py: generateKey with publicExponent set afterwards.
    params = _obj({"name": "RSASSA-PKCS1-v1_5", "modulusLength": 2048,
                   "hash": "SHA-256"})
    params.publicExponent = Uint8Array.new(bytes([1, 0, 1]))
    pair = run(subtle.generateKey(params, True, to_js(["sign", "verify"])))
    spki = run(subtle.exportKey("spki", pair.publicKey))
    run(subtle.exportKey("pkcs8", pair.privateKey))
    new_sig = run(subtle.sign("RSASSA-PKCS1-v1_5", pair.privateKey, message))
    assert node_verify({"kind": "rsa", "spki": _hex(spki),
                        "sig": _hex(new_sig), "message": vectors["message"]})


def test_get_random_values_fills_in_place():
    array = Uint8Array.new(16)
    same = crypto.getRandomValues(array)
    assert same is array
    assert len(bytes(array.to_py())) == 16
    assert bytes(array.to_py()) != bytes(16)
