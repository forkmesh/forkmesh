"""Regression coverage for combining mesh and Office chat rooms."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MESSAGES = (ROOT / "desktop/src/MainWindowMessages.cpp").read_text(
    encoding="utf-8"
)
SETUP = (ROOT / "desktop/src/MainWindowSetup.cpp").read_text(encoding="utf-8")
NAVIGATION = (ROOT / "desktop/src/MainWindowNavigation.cpp").read_text(
    encoding="utf-8"
)
INTERNAL = (ROOT / "desktop/src/MainWindowInternal.h").read_text(
    encoding="utf-8"
)
MIRROR = (ROOT / "desktop/src/OfficeChannelMirror.cpp").read_text(
    encoding="utf-8"
)
SERVER_NODE_H = (ROOT / "desktop/src/ServerNode.h").read_text(encoding="utf-8")


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


def test_only_the_world_room_is_installed_on_the_relay():
    assert 'm_backend->addChannel(QStringLiteral("general"));' in SETUP
    assert "m_backend->addChannel(kWelcomeChannel);" not in SETUP
    assert 'm_backend->addChannel(QStringLiteral("random"));' not in SETUP
    assert 'QStringList m_channels{"#general"};' in SERVER_NODE_H
    assert 'kWelcomeChannel = QStringLiteral("#office/welcome");' in INTERNAL


def test_everyone_channels_lead_the_sidebar_and_relay_reads_world():
    function = MESSAGES.split(
        "void MainWindow::setChannels(const QStringList &channels)", 1
    )[1].split("\n}\n", 1)[0]

    everyone = function.index(
        "for (const QString &conversation : everyone)")
    relay = function.index("for (const QString &channel : incomingChannels)")
    assert everyone < relay
    assert "isRetiredRelayChannel(channel)" in function
    assert "switchConversation(everyoneGeneral);" in function
    assert "return forkmesh::office::worldLabel();" in NAVIGATION
    assert 'return QStringLiteral("World");' in MIRROR
