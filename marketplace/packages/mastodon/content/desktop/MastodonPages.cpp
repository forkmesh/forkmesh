#include "MastodonFeature.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace forkmesh::mastodon {

namespace {

QColor notificationColor(const QString &type)
{
    if (type == QLatin1String("favourite"))
        return favouriteColor();
    if (type == QLatin1String("reblog"))
        return boostColor();
    return {};
}

QString notificationIcon(const QString &type)
{
    if (type == QLatin1String("favourite"))
        return QStringLiteral("star-fill");
    if (type == QLatin1String("reblog"))
        return QStringLiteral("boost");
    if (type == QLatin1String("follow") || type == QLatin1String("follow_request"))
        return QStringLiteral("person");
    if (type == QLatin1String("mention"))
        return QStringLiteral("at");
    if (type == QLatin1String("poll"))
        return QStringLiteral("graph");
    if (type == QLatin1String("update"))
        return QStringLiteral("pencil");
    if (type == QLatin1String("status"))
        return QStringLiteral("bell");
    return QStringLiteral("alert");
}

QString weekUses(const QJsonObject &tag, qint64 *accounts)
{
    qint64 uses = 0;
    qint64 people = 0;
    for (const QJsonValue &value : tag.value(QStringLiteral("history")).toArray()) {
        uses += value.toObject().value(QStringLiteral("uses")).toVariant().toLongLong();
        people += value.toObject().value(QStringLiteral("accounts")).toVariant().toLongLong();
    }
    if (accounts)
        *accounts = people;
    return countText(uses);
}

QString longDate(const QString &iso)
{
    const QDateTime when = QDateTime::fromString(iso, Qt::ISODateWithMs);
    return when.isValid() ? QLocale().toString(when.toLocalTime(), QLocale::LongFormat) : QString();
}

QString encoded(const QString &text)
{
    return QString::fromUtf8(QUrl::toPercentEncoding(text));
}

}

QFrame *MastodonFeature::panelRow()
{
    auto *frame = new QFrame;
    frame->setObjectName(QStringLiteral("mastodonPanel"));
    QColor separator = palette().color(QPalette::WindowText);
    separator.setAlphaF(0.12);
    frame->setStyleSheet(QStringLiteral("QFrame#mastodonPanel { background: transparent; border: none; border-bottom: 1px solid rgba(%1,%2,%3,%4); }")
                             .arg(separator.red())
                             .arg(separator.green())
                             .arg(separator.blue())
                             .arg(QString::number(separator.alphaF(), 'f', 2)));
    return frame;
}

QPushButton *MastodonFeature::textButton(const QString &text, const QString &style)
{
    auto *button = new QPushButton(text);
    button->setObjectName(style);
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

QWidget *MastodonFeature::tagRow(const QJsonObject &tag, const QString &mode)
{
    const QString name = tag.value(QStringLiteral("name")).toString();
    QFrame *frame = panelRow();
    auto *row = new QHBoxLayout(frame);
    row->setContentsMargins(16, 10, 16, 10);
    row->setSpacing(10);
    auto *mark = new QLabel;
    mark->setPixmap(icon(QStringLiteral("hash"), accentColor(this), 20).pixmap(20, 20));
    mark->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
    row->addWidget(mark);
    auto *texts = new QVBoxLayout;
    texts->setSpacing(1);
    auto *title = richLabel(QStringLiteral("<a href=\"%1:%2\" style=\"text-decoration:none;\"><b>#%3</b></a>")
                                .arg(kTagScheme, name.toHtmlEscaped(), name.toHtmlEscaped()));
    title->setStyleSheet(QStringLiteral("font-size: 14px; background: transparent; border: none;"));
    connect(title, &QLabel::linkActivated, this, [this](const QString &link) { openLink(link); });
    texts->addWidget(title);
    qint64 people = 0;
    const QString uses = weekUses(tag, &people);
    if (!tag.value(QStringLiteral("history")).toArray().isEmpty()) {
        auto *facts = mutedLabel(QStringLiteral("%1 posts by %2 people this week").arg(uses, countText(people)), frame);
        facts->setStyleSheet(facts->styleSheet() + QStringLiteral(" border: none;"));
        texts->addWidget(facts);
    }
    row->addLayout(texts, 1);
    const bool following = tag.value(QStringLiteral("following")).toBool();
    if (mode == QLatin1String("follow") && tag.contains(QStringLiteral("following"))) {
        auto *button = textButton(following ? QStringLiteral("Unfollow") : QStringLiteral("Follow"),
                                  following ? QStringLiteral("ghostButton") : QStringLiteral("primaryButton"));
        connect(button, &QPushButton::clicked, this, [this, name, following, button] {
            followTag(name, !following);
            button->setEnabled(false);
        });
        row->addWidget(button);
    } else if (mode == QLatin1String("unfollow")) {
        auto *button = textButton(QStringLiteral("Unfollow"));
        connect(button, &QPushButton::clicked, this, [this, name] { followTag(name, false); });
        row->addWidget(button);
    } else if (mode.startsWith(QLatin1String("unfeature:"))) {
        auto *button = textButton(QStringLiteral("Remove from profile"));
        const QString id = mode.mid(10);
        connect(button, &QPushButton::clicked, this, [this, name, id] { featureTag(name, id); });
        row->addWidget(button);
    } else if (mode == QLatin1String("feature")) {
        auto *button = textButton(QStringLiteral("Feature on profile"));
        connect(button, &QPushButton::clicked, this, [this, name] { featureTag(name, QString()); });
        row->addWidget(button);
    }
    auto *open = textButton(QStringLiteral("Open"));
    connect(open, &QPushButton::clicked, this, [this, name] { openPage({QStringLiteral("tag"), name, QString()}); });
    row->addWidget(open);
    return frame;
}

void MastodonFeature::showHome()
{
    beginPage(QStringLiteral("Home"), QStringLiteral("Posts from people and hashtags you follow"));
    m_cardContext->filterContext = QStringLiteral("home");
    auto *prompt = new QPushButton(QStringLiteral("What's on your mind?"));
    prompt->setObjectName(QStringLiteral("mastodonComposePrompt"));
    prompt->setCursor(Qt::IBeamCursor);
    QColor border = palette().color(QPalette::WindowText);
    border.setAlphaF(0.16);
    prompt->setStyleSheet(QStringLiteral("QPushButton#mastodonComposePrompt { text-align: left; padding: 12px 16px; margin: 4px 12px 10px 12px;"
                                         " border: 1px solid rgba(%1,%2,%3,0.16); border-radius: 10px; color: %4; background: transparent; }")
                              .arg(border.red())
                              .arg(border.green())
                              .arg(border.blue())
                              .arg(mutedColor(this).name()));
    connect(prompt, &QPushButton::clicked, this, [this] { compose(); });
    addRow(prompt);
    loadStatuses(QStringLiteral("/api/v1/timelines/home"), QUrlQuery(QStringLiteral("limit=30")), QStringLiteral("home"),
                 QStringLiteral("Your home timeline is empty. Follow people or hashtags to fill it — Explore is a good start."));
}

void MastodonFeature::showNotifications()
{
    beginPage(QStringLiteral("Notifications"));
    m_cardContext->filterContext = QStringLiteral("notifications");
    setTabs({{QString(), QStringLiteral("All")},
             {QStringLiteral("mention"), QStringLiteral("Mentions")},
             {QStringLiteral("follow"), QStringLiteral("Follows")},
             {QStringLiteral("favourite"), QStringLiteral("Favourites")},
             {QStringLiteral("reblog"), QStringLiteral("Boosts")},
             {QStringLiteral("poll"), QStringLiteral("Polls & edits")}});
    auto *clear = textButton(QStringLiteral("Clear all"));
    connect(clear, &QPushButton::clicked, this, [this] {
        if (confirm(QStringLiteral("Clear notifications"), QStringLiteral("Remove all your notifications? This cannot be undone.")))
            clearNotifications();
    });
    addHeaderAction(clear);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("limit"), QStringLiteral("30"));
    const QString tab = m_page.tab;
    if (tab == QLatin1String("follow")) {
        query.addQueryItem(QStringLiteral("types[]"), QStringLiteral("follow"));
        query.addQueryItem(QStringLiteral("types[]"), QStringLiteral("follow_request"));
    } else if (tab == QLatin1String("poll")) {
        query.addQueryItem(QStringLiteral("types[]"), QStringLiteral("poll"));
        query.addQueryItem(QStringLiteral("types[]"), QStringLiteral("update"));
    } else if (!tab.isEmpty()) {
        query.addQueryItem(QStringLiteral("types[]"), tab);
    }
    const quint64 generation = m_generation;
    setLoading(true);
    m_client->get(QStringLiteral("/api/v1/notifications"), query, [this, generation](const Result &result) {
        if (current(generation))
            showNotificationsResult(result, generation, false);
    });
}

void MastodonFeature::showNotificationsResult(const Result &result, quint64 generation, bool append)
{
    setLoading(false);
    if (!result.ok) {
        setNotice(result.error, true);
        return;
    }
    const QJsonArray notifications = result.array();
    if (notifications.isEmpty() && !append) {
        setEmpty(QStringLiteral("No notifications here yet."));
        return;
    }
    if (!append && m_page.tab.isEmpty())
        markNotificationsRead(notifications.first().toObject().value(QStringLiteral("id")).toString());
    for (const QJsonValue &value : notifications)
        addRow(notificationRow(value.toObject()));
    if (result.next.isValid() && !notifications.isEmpty()) {
        const QUrl next = result.next;
        setLoadMore([this, next] {
            const quint64 page = m_generation;
            setLoading(true);
            m_client->getUrl(next, [this, page](const Result &more) {
                if (current(page))
                    showNotificationsResult(more, page, true);
            });
        });
    }
    Q_UNUSED(generation);
}

QWidget *MastodonFeature::notificationRow(const QJsonObject &notification)
{
    const QString type = notification.value(QStringLiteral("type")).toString();
    const QJsonObject account = notification.value(QStringLiteral("account")).toObject();
    const QJsonObject status = notification.value(QStringLiteral("status")).toObject();
    const QString id = notification.value(QStringLiteral("id")).toString();
    if (type == QLatin1String("mention") && !status.isEmpty())
        return new PostCard(status, cardContext());

    QFrame *frame = panelRow();
    frame->setObjectName(QStringLiteral("mastodonNotification"));
    frame->setStyleSheet(frame->styleSheet().replace(QStringLiteral("mastodonPanel"), QStringLiteral("mastodonNotification")));
    auto *layout = new QVBoxLayout(frame);
    layout->setContentsMargins(16, 10, 16, 10);
    layout->setSpacing(6);
    auto *header = new QHBoxLayout;
    header->setSpacing(8);
    auto *mark = new QLabel;
    QColor color = notificationColor(type);
    if (!color.isValid())
        color = accentColor(this);
    mark->setPixmap(icon(notificationIcon(type), color, 16).pixmap(16, 16));
    mark->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
    header->addWidget(mark);
    auto *avatar = new AvatarLabel(28);
    avatar->setAccount(m_client.get(), account);
    avatar->setCursor(Qt::PointingHandCursor);
    avatar->clicked = [this, account] { openAccountObject(account); };
    header->addWidget(avatar);
    QString sentence = notificationSentence(type);
    QString text;
    if (account.isEmpty() || type == QLatin1String("poll") || type == QLatin1String("severed_relationships") ||
        type == QLatin1String("moderation_warning") || type == QLatin1String("annual_report"))
        text = escapeText(sentence.left(1).toUpper() + sentence.mid(1));
    else
        text = QStringLiteral("<a href=\"%1:%2\" style=\"text-decoration:none;\"><b>%3</b></a> %4")
                   .arg(kAccountScheme, account.value(QStringLiteral("id")).toString(), escapeText(displayName(account)),
                        escapeText(sentence));
    auto *label = richLabel(text);
    label->setStyleSheet(QStringLiteral("font-size: 13px; background: transparent; border: none;"));
    connect(label, &QLabel::linkActivated, this, [this](const QString &link) { openLink(link); });
    header->addWidget(label, 1);
    auto *when = mutedLabel(relativeTime(notification.value(QStringLiteral("created_at")).toString(), QDateTime::currentDateTimeUtc()), frame);
    when->setStyleSheet(when->styleSheet() + QStringLiteral(" border: none;"));
    header->addWidget(when);
    auto *dismiss = iconButton(QStringLiteral("x"), QStringLiteral("Dismiss"), frame);
    QPointer<QFrame> guard(frame);
    connect(dismiss, &QToolButton::clicked, this, [this, id, guard] {
        dismissNotification(id);
        if (guard)
            guard->hide();
    });
    header->addWidget(dismiss);
    layout->addLayout(header);

    if (type == QLatin1String("follow")) {
        auto *row = new QHBoxLayout;
        row->setContentsMargins(60, 0, 0, 0);
        row->addWidget(mutedLabel(fullHandle(account, m_client->instance()), frame), 1);
        QPointer<QFrame> rowGuard(frame);
        fetchRelationships({account.value(QStringLiteral("id")).toString()},
                           [this, rowGuard, row, account](const QHash<QString, QJsonObject> &relationships) {
                               if (rowGuard)
                                   row->addWidget(followButton(account, relationships.value(account.value(QStringLiteral("id")).toString())));
                           });
        layout->addLayout(row);
    } else if (type == QLatin1String("follow_request")) {
        auto *row = new QHBoxLayout;
        row->setContentsMargins(60, 0, 0, 0);
        row->addWidget(mutedLabel(fullHandle(account, m_client->instance()), frame), 1);
        const QString accountId = account.value(QStringLiteral("id")).toString();
        auto *accept = textButton(QStringLiteral("Accept"), QStringLiteral("primaryButton"));
        connect(accept, &QPushButton::clicked, this, [this, accountId] { answerFollowRequest(accountId, true); });
        row->addWidget(accept);
        auto *reject = textButton(QStringLiteral("Reject"));
        connect(reject, &QPushButton::clicked, this, [this, accountId] { answerFollowRequest(accountId, false); });
        row->addWidget(reject);
        layout->addLayout(row);
    } else if (!status.isEmpty()) {
        if (type == QLatin1String("status") || type == QLatin1String("update") || type == QLatin1String("quote")) {
            auto *card = new PostCard(status, cardContext());
            layout->addWidget(card);
        } else {
            const QString excerpt = elide(plainText(status.value(QStringLiteral("content")).toString()), 220);
            auto *quote = richLabel(QStringLiteral("<a href=\"fm-thread:%1\" style=\"text-decoration:none; color:%2;\">%3</a>")
                                        .arg(status.value(QStringLiteral("id")).toString().toHtmlEscaped(), mutedColor(this).name(),
                                             escapeText(excerpt.isEmpty() ? QStringLiteral("(media)") : excerpt)));
            quote->setStyleSheet(QStringLiteral("font-size: 13px; background: transparent; border: none; padding-left: 60px;"));
            connect(quote, &QLabel::linkActivated, this, [this](const QString &link) { openLink(link); });
            layout->addWidget(quote);
        }
    } else if (type == QLatin1String("severed_relationships") || type == QLatin1String("moderation_warning")) {
        auto *open = textButton(QStringLiteral("Open on the server"));
        connect(open, &QPushButton::clicked, this, [this, type] {
            openOnInstance(type == QLatin1String("moderation_warning") ? QStringLiteral("/disputes/strikes")
                                                                       : QStringLiteral("/severed_relationships"));
        });
        layout->addWidget(open, 0, Qt::AlignLeft);
    }
    return frame;
}

void MastodonFeature::showExplore()
{
    beginPage(QStringLiteral("Explore"), QStringLiteral("What is trending on %1").arg(instanceHost(m_client->instance())));
    setTabs({{QString(), QStringLiteral("Posts")},
             {QStringLiteral("tags"), QStringLiteral("Hashtags")},
             {QStringLiteral("links"), QStringLiteral("News")},
             {QStringLiteral("people"), QStringLiteral("People")}});
    const quint64 generation = m_generation;
    if (m_page.tab.isEmpty()) {
        loadStatuses(QStringLiteral("/api/v1/trends/statuses"), QUrlQuery(QStringLiteral("limit=30")), QStringLiteral("public"),
                     QStringLiteral("Nothing is trending right now."));
    } else if (m_page.tab == QLatin1String("tags")) {
        setLoading(true);
        m_client->get(QStringLiteral("/api/v1/trends/tags"), QUrlQuery(QStringLiteral("limit=20")), [this, generation](const Result &result) {
            if (!current(generation))
                return;
            setLoading(false);
            if (!result.ok) {
                setNotice(result.error, true);
                return;
            }
            if (result.array().isEmpty())
                setEmpty(QStringLiteral("No hashtags are trending right now."));
            for (const QJsonValue &value : result.array())
                addRow(tagRow(value.toObject(), QStringLiteral("follow")));
        });
    } else if (m_page.tab == QLatin1String("links")) {
        setLoading(true);
        m_client->get(QStringLiteral("/api/v1/trends/links"), QUrlQuery(QStringLiteral("limit=20")), [this, generation](const Result &result) {
            if (!current(generation))
                return;
            setLoading(false);
            if (!result.ok) {
                setNotice(result.error, true);
                return;
            }
            if (result.array().isEmpty())
                setEmpty(QStringLiteral("No news is trending right now."));
            for (const QJsonValue &value : result.array()) {
                const QJsonObject link = value.toObject();
                QFrame *frame = panelRow();
                auto *layout = new QVBoxLayout(frame);
                layout->setContentsMargins(16, 12, 16, 12);
                layout->setSpacing(3);
                QString provider = link.value(QStringLiteral("provider_name")).toString();
                if (provider.isEmpty())
                    provider = QUrl(link.value(QStringLiteral("url")).toString()).host();
                auto *source = mutedLabel(provider, frame);
                source->setStyleSheet(source->styleSheet() + QStringLiteral(" border: none;"));
                layout->addWidget(source);
                auto *title = richLabel(QStringLiteral("<a href=\"%1\" style=\"text-decoration:none;\"><b>%2</b></a>")
                                            .arg(link.value(QStringLiteral("url")).toString().toHtmlEscaped(),
                                                 escapeText(link.value(QStringLiteral("title")).toString())));
                title->setStyleSheet(QStringLiteral("font-size: 14px; background: transparent; border: none;"));
                connect(title, &QLabel::linkActivated, this, [this](const QString &url) { openLink(url); });
                layout->addWidget(title);
                const QString description = elide(link.value(QStringLiteral("description")).toString(), 220);
                if (!description.isEmpty()) {
                    auto *summary = new QLabel(description);
                    summary->setWordWrap(true);
                    summary->setStyleSheet(QStringLiteral("font-size: 13px; background: transparent; border: none;"));
                    layout->addWidget(summary);
                }
                qint64 people = 0;
                const QString uses = weekUses(link, &people);
                auto *facts = mutedLabel(QStringLiteral("Shared %1 times by %2 people this week").arg(uses, countText(people)), frame);
                facts->setStyleSheet(facts->styleSheet() + QStringLiteral(" border: none;"));
                layout->addWidget(facts);
                addRow(frame);
            }
        });
    } else {
        loadAccountList(QStringLiteral("/api/v2/suggestions"), QUrlQuery(QStringLiteral("limit=40")),
                        QStringLiteral("No suggestions right now."), QStringLiteral("suggestions"));
    }
}

void MastodonFeature::showConversations()
{
    beginPage(QStringLiteral("Private mentions"), QStringLiteral("Conversations only the mentioned people can see"));
    auto *start = textButton(QStringLiteral("New private mention"), QStringLiteral("primaryButton"));
    connect(start, &QPushButton::clicked, this, [this] { compose({}, QString(), QStringLiteral("direct")); });
    addHeaderAction(start);
    const quint64 generation = m_generation;
    setLoading(true);
    auto show = std::make_shared<std::function<void(const Result &, bool)>>();
    std::weak_ptr<std::function<void(const Result &, bool)>> weakShow = show;
    *show = [this, generation, weakShow](const Result &result, bool append) {
        if (!current(generation) && !append)
            return;
        setLoading(false);
        if (!result.ok) {
            setNotice(result.error, true);
            return;
        }
        if (result.array().isEmpty() && !append)
            setEmpty(QStringLiteral("No private mentions yet."));
        for (const QJsonValue &value : result.array()) {
            const QJsonObject conversation = value.toObject();
            const QJsonObject last = conversation.value(QStringLiteral("last_status")).toObject();
            const QJsonArray accounts = conversation.value(QStringLiteral("accounts")).toArray();
            const QString id = conversation.value(QStringLiteral("id")).toString();
            const bool unread = conversation.value(QStringLiteral("unread")).toBool();
            QFrame *frame = panelRow();
            frame->setCursor(Qt::PointingHandCursor);
            auto *row = new QHBoxLayout(frame);
            row->setContentsMargins(16, 10, 16, 10);
            row->setSpacing(12);
            auto *avatar = new AvatarLabel(40);
            if (!accounts.isEmpty())
                avatar->setAccount(m_client.get(), accounts.first().toObject());
            row->addWidget(avatar, 0, Qt::AlignTop);
            QStringList names;
            for (const QJsonValue &account : accounts)
                names << displayName(account.toObject());
            auto *texts = new QVBoxLayout;
            texts->setSpacing(2);
            auto *who = new QLabel(QStringLiteral("%1<b>%2</b>")
                                       .arg(unread ? QStringLiteral("<span style=\"color:%1;\">&#9679;</span> ").arg(accentColor(this).name()) : QString(),
                                            escapeText(names.isEmpty() ? QStringLiteral("Only you") : names.join(QStringLiteral(", ")))));
            who->setTextFormat(Qt::RichText);
            who->setStyleSheet(QStringLiteral("font-size: 14px; background: transparent; border: none;"));
            texts->addWidget(who);
            const QString lastId = last.value(QStringLiteral("id")).toString();
            auto *excerpt = richLabel(QStringLiteral("<a href=\"fm-thread:%1\" style=\"text-decoration:none; color:%2;\">%3</a>")
                                          .arg(lastId.toHtmlEscaped(), palette().color(QPalette::WindowText).name(),
                                               escapeText(elide(plainText(last.value(QStringLiteral("content")).toString()), 200))));
            excerpt->setStyleSheet(QStringLiteral("font-size: 13px; background: transparent; border: none;"));
            connect(excerpt, &QLabel::linkActivated, this, [this, id, unread](const QString &link) {
                if (unread)
                    m_client->post(QStringLiteral("/api/v1/conversations/%1/read").arg(id), {});
                openLink(link);
            });
            texts->addWidget(excerpt);
            row->addLayout(texts, 1);
            row->addWidget(mutedLabel(relativeTime(last.value(QStringLiteral("created_at")).toString(), QDateTime::currentDateTimeUtc()), frame),
                           0, Qt::AlignTop);
            auto *remove = iconButton(QStringLiteral("trash"), QStringLiteral("Remove this conversation"), frame);
            QPointer<QFrame> guard(frame);
            connect(remove, &QToolButton::clicked, this, [this, id, guard] {
                if (!confirm(QStringLiteral("Remove conversation"), QStringLiteral("Remove this conversation from the list? The posts stay.")))
                    return;
                run("DELETE", QStringLiteral("/api/v1/conversations/") + id, {}, QStringLiteral("Conversation removed."));
                if (guard)
                    guard->hide();
            });
            row->addWidget(remove, 0, Qt::AlignTop);
            addRow(frame);
        }
        const auto show = weakShow.lock();
        if (show && result.next.isValid() && !result.array().isEmpty()) {
            const QUrl next = result.next;
            setLoadMore([this, next, show] {
                const quint64 page = m_generation;
                setLoading(true);
                m_client->getUrl(next, [this, page, show](const Result &more) {
                    if (current(page))
                        (*show)(more, true);
                });
            });
        }
    };
    m_client->get(QStringLiteral("/api/v1/conversations"), QUrlQuery(QStringLiteral("limit=30")), [show](const Result &result) {
        (*show)(result, false);
    });
}

void MastodonFeature::showThread()
{
    beginPage(QStringLiteral("Post"), QStringLiteral("The conversation around this post"));
    m_cardContext->filterContext = QStringLiteral("thread");
    const quint64 generation = m_generation;
    const QString id = m_page.arg;
    setLoading(true);
    m_client->get(QStringLiteral("/api/v1/statuses/") + id, {}, [this, generation, id](const Result &result) {
        if (!current(generation))
            return;
        if (!result.ok) {
            setLoading(false);
            setNotice(result.error, true);
            return;
        }
        const QJsonObject focal = result.object();
        auto *reply = textButton(QStringLiteral("Reply"), QStringLiteral("primaryButton"));
        connect(reply, &QPushButton::clicked, this, [this, focal] { compose(focal); });
        addHeaderAction(reply);
        m_client->get(QStringLiteral("/api/v1/statuses/%1/context").arg(id), {}, [this, generation, focal](const Result &context) {
            if (!current(generation))
                return;
            setLoading(false);
            CardContextPtr cards = cardContext();
            if (context.ok) {
                for (const QJsonValue &value : context.object().value(QStringLiteral("ancestors")).toArray())
                    addRow(new PostCard(value.toObject(), cards));
            }
            auto *focused = new PostCard(focal, cards, true);
            addRow(focused);
            m_seenStatuses.insert(focal.value(QStringLiteral("id")).toString(), focal);
            if (!context.ok) {
                setNotice(context.error, true);
                return;
            }
            const QJsonArray descendants = context.object().value(QStringLiteral("descendants")).toArray();
            for (const QJsonValue &value : descendants)
                addRow(new PostCard(value.toObject(), cards));
            if (descendants.isEmpty())
                setEmpty(QStringLiteral("No replies yet."));
            QTimer::singleShot(0, this, [this, generation] {
                if (!current(generation))
                    return;
                for (PostCard *card : postCards()) {
                    if (card->shownId() == m_page.arg)
                        m_scroll->ensureWidgetVisible(card, 0, 40);
                }
            });
        });
    });
}

void MastodonFeature::showAccount()
{
    const QString id = m_page.arg;
    const bool own = id == m_self.value(QStringLiteral("id")).toString();
    beginPage(QStringLiteral("Profile"));
    m_cardContext->filterContext = QStringLiteral("account");
    setTabs({{QString(), QStringLiteral("Posts")},
             {QStringLiteral("replies"), QStringLiteral("Posts and replies")},
             {QStringLiteral("media"), QStringLiteral("Media")}});
    const quint64 generation = m_generation;
    setLoading(true);
    m_client->get(QStringLiteral("/api/v1/accounts/") + id, {}, [this, generation, id, own](const Result &result) {
        if (!current(generation))
            return;
        if (!result.ok) {
            setLoading(false);
            setNotice(result.error, true);
            return;
        }
        const QJsonObject account = result.object();
        m_title->setText(displayName(account));
        m_subtitle->setText(fullHandle(account, m_client->instance()));
        m_subtitle->show();

        auto *card = new QWidget;
        card->setObjectName(QStringLiteral("mastodonProfileHeader"));
        auto *layout = new QVBoxLayout(card);
        layout->setContentsMargins(0, 0, 0, 8);
        layout->setSpacing(10);
        const QString headerUrl = account.value(QStringLiteral("header_static")).toString();
        if (!headerUrl.isEmpty() && !headerUrl.contains(QLatin1String("/headers/original/missing.png"))) {
            auto *banner = new MediaTile(QJsonObject{{QStringLiteral("type"), QStringLiteral("image")}}, false, card);
            banner->setFixedHeight(150);
            banner->setCursor(Qt::ArrowCursor);
            QPointer<MediaTile> bannerGuard(banner);
            m_client->image(headerUrl, [bannerGuard](const QImage &image) {
                if (bannerGuard)
                    bannerGuard->setImage(image);
            });
            auto *bannerRow = new QHBoxLayout;
            bannerRow->setContentsMargins(12, 4, 12, 0);
            bannerRow->addWidget(banner);
            layout->addLayout(bannerRow);
        }
        auto *identity = new QHBoxLayout;
        identity->setContentsMargins(16, 0, 16, 0);
        identity->setSpacing(14);
        auto *avatar = new AvatarLabel(76);
        avatar->setAccount(m_client.get(), account);
        identity->addWidget(avatar, 0, Qt::AlignTop);
        auto *names = new QVBoxLayout;
        names->setSpacing(2);
        auto *name = new QLabel(QStringLiteral("<b>%1</b>%2%3").arg(
            escapeText(displayName(account)),
            account.value(QStringLiteral("locked")).toBool() ? QStringLiteral(" &#128274;") : QString(),
            account.value(QStringLiteral("bot")).toBool() ? QStringLiteral(" <span style=\"font-size:11px;\">BOT</span>") : QString()));
        name->setTextFormat(Qt::RichText);
        name->setWordWrap(true);
        name->setStyleSheet(QStringLiteral("font-size: 20px;"));
        names->addWidget(name);
        auto *handle = mutedLabel(fullHandle(account, m_client->instance()), card);
        handle->setTextInteractionFlags(Qt::TextSelectableByMouse);
        names->addWidget(handle);
        auto *badges = mutedLabel(QString(), card);
        names->addWidget(badges);
        names->addStretch(1);
        identity->addLayout(names, 1);
        auto *buttons = new QHBoxLayout;
        buttons->setSpacing(6);
        identity->addLayout(buttons);
        layout->addLayout(identity);

        const RichText note = renderContent(account.value(QStringLiteral("note")).toString());
        if (!note.html.isEmpty()) {
            auto *bio = richLabel(note.html);
            bio->setContentsMargins(16, 0, 16, 0);
            bio->setStyleSheet(QStringLiteral("font-size: 14px;"));
            connect(bio, &QLabel::linkActivated, this, [this](const QString &link) { openLink(link); });
            layout->addWidget(bio);
        }
        const QJsonArray fields = account.value(QStringLiteral("fields")).toArray();
        if (!fields.isEmpty()) {
            auto *grid = new QGridLayout;
            grid->setContentsMargins(16, 0, 16, 0);
            grid->setHorizontalSpacing(14);
            grid->setVerticalSpacing(4);
            int row = 0;
            for (const QJsonValue &value : fields) {
                const QJsonObject field = value.toObject();
                auto *key = mutedLabel(field.value(QStringLiteral("name")).toString().toUpper(), card);
                grid->addWidget(key, row, 0, Qt::AlignTop);
                const bool verified = !field.value(QStringLiteral("verified_at")).toString().isEmpty();
                auto *fieldValue = richLabel(renderContent(field.value(QStringLiteral("value")).toString()).html +
                                             (verified ? QStringLiteral(" <span style=\"color:#2fa66a;\">&#10003;</span>") : QString()));
                fieldValue->setStyleSheet(QStringLiteral("font-size: 13px;"));
                if (verified)
                    fieldValue->setToolTip(QStringLiteral("Ownership of this link was checked on %1").arg(longDate(field.value(QStringLiteral("verified_at")).toString())));
                connect(fieldValue, &QLabel::linkActivated, this, [this](const QString &link) { openLink(link); });
                grid->addWidget(fieldValue, row, 1);
                ++row;
            }
            grid->setColumnStretch(1, 1);
            layout->addLayout(grid);
        }
        auto *stats = richLabel(QStringLiteral("<b>%1</b> posts &nbsp;&middot;&nbsp; <a href=\"following\" style=\"text-decoration:none;\"><b>%2</b> following</a>"
                                               " &nbsp;&middot;&nbsp; <a href=\"followers\" style=\"text-decoration:none;\"><b>%3</b> followers</a>"
                                               " &nbsp;&middot;&nbsp; joined %4")
                                    .arg(countText(account.value(QStringLiteral("statuses_count")).toInteger()),
                                         countText(account.value(QStringLiteral("following_count")).toInteger()),
                                         countText(account.value(QStringLiteral("followers_count")).toInteger()),
                                         QLocale().toString(QDateTime::fromString(account.value(QStringLiteral("created_at")).toString(), Qt::ISODateWithMs).date(),
                                                            QStringLiteral("MMMM yyyy"))));
        stats->setContentsMargins(16, 0, 16, 0);
        stats->setStyleSheet(QStringLiteral("font-size: 13px;"));
        connect(stats, &QLabel::linkActivated, this, [this, id](const QString &link) {
            openPage({QStringLiteral("connections"), id, link == QLatin1String("followers") ? QStringLiteral("followers") : QString()});
        });
        layout->addWidget(stats);
        const QJsonObject moved = account.value(QStringLiteral("moved")).toObject();
        if (!moved.isEmpty()) {
            auto *movedLabel = richLabel(QStringLiteral("This account has moved to <a href=\"%1:%2\">%3</a>.")
                                             .arg(kAccountScheme, moved.value(QStringLiteral("id")).toString(),
                                                  escapeText(fullHandle(moved, m_client->instance()))));
            movedLabel->setContentsMargins(16, 0, 16, 0);
            connect(movedLabel, &QLabel::linkActivated, this, [this](const QString &link) { openLink(link); });
            layout->addWidget(movedLabel);
        }
        addRow(card);

        if (own) {
            auto *edit = textButton(QStringLiteral("Edit profile"));
            connect(edit, &QPushButton::clicked, this, [this] { openPage({QStringLiteral("profile"), QString(), QString()}); });
            buttons->addWidget(edit);
        } else {
            QPointer<QWidget> cardGuard(card);
            QPointer<QLabel> badgeGuard(badges);
            fetchRelationships({id}, [this, generation, account, id, cardGuard, badgeGuard, buttons](const QHash<QString, QJsonObject> &relationships) {
                if (!current(generation) || !cardGuard)
                    return;
                const QJsonObject relationship = relationships.value(id);
                QStringList facts;
                if (relationship.value(QStringLiteral("followed_by")).toBool())
                    facts << QStringLiteral("Follows you");
                if (relationship.value(QStringLiteral("muting")).toBool())
                    facts << QStringLiteral("Muted");
                if (relationship.value(QStringLiteral("blocking")).toBool())
                    facts << QStringLiteral("Blocked");
                if (relationship.value(QStringLiteral("domain_blocking")).toBool())
                    facts << QStringLiteral("Server blocked");
                if (relationship.value(QStringLiteral("notifying")).toBool())
                    facts << QStringLiteral("Notifying you of posts");
                if (!relationship.value(QStringLiteral("note")).toString().isEmpty())
                    facts << QStringLiteral("Note: %1").arg(elide(relationship.value(QStringLiteral("note")).toString(), 60));
                if (badgeGuard)
                    badgeGuard->setText(facts.join(QStringLiteral(" · ")));
                buttons->addWidget(followButton(account, relationship));
                auto *more = iconButton(QStringLiteral("kebab-horizontal"), QStringLiteral("More"), cardGuard);
                connect(more, &QToolButton::clicked, this, [this, account, relationship, more] { openAccountMenu(account, relationship, more); });
                buttons->addWidget(more);
            });
        }

        QUrlQuery query;
        query.addQueryItem(QStringLiteral("limit"), QStringLiteral("30"));
        if (m_page.tab.isEmpty())
            query.addQueryItem(QStringLiteral("exclude_replies"), QStringLiteral("true"));
        if (m_page.tab == QLatin1String("media"))
            query.addQueryItem(QStringLiteral("only_media"), QStringLiteral("true"));
        const QString path = QStringLiteral("/api/v1/accounts/%1/statuses").arg(id);
        if (!m_page.tab.isEmpty()) {
            loadStatuses(path, query, QStringLiteral("account"), QStringLiteral("No posts here yet."));
            return;
        }
        m_client->get(path, QUrlQuery(QStringLiteral("pinned=true")), [this, generation, path, query](const Result &pinned) {
            if (!current(generation))
                return;
            if (pinned.ok) {
                for (const QJsonValue &value : pinned.array()) {
                    QJsonObject status = value.toObject();
                    status.insert(QStringLiteral("pinned"), true);
                    addRow(new PostCard(status, cardContext()));
                }
            }
            loadStatuses(path, query, QStringLiteral("account"), QStringLiteral("No posts yet."));
        });
    });
}

void MastodonFeature::showConnections()
{
    const bool followers = m_page.tab == QLatin1String("followers");
    beginPage(followers ? QStringLiteral("Followers") : QStringLiteral("Following"));
    setTabs({{QString(), QStringLiteral("Following")}, {QStringLiteral("followers"), QStringLiteral("Followers")}});
    loadAccountList(QStringLiteral("/api/v1/accounts/%1/%2").arg(m_page.arg, followers ? QStringLiteral("followers") : QStringLiteral("following")),
                    QUrlQuery(QStringLiteral("limit=40")),
                    followers ? QStringLiteral("No followers to show. Some people hide this list.")
                              : QStringLiteral("Not following anyone, or the list is hidden."),
                    QStringLiteral("plain"));
}

void MastodonFeature::showFollows()
{
    beginPage(QStringLiteral("Followers & following"), QStringLiteral("Manage who you follow and who follows you"));
    setTabs({{QString(), QStringLiteral("Following")},
             {QStringLiteral("followers"), QStringLiteral("Followers")},
             {QStringLiteral("requests"), m_followRequests > 0 ? QStringLiteral("Requests (%1)").arg(m_followRequests) : QStringLiteral("Requests")},
             {QStringLiteral("suggestions"), QStringLiteral("Suggestions")}});
    const QString self = m_self.value(QStringLiteral("id")).toString();
    if (m_page.tab == QLatin1String("followers")) {
        loadAccountList(QStringLiteral("/api/v1/accounts/%1/followers").arg(self), QUrlQuery(QStringLiteral("limit=40")),
                        QStringLiteral("Nobody follows you yet."), QStringLiteral("followers"));
    } else if (m_page.tab == QLatin1String("requests")) {
        if (!m_self.value(QStringLiteral("locked")).toBool())
            setNotice(QStringLiteral("Your account accepts followers automatically. Turn on approval in Profile to review requests."));
        loadAccountList(QStringLiteral("/api/v1/follow_requests"), QUrlQuery(QStringLiteral("limit=40")),
                        QStringLiteral("No pending follow requests."), QStringLiteral("requests"));
    } else if (m_page.tab == QLatin1String("suggestions")) {
        loadAccountList(QStringLiteral("/api/v2/suggestions"), QUrlQuery(QStringLiteral("limit=40")),
                        QStringLiteral("No suggestions right now."), QStringLiteral("suggestions"));
    } else {
        loadAccountList(QStringLiteral("/api/v1/accounts/%1/following").arg(self), QUrlQuery(QStringLiteral("limit=40")),
                        QStringLiteral("You are not following anyone yet."), QStringLiteral("following"));
    }
}

void MastodonFeature::showModeration()
{
    beginPage(QStringLiteral("Blocks & mutes"), QStringLiteral("People and servers you have hidden"));
    setTabs({{QString(), QStringLiteral("Blocked")}, {QStringLiteral("muted"), QStringLiteral("Muted")},
             {QStringLiteral("domains"), QStringLiteral("Blocked servers")}});
    if (m_page.tab == QLatin1String("muted")) {
        loadAccountList(QStringLiteral("/api/v1/mutes"), QUrlQuery(QStringLiteral("limit=40")), QStringLiteral("You have not muted anyone."),
                        QStringLiteral("muted"));
        return;
    }
    if (m_page.tab != QLatin1String("domains")) {
        loadAccountList(QStringLiteral("/api/v1/blocks"), QUrlQuery(QStringLiteral("limit=40")), QStringLiteral("You have not blocked anyone."),
                        QStringLiteral("blocked"));
        return;
    }
    QFrame *add = panelRow();
    auto *addRowLayout = new QHBoxLayout(add);
    addRowLayout->setContentsMargins(16, 10, 16, 10);
    auto *field = new QLineEdit;
    field->setObjectName(QStringLiteral("mastodonDomainField"));
    field->setPlaceholderText(QStringLiteral("example.social"));
    addRowLayout->addWidget(field, 1);
    auto *block = textButton(QStringLiteral("Block server"), QStringLiteral("dangerButton"));
    const auto submit = [this, field] {
        const QString domain = field->text().trimmed();
        if (domain.isEmpty())
            return;
        if (confirm(QStringLiteral("Block server"),
                    QStringLiteral("Hide everything from %1 and remove your followers there?").arg(domain)))
            blockDomain(domain, true);
    };
    connect(block, &QPushButton::clicked, this, submit);
    connect(field, &QLineEdit::returnPressed, this, submit);
    addRowLayout->addWidget(block);
    addRow(add);
    const quint64 generation = m_generation;
    setLoading(true);
    m_client->get(QStringLiteral("/api/v1/domain_blocks"), QUrlQuery(QStringLiteral("limit=100")), [this, generation](const Result &result) {
        if (!current(generation))
            return;
        setLoading(false);
        if (!result.ok) {
            setNotice(result.error, true);
            return;
        }
        if (result.array().isEmpty())
            setEmpty(QStringLiteral("You have not blocked any servers."));
        for (const QJsonValue &value : result.array()) {
            const QString domain = value.toString();
            QFrame *frame = panelRow();
            auto *row = new QHBoxLayout(frame);
            row->setContentsMargins(16, 10, 16, 10);
            auto *label = new QLabel(domain);
            label->setStyleSheet(QStringLiteral("font-size: 14px; background: transparent; border: none;"));
            row->addWidget(label, 1);
            auto *unblock = textButton(QStringLiteral("Unblock"));
            connect(unblock, &QPushButton::clicked, this, [this, domain] { blockDomain(domain, false); });
            row->addWidget(unblock);
            addRow(frame);
        }
    });
}

void MastodonFeature::showLists()
{
    beginPage(QStringLiteral("Lists"), QStringLiteral("Group the people you follow into separate timelines"));
    QFrame *create = panelRow();
    auto *createLayout = new QHBoxLayout(create);
    createLayout->setContentsMargins(16, 10, 16, 10);
    auto *field = new QLineEdit;
    field->setObjectName(QStringLiteral("mastodonNewList"));
    field->setPlaceholderText(QStringLiteral("New list name"));
    createLayout->addWidget(field, 1);
    auto *button = textButton(QStringLiteral("Create list"), QStringLiteral("primaryButton"));
    const auto submit = [this, field] { createList(field->text()); };
    connect(button, &QPushButton::clicked, this, submit);
    connect(field, &QLineEdit::returnPressed, this, submit);
    createLayout->addWidget(button);
    addRow(create);
    const quint64 generation = m_generation;
    setLoading(true);
    m_client->get(QStringLiteral("/api/v1/lists"), {}, [this, generation](const Result &result) {
        if (!current(generation))
            return;
        setLoading(false);
        if (!result.ok) {
            setNotice(result.error, true);
            return;
        }
        m_lists = result.array();
        if (m_lists.isEmpty())
            setEmpty(QStringLiteral("You have no lists yet."));
        for (const QJsonValue &value : std::as_const(m_lists)) {
            const QJsonObject list = value.toObject();
            const QString id = list.value(QStringLiteral("id")).toString();
            const QString title = list.value(QStringLiteral("title")).toString();
            QFrame *frame = panelRow();
            auto *layout = new QVBoxLayout(frame);
            layout->setContentsMargins(16, 10, 16, 10);
            layout->setSpacing(6);
            auto *top = new QHBoxLayout;
            auto *name = new QLabel(QStringLiteral("<b>%1</b>").arg(escapeText(title)));
            name->setTextFormat(Qt::RichText);
            name->setStyleSheet(QStringLiteral("font-size: 14px; background: transparent; border: none;"));
            top->addWidget(name, 1);
            auto *open = textButton(QStringLiteral("Open"));
            connect(open, &QPushButton::clicked, this, [this, id] { openPage({QStringLiteral("list"), id, QString()}); });
            top->addWidget(open);
            auto *members = textButton(QStringLiteral("Members"));
            connect(members, &QPushButton::clicked, this, [this, id] { openPage({QStringLiteral("list-members"), id, QString()}); });
            top->addWidget(members);
            auto *rename = textButton(QStringLiteral("Rename"));
            connect(rename, &QPushButton::clicked, this, [this, id, title] {
                bool ok = false;
                const QString value = QInputDialog::getText(this, QStringLiteral("Rename list"), QStringLiteral("Name"),
                                                            QLineEdit::Normal, title, &ok);
                if (ok && !value.trimmed().isEmpty())
                    updateList(id, {{QStringLiteral("title"), value.trimmed()}});
            });
            top->addWidget(rename);
            auto *remove = textButton(QStringLiteral("Delete"), QStringLiteral("dangerButton"));
            connect(remove, &QPushButton::clicked, this, [this, id, title] {
                if (confirm(QStringLiteral("Delete list"), QStringLiteral("Delete the list “%1”? The people in it are not unfollowed.").arg(title)))
                    deleteList(id);
            });
            top->addWidget(remove);
            layout->addLayout(top);
            auto *options = new QHBoxLayout;
            options->setSpacing(10);
            options->addWidget(mutedLabel(QStringLiteral("Show replies to"), frame));
            auto *policy = new QComboBox;
            policy->addItem(QStringLiteral("Any followed user"), QStringLiteral("followed"));
            policy->addItem(QStringLiteral("Members of the list"), QStringLiteral("list"));
            policy->addItem(QStringLiteral("No one"), QStringLiteral("none"));
            policy->setCurrentIndex(qMax(0, policy->findData(list.value(QStringLiteral("replies_policy")).toString(QStringLiteral("list")))));
            connect(policy, &QComboBox::activated, this, [this, id, policy] {
                updateList(id, {{QStringLiteral("replies_policy"), policy->currentData().toString()}});
            });
            options->addWidget(policy);
            if (list.contains(QStringLiteral("exclusive"))) {
                auto *exclusive = new QCheckBox(QStringLiteral("Hide these members' posts from Home"));
                exclusive->setChecked(list.value(QStringLiteral("exclusive")).toBool());
                connect(exclusive, &QCheckBox::toggled, this, [this, id](bool on) {
                    updateList(id, {{QStringLiteral("exclusive"), on}});
                });
                options->addWidget(exclusive);
            }
            options->addStretch(1);
            layout->addLayout(options);
            addRow(frame);
        }
        rebuildNav();
    });
}

void MastodonFeature::showListMembers()
{
    const QString listId = m_page.arg;
    QString title = QStringLiteral("List");
    for (const QJsonValue &value : std::as_const(m_lists)) {
        if (value.toObject().value(QStringLiteral("id")).toString() == listId)
            title = value.toObject().value(QStringLiteral("title")).toString();
    }
    beginPage(QStringLiteral("%1 · members").arg(title), QStringLiteral("Only people you follow can be added"));
    auto *timeline = textButton(QStringLiteral("Open timeline"));
    connect(timeline, &QPushButton::clicked, this, [this, listId] { openPage({QStringLiteral("list"), listId, QString()}); });
    addHeaderAction(timeline);
    QFrame *search = panelRow();
    auto *searchLayout = new QVBoxLayout(search);
    searchLayout->setContentsMargins(16, 10, 16, 10);
    auto *field = new QLineEdit;
    field->setObjectName(QStringLiteral("mastodonListSearch"));
    field->setPlaceholderText(QStringLiteral("Search people you follow to add them"));
    field->setClearButtonEnabled(true);
    searchLayout->addWidget(field);
    auto *results = new QWidget;
    auto *resultsLayout = new QVBoxLayout(results);
    resultsLayout->setContentsMargins(0, 0, 0, 0);
    resultsLayout->setSpacing(0);
    searchLayout->addWidget(results);
    addRow(search);
    QPointer<QWidget> resultsGuard(results);
    connect(field, &QLineEdit::returnPressed, this, [this, field, resultsGuard, listId] {
        const QString query = field->text().trimmed();
        if (query.isEmpty() || !resultsGuard)
            return;
        QUrlQuery params;
        params.addQueryItem(QStringLiteral("q"), query);
        params.addQueryItem(QStringLiteral("following"), QStringLiteral("true"));
        params.addQueryItem(QStringLiteral("limit"), QStringLiteral("10"));
        const quint64 generation = m_generation;
        m_client->get(QStringLiteral("/api/v1/accounts/search"), params, [this, generation, resultsGuard, listId](const Result &result) {
            if (!current(generation) || !resultsGuard)
                return;
            QLayout *layout = resultsGuard->layout();
            while (QLayoutItem *item = layout->takeAt(0)) {
                if (QWidget *gone = item->widget()) {
                    gone->hide();
                    gone->deleteLater();
                }
                delete item;
            }
            if (!result.ok) {
                setNotice(result.error, true);
                return;
            }
            if (result.array().isEmpty())
                layout->addWidget(mutedLabel(QStringLiteral("Nobody you follow matches."), resultsGuard));
            for (const QJsonValue &value : result.array()) {
                auto *row = new AccountRow(value.toObject(), cardContext(), false);
                decorateAccount(row, QStringLiteral("add:") + listId, {});
                layout->addWidget(row);
            }
        });
    });
    addSection(QStringLiteral("Members"));
    loadAccountList(QStringLiteral("/api/v1/lists/%1/accounts").arg(listId), QUrlQuery(QStringLiteral("limit=40")),
                    QStringLiteral("This list has no members yet."), QStringLiteral("members:") + listId);
}

void MastodonFeature::showHashtags()
{
    beginPage(QStringLiteral("Hashtags"), QStringLiteral("Hashtags you follow show up in your Home timeline"));
    QFrame *add = panelRow();
    auto *addLayout = new QHBoxLayout(add);
    addLayout->setContentsMargins(16, 10, 16, 10);
    auto *field = new QLineEdit;
    field->setObjectName(QStringLiteral("mastodonFollowTag"));
    field->setPlaceholderText(QStringLiteral("#hashtag"));
    addLayout->addWidget(field, 1);
    auto *follow = textButton(QStringLiteral("Follow hashtag"), QStringLiteral("primaryButton"));
    const auto submit = [this, field] { followTag(field->text(), true); };
    connect(follow, &QPushButton::clicked, this, submit);
    connect(field, &QLineEdit::returnPressed, this, submit);
    addLayout->addWidget(follow);
    addRow(add);
    addSection(QStringLiteral("Followed hashtags"));
    const quint64 generation = m_generation;
    setLoading(true);
    m_client->get(QStringLiteral("/api/v1/followed_tags"), QUrlQuery(QStringLiteral("limit=100")), [this, generation](const Result &result) {
        if (!current(generation))
            return;
        setLoading(false);
        if (!result.ok) {
            setNotice(result.error, true);
            return;
        }
        if (result.array().isEmpty())
            setEmpty(QStringLiteral("You are not following any hashtags."));
        for (const QJsonValue &value : result.array())
            addRow(tagRow(value.toObject(), QStringLiteral("unfollow")));
        addSection(QStringLiteral("Featured on your profile"));
        m_client->get(QStringLiteral("/api/v1/featured_tags"), {}, [this, generation](const Result &featured) {
            if (!current(generation))
                return;
            if (featured.ok && featured.array().isEmpty())
                setEmpty(QStringLiteral("No hashtags are featured on your profile."));
            for (const QJsonValue &value : featured.array()) {
                const QJsonObject tag = value.toObject();
                addRow(tagRow(tag, QStringLiteral("unfeature:") + tag.value(QStringLiteral("id")).toString()));
            }
            m_client->get(QStringLiteral("/api/v1/featured_tags/suggestions"), {}, [this, generation](const Result &suggested) {
                if (!current(generation) || !suggested.ok || suggested.array().isEmpty())
                    return;
                addSection(QStringLiteral("Hashtags you use often"));
                for (const QJsonValue &value : suggested.array())
                    addRow(tagRow(value.toObject(), QStringLiteral("feature")));
            });
        });
    });
}

void MastodonFeature::showTag()
{
    const QString name = m_page.arg;
    beginPage(QStringLiteral("#") + name, QStringLiteral("Public posts with this hashtag"));
    setTabs({{QString(), QStringLiteral("All")}, {QStringLiteral("local"), QStringLiteral("This server")},
             {QStringLiteral("media"), QStringLiteral("Media")}});
    const quint64 generation = m_generation;
    m_client->get(QStringLiteral("/api/v1/tags/") + encoded(name), {}, [this, generation, name](const Result &result) {
        if (!current(generation) || !result.ok)
            return;
        const bool following = result.object().value(QStringLiteral("following")).toBool();
        auto *button = textButton(following ? QStringLiteral("Unfollow") : QStringLiteral("Follow"),
                                  following ? QStringLiteral("ghostButton") : QStringLiteral("primaryButton"));
        connect(button, &QPushButton::clicked, this, [this, name, following] { followTag(name, !following); });
        addHeaderAction(button);
        const bool featuring = result.object().value(QStringLiteral("featuring")).toBool();
        if (!featuring) {
            auto *feature = textButton(QStringLiteral("Feature on profile"));
            connect(feature, &QPushButton::clicked, this, [this, name] { featureTag(name, QString()); });
            addHeaderAction(feature);
        }
        qint64 people = 0;
        const QString uses = weekUses(result.object(), &people);
        m_subtitle->setText(QStringLiteral("%1 posts by %2 people this week").arg(uses, countText(people)));
    });
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("limit"), QStringLiteral("30"));
    if (m_page.tab == QLatin1String("local"))
        query.addQueryItem(QStringLiteral("local"), QStringLiteral("true"));
    if (m_page.tab == QLatin1String("media"))
        query.addQueryItem(QStringLiteral("only_media"), QStringLiteral("true"));
    loadStatuses(QStringLiteral("/api/v1/timelines/tag/") + encoded(name), query, QStringLiteral("public"),
                 QStringLiteral("No posts with #%1 yet.").arg(name));
}

void MastodonFeature::showProfileEditor()
{
    beginPage(QStringLiteral("Profile"), QStringLiteral("How others see you on Mastodon"));
    auto *view = textButton(QStringLiteral("View profile"));
    connect(view, &QPushButton::clicked, this, [this] { openAccountId(m_self.value(QStringLiteral("id")).toString()); });
    addHeaderAction(view);
    const quint64 generation = m_generation;
    setLoading(true);
    m_client->get(QStringLiteral("/api/v1/accounts/verify_credentials"), {}, [this, generation](const Result &result) {
        if (!current(generation))
            return;
        setLoading(false);
        if (!result.ok) {
            setNotice(result.error, true);
            return;
        }
        m_self = result.object();
        const QJsonObject source = m_self.value(QStringLiteral("source")).toObject();
        auto avatarPath = std::make_shared<QString>();
        auto headerPath = std::make_shared<QString>();

        auto *form = new QWidget;
        form->setObjectName(QStringLiteral("mastodonProfileForm"));
        auto *layout = new QVBoxLayout(form);
        layout->setContentsMargins(16, 8, 16, 16);
        layout->setSpacing(12);

        auto *banner = new MediaTile(QJsonObject{{QStringLiteral("type"), QStringLiteral("image")}}, false, form);
        banner->setFixedHeight(140);
        banner->setCursor(Qt::ArrowCursor);
        QPointer<MediaTile> bannerGuard(banner);
        m_client->image(m_self.value(QStringLiteral("header_static")).toString(), [bannerGuard](const QImage &image) {
            if (bannerGuard)
                bannerGuard->setImage(image);
        });
        layout->addWidget(banner);
        auto *pictures = new QHBoxLayout;
        auto *avatar = new AvatarLabel(72);
        avatar->setAccount(m_client.get(), m_self);
        pictures->addWidget(avatar);
        auto *pictureButtons = new QVBoxLayout;
        auto *changeAvatar = textButton(QStringLiteral("Change picture…"));
        QPointer<AvatarLabel> avatarGuard(avatar);
        connect(changeAvatar, &QPushButton::clicked, this, [this, avatarPath, avatarGuard] {
            const QString file = QFileDialog::getOpenFileName(this, QStringLiteral("Profile picture"), QString(),
                                                              QStringLiteral("Images (*.png *.jpg *.jpeg *.gif *.webp)"));
            if (file.isEmpty())
                return;
            *avatarPath = file;
            if (avatarGuard)
                avatarGuard->setPixmap(roundedPixmap(QImage(file), 72, avatarGuard->devicePixelRatioF(), false));
        });
        pictureButtons->addWidget(changeAvatar);
        auto *changeHeader = textButton(QStringLiteral("Change header…"));
        connect(changeHeader, &QPushButton::clicked, this, [this, headerPath, bannerGuard] {
            const QString file = QFileDialog::getOpenFileName(this, QStringLiteral("Header image"), QString(),
                                                              QStringLiteral("Images (*.png *.jpg *.jpeg *.gif *.webp)"));
            if (file.isEmpty())
                return;
            *headerPath = file;
            if (bannerGuard)
                bannerGuard->setImage(QImage(file));
        });
        pictureButtons->addWidget(changeHeader);
        pictures->addLayout(pictureButtons);
        pictures->addStretch(1);
        layout->addLayout(pictures);

        auto *grid = new QFormLayout;
        grid->setLabelAlignment(Qt::AlignRight);
        auto *name = new QLineEdit(m_self.value(QStringLiteral("display_name")).toString());
        name->setObjectName(QStringLiteral("mastodonDisplayName"));
        name->setMaxLength(30);
        grid->addRow(QStringLiteral("Display name"), name);
        auto *bio = new QPlainTextEdit(source.value(QStringLiteral("note")).toString());
        bio->setObjectName(QStringLiteral("mastodonBio"));
        bio->setFixedHeight(110);
        grid->addRow(QStringLiteral("Bio"), bio);
        layout->addLayout(grid);

        auto *fieldsHint = mutedLabel(QStringLiteral("Extra fields appear as a table on your profile. Links are verified when the page links back with rel=\"me\"."), form);
        fieldsHint->setWordWrap(true);
        layout->addWidget(fieldsHint);
        auto *fieldsGrid = new QGridLayout;
        fieldsGrid->setHorizontalSpacing(8);
        fieldsGrid->setVerticalSpacing(6);
        QList<QPair<QLineEdit *, QLineEdit *>> fieldEdits;
        const QJsonArray fields = source.value(QStringLiteral("fields")).toArray();
        for (int index = 0; index < 4; ++index) {
            const QJsonObject field = index < fields.size() ? fields.at(index).toObject() : QJsonObject();
            auto *label = new QLineEdit(field.value(QStringLiteral("name")).toString());
            label->setPlaceholderText(QStringLiteral("Label"));
            label->setMaxLength(255);
            auto *content = new QLineEdit(field.value(QStringLiteral("value")).toString());
            content->setPlaceholderText(QStringLiteral("Content"));
            content->setMaxLength(255);
            fieldsGrid->addWidget(label, index, 0);
            fieldsGrid->addWidget(content, index, 1);
            fieldEdits.append(qMakePair(label, content));
        }
        fieldsGrid->setColumnStretch(0, 2);
        fieldsGrid->setColumnStretch(1, 3);
        layout->addLayout(fieldsGrid);

        QList<QPair<QString, QCheckBox *>> toggles;
        const auto toggle = [&](const QString &key, const QString &label, bool value) {
            auto *box = new QCheckBox(label);
            box->setChecked(value);
            layout->addWidget(box);
            toggles.append(qMakePair(key, box));
        };
        toggle(QStringLiteral("locked"), QStringLiteral("Require my approval for new followers"), m_self.value(QStringLiteral("locked")).toBool());
        toggle(QStringLiteral("bot"), QStringLiteral("This account mainly posts automatically"), m_self.value(QStringLiteral("bot")).toBool());
        toggle(QStringLiteral("discoverable"), QStringLiteral("Suggest my account to others and list it in the profile directory"),
               m_self.value(QStringLiteral("discoverable")).toBool());
        if (m_self.contains(QStringLiteral("indexable")) || source.contains(QStringLiteral("indexable")))
            toggle(QStringLiteral("indexable"), QStringLiteral("Let anyone find my public posts in search"),
                   source.value(QStringLiteral("indexable")).toBool(m_self.value(QStringLiteral("indexable")).toBool()));
        if (source.contains(QStringLiteral("hide_collections")) || m_self.contains(QStringLiteral("hide_collections")))
            toggle(QStringLiteral("hide_collections"), QStringLiteral("Hide who I follow and who follows me"),
                   source.value(QStringLiteral("hide_collections")).toBool(m_self.value(QStringLiteral("hide_collections")).toBool()));

        auto *save = textButton(QStringLiteral("Save profile"), QStringLiteral("primaryButton"));
        save->setObjectName(QStringLiteral("primaryButton"));
        save->setProperty("mastodonRole", QStringLiteral("saveProfile"));
        connect(save, &QPushButton::clicked, this, [this, name, bio, fieldEdits, toggles, avatarPath, headerPath] {
            QJsonObject values{{QStringLiteral("display_name"), name->text()}, {QStringLiteral("note"), bio->toPlainText()}};
            for (int index = 0; index < fieldEdits.size(); ++index) {
                values.insert(QStringLiteral("fields_attributes[%1][name]").arg(index), fieldEdits.at(index).first->text());
                values.insert(QStringLiteral("fields_attributes[%1][value]").arg(index), fieldEdits.at(index).second->text());
            }
            for (const auto &entry : toggles)
                values.insert(entry.first, entry.second->isChecked());
            saveProfile(values, *avatarPath, *headerPath);
        });
        auto *saveRow = new QHBoxLayout;
        saveRow->addWidget(save);
        saveRow->addStretch(1);
        layout->addLayout(saveRow);
        addRow(form);

        addSection(QStringLiteral("Featured hashtags"));
        m_client->get(QStringLiteral("/api/v1/featured_tags"), {}, [this, generation](const Result &featured) {
            if (!current(generation))
                return;
            QFrame *add = panelRow();
            auto *addLayout = new QHBoxLayout(add);
            addLayout->setContentsMargins(16, 8, 16, 8);
            auto *field = new QLineEdit;
            field->setPlaceholderText(QStringLiteral("#hashtag to feature"));
            addLayout->addWidget(field, 1);
            auto *feature = textButton(QStringLiteral("Feature"));
            const auto submit = [this, field] { featureTag(field->text(), QString()); };
            connect(feature, &QPushButton::clicked, this, submit);
            connect(field, &QLineEdit::returnPressed, this, submit);
            addLayout->addWidget(feature);
            addRow(add);
            for (const QJsonValue &value : featured.array()) {
                const QJsonObject tag = value.toObject();
                addRow(tagRow(tag, QStringLiteral("unfeature:") + tag.value(QStringLiteral("id")).toString()));
            }
            addSection(QStringLiteral("Featured profiles"));
            loadAccountList(QStringLiteral("/api/v1/endorsements"), QUrlQuery(QStringLiteral("limit=40")),
                            QStringLiteral("Feature people you recommend from the menu on their profile."), QStringLiteral("endorsed"));
        });
    });
}

void MastodonFeature::showFilters()
{
    beginPage(QStringLiteral("Filters"), QStringLiteral("Hide or warn about posts with words you choose"));
    auto *add = textButton(QStringLiteral("New filter"), QStringLiteral("primaryButton"));
    connect(add, &QPushButton::clicked, this, [this] {
        FilterDialog dialog({}, this);
        if (dialog.exec() == QDialog::Accepted)
            saveFilter(QString(), dialog.body());
    });
    addHeaderAction(add);
    const quint64 generation = m_generation;
    setLoading(true);
    m_client->get(QStringLiteral("/api/v2/filters"), {}, [this, generation](const Result &result) {
        if (!current(generation))
            return;
        setLoading(false);
        if (!result.ok) {
            setNotice(result.status == 404 ? QStringLiteral("This server does not offer filter management (it needs Mastodon 4.0 or newer).")
                                           : result.error,
                      true);
            return;
        }
        if (result.array().isEmpty())
            setEmpty(QStringLiteral("No filters yet. Filters hide or put a warning on posts that contain words you choose."));
        static const QHash<QString, QString> contextNames{
            {QStringLiteral("home"), QStringLiteral("Home")},           {QStringLiteral("notifications"), QStringLiteral("Notifications")},
            {QStringLiteral("public"), QStringLiteral("Public")},       {QStringLiteral("thread"), QStringLiteral("Conversations")},
            {QStringLiteral("account"), QStringLiteral("Profiles")},
        };
        for (const QJsonValue &value : result.array()) {
            const QJsonObject filter = value.toObject();
            const QString id = filter.value(QStringLiteral("id")).toString();
            QFrame *frame = panelRow();
            auto *row = new QHBoxLayout(frame);
            row->setContentsMargins(16, 10, 16, 10);
            auto *texts = new QVBoxLayout;
            texts->setSpacing(2);
            auto *title = new QLabel(QStringLiteral("<b>%1</b>").arg(escapeText(filter.value(QStringLiteral("title")).toString())));
            title->setTextFormat(Qt::RichText);
            title->setStyleSheet(QStringLiteral("font-size: 14px; background: transparent; border: none;"));
            texts->addWidget(title);
            QStringList contexts;
            for (const QJsonValue &context : filter.value(QStringLiteral("context")).toArray())
                contexts << contextNames.value(context.toString(), context.toString());
            QStringList keywords;
            for (const QJsonValue &keyword : filter.value(QStringLiteral("keywords")).toArray())
                keywords << keyword.toObject().value(QStringLiteral("keyword")).toString();
            const QString action = filter.value(QStringLiteral("filter_action")).toString();
            QStringList facts{contexts.join(QStringLiteral(", ")),
                              action == QLatin1String("hide") ? QStringLiteral("hidden")
                              : action == QLatin1String("blur") ? QStringLiteral("media blurred")
                                                                 : QStringLiteral("warning")};
            const QString expires = filter.value(QStringLiteral("expires_at")).toString();
            facts << (expires.isEmpty() ? QStringLiteral("never expires") : QStringLiteral("until %1").arg(longDate(expires)));
            auto *meta = mutedLabel(facts.join(QStringLiteral(" · ")), frame);
            meta->setStyleSheet(meta->styleSheet() + QStringLiteral(" border: none;"));
            texts->addWidget(meta);
            auto *words = new QLabel(keywords.isEmpty() ? QStringLiteral("No keywords") : elide(keywords.join(QStringLiteral(", ")), 160));
            words->setWordWrap(true);
            words->setStyleSheet(QStringLiteral("font-size: 13px; background: transparent; border: none;"));
            texts->addWidget(words);
            row->addLayout(texts, 1);
            auto *edit = textButton(QStringLiteral("Edit"));
            connect(edit, &QPushButton::clicked, this, [this, filter, id] {
                FilterDialog dialog(filter, this);
                if (dialog.exec() == QDialog::Accepted)
                    saveFilter(id, dialog.body());
            });
            row->addWidget(edit);
            auto *remove = textButton(QStringLiteral("Delete"), QStringLiteral("dangerButton"));
            connect(remove, &QPushButton::clicked, this, [this, id, filter] {
                if (confirm(QStringLiteral("Delete filter"), QStringLiteral("Delete the filter “%1”?").arg(filter.value(QStringLiteral("title")).toString())))
                    deleteFilter(id);
            });
            row->addWidget(remove);
            addRow(frame);
        }
    });
}

void MastodonFeature::showScheduled()
{
    beginPage(QStringLiteral("Scheduled posts"), QStringLiteral("Posts waiting to go out"));
    auto *add = textButton(QStringLiteral("Schedule a post"), QStringLiteral("primaryButton"));
    connect(add, &QPushButton::clicked, this, [this] {
        ComposeDialog *dialog = compose();
        dialog->setScheduledAt(QDateTime::currentDateTime().addSecs(3600));
    });
    addHeaderAction(add);
    const quint64 generation = m_generation;
    setLoading(true);
    m_client->get(QStringLiteral("/api/v1/scheduled_statuses"), QUrlQuery(QStringLiteral("limit=40")), [this, generation](const Result &result) {
        if (!current(generation))
            return;
        setLoading(false);
        if (!result.ok) {
            setNotice(result.error, true);
            return;
        }
        if (result.array().isEmpty())
            setEmpty(QStringLiteral("Nothing is scheduled. Pick “Schedule for” when writing a post."));
        for (const QJsonValue &value : result.array()) {
            const QJsonObject scheduled = value.toObject();
            const QString id = scheduled.value(QStringLiteral("id")).toString();
            const QJsonObject params = scheduled.value(QStringLiteral("params")).toObject();
            QFrame *frame = panelRow();
            auto *row = new QHBoxLayout(frame);
            row->setContentsMargins(16, 10, 16, 10);
            auto *texts = new QVBoxLayout;
            texts->setSpacing(2);
            const QString when = scheduled.value(QStringLiteral("scheduled_at")).toString();
            auto *title = new QLabel(QStringLiteral("<b>%1</b>").arg(escapeText(longDate(when))));
            title->setTextFormat(Qt::RichText);
            title->setStyleSheet(QStringLiteral("font-size: 14px; background: transparent; border: none;"));
            texts->addWidget(title);
            auto *body = new QLabel(elide(params.value(QStringLiteral("text")).toString(), 240));
            body->setWordWrap(true);
            body->setStyleSheet(QStringLiteral("font-size: 13px; background: transparent; border: none;"));
            texts->addWidget(body);
            QStringList facts{visibilityLabel(params.value(QStringLiteral("visibility")).toString())};
            const int media = static_cast<int>(scheduled.value(QStringLiteral("media_attachments")).toArray().size());
            if (media > 0)
                facts << QStringLiteral("%1 attachment%2").arg(media).arg(media == 1 ? QString() : QStringLiteral("s"));
            if (!params.value(QStringLiteral("poll")).toObject().isEmpty())
                facts << QStringLiteral("poll");
            auto *meta = mutedLabel(facts.join(QStringLiteral(" · ")), frame);
            meta->setStyleSheet(meta->styleSheet() + QStringLiteral(" border: none;"));
            texts->addWidget(meta);
            row->addLayout(texts, 1);
            auto *move = textButton(QStringLiteral("Reschedule"));
            connect(move, &QPushButton::clicked, this, [this, id, when] {
                QDialog dialog(this);
                dialog.setWindowTitle(QStringLiteral("Reschedule"));
                auto *layout = new QVBoxLayout(&dialog);
                auto *picker = new QDateTimeEdit(QDateTime::fromString(when, Qt::ISODateWithMs).toLocalTime());
                picker->setCalendarPopup(true);
                picker->setMinimumDateTime(QDateTime::currentDateTime().addSecs(300));
                picker->setDisplayFormat(QStringLiteral("d MMM yyyy  HH:mm"));
                layout->addWidget(picker);
                auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
                connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
                connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
                layout->addWidget(buttons);
                if (dialog.exec() == QDialog::Accepted)
                    reschedule(id, picker->dateTime());
            });
            row->addWidget(move);
            auto *cancel = textButton(QStringLiteral("Cancel post"), QStringLiteral("dangerButton"));
            connect(cancel, &QPushButton::clicked, this, [this, id] {
                if (confirm(QStringLiteral("Cancel scheduled post"), QStringLiteral("Cancel this post? It will not be published.")))
                    cancelScheduled(id);
            });
            row->addWidget(cancel);
            addRow(frame);
        }
    });
}

void MastodonFeature::showPreferences()
{
    beginPage(QStringLiteral("Preferences"), QStringLiteral("Settings for %1").arg(activeAccount().handle()));
    const QJsonObject source = m_self.value(QStringLiteral("source")).toObject();

    addSection(QStringLiteral("Posting defaults"));
    auto *posting = new QWidget;
    auto *postingForm = new QFormLayout(posting);
    postingForm->setContentsMargins(16, 4, 16, 8);
    auto *visibility = new QComboBox;
    visibility->setObjectName(QStringLiteral("mastodonDefaultVisibility"));
    for (const QString &value : {QStringLiteral("public"), QStringLiteral("unlisted"), QStringLiteral("private")})
        visibility->addItem(icon(visibilityIcon(value), mutedColor(this)), visibilityLabel(value), value);
    visibility->setCurrentIndex(qMax(0, visibility->findData(source.value(QStringLiteral("privacy")).toString(QStringLiteral("public")))));
    postingForm->addRow(QStringLiteral("Who can see new posts"), visibility);
    auto *language = new QComboBox;
    language->addItem(QStringLiteral("Detect automatically"), QString());
    for (const QString &code : languageCodes())
        language->addItem(languageName(code), code);
    const int languageIndex = language->findData(source.value(QStringLiteral("language")).toString());
    language->setCurrentIndex(qMax(0, languageIndex));
    postingForm->addRow(QStringLiteral("Language of new posts"), language);
    auto *sensitive = new QCheckBox(QStringLiteral("Always mark media as sensitive"));
    sensitive->setChecked(source.value(QStringLiteral("sensitive")).toBool());
    postingForm->addRow(QString(), sensitive);
    auto *save = textButton(QStringLiteral("Save posting defaults"), QStringLiteral("primaryButton"));
    connect(save, &QPushButton::clicked, this, [this, visibility, sensitive, language] {
        savePostingDefaults(visibility->currentData().toString(), sensitive->isChecked(), language->currentData().toString());
    });
    postingForm->addRow(QString(), save);
    addRow(posting);

    addSection(QStringLiteral("Reading"));
    auto *reading = new QWidget;
    auto *readingForm = new QFormLayout(reading);
    readingForm->setContentsMargins(16, 4, 16, 8);
    addRow(reading);
    const quint64 generation = m_generation;
    QPointer<QWidget> readingGuard(reading);
    m_client->get(QStringLiteral("/api/v1/preferences"), {}, [this, generation, readingGuard, readingForm](const Result &result) {
        if (!current(generation) || !readingGuard || !result.ok)
            return;
        const QJsonObject preferences = result.object();
        const QString media = preferences.value(QStringLiteral("reading:expand:media")).toString();
        readingForm->addRow(QStringLiteral("Sensitive media"),
                            new QLabel(media == QLatin1String("show_all")   ? QStringLiteral("Always shown")
                                       : media == QLatin1String("hide_all") ? QStringLiteral("Always hidden")
                                                                            : QStringLiteral("Hidden when marked sensitive")));
        readingForm->addRow(QStringLiteral("Content warnings"),
                            new QLabel(preferences.value(QStringLiteral("reading:expand:spoilers")).toBool() ? QStringLiteral("Always expanded")
                                                                                                            : QStringLiteral("Collapsed")));
        readingForm->addRow(QStringLiteral("Animated images"),
                            new QLabel(preferences.value(QStringLiteral("reading:autoplay:gifs")).toBool() ? QStringLiteral("Play automatically")
                                                                                                          : QStringLiteral("Play on hover")));
        auto *change = textButton(QStringLiteral("Change on the server"));
        connect(change, &QPushButton::clicked, this, [this] { openOnInstance(QStringLiteral("/settings/preferences/appearance")); });
        readingForm->addRow(QString(), change);
    });

    addSection(QStringLiteral("Account settings on %1").arg(instanceHost(m_client->instance())));
    auto *settingsHint = mutedLabel(QStringLiteral("Mastodon only offers these on its own website; they open in your browser."), m_column);
    settingsHint->setWordWrap(true);
    settingsHint->setContentsMargins(16, 0, 16, 0);
    addRow(settingsHint);
    auto *links = new QWidget;
    auto *grid = new QGridLayout(links);
    grid->setContentsMargins(16, 6, 16, 8);
    grid->setSpacing(6);
    const QList<QPair<QString, QString>> pages{
        {QStringLiteral("E-mail notifications"), QStringLiteral("/settings/preferences/notifications")},
        {QStringLiteral("E-mail and password"), QStringLiteral("/auth/edit")},
        {QStringLiteral("Two-factor authentication"), QStringLiteral("/settings/otp_authentication")},
        {QStringLiteral("Import and export"), QStringLiteral("/settings/export")},
        {QStringLiteral("Authorized apps"), QStringLiteral("/oauth/authorized_applications")},
        {QStringLiteral("Move to another account"), QStringLiteral("/settings/migration")},
        {QStringLiteral("Account aliases"), QStringLiteral("/settings/aliases")},
        {QStringLiteral("Delete account"), QStringLiteral("/settings/delete")},
    };
    for (int index = 0; index < pages.size(); ++index) {
        auto *button = textButton(pages.at(index).first,
                                  pages.at(index).second == QLatin1String("/settings/delete") ? QStringLiteral("dangerButton")
                                                                                             : QStringLiteral("ghostButton"));
        button->setIcon(icon(QStringLiteral("link-external"), mutedColor(this)));
        const QString path = pages.at(index).second;
        connect(button, &QPushButton::clicked, this, [this, path] { openOnInstance(path); });
        grid->addWidget(button, index / 2, index % 2);
    }
    addRow(links);

    addSection(QStringLiteral("Accounts on this computer"));
    for (const SavedAccount &account : std::as_const(m_accounts)) {
        QFrame *frame = panelRow();
        auto *row = new QHBoxLayout(frame);
        row->setContentsMargins(16, 8, 16, 8);
        row->setSpacing(10);
        auto *avatar = new AvatarLabel(32);
        avatar->setSource(m_client.get(), account.avatar, account.displayName);
        row->addWidget(avatar);
        auto *label = new QLabel(QStringLiteral("<b>%1</b> &nbsp;%2%3")
                                     .arg(escapeText(account.displayName), escapeText(account.handle()),
                                          account.key() == m_activeKey ? QStringLiteral(" &nbsp;· active") : QString()));
        label->setTextFormat(Qt::RichText);
        label->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
        row->addWidget(label, 1);
        const QString key = account.key();
        if (key != m_activeKey) {
            auto *use = textButton(QStringLiteral("Switch"));
            connect(use, &QPushButton::clicked, this, [this, key] { switchAccount(key); });
            row->addWidget(use);
        }
        auto *out = textButton(QStringLiteral("Sign out"));
        const QString handle = account.handle();
        connect(out, &QPushButton::clicked, this, [this, key, handle] {
            if (confirm(QStringLiteral("Sign out"), QStringLiteral("Sign out of %1 on this computer?").arg(handle)))
                signOut(key);
        });
        row->addWidget(out);
        addRow(frame);
    }
    auto *another = textButton(QStringLiteral("Add another account"));
    connect(another, &QPushButton::clicked, this, [this] { showSignIn(); });
    auto *anotherRow = new QWidget;
    auto *anotherLayout = new QHBoxLayout(anotherRow);
    anotherLayout->setContentsMargins(16, 8, 16, 8);
    anotherLayout->addWidget(another);
    anotherLayout->addStretch(1);
    addRow(anotherRow);
}

void MastodonFeature::showSearch()
{
    const QString query = m_page.arg;
    beginPage(QStringLiteral("Search"), QStringLiteral("Results for “%1”").arg(query));
    if (m_searchField->text() != query)
        m_searchField->setText(query);
    setTabs({{QString(), QStringLiteral("All")}, {QStringLiteral("accounts"), QStringLiteral("People")},
             {QStringLiteral("hashtags"), QStringLiteral("Hashtags")}, {QStringLiteral("statuses"), QStringLiteral("Posts")}});
    QUrlQuery params;
    params.addQueryItem(QStringLiteral("q"), query);
    params.addQueryItem(QStringLiteral("resolve"), QStringLiteral("true"));
    params.addQueryItem(QStringLiteral("limit"), m_page.tab.isEmpty() ? QStringLiteral("8") : QStringLiteral("40"));
    if (!m_page.tab.isEmpty())
        params.addQueryItem(QStringLiteral("type"), m_page.tab);
    const quint64 generation = m_generation;
    setLoading(true);
    m_client->get(QStringLiteral("/api/v2/search"), params, [this, generation](const Result &result) {
        if (!current(generation))
            return;
        setLoading(false);
        if (!result.ok) {
            setNotice(result.error, true);
            return;
        }
        const QJsonObject found = result.object();
        const QJsonArray accounts = found.value(QStringLiteral("accounts")).toArray();
        const QJsonArray tags = found.value(QStringLiteral("hashtags")).toArray();
        const QJsonArray statuses = found.value(QStringLiteral("statuses")).toArray();
        if (accounts.isEmpty() && tags.isEmpty() && statuses.isEmpty()) {
            setEmpty(QStringLiteral("Nothing found. Searching posts needs the words to appear in posts you can see, or a full link to a post."));
            return;
        }
        const bool all = m_page.tab.isEmpty();
        if (!accounts.isEmpty()) {
            if (all)
                addSection(QStringLiteral("People"));
            QList<QPointer<AccountRow>> rows;
            QStringList ids;
            for (const QJsonValue &value : accounts) {
                auto *row = new AccountRow(value.toObject(), cardContext());
                addRow(row);
                rows.append(row);
                ids.append(value.toObject().value(QStringLiteral("id")).toString());
            }
            fetchRelationships(ids, [this, rows, generation](const QHash<QString, QJsonObject> &relationships) {
                if (!current(generation))
                    return;
                for (const QPointer<AccountRow> &row : rows) {
                    if (row)
                        decorateAccount(row, QStringLiteral("plain"), relationships.value(row->account().value(QStringLiteral("id")).toString()));
                }
            });
        }
        if (!tags.isEmpty()) {
            if (all)
                addSection(QStringLiteral("Hashtags"));
            for (const QJsonValue &value : tags)
                addRow(tagRow(value.toObject(), QStringLiteral("follow")));
        }
        if (!statuses.isEmpty()) {
            if (all)
                addSection(QStringLiteral("Posts"));
            for (const QJsonValue &value : statuses)
                addRow(new PostCard(value.toObject(), cardContext()));
        }
    });
}

}
