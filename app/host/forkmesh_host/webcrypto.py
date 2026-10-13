"""WebCrypto (``crypto.subtle`` / ``crypto.getRandomValues``) on ``cryptography``.

Covers the algorithms the Worker app uses, with WebCrypto argument shapes:

* digest: SHA-1, SHA-256, SHA-384, SHA-512
* HMAC (raw/jwk import, generateKey, sign/verify)
* Ed25519 (raw/spki/pkcs8/jwk, generateKey, sign/verify)
* ECDSA P-256/P-384 (spki/pkcs8/jwk/raw public, sign/verify; signatures are
  IEEE P1363 ``r||s`` like WebCrypto, not DER)
* RSASSA-PKCS1-v1_5 (spki/pkcs8/jwk, generateKey, sign/verify)
* AES-GCM (raw/jwk, encrypt/decrypt with iv/additionalData/tagLength)
* PBKDF2 / HKDF deriveBits and deriveKey

Results that WebCrypto returns as ``ArrayBuffer`` come back as the host's
``js.ArrayBuffer``. PBKDF2, RSA and bulk AES run in a worker thread
(``asyncio.to_thread``) so they never stall the event loop.
"""

from __future__ import annotations

import asyncio
import base64
import hashlib
import hmac
import os

from cryptography.exceptions import InvalidSignature, InvalidTag
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, ed25519, padding, rsa
from cryptography.hazmat.primitives.asymmetric.utils import (
    decode_dss_signature, encode_dss_signature)
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from cryptography.hazmat.primitives.kdf.pbkdf2 import PBKDF2HMAC

from .ffi import JsArray, JsException, JsNull, JsObject, JsProxy

# Work below this many bytes stays on the event loop; the thread hop costs
# more than the operation.
THREAD_THRESHOLD_BYTES = 64 * 1024

_CANONICAL = {name.lower(): name for name in (
    "SHA-1", "SHA-256", "SHA-384", "SHA-512", "HMAC", "Ed25519", "ECDSA",
    "ECDH", "RSASSA-PKCS1-v1_5", "RSA-PSS", "RSA-OAEP", "AES-GCM",
    "AES-CBC", "AES-CTR", "AES-KW", "PBKDF2", "HKDF", "X25519",
)}
_HASHES = {
    "SHA-1": (hashes.SHA1, hashlib.sha1, 64),
    "SHA-256": (hashes.SHA256, hashlib.sha256, 64),
    "SHA-384": (hashes.SHA384, hashlib.sha384, 128),
    "SHA-512": (hashes.SHA512, hashlib.sha512, 128),
}
_CURVES = {"P-256": (ec.SECP256R1, 32, "SHA-256"),
           "P-384": (ec.SECP384R1, 48, "SHA-384"),
           "P-521": (ec.SECP521R1, 66, "SHA-512")}


def _err(message, name="OperationError"):
    return JsException(message, name)


def _get(obj, name, default=None):
    if obj is None or isinstance(obj, JsNull):
        return default
    if isinstance(obj, dict):
        value = obj.get(name, default)
    else:
        value = getattr(obj, name, default)
    return default if value is None else value


def _normalize(algorithm):
    """Return (canonical name, params) for a string or dict algorithm."""
    if isinstance(algorithm, str):
        name, params = algorithm, {}
    else:
        name = _get(algorithm, "name")
        params = algorithm
        if name is None:
            raise _err("Algorithm: name is missing", "TypeError")
    canonical = _CANONICAL.get(str(name).lower())
    if canonical is None:
        raise _err("Unrecognized algorithm name: %s" % name,
                   "NotSupportedError")
    return canonical, params


def _hash_name(value):
    if value is None:
        raise _err("hash is required", "TypeError")
    name, _ = _normalize(value)
    if name not in _HASHES:
        raise _err("Unsupported hash %s" % name, "NotSupportedError")
    return name


def _bytes(value):
    from .js import _as_bytes
    return _as_bytes(value)


def _buffer(data):
    from .js import ArrayBuffer
    return ArrayBuffer(bytearray(data))


def _usages(value):
    if value is None:
        return JsArray()
    return JsArray(str(v) for v in value)


def _b64u(data):
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode("ascii")


def _b64u_decode(text):
    text = str(text)
    return base64.urlsafe_b64decode(text + "=" * (-len(text) % 4))


def _int_b64u(value, length=None):
    length = length or max(1, (value.bit_length() + 7) // 8)
    return _b64u(value.to_bytes(length, "big"))


def _b64u_int(text):
    return int.from_bytes(_b64u_decode(text), "big")


class CryptoKey(JsProxy):
    def __init__(self, type_, algorithm, extractable, usages, material):
        self.type = type_
        self.algorithm = algorithm
        self.extractable = bool(extractable)
        self.usages = _usages(usages)
        self._material = material

    def __repr__(self):
        return "CryptoKey(%s, %s)" % (self.type, self.algorithm.get("name"))

    def to_py(self):
        return self


async def _maybe_thread(size, fn, *args):
    if size >= THREAD_THRESHOLD_BYTES:
        return await asyncio.to_thread(fn, *args)
    return fn(*args)


def _check_usage(key, usage):
    if not isinstance(key, CryptoKey):
        raise _err("parameter is not a CryptoKey", "TypeError")
    if usage not in key.usages:
        raise _err("key does not support the '%s' operation" % usage,
                   "InvalidAccessError")


# --------------------------------------------------------------------------
# import

def _import_key(fmt, data, algorithm, extractable, usages):
    name, params = _normalize(algorithm)
    fmt = str(fmt)
    if name == "HMAC":
        hash_name = _hash_name(_get(params, "hash"))
        if fmt == "raw":
            secret = _bytes(data)
        elif fmt == "jwk":
            secret = _b64u_decode(_get(data, "k"))
        else:
            raise _err("HMAC keys import as raw or jwk", "NotSupportedError")
        return CryptoKey("secret", JsObject(
            name="HMAC", hash=JsObject(name=hash_name),
            length=len(secret) * 8), extractable, usages, secret)
    if name in ("AES-GCM", "AES-CBC", "AES-CTR", "AES-KW"):
        if fmt == "raw":
            secret = _bytes(data)
        elif fmt == "jwk":
            secret = _b64u_decode(_get(data, "k"))
        else:
            raise _err("AES keys import as raw or jwk", "NotSupportedError")
        if len(secret) not in (16, 24, 32):
            raise _err("AES key data must be 128, 192 or 256 bits",
                       "DataError")
        return CryptoKey("secret", JsObject(name=name,
                                            length=len(secret) * 8),
                         extractable, usages, secret)
    if name in ("PBKDF2", "HKDF"):
        if fmt != "raw":
            raise _err("%s keys import as raw" % name, "NotSupportedError")
        return CryptoKey("secret", JsObject(name=name), False, usages,
                         _bytes(data))
    if name == "Ed25519":
        algo = JsObject(name="Ed25519")
        if fmt == "raw":
            return CryptoKey("public", algo, True, usages,
                             ed25519.Ed25519PublicKey.from_public_bytes(
                                 _bytes(data)))
        if fmt == "spki":
            key = serialization.load_der_public_key(_bytes(data))
            if not isinstance(key, ed25519.Ed25519PublicKey):
                raise _err("not an Ed25519 public key", "DataError")
            return CryptoKey("public", algo, True, usages, key)
        if fmt == "pkcs8":
            key = serialization.load_der_private_key(_bytes(data), None)
            if not isinstance(key, ed25519.Ed25519PrivateKey):
                raise _err("not an Ed25519 private key", "DataError")
            return CryptoKey("private", algo, extractable, usages, key)
        if fmt == "jwk":
            if _get(data, "kty") not in (None, "OKP") or \
                    _get(data, "crv", "Ed25519") != "Ed25519":
                raise _err("not an Ed25519 JWK", "DataError")
            d = _get(data, "d")
            if d:
                key = ed25519.Ed25519PrivateKey.from_private_bytes(
                    _b64u_decode(d))
                x = _get(data, "x")
                if x and key.public_key().public_bytes_raw() != \
                        _b64u_decode(x):
                    raise _err("JWK x does not match d", "DataError")
                return CryptoKey("private", algo, extractable, usages, key)
            return CryptoKey("public", algo, True, usages,
                             ed25519.Ed25519PublicKey.from_public_bytes(
                                 _b64u_decode(_get(data, "x"))))
        raise _err("unsupported Ed25519 key format %s" % fmt,
                   "NotSupportedError")
    if name in ("ECDSA", "ECDH"):
        curve_name = str(_get(params, "namedCurve", ""))
        if curve_name not in _CURVES:
            raise _err("unsupported curve %s" % curve_name,
                       "NotSupportedError")
        curve_cls, size, _ = _CURVES[curve_name]
        algo = JsObject(name=name, namedCurve=curve_name)
        if fmt == "raw":
            key = ec.EllipticCurvePublicKey.from_encoded_point(
                curve_cls(), _bytes(data))
            return CryptoKey("public", algo, True, usages, key)
        if fmt == "spki":
            key = serialization.load_der_public_key(_bytes(data))
            return CryptoKey("public", algo, True, usages, key)
        if fmt == "pkcs8":
            key = serialization.load_der_private_key(_bytes(data), None)
            return CryptoKey("private", algo, extractable, usages, key)
        if fmt == "jwk":
            x = _b64u_int(_get(data, "x"))
            y = _b64u_int(_get(data, "y"))
            public = ec.EllipticCurvePublicNumbers(x, y, curve_cls())
            d = _get(data, "d")
            if d:
                key = ec.EllipticCurvePrivateNumbers(
                    _b64u_int(d), public).private_key()
                return CryptoKey("private", algo, extractable, usages, key)
            return CryptoKey("public", algo, True, usages,
                             public.public_key())
        raise _err("unsupported EC key format %s" % fmt,
                   "NotSupportedError")
    if name in ("RSASSA-PKCS1-v1_5", "RSA-PSS", "RSA-OAEP"):
        hash_name = _hash_name(_get(params, "hash"))
        if fmt == "spki":
            key = serialization.load_der_public_key(_bytes(data))
            kind = "public"
        elif fmt == "pkcs8":
            key = serialization.load_der_private_key(_bytes(data), None)
            kind = "private"
        elif fmt == "jwk":
            public = rsa.RSAPublicNumbers(_b64u_int(_get(data, "e")),
                                          _b64u_int(_get(data, "n")))
            if _get(data, "d"):
                key = rsa.RSAPrivateNumbers(
                    _b64u_int(_get(data, "p")), _b64u_int(_get(data, "q")),
                    _b64u_int(_get(data, "d")), _b64u_int(_get(data, "dp")),
                    _b64u_int(_get(data, "dq")), _b64u_int(_get(data, "qi")),
                    public).private_key()
                kind = "private"
            else:
                key = public.public_key()
                kind = "public"
        else:
            raise _err("unsupported RSA key format %s" % fmt,
                       "NotSupportedError")
        numbers = (key.public_key() if kind == "private"
                   else key).public_numbers()
        from .js import Uint8Array
        exponent = numbers.e.to_bytes(
            max(1, (numbers.e.bit_length() + 7) // 8), "big")
        algo = JsObject(name=name, hash=JsObject(name=hash_name),
                        modulusLength=numbers.n.bit_length(),
                        publicExponent=Uint8Array.new(exponent))
        return CryptoKey(kind, algo, extractable if kind == "private"
                         else True, usages, key)
    raise _err("importKey does not support %s" % name, "NotSupportedError")


# --------------------------------------------------------------------------
# export

def _export_key(fmt, key):
    if not isinstance(key, CryptoKey):
        raise _err("parameter is not a CryptoKey", "TypeError")
    if not key.extractable:
        raise _err("key is not extractable", "InvalidAccessError")
    name = key.algorithm["name"]
    material = key._material
    fmt = str(fmt)
    if key.type == "secret":
        if fmt == "raw":
            return _buffer(material)
        if fmt == "jwk":
            jwk = JsObject(kty="oct", k=_b64u(material), ext=True,
                           key_ops=JsArray(key.usages))
            if name == "HMAC":
                jwk["alg"] = "HS" + key.algorithm["hash"]["name"][4:]
            elif name.startswith("AES-"):
                jwk["alg"] = "A%d%s" % (len(material) * 8, name[4:])
            return jwk
        raise _err("unsupported export format %s" % fmt, "NotSupportedError")
    if fmt == "spki":
        public = material if key.type == "public" else material.public_key()
        if key.type != "public":
            raise _err("spki export needs a public key", "InvalidAccessError")
        return _buffer(public.public_bytes(
            serialization.Encoding.DER,
            serialization.PublicFormat.SubjectPublicKeyInfo))
    if fmt == "pkcs8":
        if key.type != "private":
            raise _err("pkcs8 export needs a private key",
                       "InvalidAccessError")
        return _buffer(material.private_bytes(
            serialization.Encoding.DER, serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption()))
    if fmt == "raw":
        if key.type != "public":
            raise _err("raw export needs a public key", "InvalidAccessError")
        if name == "Ed25519":
            return _buffer(material.public_bytes_raw())
        if name in ("ECDSA", "ECDH"):
            return _buffer(material.public_bytes(
                serialization.Encoding.X962,
                serialization.PublicFormat.UncompressedPoint))
        raise _err("raw export unsupported for %s" % name,
                   "NotSupportedError")
    if fmt == "jwk":
        ops = JsArray(key.usages)
        if name == "Ed25519":
            if key.type == "private":
                pub = material.public_key().public_bytes_raw()
                seed = material.private_bytes_raw()
                return JsObject(kty="OKP", crv="Ed25519", x=_b64u(pub),
                                d=_b64u(seed), ext=True, key_ops=ops)
            return JsObject(kty="OKP", crv="Ed25519",
                            x=_b64u(material.public_bytes_raw()), ext=True,
                            key_ops=ops)
        if name in ("ECDSA", "ECDH"):
            curve = key.algorithm["namedCurve"]
            size = _CURVES[curve][1]
            public = material if key.type == "public" else \
                material.public_key()
            numbers = public.public_numbers()
            jwk = JsObject(kty="EC", crv=curve,
                           x=_int_b64u(numbers.x, size),
                           y=_int_b64u(numbers.y, size), ext=True,
                           key_ops=ops)
            if key.type == "private":
                jwk["d"] = _int_b64u(
                    material.private_numbers().private_value, size)
            return jwk
        if name.startswith("RSA"):
            public = material if key.type == "public" else \
                material.public_key()
            numbers = public.public_numbers()
            jwk = JsObject(kty="RSA", n=_int_b64u(numbers.n),
                           e=_int_b64u(numbers.e), ext=True, key_ops=ops,
                           alg="RS" + key.algorithm["hash"]["name"][4:])
            if key.type == "private":
                private = material.private_numbers()
                jwk.update(d=_int_b64u(private.d), p=_int_b64u(private.p),
                           q=_int_b64u(private.q),
                           dp=_int_b64u(private.dmp1),
                           dq=_int_b64u(private.dmq1),
                           qi=_int_b64u(private.iqmp))
            return jwk
    raise _err("unsupported export format %s" % fmt, "NotSupportedError")


# --------------------------------------------------------------------------
# generate

def _generate_key(algorithm, extractable, usages):
    name, params = _normalize(algorithm)
    usages = _usages(usages)
    if name == "Ed25519":
        private = ed25519.Ed25519PrivateKey.generate()
        algo = JsObject(name="Ed25519")
        return JsObject(
            publicKey=CryptoKey("public", algo, True,
                                [u for u in usages if u == "verify"],
                                private.public_key()),
            privateKey=CryptoKey("private", JsObject(algo), extractable,
                                 [u for u in usages if u == "sign"],
                                 private))
    if name in ("ECDSA", "ECDH"):
        curve = str(_get(params, "namedCurve", ""))
        if curve not in _CURVES:
            raise _err("unsupported curve %s" % curve, "NotSupportedError")
        private = ec.generate_private_key(_CURVES[curve][0]())
        algo = JsObject(name=name, namedCurve=curve)
        pub_use = ("verify",) if name == "ECDSA" else ()
        priv_use = ("sign",) if name == "ECDSA" else (
            "deriveBits", "deriveKey")
        return JsObject(
            publicKey=CryptoKey("public", algo, True,
                                [u for u in usages if u in pub_use],
                                private.public_key()),
            privateKey=CryptoKey("private", JsObject(algo), extractable,
                                 [u for u in usages if u in priv_use],
                                 private))
    if name in ("RSASSA-PKCS1-v1_5", "RSA-PSS", "RSA-OAEP"):
        bits = int(_get(params, "modulusLength", 2048))
        exponent_raw = _get(params, "publicExponent")
        exponent = int.from_bytes(_bytes(exponent_raw), "big") \
            if exponent_raw is not None else 65537
        hash_name = _hash_name(_get(params, "hash"))
        private = rsa.generate_private_key(exponent, bits)
        from .js import Uint8Array
        algo = JsObject(name=name, hash=JsObject(name=hash_name),
                        modulusLength=bits,
                        publicExponent=Uint8Array.new(
                            exponent.to_bytes(3, "big")))
        return JsObject(
            publicKey=CryptoKey("public", algo, True,
                                [u for u in usages
                                 if u in ("verify", "encrypt", "wrapKey")],
                                private.public_key()),
            privateKey=CryptoKey("private", JsObject(algo), extractable,
                                 [u for u in usages
                                  if u in ("sign", "decrypt", "unwrapKey")],
                                 private))
    if name == "HMAC":
        hash_name = _hash_name(_get(params, "hash"))
        length = int(_get(params, "length", _HASHES[hash_name][2] * 8))
        secret = os.urandom(length // 8)
        return CryptoKey("secret", JsObject(name="HMAC",
                                            hash=JsObject(name=hash_name),
                                            length=length),
                         extractable, usages, secret)
    if name.startswith("AES-"):
        length = int(_get(params, "length", 256))
        return CryptoKey("secret", JsObject(name=name, length=length),
                         extractable, usages, os.urandom(length // 8))
    raise _err("generateKey does not support %s" % name, "NotSupportedError")


# --------------------------------------------------------------------------
# sign / verify

def _ecdsa_hash(params, key):
    hash_value = _get(params, "hash")
    if hash_value is None:
        raise _err("ECDSA needs a hash", "TypeError")
    return _hash_name(hash_value)


def _sign(algorithm, key, data):
    name, params = _normalize(algorithm)
    _check_usage(key, "sign")
    if key.algorithm["name"] != name:
        raise _err("key algorithm does not match", "InvalidAccessError")
    if name == "HMAC":
        hash_name = key.algorithm["hash"]["name"]
        return hmac.new(key._material, data, _HASHES[hash_name][1]).digest()
    if name == "Ed25519":
        return key._material.sign(data)
    if name == "ECDSA":
        hash_name = _ecdsa_hash(params, key)
        der = key._material.sign(data, ec.ECDSA(_HASHES[hash_name][0]()))
        r, s = decode_dss_signature(der)
        size = _CURVES[key.algorithm["namedCurve"]][1]
        return r.to_bytes(size, "big") + s.to_bytes(size, "big")
    if name == "RSASSA-PKCS1-v1_5":
        hash_name = key.algorithm["hash"]["name"]
        return key._material.sign(data, padding.PKCS1v15(),
                                  _HASHES[hash_name][0]())
    if name == "RSA-PSS":
        hash_name = key.algorithm["hash"]["name"]
        return key._material.sign(
            data, padding.PSS(padding.MGF1(_HASHES[hash_name][0]()),
                              int(_get(params, "saltLength", 32))),
            _HASHES[hash_name][0]())
    raise _err("sign does not support %s" % name, "NotSupportedError")


def _verify(algorithm, key, signature, data):
    name, params = _normalize(algorithm)
    _check_usage(key, "verify")
    if key.algorithm["name"] != name:
        raise _err("key algorithm does not match", "InvalidAccessError")
    try:
        if name == "HMAC":
            hash_name = key.algorithm["hash"]["name"]
            expected = hmac.new(key._material, data,
                                _HASHES[hash_name][1]).digest()
            return hmac.compare_digest(expected, signature)
        if name == "Ed25519":
            key._material.verify(signature, data)
            return True
        if name == "ECDSA":
            hash_name = _ecdsa_hash(params, key)
            size = _CURVES[key.algorithm["namedCurve"]][1]
            if len(signature) != 2 * size:
                return False
            der = encode_dss_signature(
                int.from_bytes(signature[:size], "big"),
                int.from_bytes(signature[size:], "big"))
            key._material.verify(der, data,
                                 ec.ECDSA(_HASHES[hash_name][0]()))
            return True
        if name == "RSASSA-PKCS1-v1_5":
            hash_name = key.algorithm["hash"]["name"]
            key._material.verify(signature, data, padding.PKCS1v15(),
                                 _HASHES[hash_name][0]())
            return True
        if name == "RSA-PSS":
            hash_name = key.algorithm["hash"]["name"]
            key._material.verify(
                signature, data,
                padding.PSS(padding.MGF1(_HASHES[hash_name][0]()),
                            int(_get(params, "saltLength", 32))),
                _HASHES[hash_name][0]())
            return True
    except (InvalidSignature, ValueError):
        return False
    raise _err("verify does not support %s" % name, "NotSupportedError")


# --------------------------------------------------------------------------
# encrypt / decrypt

def _aes_gcm(encrypt, params, key, data):
    iv = _bytes(_get(params, "iv"))
    aad = _get(params, "additionalData")
    aad = _bytes(aad) if aad is not None else None
    tag_bits = int(_get(params, "tagLength", 128))
    tag_len = tag_bits // 8
    if tag_len == 16:
        cipher = AESGCM(key._material)
        try:
            if encrypt:
                return cipher.encrypt(iv, data, aad)
            return cipher.decrypt(iv, data, aad)
        except InvalidTag:
            raise _err("The operation failed for an operation-specific "
                       "reason") from None
    if encrypt:
        encryptor = Cipher(algorithms.AES(key._material),
                           modes.GCM(iv)).encryptor()
        if aad:
            encryptor.authenticate_additional_data(aad)
        out = encryptor.update(data) + encryptor.finalize()
        return out + encryptor.tag[:tag_len]
    body, tag = data[:-tag_len], data[-tag_len:]
    decryptor = Cipher(algorithms.AES(key._material),
                       modes.GCM(iv, tag, min_tag_length=tag_len)).decryptor()
    if aad:
        decryptor.authenticate_additional_data(aad)
    try:
        return decryptor.update(body) + decryptor.finalize()
    except InvalidTag:
        raise _err("The operation failed for an operation-specific "
                   "reason") from None


def _crypt(encrypt, algorithm, key, data):
    name, params = _normalize(algorithm)
    _check_usage(key, "encrypt" if encrypt else "decrypt")
    if key.algorithm["name"] != name:
        raise _err("key algorithm does not match", "InvalidAccessError")
    if name == "AES-GCM":
        return _aes_gcm(encrypt, params, key, data)
    if name == "AES-CBC":
        iv = _bytes(_get(params, "iv"))
        from cryptography.hazmat.primitives import padding as sym_padding
        cipher = Cipher(algorithms.AES(key._material), modes.CBC(iv))
        if encrypt:
            padder = sym_padding.PKCS7(128).padder()
            padded = padder.update(data) + padder.finalize()
            op = cipher.encryptor()
            return op.update(padded) + op.finalize()
        op = cipher.decryptor()
        try:
            padded = op.update(data) + op.finalize()
            unpadder = sym_padding.PKCS7(128).unpadder()
            return unpadder.update(padded) + unpadder.finalize()
        except ValueError:
            raise _err("decryption failed") from None
    if name == "RSA-OAEP":
        hash_cls = _HASHES[key.algorithm["hash"]["name"]][0]
        pad = padding.OAEP(padding.MGF1(hash_cls()), hash_cls(), None)
        if encrypt:
            return key._material.encrypt(data, pad)
        try:
            return key._material.decrypt(data, pad)
        except ValueError:
            raise _err("decryption failed") from None
    raise _err("encrypt does not support %s" % name, "NotSupportedError")


# --------------------------------------------------------------------------
# derive

def _derive_bits(algorithm, key, length):
    name, params = _normalize(algorithm)
    if not isinstance(key, CryptoKey) or key.algorithm["name"] != name:
        raise _err("key algorithm does not match", "InvalidAccessError")
    if length is None or int(length) % 8:
        raise _err("length must be a multiple of 8", "OperationError")
    size = int(length) // 8
    hash_name = _hash_name(_get(params, "hash"))
    if name == "PBKDF2":
        iterations = int(_get(params, "iterations", 0))
        if iterations <= 0:
            raise _err("iterations must be > 0", "OperationError")
        salt = _bytes(_get(params, "salt", b""))
        return hashlib.pbkdf2_hmac(hash_name.replace("-", "").lower(),
                                   key._material, salt, iterations, size)
    if name == "HKDF":
        salt = _bytes(_get(params, "salt", b""))
        info = _bytes(_get(params, "info", b""))
        return HKDF(_HASHES[hash_name][0](), size, salt, info).derive(
            key._material)
    raise _err("deriveBits does not support %s" % name, "NotSupportedError")


class SubtleCrypto(JsProxy):
    async def digest(self, algorithm, data):
        name, _ = _normalize(algorithm)
        if name not in _HASHES:
            raise _err("Unsupported digest %s" % name, "NotSupportedError")
        payload = _bytes(data)
        factory = _HASHES[name][1]
        result = await _maybe_thread(
            len(payload), lambda: factory(payload).digest())
        return _buffer(result)

    async def importKey(self, fmt, key_data, algorithm, extractable,
                        usages):
        name, _ = _normalize(algorithm)
        if name.startswith("RSA"):
            return await asyncio.to_thread(
                _import_key, fmt, key_data, algorithm, extractable, usages)
        return _import_key(fmt, key_data, algorithm, extractable, usages)

    async def exportKey(self, fmt, key):
        return _export_key(fmt, key)

    async def generateKey(self, algorithm, extractable, usages):
        name, _ = _normalize(algorithm)
        if name.startswith("RSA"):
            return await asyncio.to_thread(
                _generate_key, algorithm, extractable, usages)
        return _generate_key(algorithm, extractable, usages)

    async def sign(self, algorithm, key, data):
        payload = _bytes(data)
        name = key.algorithm["name"] if isinstance(key, CryptoKey) else ""
        if name.startswith("RSA") or len(payload) >= THREAD_THRESHOLD_BYTES:
            result = await asyncio.to_thread(_sign, algorithm, key, payload)
        else:
            result = _sign(algorithm, key, payload)
        return _buffer(result)

    async def verify(self, algorithm, key, signature, data):
        payload = _bytes(data)
        sig = _bytes(signature)
        name = key.algorithm["name"] if isinstance(key, CryptoKey) else ""
        if name.startswith("RSA") or len(payload) >= THREAD_THRESHOLD_BYTES:
            return await asyncio.to_thread(_verify, algorithm, key, sig,
                                           payload)
        return _verify(algorithm, key, sig, payload)

    async def encrypt(self, algorithm, key, data):
        payload = _bytes(data)
        return _buffer(await _maybe_thread(
            len(payload), _crypt, True, algorithm, key, payload))

    async def decrypt(self, algorithm, key, data):
        payload = _bytes(data)
        return _buffer(await _maybe_thread(
            len(payload), _crypt, False, algorithm, key, payload))

    async def deriveBits(self, algorithm, base_key, length=None):
        _check_usage(base_key, "deriveBits")
        return _buffer(await asyncio.to_thread(
            _derive_bits, algorithm, base_key, length))

    async def deriveKey(self, algorithm, base_key, derived_type,
                        extractable, usages):
        _check_usage(base_key, "deriveKey")
        derived_name, derived_params = _normalize(derived_type)
        if derived_name.startswith("AES-"):
            length = int(_get(derived_params, "length", 256))
        elif derived_name == "HMAC":
            hash_name = _hash_name(_get(derived_params, "hash"))
            length = int(_get(derived_params, "length",
                              _HASHES[hash_name][2] * 8))
        else:
            raise _err("deriveKey cannot make %s keys" % derived_name,
                       "NotSupportedError")
        raw = await asyncio.to_thread(_derive_bits, algorithm, base_key,
                                      length)
        return _import_key("raw", raw, derived_type, extractable, usages)


class Crypto(JsProxy):
    def __init__(self):
        self.subtle = SubtleCrypto()

    def getRandomValues(self, array):
        from .js import Uint8Array
        if isinstance(array, Uint8Array):
            if array.length > 65536:
                raise _err("getRandomValues: quota exceeded",
                           "QuotaExceededError")
            array.set(os.urandom(array.length))
            return array
        if isinstance(array, (bytearray, memoryview)):
            array[:] = os.urandom(len(array))
            return array
        raise _err("getRandomValues needs an integer TypedArray",
                   "TypeMismatchError")

    def randomUUID(self):
        import uuid
        return str(uuid.uuid4())


crypto = Crypto()
