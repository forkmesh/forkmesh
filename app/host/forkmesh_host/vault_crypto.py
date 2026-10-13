"""Encryption of the host secrets vault (``host_secrets`` table).

Pure module: no app, no host and no I/O imports, so the operator tool
(tools/next_vault.py, on the desktop) and the node (forkmesh_host.vault)
share exactly one implementation.

* Key: ``HKDF-SHA256(ikm=DATA_KEY utf-8, salt=b"fmnext-vault-v1",
  info=b"fmnext-host-secrets")``, 32 bytes, AES-256-GCM.
* Nonce: 96 random bits per row (a row is rewritten with a fresh nonce on
  every sync).
* AAD: ``b"fmnext-secret-v1\\0" + name``: a ciphertext only opens under the
  name it was sealed for, so swapping rows between names fails.
* ``key_id``: the first 16 hex of ``SHA-256(HKDF(..., info=b"fmnext-key-id"))``.
  It tells a wrong key apart without trying a decrypt (no oracle), and
  reveals nothing usable about the key.
* ``__vault_check__``: a reserved row sealing a fixed plaintext, so a node
  can prove the key it was handed is the one the vault was written with.

``DATA_KEY`` itself is never a row: it is the vault's key and exists only on
the operator's desktop and, after an unseal, in the node's process memory.
"""

from __future__ import annotations

import base64
import hashlib
import hmac
import os
import re

from cryptography.exceptions import InvalidTag
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

SALT = b"fmnext-vault-v1"
ENC_INFO = b"fmnext-host-secrets"
KEY_ID_INFO = b"fmnext-key-id"
AAD_PREFIX = b"fmnext-secret-v1\0"
CHECK_NAME = "__vault_check__"
CHECK_PLAINTEXT = "fmnext-vault-check-v1"
NONCE_BYTES = 12
# Secrets that stay systemd credentials: rqlite and the mesh need them before
# any key exists (the app cannot read the vault without reaching rqlite).
BOOTSTRAP_NAMES = ("RQLITE_PASSWORD", "FORKMESH_MESH_SECRET")
# The vault key; never stored in the vault or on a node's disk.
KEY_NAME = "DATA_KEY"
NAME_RE = re.compile(r"^[A-Z][A-Z0-9_]{1,63}$")
MIN_KEY_LENGTH = 32
# Same list as entry._INSECURE_DATA_KEYS: placeholders are refused.
INSECURE_KEYS = frozenset({
    "", "forkmesh-dev-data-key", "forkmesh-dev-data-key-change-me"})


class VaultError(Exception):
    """A vault operation failed; the message never contains secret values."""


def _b64(data: bytes) -> str:
    return base64.b64encode(data).decode("ascii")


def _unb64(text) -> bytes:
    try:
        return base64.b64decode(str(text), validate=True)
    except (ValueError, TypeError):
        raise VaultError("malformed base64 in vault row") from None


def _hkdf(ikm: bytes, info: bytes) -> bytes:
    return HKDF(algorithm=hashes.SHA256(), length=32, salt=SALT,
                info=info).derive(ikm)


def check_key_text(data_key) -> str:
    """Normalise and sanity-check a DATA_KEY (raises VaultError)."""
    if isinstance(data_key, (bytes, bytearray)):
        data_key = bytes(data_key).decode("utf-8")
    key = str(data_key or "").strip()
    if key in INSECURE_KEYS:
        raise VaultError("DATA_KEY is empty or a known placeholder")
    if len(key) < MIN_KEY_LENGTH:
        raise VaultError("DATA_KEY is shorter than %d characters"
                         % MIN_KEY_LENGTH)
    if "\n" in key or "\r" in key or "\0" in key:
        raise VaultError("DATA_KEY contains a line break or NUL")
    return key


def valid_name(name) -> bool:
    return name == CHECK_NAME or bool(NAME_RE.match(str(name or "")))


def aad(name: str) -> bytes:
    return AAD_PREFIX + name.encode("utf-8")


class VaultKey:
    """The derived vault keys. Holds no copy of DATA_KEY itself."""

    __slots__ = ("_aead", "key_id")

    def __init__(self, data_key):
        ikm = check_key_text(data_key).encode("utf-8")
        self._aead = AESGCM(_hkdf(ikm, ENC_INFO))
        self.key_id = hashlib.sha256(_hkdf(ikm, KEY_ID_INFO)).hexdigest()[:16]

    def __repr__(self):
        return "VaultKey(key_id=%s)" % self.key_id

    def encrypt(self, name: str, value: str, nonce: bytes | None = None):
        """Return the row dict (name, nonce, ciphertext, key_id)."""
        if not valid_name(name) or name == KEY_NAME:
            raise VaultError("%r cannot be stored in the vault" % name)
        nonce = nonce or os.urandom(NONCE_BYTES)
        if len(nonce) != NONCE_BYTES:
            raise VaultError("nonce must be %d bytes" % NONCE_BYTES)
        sealed = self._aead.encrypt(nonce, str(value).encode("utf-8"),
                                    aad(name))
        return {"name": name, "nonce": _b64(nonce),
                "ciphertext": _b64(sealed), "key_id": self.key_id}

    def decrypt(self, row) -> str:
        name = str(row.get("name") or "")
        if not valid_name(name):
            raise VaultError("vault row has an invalid name")
        if not hmac.compare_digest(str(row.get("key_id") or ""),
                                   self.key_id):
            raise VaultError("vault row %s was sealed with another key "
                             "(key_id mismatch)" % name)
        nonce = _unb64(row.get("nonce"))
        if len(nonce) != NONCE_BYTES:
            raise VaultError("vault row %s has a bad nonce" % name)
        try:
            plain = self._aead.decrypt(nonce, _unb64(row.get("ciphertext")),
                                       aad(name))
        except InvalidTag:
            raise VaultError("vault row %s failed authentication (tampered, "
                             "swapped or wrong key)" % name) from None
        return plain.decode("utf-8")

    def check_row(self):
        return self.encrypt(CHECK_NAME, CHECK_PLAINTEXT)


def open_rows(key: VaultKey, rows) -> dict:
    """Verify the check row and decrypt every row: {name: value}.

    Fails closed: a missing or wrong check row, a row sealed under another
    key or any failed authentication raises VaultError and nothing is
    returned.
    """
    by_name = {}
    for row in rows or []:
        name = str(row.get("name") or "")
        if name in by_name:
            raise VaultError("duplicate vault row %s" % name)
        by_name[name] = row
    check = by_name.pop(CHECK_NAME, None)
    if check is None:
        raise VaultError("vault_empty: no %s row (run tools/next_vault.py "
                         "sync)" % CHECK_NAME)
    if not hmac.compare_digest(str(check.get("key_id") or ""), key.key_id):
        raise VaultError("wrong_key: key_id does not match the vault")
    if key.decrypt(check) != CHECK_PLAINTEXT:
        raise VaultError("wrong_key: check row does not match")
    values = {}
    for name, row in sorted(by_name.items()):
        if name == KEY_NAME or name in BOOTSTRAP_NAMES:
            raise VaultError("vault row %s is not allowed" % name)
        values[name] = key.decrypt(row)
    return values


# -- peer unseal (vault.py PeerUnseal) -----------------------------------------
#
# A sealed node asks a live, unsealed core peer for DATA_KEY over the
# WireGuard mesh. The request carries an ephemeral X25519 public key; the
# peer answers with DATA_KEY sealed to it (its own ephemeral X25519 key +
# HKDF-SHA256 + AES-256-GCM), the whole exchange bound into the AAD. A
# passive observer, or anyone replaying the response, learns nothing without
# the requester's ephemeral private key, which never leaves its memory.

PEER_SALT = b"fmnext-peer-unseal-v1"
PEER_LABEL = b"fmnext-peer-unseal-v1\0"


def peer_transcript(requester, responder, requester_pub: bytes,
                    responder_pub: bytes, request_nonce: str) -> bytes:
    return b"".join((
        PEER_LABEL, str(requester).encode(), b"\0", str(responder).encode(),
        b"\0", requester_pub, responder_pub, str(request_nonce).encode()))


def peer_keypair():
    """(private key object, raw 32-byte public key) for one request."""
    from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey
    from cryptography.hazmat.primitives import serialization
    private = X25519PrivateKey.generate()
    public = private.public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    return private, public


def _peer_aead(private, peer_pub: bytes, transcript: bytes):
    from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PublicKey
    try:
        shared = private.exchange(X25519PublicKey.from_public_bytes(peer_pub))
    except ValueError:
        raise VaultError("bad X25519 public key") from None
    key = HKDF(algorithm=hashes.SHA256(), length=32, salt=PEER_SALT,
               info=hashlib.sha256(transcript).digest()).derive(shared)
    return AESGCM(key)


def peer_seal(data_key, requester, responder, requester_pub_b64,
              request_nonce):
    """Responder side: DATA_KEY sealed to the requester's ephemeral key."""
    requester_pub = _unb64(requester_pub_b64)
    if len(requester_pub) != 32:
        raise VaultError("requester key must be 32 bytes")
    private, public = peer_keypair()
    transcript = peer_transcript(requester, responder, requester_pub, public,
                                 request_nonce)
    nonce = os.urandom(NONCE_BYTES)
    sealed = _peer_aead(private, requester_pub, transcript).encrypt(
        nonce, check_key_text(data_key).encode("utf-8"), transcript)
    return {"responder": str(responder), "pub": _b64(public),
            "nonce": _b64(nonce), "ciphertext": _b64(sealed)}


def peer_open(private, requester_pub: bytes, requester, responder,
              request_nonce, reply) -> str:
    """Requester side: open a peer_seal reply (raises VaultError)."""
    if str(reply.get("responder") or "") != str(responder):
        raise VaultError("reply names another responder")
    responder_pub = _unb64(reply.get("pub"))
    nonce = _unb64(reply.get("nonce"))
    if len(responder_pub) != 32 or len(nonce) != NONCE_BYTES:
        raise VaultError("malformed peer reply")
    transcript = peer_transcript(requester, responder, requester_pub,
                                 responder_pub, request_nonce)
    try:
        plain = _peer_aead(private, responder_pub, transcript).decrypt(
            nonce, _unb64(reply.get("ciphertext")), transcript)
    except InvalidTag:
        raise VaultError("peer reply failed authentication") from None
    return check_key_text(plain.decode("utf-8"))
