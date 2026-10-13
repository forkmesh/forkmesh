from __future__ import annotations

import base64
import hmac
import json
import os
import stat
from pathlib import Path
from urllib.parse import unquote_to_bytes

KIND = "forkmesh.vault"
SCHEME = "ed25519-sig-hkdf-sha256-v1"
MARKER = "[stored in the ForkMesh vault]"
MAX_FILE_BYTES = 32 << 20
KEY_BYTES = 32
SALT_BYTES = 32
NONCE_BYTES = 12
TAG_BYTES = 16
LABEL_KEK = b"forkmesh-vault-kek-v1"
LABEL_WRAP = b"forkmesh-vault-wrap-v1"
LABEL_ENTRY_KEY = b"forkmesh-vault-entry-key-v1"
LABEL_ENTRY = b"forkmesh-vault-entry-v1"
LABEL_MAC = b"forkmesh-vault-mac-v1"
LABEL_KEY_ID = b"forkmesh-vault-key-id-v1"


class VaultError(Exception):
    def __init__(self, code: str):
        super().__init__(code)
        self.code = code


def vault_path(data: Path) -> Path:
    return Path(data) / "vault" / "secrets.json"


def identity_path(data: Path) -> Path:
    return Path(data) / "identity" / "ed25519.pem"


def _b64(value, size: int = 0) -> bytes:
    if not isinstance(value, str):
        raise VaultError("vault_invalid")
    try:
        raw = base64.b64decode(value.encode("ascii"), validate=True)
    except (ValueError, UnicodeEncodeError):
        raise VaultError("vault_invalid") from None
    if size and len(raw) != size:
        raise VaultError("vault_invalid")
    return raw


def _hkdf(secret: bytes, salt: bytes, info: bytes, size: int) -> bytes:
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.kdf.hkdf import HKDF

    return HKDF(algorithm=hashes.SHA256(), length=size, salt=salt or None, info=info).derive(secret)


def _open(key: bytes, nonce: bytes, sealed: bytes, aad: bytes) -> bytes | None:
    from cryptography.exceptions import InvalidTag
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM

    if len(key) != KEY_BYTES or len(nonce) != NONCE_BYTES or len(sealed) < TAG_BYTES:
        return None
    try:
        return AESGCM(key).decrypt(nonce, sealed, aad)
    except InvalidTag:
        return None


def _load_identity(path: Path):
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

    try:
        raw = Path(path).read_bytes()[: 64 * 1024]
        key = serialization.load_pem_private_key(raw, password=None)
    except (OSError, ValueError, TypeError):
        raise VaultError("vault_no_identity") from None
    if not isinstance(key, Ed25519PrivateKey):
        raise VaultError("vault_no_identity")
    public = key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    return key, base64.urlsafe_b64encode(public).rstrip(b"=").decode("ascii")


def _integer(value) -> int:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return 0
    return int(value)


def _canonical(doc: dict, entries: dict) -> bytes:
    out = LABEL_MAC + b"\n"
    out += str(_integer(doc.get("v"))).encode() + b"\n"
    out += str(doc.get("keyId", "")).encode() + b"\n"
    out += str(_integer(doc.get("generation"))).encode() + b"\n"
    for name in sorted(key.encode("utf-8") for key in entries):
        entry = entries[name.decode("utf-8")]
        if not isinstance(entry, dict):
            entry = {}
        out += str(len(name)).encode() + b":" + name + b"\n"
        out += str(entry.get("nonce", "")).encode() + b"\n"
        out += str(entry.get("data", "")).encode() + b"\n"
        out += str(_integer(entry.get("at"))).encode() + b"\n"
    return out


def read_namespace(path: Path, identity: Path, namespace: str) -> dict[str, tuple[bytes, int]]:
    path = Path(path)
    try:
        info = os.lstat(path)
    except FileNotFoundError:
        raise VaultError("vault_missing") from None
    except OSError:
        raise VaultError("vault_io") from None
    if not stat.S_ISREG(info.st_mode) or info.st_size > MAX_FILE_BYTES:
        raise VaultError("vault_invalid")
    try:
        doc = json.loads(path.read_bytes())
    except OSError:
        raise VaultError("vault_io") from None
    except ValueError:
        raise VaultError("vault_invalid") from None
    if not isinstance(doc, dict):
        raise VaultError("vault_invalid")
    key_id = doc.get("keyId")
    wraps = doc.get("wraps")
    entries = doc.get("entries") or {}
    if (doc.get("kind") != KIND or doc.get("v") != 1 or not isinstance(key_id, str) or len(key_id) != 16
            or not isinstance(wraps, list) or not wraps or not isinstance(entries, dict)):
        raise VaultError("vault_invalid")
    key, public = _load_identity(identity)
    dek = None
    matched = False
    for wrap in wraps:
        if not isinstance(wrap, dict) or wrap.get("scheme") != SCHEME or wrap.get("identity") != public:
            continue
        matched = True
        try:
            salt = _b64(wrap.get("salt"), SALT_BYTES)
            nonce = _b64(wrap.get("nonce"), NONCE_BYTES)
            sealed = _b64(wrap.get("key"), KEY_BYTES + TAG_BYTES)
        except VaultError:
            continue
        signature = key.sign(LABEL_KEK + b"\n" + public.encode("ascii"))
        kek = _hkdf(signature, salt, LABEL_KEK, KEY_BYTES)
        candidate = _open(kek, nonce, sealed, LABEL_WRAP + b"\0" + public.encode("ascii") + b"\0" + key_id.encode())
        if candidate is not None and _hkdf(candidate, b"", LABEL_KEY_ID, 8).hex() == key_id:
            dek = candidate
            break
    if dek is None:
        raise VaultError("vault_tampered" if matched else "vault_locked")
    want = _b64(doc.get("mac"), 32) if isinstance(doc.get("mac"), str) else b""
    mac_key = _hkdf(dek, b"", LABEL_MAC, KEY_BYTES)
    if len(want) != 32 or not hmac.compare_digest(hmac.new(mac_key, _canonical(doc, entries), "sha256").digest(), want):
        raise VaultError("vault_tampered")
    entry_key = _hkdf(dek, b"", LABEL_ENTRY_KEY, KEY_BYTES)
    prefix = namespace + "/"
    out: dict[str, tuple[bytes, int]] = {}
    for name, entry in entries.items():
        if not name.startswith(prefix):
            continue
        if not isinstance(entry, dict):
            raise VaultError("vault_tampered")
        try:
            nonce = _b64(entry.get("nonce"), NONCE_BYTES)
            sealed = _b64(entry.get("data"))
        except VaultError:
            raise VaultError("vault_tampered") from None
        plain = _open(entry_key, nonce, sealed, LABEL_ENTRY + b"\0" + key_id.encode() + b"\0" + name.encode("utf-8"))
        if plain is None:
            raise VaultError("vault_tampered")
        try:
            label = unquote_to_bytes(name[len(prefix):]).decode("utf-8")
        except UnicodeDecodeError:
            raise VaultError("vault_tampered") from None
        out[label] = (plain, _integer(entry.get("at")))
    return out


def is_marker(text: str) -> bool:
    return text.strip() == MARKER


def release_secret(path: Path, name: str, data: Path) -> str:
    text = Path(path).read_text()
    if not is_marker(text):
        return text.strip()
    values = read_namespace(vault_path(data), identity_path(data), "release")
    if name not in values:
        raise VaultError("vault_missing_entry")
    return values[name][0].decode("utf-8").strip()
