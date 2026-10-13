"""SSH public-key and gateway helpers for ForkMesh.

The Cloudflare Worker stores only normalized public keys (encrypted at rest)
and authorizes a separately operated SSH gateway.  It never receives or stores
private keys and it does not attempt to terminate the raw SSH protocol.

This module is stdlib-only so its parsing and URL contracts can be tested
outside the Workers runtime.
"""

from __future__ import annotations

import base64
import binascii
import hashlib
import ipaddress
import re


MAX_PUBLIC_KEY_LINE = 16 * 1024
MAX_KEY_LABEL = 80
MAX_KEYS_PER_ACCOUNT = 20
MAX_GATEWAY_REPOSITORIES = 1000
MAX_GATEWAY_ALLOWLIST_BYTES = 128 * 1024
MIN_RSA_BITS = 3072
MAX_RSA_BITS = 16384

SUPPORTED_KEY_TYPES = frozenset(
    {
        "ssh-ed25519",
        "sk-ssh-ed25519@openssh.com",
        "ecdsa-sha2-nistp256",
        "ecdsa-sha2-nistp384",
        "ecdsa-sha2-nistp521",
        "sk-ecdsa-sha2-nistp256@openssh.com",
        "ssh-rsa",
    }
)

_DNS_LABEL_RE = re.compile(
    r"^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$", re.IGNORECASE
)
_OWNER_RE = re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")
_REPO_RE = re.compile(r"^[A-Za-z0-9._-]{1,100}$")
_KEY_ID_RE = re.compile(r"^sk_[A-Za-z0-9_-]{16,80}$")
_EXECUTABLE_RE = re.compile(r"^/[A-Za-z0-9_./+-]{1,511}$")
_GATEWAY_TOKEN_RE = re.compile(r"^[A-Za-z0-9_-]{32,512}$")


class SshPublicKeyError(ValueError):
    """A user-supplied SSH public key is malformed or unsupported."""


def _read_ssh_string(blob: bytes, offset: int) -> tuple[bytes, int]:
    if offset + 4 > len(blob):
        raise SshPublicKeyError("invalid_key_blob")
    length = int.from_bytes(blob[offset : offset + 4], "big")
    offset += 4
    if length < 0 or offset + length > len(blob):
        raise SshPublicKeyError("invalid_key_blob")
    return blob[offset : offset + length], offset + length


def _decode_text(value: bytes) -> str:
    try:
        return value.decode("ascii")
    except UnicodeDecodeError as error:
        raise SshPublicKeyError("invalid_key_blob") from error


def _positive_mpint(value: bytes) -> int:
    # RFC 4251 mpints are signed two's-complement. RSA e/n must be positive and
    # minimally encoded (a leading zero is allowed only to clear the sign bit).
    if not value or value[0] & 0x80:
        raise SshPublicKeyError("invalid_rsa_key")
    if len(value) > 1 and value[0] == 0 and not value[1] & 0x80:
        raise SshPublicKeyError("invalid_rsa_key")
    return int.from_bytes(value, "big", signed=False)


def _validate_key_blob(key_type: str, blob: bytes) -> None:
    encoded_type, offset = _read_ssh_string(blob, 0)
    if _decode_text(encoded_type) != key_type:
        raise SshPublicKeyError("key_type_mismatch")

    if key_type == "ssh-ed25519":
        public, offset = _read_ssh_string(blob, offset)
        if len(public) != 32:
            raise SshPublicKeyError("invalid_ed25519_key")
    elif key_type == "sk-ssh-ed25519@openssh.com":
        public, offset = _read_ssh_string(blob, offset)
        application, offset = _read_ssh_string(blob, offset)
        if len(public) != 32 or not application or len(application) > 1024:
            raise SshPublicKeyError("invalid_security_key")
    elif key_type.startswith("ecdsa-sha2-"):
        curve, offset = _read_ssh_string(blob, offset)
        point, offset = _read_ssh_string(blob, offset)
        expected_curve = key_type.removeprefix("ecdsa-sha2-")
        if _decode_text(curve) != expected_curve:
            raise SshPublicKeyError("invalid_ecdsa_curve")
        coordinate_bytes = {
            "nistp256": 32,
            "nistp384": 48,
            "nistp521": 66,
        }[expected_curve]
        if len(point) != 1 + 2 * coordinate_bytes or point[:1] != b"\x04":
            raise SshPublicKeyError("invalid_ecdsa_point")
    elif key_type == "sk-ecdsa-sha2-nistp256@openssh.com":
        curve, offset = _read_ssh_string(blob, offset)
        point, offset = _read_ssh_string(blob, offset)
        application, offset = _read_ssh_string(blob, offset)
        if (
            curve != b"nistp256"
            or len(point) != 65
            or point[:1] != b"\x04"
            or not application
            or len(application) > 1024
        ):
            raise SshPublicKeyError("invalid_security_key")
    elif key_type == "ssh-rsa":
        exponent, offset = _read_ssh_string(blob, offset)
        modulus, offset = _read_ssh_string(blob, offset)
        if not exponent or not modulus:
            raise SshPublicKeyError("invalid_rsa_key")
        exponent_value = _positive_mpint(exponent)
        modulus_value = _positive_mpint(modulus)
        bits = modulus_value.bit_length()
        if exponent_value < 3 or exponent_value % 2 == 0:
            raise SshPublicKeyError("invalid_rsa_key")
        if bits < MIN_RSA_BITS:
            raise SshPublicKeyError("rsa_key_too_small")
        if bits > MAX_RSA_BITS:
            raise SshPublicKeyError("rsa_key_too_large")
    else:  # pragma: no cover - the allowlist is checked before this function.
        raise SshPublicKeyError("unsupported_key_type")

    if offset != len(blob):
        raise SshPublicKeyError("invalid_key_blob")


def parse_public_key(value: object) -> dict[str, str]:
    """Normalize one OpenSSH public-key line and calculate its fingerprint.

    Comments are returned separately and are never included in the normalized
    key.  Newlines and authorized_keys options are rejected, which prevents a
    pasted value from smuggling a second key or a forced command.
    """

    raw = str(value or "").strip()
    if not raw or len(raw.encode("utf-8")) > MAX_PUBLIC_KEY_LINE:
        raise SshPublicKeyError("invalid_public_key")
    if "\r" in raw or "\n" in raw or "\x00" in raw:
        raise SshPublicKeyError("one_public_key_required")
    parts = raw.split(None, 2)
    if len(parts) < 2:
        raise SshPublicKeyError("invalid_public_key")
    key_type, encoded = parts[0], parts[1]
    if key_type not in SUPPORTED_KEY_TYPES:
        raise SshPublicKeyError("unsupported_key_type")
    try:
        blob = base64.b64decode(encoded, validate=True)
    except (binascii.Error, ValueError) as error:
        raise SshPublicKeyError("invalid_key_encoding") from error
    if not blob:
        raise SshPublicKeyError("invalid_key_blob")
    _validate_key_blob(key_type, blob)
    canonical = base64.b64encode(blob).decode("ascii")
    fingerprint = (
        "SHA256:"
        + base64.b64encode(hashlib.sha256(blob).digest())
        .decode("ascii")
        .rstrip("=")
    )
    return {
        "keyType": key_type,
        "publicKey": key_type + " " + canonical,
        "fingerprint": fingerprint,
        "comment": parts[2].strip() if len(parts) == 3 else "",
    }


def clean_key_label(value: object, fallback: object = "") -> str:
    """Return a private, display-only key label without control characters."""

    candidate = str(value or "").strip() or str(fallback or "").strip()
    candidate = re.sub(r"[\x00-\x1f\x7f]+", " ", candidate)
    candidate = re.sub(r"\s+", " ", candidate).strip()
    return candidate[:MAX_KEY_LABEL] or "SSH key"


def configured_gateway_host(value: object) -> str:
    """Validate a bare DNS name or IP address for SSH URL publication."""

    host = str(value or "").strip().lower()
    if not host or len(host) > 253:
        return ""
    if any(char in host for char in "/@?#") or "://" in host:
        return ""
    unbracketed = host[1:-1] if host.startswith("[") and host.endswith("]") else host
    try:
        parsed_ip = ipaddress.ip_address(unbracketed)
        return str(parsed_ip)
    except ValueError:
        pass
    if host.endswith("."):
        host = host[:-1]
    labels = host.split(".")
    if not labels or any(not _DNS_LABEL_RE.fullmatch(label) for label in labels):
        return ""
    return host


def configured_gateway_token(value: object) -> str:
    """Return a header-safe gateway bearer token or an empty string.

    The token remains a Worker secret.  This helper only prevents a malformed
    deployment value (for example one containing a newline) from making the
    catalog advertise an SSH transport that the gateway cannot use.
    """

    token = str(value or "")
    if not _GATEWAY_TOKEN_RE.fullmatch(token):
        return ""
    return token


def parse_gateway_repository_allowlist(value: object) -> dict[str, str]:
    """Parse the Worker-side copy of the gateway's explicit repo allowlist.

    Format: ``owner/repo=read-write,owner/other=read-only``.  A single invalid
    or conflicting entry fails the whole value closed.  Repository names are
    canonicalized only for comparison; the catalog continues to publish the
    repository's own spelling.
    """

    raw = str(value or "").strip()
    if not raw or len(raw.encode("utf-8")) > MAX_GATEWAY_ALLOWLIST_BYTES:
        return {}
    repositories: dict[str, str] = {}
    entries = raw.split(",")
    if len(entries) > MAX_GATEWAY_REPOSITORIES:
        return {}
    for raw_entry in entries:
        entry = raw_entry.strip()
        namespace, separator, raw_access = entry.rpartition("=")
        pieces = namespace.split("/")
        access = raw_access.strip().lower()
        if access == "ro":
            access = "read-only"
        elif access == "rw":
            access = "read-write"
        if (
            separator != "="
            or len(pieces) != 2
            or not _OWNER_RE.fullmatch(pieces[0].strip().lower())
            or not _REPO_RE.fullmatch(pieces[1].strip())
            or access not in ("read-only", "read-write")
        ):
            return {}
        key = pieces[0].strip().lower() + "/" + pieces[1].strip().lower()
        if key in repositories:
            return {}
        repositories[key] = access
    return repositories


def parse_gateway_node_hosts(value: object) -> dict[str, str]:
    """Parse optional per-node SSH gateway hosts.

    Format: ``node-a=ssh-a.example.org,node-b=192.0.2.10``.  The mapping only
    selects the transport host; repository authorization remains independently
    constrained by ``SSH_GATEWAY_REPOSITORIES``.  As with that allowlist, one
    malformed or conflicting entry fails the whole value closed.
    """

    raw = str(value or "").strip()
    if not raw:
        return {}
    if len(raw.encode("utf-8")) > MAX_GATEWAY_ALLOWLIST_BYTES:
        return {}
    hosts: dict[str, str] = {}
    entries = raw.split(",")
    if len(entries) > MAX_GATEWAY_REPOSITORIES:
        return {}
    for raw_entry in entries:
        raw_owner, separator, raw_host = raw_entry.strip().partition("=")
        owner = raw_owner.strip().lower()
        host = configured_gateway_host(raw_host)
        if (
            separator != "="
            or not _OWNER_RE.fullmatch(owner)
            or not host
            or owner in hosts
        ):
            return {}
        hosts[owner] = host
    return hosts


def gateway_repository_access(
    repositories: object, owner: object, repository: object
) -> str:
    """Return the configured access for one canonical repository."""

    if not isinstance(repositories, dict):
        return ""
    clean_owner = str(owner or "").strip().lower()
    clean_repo = str(repository or "").strip().lower()
    if not _OWNER_RE.fullmatch(clean_owner) or not _REPO_RE.fullmatch(clean_repo):
        return ""
    access = repositories.get(clean_owner + "/" + clean_repo)
    return access if access in ("read-only", "read-write") else ""


def ssh_repository_url(
    host: object, port: object, owner: object, repository: object
) -> str:
    """Build an SSH URL only when an independently operated gateway exists."""

    clean_host = configured_gateway_host(host)
    clean_owner = str(owner or "").strip().lower()
    clean_repo = str(repository or "").strip()
    if (
        not clean_host
        or not _OWNER_RE.fullmatch(clean_owner)
        or not _REPO_RE.fullmatch(clean_repo)
    ):
        return ""
    try:
        clean_port = int(port or 22)
    except (TypeError, ValueError):
        return ""
    if not 1 <= clean_port <= 65535:
        return ""
    display_host = (
        "[" + clean_host + "]" if ":" in clean_host and not clean_host.startswith("[")
        else clean_host
    )
    port_part = "" if clean_port == 22 else ":" + str(clean_port)
    return (
        "ssh://git@"
        + display_host
        + port_part
        + "/"
        + clean_owner
        + "/"
        + clean_repo
        + ".git"
    )


def forced_authorized_key_line(
    public_key: object, key_id: object, executable: object
) -> str:
    """Return the restrictive line emitted by AuthorizedKeysCommand."""

    parsed = parse_public_key(public_key)
    clean_id = str(key_id or "").strip()
    command = str(executable or "").strip()
    if not _KEY_ID_RE.fullmatch(clean_id):
        raise ValueError("invalid_key_id")
    if (
        not _EXECUTABLE_RE.fullmatch(command)
        or any(part in ("", ".", "..") for part in command.split("/")[1:])
    ):
        raise ValueError("invalid_gateway_executable")
    options = (
        'restrict,command="'
        + command
        + " serve --key-id "
        + clean_id
        + '"'
    )
    return options + " " + parsed["publicKey"]
