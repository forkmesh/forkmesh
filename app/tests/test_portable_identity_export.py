#!/usr/bin/env python3
"""Portable identity recovery-kit contract shared by web and mobile clients."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
WEB = ROOT / "app" / "public" / "portable-identity.js"
MOBILE = (ROOT / "native" / "android" / "core" / "src" / "main" / "kotlin" / "com" / "forkmesh"
          / "nativeapp" / "security" / "PortableIdentity.kt")
DESKTOP = ROOT / "desktop" / "src" / "ForkMeshIdentity.cpp"
DOCS = ROOT / "www" / "docs" / "portable-identities.md"


def test_web_and_mobile_use_the_same_provider_neutral_envelope():
    web = WEB.read_text(encoding="utf-8")
    mobile = MOBILE.read_text(encoding="utf-8")
    desktop = DESKTOP.read_text(encoding="utf-8")
    for source in (web, mobile, desktop):
        assert "forkmesh.portable-identity" in source
        assert "forkmesh.portable-identity/v1" in source
        assert "PBKDF2-HMAC-SHA-256" in source
        assert "AES-256-GCM" in source
        assert "600000" in source
        assert "128 * 1024" in source or "128 * 1024" in source.replace(" ", "")


def test_export_is_client_side_and_authenticated_before_import():
    web = WEB.read_text(encoding="utf-8")
    mobile = MOBILE.read_text(encoding="utf-8")
    desktop = DESKTOP.read_text(encoding="utf-8")
    assert "additionalData: encoder.encode(PORTABLE_IDENTITY_AAD)" in web
    assert "Utf8.encode(AAD)" in mobile
    assert "Recovery passphrase is incorrect or the file was changed" in web
    assert "Recovery passphrase is incorrect or the file was changed" in mobile
    assert "fetch(" not in web
    assert "EVP_EncryptUpdate(ctx, nullptr, &length" in desktop
    assert "kPortableAad" in desktop


def test_recovery_and_delegation_drills_are_documented():
    docs = DOCS.read_text(encoding="utf-8").lower()
    for phrase in (
        "lost device",
        "compromised device",
        "key rotation",
        "social recovery",
        "organization delegation",
        "passkey",
        "hardware key",
        "revoked keys",
    ):
        assert phrase in docs
