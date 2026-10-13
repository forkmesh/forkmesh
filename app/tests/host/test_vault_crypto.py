"""forkmesh_host.vault_crypto: the host secrets vault format."""

from __future__ import annotations

import base64

import pytest

from forkmesh_host import vault_crypto
from forkmesh_host.vault_crypto import VaultError, VaultKey

KEY = "unit-test-data-key-" + "x" * 40
OTHER = "unit-test-other-key-" + "y" * 40


def sealed(key=KEY, values=None):
    vault = VaultKey(key)
    values = values or {"ADMIN_PASS": "admin-secret", "SENTRY_DSN": "https://x@y/1"}
    return [vault.check_row()] + [vault.encrypt(n, v) for n, v in values.items()]


def test_roundtrip():
    rows = sealed()
    assert vault_crypto.open_rows(VaultKey(KEY), rows) == {
        "ADMIN_PASS": "admin-secret", "SENTRY_DSN": "https://x@y/1"}
    # Fresh random nonce per encryption.
    a, b = VaultKey(KEY).encrypt("ADMIN_PASS", "v"), VaultKey(KEY).encrypt("ADMIN_PASS", "v")
    assert a["nonce"] != b["nonce"] and a["ciphertext"] != b["ciphertext"]
    assert len(base64.b64decode(a["nonce"])) == 12


def test_rows_hold_no_plaintext_or_key():
    rows = sealed()
    blob = repr(rows)
    assert "admin-secret" not in blob and KEY not in blob
    assert KEY not in repr(VaultKey(KEY))


def test_key_id_is_stable_and_distinguishes_keys():
    assert VaultKey(KEY).key_id == VaultKey(KEY).key_id
    assert len(VaultKey(KEY).key_id) == 16
    assert VaultKey(KEY).key_id != VaultKey(OTHER).key_id
    # Surrounding whitespace (a trailing newline from a file) is not the key.
    assert VaultKey(KEY + "\n").key_id == VaultKey(KEY).key_id


def test_wrong_key_is_detected_by_key_id_without_decrypting(monkeypatch):
    rows = sealed()
    wrong = VaultKey(OTHER)
    calls = []
    monkeypatch.setattr(VaultKey, "decrypt", lambda self, row: calls.append(row))
    with pytest.raises(VaultError, match="wrong_key"):
        vault_crypto.open_rows(wrong, rows)
    assert calls == []


def test_swapping_ciphertexts_between_names_fails():
    rows = sealed()
    by_name = {r["name"]: r for r in rows}
    a, b = by_name["ADMIN_PASS"], by_name["SENTRY_DSN"]
    a["nonce"], b["nonce"] = b["nonce"], a["nonce"]
    a["ciphertext"], b["ciphertext"] = b["ciphertext"], a["ciphertext"]
    with pytest.raises(VaultError, match="failed authentication"):
        vault_crypto.open_rows(VaultKey(KEY), rows)
    # A row renamed in place fails too.
    row = VaultKey(KEY).encrypt("ADMIN_PASS", "v")
    row["name"] = "WWW_EDGE_SECRET"
    with pytest.raises(VaultError):
        VaultKey(KEY).decrypt(row)


def test_tampering_is_detected():
    rows = sealed()
    target = next(r for r in rows if r["name"] == "ADMIN_PASS")
    raw = bytearray(base64.b64decode(target["ciphertext"]))
    raw[0] ^= 1
    target["ciphertext"] = base64.b64encode(bytes(raw)).decode()
    with pytest.raises(VaultError, match="ADMIN_PASS"):
        vault_crypto.open_rows(VaultKey(KEY), rows)
    rows = sealed()
    next(r for r in rows if r["name"] == "SENTRY_DSN")["nonce"] = "!!"
    with pytest.raises(VaultError):
        vault_crypto.open_rows(VaultKey(KEY), rows)


def test_rows_under_another_key_fail_closed():
    rows = sealed() + [VaultKey(OTHER).encrypt("WWW_EDGE_SECRET", "x")]
    with pytest.raises(VaultError, match="another key"):
        vault_crypto.open_rows(VaultKey(KEY), rows)


def test_check_row_required_and_verified():
    rows = [r for r in sealed() if r["name"] != vault_crypto.CHECK_NAME]
    with pytest.raises(VaultError, match="vault_empty"):
        vault_crypto.open_rows(VaultKey(KEY), rows)
    key = VaultKey(KEY)
    bogus = key.encrypt(vault_crypto.CHECK_NAME, "something else")
    with pytest.raises(VaultError, match="wrong_key"):
        vault_crypto.open_rows(key, [bogus])


def test_forbidden_names_and_keys():
    key = VaultKey(KEY)
    with pytest.raises(VaultError):
        key.encrypt("DATA_KEY", "x")
    with pytest.raises(VaultError):
        key.encrypt("lower_case", "x")
    for bootstrap in vault_crypto.BOOTSTRAP_NAMES:
        with pytest.raises(VaultError, match="not allowed"):
            vault_crypto.open_rows(key, [key.check_row(), key.encrypt(bootstrap, "x")])
    for bad in ("", "forkmesh-dev-data-key", "short", "a" * 40 + "\nb"):
        with pytest.raises(VaultError):
            VaultKey(bad)
