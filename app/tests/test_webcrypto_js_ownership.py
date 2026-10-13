"""WebCrypto must never retain a Python-owned WASM byte view."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_async_webcrypto_buffers_are_copied_into_js_memory():
    entry = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
    events = (ROOT / "src" / "events.py").read_text(encoding="utf-8")
    fediverse = (ROOT / "src" / "fediverse_routes.py").read_text(
        encoding="utf-8")

    assert "return Uint8Array.new(_to_js(bytes(value)))" in events
    assert "return Uint8Array.new(_to_js(bytes(value)))" in fediverse
    assert entry.count("Uint8Array.new(_to_js(bytes(") >= 20

    unsafe = (
        "_to_js(raw_key)", "_to_js(signature)", "_to_js(data_bytes)",
        "_to_js(text.encode())", "_to_js(secret.encode())",
        "_to_js(passphrase.encode())", "_to_js(plaintext)",
        "_to_js(password.encode())", "_to_js(msg)", "_to_js(norm)",
        "_to_js(blob[:12])", "_to_js(blob[12:])", "_to_js(der)",
        "_to_js(base64.b64decode(priv_b64))",
    )
    combined = entry + events + fediverse
    for snippet in unsafe:
        assert snippet not in combined, snippet
