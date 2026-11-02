from pathlib import Path


PAGE = Path(__file__).resolve().parents[1] / "content" / "web" / "page.js"


def test_server_html_is_read_in_an_inert_document():
    source = PAGE.read_text(encoding="utf-8")
    helper = next(line for line in source.splitlines() if "const text = (html)" in line)
    assert "DOMParser" in helper
    assert "createElement" not in helper and "innerHTML" not in helper
