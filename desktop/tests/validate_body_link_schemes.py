#!/usr/bin/env python3

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPO_DETAIL = (ROOT / "src/MainWindowRepoDetail.cpp").read_text(encoding="utf-8")
EDITOR = (ROOT / "src/MarkdownEditor.cpp").read_text(encoding="utf-8")


def function_body(source: str, signature: str) -> str:
    begin = source.index(signature)
    end = source.index("\n}\n", begin)
    return source[begin:end]


def main() -> None:
    for name in ("openReferenceLink", "openBodyReference"):
        body = function_body(REPO_DETAIL, f"void MainWindow::{name}(const QString &href)")
        assert "ChatLinkText::externalLinkUrl(href)" in body, (
            f"{name} must hand only http(s) and mailto targets to the OS")
        assert "QDesktopServices::openUrl(QUrl(href))" not in body
        assert "QUrl::fromUserInput(href)" not in body
        assert body.count("QDesktopServices::openUrl(") == 1, (
            f"{name} opens a URL the scheme filter did not check")
    assert "setOpenExternalLinks(true)" not in EDITOR, (
        "the Markdown preview must not open links without the scheme filter")
    assert "ChatLinkText::externalLinkUrl(" in EDITOR
    print("body link scheme contract passed")


if __name__ == "__main__":
    main()
