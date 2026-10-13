#include "LocalControlDiscord.h"
#include "LocalControlHosts.h"

#include <QJsonArray>
#include <QJsonObject>

#include <iostream>

using namespace forkmesh::localcontrol::discord;
namespace hostctl = forkmesh::localcontrol::hosts;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

static void testIds()
{
    check(familyPrefix() == QStringLiteral("discord."), "prefix");
    check(validChannelId(QStringLiteral("123456789012345678")), "snowflake accepted");
    check(!validChannelId(QString()), "empty refused");
    check(!validChannelId(QStringLiteral("12a")), "letters refused");
    check(!validChannelId(QString(25, QLatin1Char('1'))), "overlong refused");
    check(validReplyId(QString()), "no reply is fine");
    check(validReplyId(QStringLiteral("discord-message:42")), "prefixed reply accepted");
    check(!validReplyId(QStringLiteral("discord-message:x")), "bad reply refused");
    check(conversationForChannelId(QStringLiteral("42")) == QStringLiteral("#discord/42"), "conversation");
    check(conversationForChannelId(QStringLiteral("../x")).isEmpty(), "no path games");
}

static void testLimits()
{
    check(boundedLimit(0) == kDefaultMessages, "default limit");
    check(boundedLimit(10) == 10, "small limit kept");
    check(boundedLimit(100000) == kMaxMessages, "limit capped");
    check(normalizedSendText(QStringLiteral("  hi  ")) == QStringLiteral("hi"), "trimmed");
    check(normalizedSendText(QStringLiteral("   ")).isEmpty(), "blank refused");
    check(normalizedSendText(QString(kMaxSendChars + 1, QLatin1Char('x'))).isEmpty(), "discord limit");
}

static void testTokensBindTheText()
{
    const QString a = sendTarget(QStringLiteral("1"), QStringLiteral("hello"), QString());
    const QString b = sendTarget(QStringLiteral("1"), QStringLiteral("hello!"), QString());
    const QString c = sendTarget(QStringLiteral("1"), QStringLiteral("hello"), QStringLiteral("9"));
    check(a != b, "different text, different target");
    check(a != c, "reply changes the target");
    check(a.startsWith(QStringLiteral("1:")), "target names the channel");
    hostctl::ConfirmationStore store;
    const QString token = store.issue(QStringLiteral("dev"), QStringLiteral("send"), a, 1000);
    check(store.consume(token, QStringLiteral("dev"), QStringLiteral("send"), b, 2000) ==
              hostctl::ConfirmationStore::Result::Mismatch,
          "a token for other text is refused");
    const QString again = store.issue(QStringLiteral("dev"), QStringLiteral("send"), a, 1000);
    check(store.consume(again, QStringLiteral("dev"), QStringLiteral("send"), a, 2000) ==
              hostctl::ConfirmationStore::Result::Ok,
          "the matching token is spent");
    check(store.consume(again, QStringLiteral("dev"), QStringLiteral("send"), a, 2000) ==
              hostctl::ConfirmationStore::Result::Missing,
          "a spent token cannot be reused");
    check(sendSummary(QStringLiteral("Discord · Srv / #general"), QStringLiteral("hi"), false) ==
              QStringLiteral("Send to Discord · Srv / #general: “hi”"),
          "summary");
    check(sendSummary(QString(), QString(100, QLatin1Char('y')), true).endsWith(QChar(0x2026) + QStringLiteral("”")),
          "long previews are cut");
}

static void testRows()
{
    ChannelFacts channel;
    channel.id = QStringLiteral("7");
    channel.display = QStringLiteral("Discord · Srv / #general");
    channel.group = QStringLiteral("Discord · Srv");
    channel.shortName = QStringLiteral("#general");
    channel.topic = QStringLiteral("  a\n topic ");
    channel.guild = true;
    channel.canSend = true;
    const QJsonObject row = channelRow(channel);
    check(row.value(QStringLiteral("name")).toString() == QStringLiteral("#general"), "short name");
    check(row.value(QStringLiteral("topic")).toString() == QStringLiteral("a topic"), "topic simplified");
    check(row.value(QStringLiteral("guild")).toBool(), "guild flag");

    MessageFacts message;
    message.id = QStringLiteral("discord-message:5");
    message.senderName = QStringLiteral("Pat");
    message.text = QString(kMaxRowTextChars + 10, QLatin1Char('z'));
    message.timestampMs = 1234;
    message.reactions.insert(QStringLiteral("like"), {QStringLiteral("a"), QStringLiteral("b")});
    message.reactions.insert(QStringLiteral("empty"), {});
    const QJsonObject out = messageRow(message);
    check(out.value(QStringLiteral("text")).toString().size() == kMaxRowTextChars, "text bounded");
    check(out.value(QStringLiteral("truncated")).toBool(), "truncation flagged");
    check(out.value(QStringLiteral("reactions")).toObject().value(QStringLiteral("like")).toInt() == 2,
          "reaction counts");
    check(!out.value(QStringLiteral("reactions")).toObject().contains(QStringLiteral("empty")),
          "empty reactions dropped");
    check(!out.contains(QStringLiteral("fileName")), "no file");
}

int main()
{
    testIds();
    testLimits();
    testTokensBindTheText();
    testRows();
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "local control discord tests passed\n";
    return 0;
}
