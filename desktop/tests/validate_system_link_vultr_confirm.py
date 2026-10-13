#!/usr/bin/env python3

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/MainWindowControlNode.cpp").read_text(encoding="utf-8")


def between(source: str, start: str, end: str) -> str:
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


def main() -> None:
    handler = between(
        SOURCE,
        "void MainWindow::openCloudflareSetupFromSystemLink(",
        'if (bounded(QStringLiteral("mode"), 16) == QLatin1String("ssh"))',
    )
    vultr = between(handler, 'QLatin1String("vultr")', "return;")
    assert "createVultrMirrorFromForm" not in vultr, (
        "a forkmesh:// link from any web page must not deploy a Vultr mirror "
        "without a click in the desktop")
    assert "resumeVultrProvision" not in vultr
    assert "m_vultrCreateButton->setFocus" in vultr
    print("system link vultr confirm contract passed")


if __name__ == "__main__":
    main()
