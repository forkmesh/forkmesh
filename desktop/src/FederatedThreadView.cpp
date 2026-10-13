#include "FederatedThreadView.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QTimer>
#include <QUrlQuery>
#include <QVBoxLayout>

namespace {

QString safeHttpsUrl(const QJsonObject &item)
{
    QString value = item.value(QStringLiteral("url")).toString();
    if (value.isEmpty())
        value = item.value(QStringLiteral("backlink")).toString();
    if (value.isEmpty())
        value = item.value(QStringLiteral("remoteId")).toString();
    const QUrl url(value);
    return url.scheme() == QLatin1String("https") && !url.host().isEmpty() &&
                   url.userInfo().isEmpty()
               ? url.toString()
               : QString();
}

QString lifecycleFor(const QJsonObject &item)
{
    const QString explicitState =
        item.value(QStringLiteral("lifecycle")).toString();
    if (!explicitState.isEmpty())
        return explicitState;
    if (item.value(QStringLiteral("tombstone")).toBool())
        return QStringLiteral("tombstoned");
    if (item.value(QStringLiteral("moderated")).toBool())
        return QStringLiteral("moderated");
    if (item.value(QStringLiteral("edited")).toBool())
        return QStringLiteral("edited");
    return QStringLiteral("active");
}

} // namespace

FederatedThreadView::FederatedThreadView(QNetworkAccessManager *network,
                                         QWidget *parent)
    : QWidget(parent), m_network(network)
{
    setObjectName(QStringLiteral("federatedThreadView"));
    // Replies hidden or deleted remotely read in italics; one rule here instead
    // of a sheet per reply each time the thread renders.
    setStyleSheet(QStringLiteral(
        "QLabel#federatedReplyUnavailable { font-style: italic; }"));
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // Its own card in the thread, with the same header bar the native comments
    // use, rather than loose text wedged between the timeline and the composer.
    auto *card = new QFrame(this);
    card->setObjectName(QStringLiteral("issueTimelineCard"));
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(0);
    outer->addWidget(card);

    auto *headerBox = new QWidget(card);
    headerBox->setObjectName(QStringLiteral("issueTimelineHeader"));
    auto *headerRow = new QHBoxLayout(headerBox);
    headerRow->setContentsMargins(16, 8, 10, 8);
    headerRow->setSpacing(8);
    m_heading = new QLabel(QStringLiteral("<b>Fediverse thread</b>"), headerBox);
    m_heading->setTextFormat(Qt::RichText);
    headerRow->addWidget(m_heading);
    headerRow->addStretch();

    m_threadLink = new QLabel(headerBox);
    m_threadLink->setTextFormat(Qt::RichText);
    m_threadLink->setOpenExternalLinks(false);
    m_threadLink->setTextInteractionFlags(Qt::TextBrowserInteraction);
    m_threadLink->hide();
    connect(m_threadLink, &QLabel::linkActivated, this,
            [](const QString &href) { openHttps(QUrl(href)); });
    headerRow->addWidget(m_threadLink);

    m_copyHandle = new QPushButton(headerBox);
    m_copyHandle->setObjectName(QStringLiteral("ghostButton"));
    m_copyHandle->setProperty("buttonSize", "sm");
    m_copyHandle->setCursor(Qt::PointingHandCursor);
    m_copyHandle->hide();
    connect(m_copyHandle, &QPushButton::clicked, this, [this] {
        if (m_handle.isEmpty())
            return;
        QApplication::clipboard()->setText(m_handle);
        m_copyHandle->setText(QStringLiteral("Copied"));
        QTimer::singleShot(1500, m_copyHandle, [this] {
            if (!m_handle.isEmpty())
                m_copyHandle->setText(QStringLiteral("Copy %1").arg(m_handle));
        });
    });
    headerRow->addWidget(m_copyHandle);
    cardLayout->addWidget(headerBox);

    auto *body = new QWidget(card);
    auto *bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(16, 10, 12, 12);
    bodyLayout->setSpacing(8);

    auto *notice = new QLabel(
        QStringLiteral("Remote ActivityPub replies retain their instance and "
                       "original backlink. They are not signed ForkMesh native "
                       "events and are never merged into the repository event log. "
                       "Follow or reply from your own instance to join in."),
        body);
    notice->setObjectName(QStringLiteral("statusLine"));
    notice->setWordWrap(true);
    notice->setTextFormat(Qt::PlainText);
    bodyLayout->addWidget(notice);

    m_replies = new QVBoxLayout;
    m_replies->setContentsMargins(0, 0, 0, 0);
    m_replies->setSpacing(8);
    bodyLayout->addLayout(m_replies);
    cardLayout->addWidget(body);
    showMessage(QStringLiteral("No remote ActivityPub replies loaded."));
}

void FederatedThreadView::setSummary(const QString &summary)
{
    if (!m_heading)
        return;
    m_heading->setText(
        summary.isEmpty()
            ? QStringLiteral("<b>Fediverse thread</b>")
            : QStringLiteral("<b>Fediverse thread</b> "
                             "<span style='color:#8b949e'>%1</span>")
                  .arg(summary.toHtmlEscaped()));
}

void FederatedThreadView::openHttps(const QUrl &url)
{
    if (url.scheme() == QLatin1String("https") && !url.host().isEmpty() &&
        url.userInfo().isEmpty())
        QDesktopServices::openUrl(url);
}

void FederatedThreadView::clearReplies()
{
    while (QLayoutItem *item = m_replies->takeAt(0)) {
        if (QWidget *widget = item->widget())
            widget->deleteLater();
        delete item;
    }
}

void FederatedThreadView::showMessage(const QString &message)
{
    setSummary(QString());
    clearReplies();
    auto *label = new QLabel(message, this);
    label->setObjectName(QStringLiteral("statusLine"));
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    m_replies->addWidget(label);
}

void FederatedThreadView::load(const QUrl &server, const QString &owner,
                               const QString &repo, const QString &kind,
                               int number)
{
    ++m_generation;
    if (m_reply)
        m_reply->abort();
    if (!m_network || !server.isValid() || server.host().isEmpty() ||
        owner.isEmpty() || repo.isEmpty() || number <= 0) {
        m_threadUrl.clear();
        m_handle.clear();
        m_threadLink->hide();
        m_copyHandle->hide();
        showMessage(QStringLiteral("Remote replies are unavailable."));
        return;
    }

    // The thread's public page is what remote instances link back to, and the
    // repository's actor handle is what a reader follows or replies to.
    m_threadUrl = server;
    m_threadUrl.setScheme(server.scheme() == QLatin1String("http")
                              ? QStringLiteral("http")
                              : QStringLiteral("https"));
    m_threadUrl.setPath(QStringLiteral("/%1/%2/%3s")
                            .arg(owner, repo, kind));
    m_threadUrl.setQuery(QString());
    m_handle = QStringLiteral("@%1.%2@%3").arg(owner, repo, server.host());
    m_threadLink->setText(
        QStringLiteral("<a href='%1'>Open thread</a>")
            .arg(m_threadUrl.toString().toHtmlEscaped()));
    m_threadLink->setToolTip(
        QStringLiteral("Open this thread's public page (%1)")
            .arg(m_threadUrl.toString()));
    m_threadLink->show();
    m_copyHandle->setText(QStringLiteral("Copy %1").arg(m_handle));
    m_copyHandle->setToolTip(
        QStringLiteral("Copy the repository's fediverse handle so you can "
                       "follow or reply to this thread from your instance"));
    m_copyHandle->show();

    QUrl url(server);
    url.setScheme(url.scheme() == QLatin1String("http")
                      ? QStringLiteral("http")
                      : QStringLiteral("https"));
    url.setPath(QStringLiteral("/api/repo/%1/%2/fedi-comments")
                    .arg(QString::fromUtf8(QUrl::toPercentEncoding(owner)),
                         QString::fromUtf8(QUrl::toPercentEncoding(repo))));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("kind"), kind);
    query.addQueryItem(QStringLiteral("number"), QString::number(number));
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setRawHeader("Accept", "application/json");
    showMessage(QStringLiteral("Loading remote ActivityPub replies…"));
    const quint64 generation = m_generation;
    QNetworkReply *reply = m_network->get(request);
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, generation] {
                const QByteArray payload = reply->readAll();
                const bool ok = reply->error() == QNetworkReply::NoError;
                reply->deleteLater();
                if (m_reply == reply)
                    m_reply.clear();
                if (generation != m_generation)
                    return;
                if (!ok) {
                    showMessage(
                        QStringLiteral("Remote replies are currently unavailable."));
                    return;
                }
                render(payload);
            });
}

void FederatedThreadView::render(const QByteArray &payload)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        showMessage(QStringLiteral("Remote reply data was invalid."));
        return;
    }
    const QJsonObject root = document.object();
    QJsonArray items = root.value(QStringLiteral("comments")).toArray();
    if (items.isEmpty())
        items = root.value(QStringLiteral("items")).toArray();
    if (items.isEmpty()) {
        showMessage(QStringLiteral("No remote ActivityPub replies yet."));
        return;
    }

    setSummary(QStringLiteral("%1 remote %2")
                   .arg(items.size())
                   .arg(items.size() == 1 ? "reply" : "replies"));
    clearReplies();
    for (const QJsonValue &value : items) {
        if (!value.isObject())
            continue;
        const QJsonObject item = value.toObject();
        const QJsonObject provenance =
            item.value(QStringLiteral("provenance")).toObject();
        const QString lifecycle = lifecycleFor(item);
        const bool unavailable =
            lifecycle == QLatin1String("tombstoned") ||
            lifecycle == QLatin1String("moderated") ||
            lifecycle == QLatin1String("awaiting-redelivery");
        QString author = item.value(QStringLiteral("authorName")).toString();
        if (author.isEmpty())
            author = item.value(QStringLiteral("author")).toString();
        if (author.isEmpty())
            author = QStringLiteral("Remote participant");
        QString instance =
            item.value(QStringLiteral("sourceInstance")).toString();
        if (instance.isEmpty())
            instance = provenance.value(QStringLiteral("instance")).toString();
        QString software =
            item.value(QStringLiteral("sourceSoftware")).toString();
        if (software.isEmpty())
            software = provenance.value(QStringLiteral("software")).toString(
                QStringLiteral("ActivityPub"));
        QString body = item.value(QStringLiteral("body")).toString();
        if (lifecycle == QLatin1String("tombstoned"))
            body = QStringLiteral("Deleted on the remote instance");
        else if (unavailable)
            body = QStringLiteral("Hidden by remote moderation");

        auto *card = new QFrame(this);
        card->setObjectName(QStringLiteral("issueTimelineCard"));
        const int depth =
            qBound(0, item.value(QStringLiteral("depth")).toInt(), 8);
        card->setContentsMargins(12 + depth * 16, 10, 12, 10);
        auto *layout = new QVBoxLayout(card);
        layout->setContentsMargins(12 + depth * 16, 10, 12, 10);
        layout->setSpacing(6);

        auto *header = new QLabel(card);
        header->setTextFormat(Qt::RichText);
        header->setWordWrap(true);
        header->setText(
            QStringLiteral("<b>%1</b>%2 <span style='color:#8b949e'>%3%4</span>")
                .arg(author.toHtmlEscaped(),
                     lifecycle == QLatin1String("edited")
                         ? QStringLiteral(" <span style='color:#8b949e'>(edited)</span>")
                         : QString(),
                     software.toHtmlEscaped(),
                     instance.isEmpty()
                         ? QString()
                         : QStringLiteral(" · %1").arg(instance.toHtmlEscaped())));
        layout->addWidget(header);

        auto *content = new QLabel(body, card);
        content->setTextFormat(Qt::PlainText);
        content->setWordWrap(true);
        if (unavailable)
            content->setObjectName(QStringLiteral("federatedReplyUnavailable"));
        layout->addWidget(content);

        auto *source = new QLabel(card);
        source->setTextFormat(Qt::RichText);
        source->setWordWrap(true);
        source->setOpenExternalLinks(false);
        const QString backlink = safeHttpsUrl(item);
        source->setText(
            QStringLiteral("<span style='color:#8b949e'>Remote ActivityPub · "
                           "not a signed native event</span>%1")
                .arg(backlink.isEmpty()
                         ? QString()
                         : QStringLiteral(
                               " · <a href='%1'>Open original</a>")
                               .arg(backlink.toHtmlEscaped())));
        connect(source, &QLabel::linkActivated, this,
                [](const QString &href) {
                    const QUrl url(href);
                    if (url.scheme() == QLatin1String("https") &&
                        !url.host().isEmpty() && url.userInfo().isEmpty())
                        QDesktopServices::openUrl(url);
                });
        layout->addWidget(source);
        m_replies->addWidget(card);
    }
    if (m_replies->count() == 0)
        showMessage(QStringLiteral("No remote ActivityPub replies yet."));
}
