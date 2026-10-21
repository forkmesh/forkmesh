"""A catalog record signed by the Go git node verifies in the catalog.

``server/testdata/catalog-v2-record.json`` is the request body the node's
``CatalogPublisher`` sends, produced and kept current by
``TestCatalogRecordFixtureForTheApp`` in ``server/catalog_test.go``. Its
description holds non-ASCII and HTML-sensitive characters, which encoding/json
writes differently from the ``json.dumps`` the catalog hashes; a node that
hashed its own encoder's output got ``bad_signature`` for every such record.
"""

import base64
import copy
import hashlib
import json
from pathlib import Path

import pytest
from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey

from catalog import safe_catalog_record

FIXTURE = (
    Path(__file__).resolve().parents[2]
    / "server" / "testdata" / "catalog-v2-record.json"
)


def _b64url(value):
    return base64.urlsafe_b64decode(value + "=" * (-len(value) % 4))


def _catalog_v2_verifies(data):
    """The catalog POST route's catalog-v2 check (catalog_routes.py)."""
    record = safe_catalog_record(data)
    if not record:
        return False
    signed_record = dict(record)
    signed_record.pop("signature", None)
    record_hash = hashlib.sha256(json.dumps(
        signed_record, sort_keys=True,
        separators=(",", ":")).encode()).hexdigest()
    canonical = ("forkmesh-catalog-v2\n" + record_hash).encode()
    key = Ed25519PublicKey.from_public_bytes(_b64url(record["maintainer"]))
    try:
        key.verify(_b64url(data["catalogSig"]), canonical)
    except InvalidSignature:
        return False
    return True


@pytest.fixture
def node_record():
    return json.loads(FIXTURE.read_text(encoding="utf-8"))


def test_fixture_covers_the_encodings_that_differ(node_record):
    description = node_record["description"]
    assert any(ord(ch) > 0x7F for ch in description)
    assert any(ord(ch) > 0xFFFF for ch in description)
    assert "\x7f" in description
    assert {"<", ">", "&"} <= set(description)
    # The wire form is encoding/json's, HTML-escaped and with literal UTF-8.
    raw = FIXTURE.read_text(encoding="utf-8")
    assert "\\u003c" in raw and "\\u0026" in raw and "—" in raw
    assert node_record["catalogSigVersion"] == 2


def test_catalog_keeps_every_field_the_node_signed(node_record):
    # A field the catalog dropped, defaulted or re-typed would change the
    # hashed record; the node's signature must cover exactly what is stored.
    sent = dict(node_record)
    sent.pop("catalogSig")
    sent.pop("catalogSigVersion")
    assert safe_catalog_record(node_record) == sent


def test_node_signed_record_verifies_in_the_catalog(node_record):
    assert _catalog_v2_verifies(node_record)


@pytest.mark.parametrize("field,value", [
    ("forkedFrom", "bob/forkmesh"),
    ("description", "Mirror <and> share"),
    ("stateHash", "ef" * 32),
    ("mirrorOf", "https://other.example.test/forkmesh/forkmesh"),
    ("agentProviders", ["codex"]),
])
def test_signature_still_binds_the_stored_fields(node_record, field, value):
    tampered = copy.deepcopy(node_record)
    tampered[field] = value
    assert not _catalog_v2_verifies(tampered)
