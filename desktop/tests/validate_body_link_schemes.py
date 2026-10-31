#!/usr/bin/env python3

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPO_DETAIL = (ROOT / "src/MainWindowRepoDetail.cpp").read_text(encoding="utf-8")
EDITOR = (ROOT / "src/MarkdownEditor.cpp").read_text(encoding="utf-8")
OWN_TEXT_LINKS = {
    "MainWindowBranches.cpp": 1,
    "MainWindowLedger.cpp": 2,
    "MainWindowLogs.cpp": 3,
    "MainWindowMembership.cpp": 1,
    "MainWindowNetworkDiagnostics.cpp": 1,
    "MainWindowReleases.cpp": 1,
    "MainWindowSetup.cpp": 1,
}
FILTERED = {
    "MainWindowIssues.cpp": "QTextBrowser(&dialog)",
    "MainWindowMarket.cpp": "marketGuideLinks",
    "MainWindowTeammateHome.cpp": "teammateHomeLink",
}


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
    for name, anchor in FILTERED.items():
        source = (ROOT / "src" / name).read_text(encoding="utf-8")
        start = source.index(anchor)
        window = source[start:start + 900]
        assert "ChatLinkText::externalLinkUrl(" in window and "setOpenExternalLinks(true)" not in window, (
            f"{name}: links in text that is not ours must go through the scheme filter")
    for path in sorted((ROOT / "src").glob("*.cpp")):
        count = path.read_text(encoding="utf-8").count("setOpenExternalLinks(true)")
        assert count <= OWN_TEXT_LINKS.get(path.name, 0), (
            f"{path.name} opens links itself; route text that is not ours through "
            "ChatLinkText::externalLinkUrl, or list a label that only holds our own links here")
    print("body link scheme contract passed")


if __name__ == "__main__":
    main()
