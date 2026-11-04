import ast
import base64
import re
from pathlib import Path

import pytest

EVENTS = Path(__file__).resolve().parents[1] / "src" / "events.py"
ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"


def _decoder():
    tree = ast.parse(EVENTS.read_text(encoding="utf-8"))
    wanted = [node for node in tree.body
              if (isinstance(node, ast.Assign) and getattr(node.targets[0], "id", "") == "_B64URL_UNPADDED")
              or (isinstance(node, ast.FunctionDef) and node.name == "b64url_decode")]
    names = {"base64": base64, "re": re}
    exec(compile(ast.Module(wanted, []), str(EVENTS), "exec"), names)
    return names["b64url_decode"]


def test_signature_decoding_rejects_aliased_encodings():
    decode = _decoder()
    raw = bytes(range(64))
    sig = base64.urlsafe_b64encode(raw).rstrip(b"=").decode()
    assert decode(sig) == raw
    aliased = sig[:-1] + ALPHABET[ALPHABET.index(sig[-1]) ^ 1]
    assert base64.urlsafe_b64decode(aliased + "==") == raw
    for variant in (aliased, sig + "==", " " + sig, sig[:40] + "." + sig[40:],
                    sig.replace("-", "+").replace("_", "/"), None):
        with pytest.raises(ValueError):
            decode(variant)


def test_ed25519_verify_decodes_signature_strictly():
    source = EVENTS.read_text(encoding="utf-8")
    body = source[source.index("async def ed25519_verify"):]
    assert "signature = b64url_decode(sig_b64url)" in body[:body.index("return")]
