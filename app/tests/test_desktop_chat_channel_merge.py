"""Regression coverage for combining mesh and Office chat rooms."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MESSAGES = (ROOT / "desktop/src/MainWindowMessages.cpp").read_text(
    encoding="utf-8"
)
SETUP = (ROOT / "desktop/src/MainWindowSetup.cpp").read_text(encoding="utf-8")


def test_office_refresh_cannot_clear_default_chat_rooms_through_aliasing():
    function = MESSAGES.split(
        "void MainWindow::setChannels(const QStringList &channels)", 1
    )[1].split("\n}\n", 1)[0]

    snapshot = function.index("const QStringList incomingChannels = channels;")
    clear = function.index("m_channels.clear();")
    iterate = function.index("for (const QString &channel : incomingChannels)")

    assert snapshot < clear < iterate
    assert "for (const QString &channel : channels)" not in function
    assert "setChannels(m_channels);" in SETUP


def test_default_rooms_are_installed_before_office_rooms_arrive():
    assert 'm_backend->addChannel(QStringLiteral("general"));' in SETUP
    assert "m_backend->addChannel(kWelcomeChannel);" in SETUP
    assert 'm_backend->addChannel(QStringLiteral("random"));' in SETUP
