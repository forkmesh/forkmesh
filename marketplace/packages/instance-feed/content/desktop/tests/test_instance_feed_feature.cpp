#include "InstanceFeedFeature.h"

#include <QApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>

using forkmesh::instancefeed::InstanceFeedFeature;

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        qInfo("PASS: %s", message);
    else {
        qCritical("FAIL: %s", message);
        ++failures;
    }
}

QJsonObject post(const QString &id, const QString &author, const QString &text)
{
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("post")},
        {QStringLiteral("id"), id.repeated(32).left(32)},
        {QStringLiteral("author"), author},
        {QStringLiteral("authorName"), author.left(1).toUpper() + author.mid(1)},
        {QStringLiteral("text"), text},
        {QStringLiteral("createdAt"), QDateTime::currentMSecsSinceEpoch() - 120000},
        {QStringLiteral("replies"), 2},
        {QStringLiteral("favourites"), 1},
        {QStringLiteral("favourited"), true},
        {QStringLiteral("canDelete"), author == QLatin1String("alice")},
    };
}

QPushButton *buttonStartingWith(QWidget &root, const QString &prefix)
{
    for (QPushButton *button : root.findChildren<QPushButton *>()) {
        if (button->isVisibleTo(&root) && button->text().startsWith(prefix))
            return button;
    }
    return nullptr;
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    check(InstanceFeedFeature::relativeLabel(0, 1000).isEmpty(), "no timestamp shows no label");
    check(InstanceFeedFeature::relativeLabel(1000, 30000) == QLatin1String("now"), "under a minute is now");
    check(InstanceFeedFeature::relativeLabel(0 + 1, 5 * 60000 + 1) == QLatin1String("5m"), "minutes");
    check(InstanceFeedFeature::relativeLabel(1, 3 * 3600000 + 1) == QLatin1String("3h"), "hours");
    check(InstanceFeedFeature::relativeLabel(1, 2 * 86400000LL + 1) == QLatin1String("2d"), "days");
    check(InstanceFeedFeature::validPostId(QString(32, QLatin1Char('a'))), "hex ids are valid");
    check(!InstanceFeedFeature::validPostId(QStringLiteral("../x")), "other ids are refused");
    check(InstanceFeedFeature::validAuthor(QStringLiteral("ada-l")) &&
              !InstanceFeedFeature::validAuthor(QStringLiteral("Ada")),
          "authors are lowercase account names");
    check(InstanceFeedFeature::timelinePath(InstanceFeedFeature::View::Timeline, QString(), QString()) ==
              QLatin1String("/api/instance-feed"),
          "timeline path");
    check(InstanceFeedFeature::timelinePath(InstanceFeedFeature::View::Profile, QStringLiteral("ada"),
                                            QStringLiteral("5-%1").arg(QString(32, QLatin1Char('b')))) ==
              QStringLiteral("/api/instance-feed?author=ada&before=5-%1").arg(QString(32, QLatin1Char('b'))),
          "profile path carries author and cursor");

    const QString html = InstanceFeedFeature::richText(
        QStringLiteral("hi @Ada <b>\nsee https://forkmesh.com/x. mail me@bob.dev"), QString());
    check(html.contains(QStringLiteral("href=\"fm-author:ada\"")), "mentions link to the profile");
    check(html.contains(QStringLiteral("href=\"https://forkmesh.com/x\"")), "https links drop trailing dots");
    check(html.contains(QStringLiteral("&lt;b&gt;")) && html.contains(QStringLiteral("<br>")),
          "text is escaped and keeps line breaks");
    check(!html.contains(QStringLiteral("fm-author:bob")), "email addresses are not mentions");

    QList<QPair<QByteArray, QString>> calls;
    QJsonObject lastBody;
    bool installed = true;
    forkmesh::market::NativePackageHost host;
    host.packageId = QStringLiteral("instance-feed");
    host.request = [&](const QByteArray &method, const QString &path, const QJsonObject &body,
                       forkmesh::market::NativePackageReplyHandler done) {
        calls.append({method, path});
        lastBody = body;
        if (!installed)
            return done(true, QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("installed"), false}}, {});
        if (method == "POST" && path == QLatin1String("/api/instance-feed/posts"))
            return done(true, QJsonObject{{QStringLiteral("ok"), true},
                                          {QStringLiteral("post"), post(QStringLiteral("c"), QStringLiteral("alice"), body.value(QStringLiteral("text")).toString())}}, {});
        if (path.startsWith(QLatin1String("/api/instance-feed/posts/")))
            return done(true, QJsonObject{{QStringLiteral("ok"), true},
                                          {QStringLiteral("post"), post(QStringLiteral("a"), QStringLiteral("ada"), QStringLiteral("root"))},
                                          {QStringLiteral("ancestors"), QJsonArray()},
                                          {QStringLiteral("replies"), QJsonArray{post(QStringLiteral("d"), QStringLiteral("alice"), QStringLiteral("reply"))}}}, {});
        QJsonObject payload{
            {QStringLiteral("ok"), true}, {QStringLiteral("installed"), true},
            {QStringLiteral("maxLength"), 500}, {QStringLiteral("next"), QString()},
            {QStringLiteral("posts"), QJsonArray{
                post(QStringLiteral("a"), QStringLiteral("ada"), QStringLiteral("Hello @lin")),
                QJsonObject{{QStringLiteral("type"), QStringLiteral("boost")},
                            {QStringLiteral("id"), QString(32, QLatin1Char('f'))},
                            {QStringLiteral("booster"), QStringLiteral("grace")},
                            {QStringLiteral("boosterName"), QStringLiteral("Grace")},
                            {QStringLiteral("post"), post(QStringLiteral("b"), QStringLiteral("lin"), QStringLiteral("Office hours"))}},
            }},
        };
        if (path.contains(QLatin1String("author=")))
            payload.insert(QStringLiteral("profile"), QJsonObject{{QStringLiteral("name"), QStringLiteral("ada")},
                                                                  {QStringLiteral("displayName"), QStringLiteral("Ada")},
                                                                  {QStringLiteral("posts"), 3}});
        done(true, payload, {});
    };

    InstanceFeedFeature page(host);
    page.resize(900, 700);
    page.show();
    QApplication::processEvents();
    check(!calls.isEmpty() && calls.first().second == QLatin1String("/api/instance-feed"),
          "the page loads the timeline on open");
    check(page.findChildren<QWidget *>(QStringLiteral("instanceFeedPost")).size() == 2,
          "a post and a boost render as two rows");
    bool boostBanner = false;
    for (QLabel *label : page.findChildren<QLabel *>())
        boostBanner = boostBanner || label->text().contains(QStringLiteral("Grace</a> boosted"));
    check(boostBanner, "boosts name the booster");
    check(buttonStartingWith(page, QStringLiteral("★ Favourited")) != nullptr, "favourited posts show it");

    auto *editor = page.findChild<QPlainTextEdit *>();
    QPushButton *postButton = buttonStartingWith(page, QStringLiteral("Post"));
    check(editor && postButton && !postButton->isEnabled(), "Post is disabled while the box is empty");
    editor->setPlainText(QStringLiteral("Hello instance"));
    check(postButton->isEnabled(), "Post enables with text");
    postButton->click();
    QApplication::processEvents();
    check(calls.last().first == "POST" && lastBody.value(QStringLiteral("text")).toString() == QLatin1String("Hello instance"),
          "posting sends the text");
    check(editor->toPlainText().isEmpty(), "the box clears after posting");
    check(page.findChildren<QWidget *>(QStringLiteral("instanceFeedPost")).size() == 3,
          "the new post appears at the top of the timeline");

    page.showProfile(QStringLiteral("ada"));
    QApplication::processEvents();
    check(calls.last().second == QLatin1String("/api/instance-feed?author=ada"), "profiles load by author");
    check(page.view() == InstanceFeedFeature::View::Profile, "profile view is shown");

    check(page.openLinkTarget(QStringLiteral("post-") + QString(32, QLatin1Char('a'))), "post deep links open");
    QApplication::processEvents();
    check(calls.last().second == QStringLiteral("/api/instance-feed/posts/") + QString(32, QLatin1Char('a')),
          "threads load by post id");
    check(!page.openLinkTarget(QStringLiteral("post-nope")), "bad deep links are refused");

    installed = false;
    page.showTimeline();
    QApplication::processEvents();
    bool notice = false;
    for (QLabel *label : page.findChildren<QLabel *>())
        notice = notice || (label->isVisibleTo(&page) && label->text().contains(QStringLiteral("Install it from the Market")));
    check(notice, "an account without the package is pointed at the Market");

    return failures == 0 ? 0 : 1;
}
