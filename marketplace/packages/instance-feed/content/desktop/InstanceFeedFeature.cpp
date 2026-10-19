#include "InstanceFeedFeature.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QStyle>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <utility>

namespace forkmesh::instancefeed {
namespace {

const QString kApi = QStringLiteral("/api/instance-feed");

QString text(const QJsonObject &object, const char *key)
{
    return object.value(QLatin1String(key)).toString();
}

int count(const QJsonObject &object, const char *key)
{
    return object.value(QLatin1String(key)).toInt();
}

bool flag(const QJsonObject &object, const char *key)
{
    return object.value(QLatin1String(key)).toBool();
}

QLabel *mutedLabel(const QString &value)
{
    auto *label = new QLabel(value);
    label->setObjectName(QStringLiteral("mutedLabel"));
    label->setWordWrap(true);
    return label;
}

QPushButton *ghostButton(const QString &label)
{
    auto *button = new QPushButton(label);
    button->setObjectName(QStringLiteral("ghostButton"));
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

QFrame *divider()
{
    auto *line = new QFrame;
    line->setObjectName(QStringLiteral("issueDivider"));
    line->setFixedHeight(1);
    return line;
}

QString escaped(const QString &value)
{
    return value.toHtmlEscaped();
}

QJsonObject statusOf(const QJsonObject &entry)
{
    return text(entry, "type") == QLatin1String("boost")
        ? entry.value(QStringLiteral("post")).toObject() : entry;
}

} // namespace

QString InstanceFeedFeature::richText(const QString &value, const QString &linkColor)
{
    static const QRegularExpression pattern(QStringLiteral(
        R"((https://[^\s<>"']+[^\s<>"'.,;:!?)\]])|(^|[^A-Za-z0-9_@/])@([A-Za-z](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)\b)"));
    const QString style = linkColor.isEmpty()
        ? QString() : QStringLiteral(" style=\"color:%1; text-decoration:none\"").arg(linkColor);
    QString html;
    qsizetype last = 0;
    auto it = pattern.globalMatch(value);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        html += escaped(value.mid(last, match.capturedStart() - last));
        if (!match.captured(1).isEmpty()) {
            const QString url = match.captured(1);
            html += QStringLiteral("<a href=\"%1\"%2>%3</a>").arg(escaped(url), style, escaped(url));
        } else {
            html += escaped(match.captured(2));
            html += QStringLiteral("<a href=\"fm-author:%1\"%2>@%3</a>")
                        .arg(match.captured(3).toLower(), style, escaped(match.captured(3)));
        }
        last = match.capturedEnd();
    }
    html += escaped(value.mid(last));
    html.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    return html;
}

QString InstanceFeedFeature::relativeLabel(qint64 createdAtMs, qint64 nowMs)
{
    if (createdAtMs <= 0)
        return QString();
    const qint64 seconds = qMax<qint64>(0, (nowMs - createdAtMs) / 1000);
    if (seconds < 60)
        return QStringLiteral("now");
    if (seconds < 3600)
        return QStringLiteral("%1m").arg(seconds / 60);
    if (seconds < 86400)
        return QStringLiteral("%1h").arg(seconds / 3600);
    if (seconds < 7 * 86400)
        return QStringLiteral("%1d").arg(seconds / 86400);
    const QDateTime when = QDateTime::fromMSecsSinceEpoch(createdAtMs);
    const QDateTime now = QDateTime::fromMSecsSinceEpoch(nowMs);
    return QLocale().toString(when.date(), when.date().year() == now.date().year()
                                               ? QStringLiteral("MMM d") : QStringLiteral("MMM d, yyyy"));
}

bool InstanceFeedFeature::validPostId(const QString &id)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{32}$"));
    return pattern.match(id).hasMatch();
}

bool InstanceFeedFeature::validAuthor(const QString &name)
{
    static const QRegularExpression pattern(QStringLiteral("^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$"));
    return pattern.match(name).hasMatch();
}

QString InstanceFeedFeature::timelinePath(View view, const QString &author, const QString &before)
{
    QUrlQuery query;
    if (view == View::Profile && validAuthor(author))
        query.addQueryItem(QStringLiteral("author"), author);
    if (!before.isEmpty())
        query.addQueryItem(QStringLiteral("before"), before);
    return query.isEmpty() ? kApi : kApi + QLatin1Char('?') + query.toString(QUrl::FullyEncoded);
}

bool InstanceFeedFeature::exportHasData(const QJsonObject &payload)
{
    const QJsonObject data = payload.value(QStringLiteral("export")).toObject();
    for (const char *key : {"posts", "boosts", "favourites"}) {
        if (!data.value(QLatin1String(key)).toArray().isEmpty())
            return true;
    }
    return false;
}

InstanceFeedFeature::InstanceFeedFeature(const forkmesh::market::NativePackageHost &host,
                                         QWidget *parent)
    : forkmesh::market::NativePackageWidget(parent), m_request(host.request)
{
    setObjectName(QStringLiteral("instanceFeedSection"));
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(20, 18, 20, 20);
    outer->setSpacing(10);

    auto *heading = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("Feed"));
    title->setObjectName(QStringLiteral("pageTitle"));
    heading->addWidget(title);
    auto *subtitle = mutedLabel(QStringLiteral("Short posts from the people on this instance, newest first"));
    subtitle->setWordWrap(false);
    heading->addWidget(subtitle);
    heading->addStretch();
    m_refreshButton = ghostButton(QStringLiteral("Refresh"));
    m_exportButton = ghostButton(QStringLiteral("Export my data"));
    heading->addWidget(m_refreshButton);
    heading->addWidget(m_exportButton);
    outer->addLayout(heading);

    m_status = mutedLabel(QStringLiteral("Loading the feed…"));
    outer->addWidget(m_status);

    m_notInstalled = mutedLabel(QStringLiteral(
        "Feed is not installed for your ForkMesh account. Install it from the Market to "
        "read and write posts with the people on this instance."));
    m_notInstalled->setAlignment(Qt::AlignHCenter);
    m_notInstalled->setContentsMargins(8, 36, 8, 8);
    outer->addWidget(m_notInstalled);

    m_content = new QWidget;
    auto *content = new QVBoxLayout(m_content);
    content->setContentsMargins(0, 0, 0, 0);
    content->setSpacing(8);

    m_replyBar = new QWidget;
    auto *replyRow = new QHBoxLayout(m_replyBar);
    replyRow->setContentsMargins(0, 0, 0, 0);
    m_replyLabel = mutedLabel(QString());
    replyRow->addWidget(m_replyLabel, 1);
    auto *cancelReply = ghostButton(QStringLiteral("Cancel reply"));
    replyRow->addWidget(cancelReply);
    content->addWidget(m_replyBar);

    m_spoiler = new QLineEdit;
    m_spoiler->setPlaceholderText(QStringLiteral("Content warning"));
    m_spoiler->setMaxLength(100);
    m_spoiler->setVisible(false);
    content->addWidget(m_spoiler);

    m_text = new QPlainTextEdit;
    m_text->setObjectName(QStringLiteral("issueComposerSm"));
    m_text->setPlaceholderText(QStringLiteral("What’s happening on the instance?"));
    m_text->setTabChangesFocus(true);
    m_text->setFixedHeight(84);
    content->addWidget(m_text);

    auto *composeRow = new QHBoxLayout;
    m_spoilerToggle = ghostButton(QStringLiteral("CW"));
    m_spoilerToggle->setCheckable(true);
    m_spoilerToggle->setToolTip(QStringLiteral("Add a content warning"));
    composeRow->addWidget(m_spoilerToggle);
    composeRow->addStretch();
    m_remaining = mutedLabel(QString::number(m_maxLength));
    m_remaining->setWordWrap(false);
    composeRow->addWidget(m_remaining);
    m_postButton = new QPushButton(QStringLiteral("Post"));
    m_postButton->setObjectName(QStringLiteral("primaryButton"));
    m_postButton->setToolTip(QStringLiteral("Post (Ctrl+Enter)"));
    composeRow->addWidget(m_postButton);
    content->addLayout(composeRow);

    auto *contextRow = new QHBoxLayout;
    m_backButton = ghostButton(QStringLiteral("← Back to the feed"));
    contextRow->addWidget(m_backButton);
    contextRow->addStretch();
    content->addLayout(contextRow);
    m_profile = new QLabel;
    m_profile->setTextFormat(Qt::RichText);
    m_profile->setWordWrap(true);
    m_profile->setContentsMargins(4, 4, 4, 8);
    content->addWidget(m_profile);

    m_scroll = new QScrollArea;
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    auto *body = new QWidget;
    auto *bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);
    auto *listHost = new QWidget;
    m_list = new QVBoxLayout(listHost);
    m_list->setContentsMargins(0, 0, 0, 0);
    m_list->setSpacing(0);
    bodyLayout->addWidget(listHost);
    auto *moreRow = new QHBoxLayout;
    moreRow->setContentsMargins(0, 10, 0, 10);
    moreRow->addStretch();
    m_moreButton = ghostButton(QStringLiteral("Load older posts"));
    moreRow->addWidget(m_moreButton);
    moreRow->addStretch();
    bodyLayout->addLayout(moreRow);
    bodyLayout->addStretch();
    m_scroll->setWidget(body);
    content->addWidget(m_scroll, 1);
    outer->addWidget(m_content, 1);

    connect(m_refreshButton, &QPushButton::clicked, this, [this] { load(); });
    connect(m_exportButton, &QPushButton::clicked, this, [this] { exportMine(); });
    connect(m_backButton, &QPushButton::clicked, this, [this] { showTimeline(); });
    connect(m_moreButton, &QPushButton::clicked, this, [this] { load(true); });
    connect(m_postButton, &QPushButton::clicked, this, [this] { submit(); });
    connect(cancelReply, &QPushButton::clicked, this, [this] {
        m_replyTo.clear();
        m_replyAuthor.clear();
        renderCompose();
    });
    connect(m_text, &QPlainTextEdit::textChanged, this, [this] { renderCompose(); });
    connect(m_spoilerToggle, &QPushButton::toggled, this, [this](bool on) {
        m_spoiler->setVisible(on);
        if (on)
            m_spoiler->setFocus();
        else
            m_spoiler->clear();
    });
    connect(m_profile, &QLabel::linkActivated, this, [this](const QString &link) { openLink(link); });
    for (const QKeySequence &keys : {QKeySequence(Qt::CTRL | Qt::Key_Return), QKeySequence(Qt::CTRL | Qt::Key_Enter)}) {
        auto *shortcut = new QShortcut(keys, m_text);
        shortcut->setContext(Qt::WidgetShortcut);
        connect(shortcut, &QShortcut::activated, this, [this] { submit(); });
    }

    m_content->setVisible(false);
    m_notInstalled->setVisible(false);
    m_exportButton->setVisible(false);
    renderCompose();
    load();
}

bool InstanceFeedFeature::dark() const
{
    return palette().color(QPalette::Window).lightness() < 128;
}

void InstanceFeedFeature::request(const QByteArray &method, const QString &path,
                                  const QJsonObject &body, ReplyHandler handler)
{
    if (!m_request) {
        handler(false, {}, QStringLiteral("Feed network access is unavailable."));
        return;
    }
    const QPointer<InstanceFeedFeature> guard(this);
    m_request(method, path, body,
              [guard, handler = std::move(handler)](bool ok, const QJsonObject &payload,
                                                    const QString &error) mutable {
        if (!guard)
            return;
        if (!ok && text(payload, "error") == QLatin1String("plugin_not_installed")) {
            guard->m_checked = true;
            guard->m_installed = false;
            guard->render();
            return;
        }
        handler(ok, payload, error);
    });
}

void InstanceFeedFeature::refresh()
{
    load();
}

void InstanceFeedFeature::showTimeline()
{
    m_view = View::Timeline;
    m_threadId.clear();
    m_author.clear();
    load();
}

void InstanceFeedFeature::showThread(const QString &postId)
{
    if (!validPostId(postId))
        return;
    m_view = View::Thread;
    m_threadId = postId;
    load();
}

void InstanceFeedFeature::showProfile(const QString &author)
{
    if (!validAuthor(author))
        return;
    m_view = View::Profile;
    m_author = author;
    load();
}

void InstanceFeedFeature::openRailTile(const QString &id)
{
    openLinkTarget(id);
}

bool InstanceFeedFeature::openLinkTarget(const QString &id)
{
    if (id.startsWith(QStringLiteral("post-")) && validPostId(id.mid(5))) {
        showThread(id.mid(5));
        return true;
    }
    if (id.startsWith(QStringLiteral("author-")) && validAuthor(id.mid(7))) {
        showProfile(id.mid(7));
        return true;
    }
    return false;
}

void InstanceFeedFeature::load(bool append)
{
    if (append && (m_loading || m_next.isEmpty()))
        return;
    const int generation = ++m_generation;
    m_loading = true;
    m_refreshButton->setEnabled(false);
    m_moreButton->setEnabled(false);
    const bool thread = m_view == View::Thread;
    const QString path = thread ? kApi + QStringLiteral("/posts/") + m_threadId
                                : timelinePath(m_view, m_author, append ? m_next : QString());
    request(QByteArrayLiteral("GET"), path, {},
            [this, generation, append, thread](bool ok, const QJsonObject &payload, const QString &error) {
        if (generation != m_generation)
            return;
        m_loading = false;
        m_refreshButton->setEnabled(true);
        m_moreButton->setEnabled(true);
        if (!ok) {
            if (thread && text(payload, "error") == QLatin1String("post_not_found")) {
                m_checked = m_installed = true;
                m_thread = QJsonObject();
                render();
                return setStatus(QStringLiteral("That post was deleted."), true);
            }
            return setStatus(QStringLiteral("The feed could not load: %1").arg(error), true);
        }
        m_checked = true;
        m_installed = thread || payload.value(QStringLiteral("installed")).toBool();
        if (!m_installed) {
            m_posts = QJsonArray();
            render();
            return setStatus(QString());
        }
        if (thread) {
            m_thread = payload;
            m_posts = QJsonArray();
        } else {
            m_maxLength = qMax(1, payload.value(QStringLiteral("maxLength")).toInt(m_maxLength));
            m_profileData = payload.value(QStringLiteral("profile")).toObject();
            const QJsonArray posts = payload.value(QStringLiteral("posts")).toArray();
            if (append) {
                for (const QJsonValue &post : posts)
                    m_posts.append(post);
            } else {
                m_posts = posts;
            }
            m_next = text(payload, "next");
        }
        setStatus(QString());
        const int scrolled = m_scroll->verticalScrollBar()->value();
        render();
        if (append)
            m_scroll->verticalScrollBar()->setValue(scrolled);
        else
            m_scroll->verticalScrollBar()->setValue(0);
    });
}

void InstanceFeedFeature::clearList()
{
    while (QLayoutItem *item = m_list->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->hide();
            widget->setParent(nullptr);
            widget->deleteLater();
        }
        delete item;
    }
}

void InstanceFeedFeature::render()
{
    m_notInstalled->setVisible(m_checked && !m_installed);
    m_content->setVisible(m_installed);
    m_exportButton->setVisible(m_installed);
    if (!m_installed)
        return;
    m_backButton->setVisible(m_view != View::Timeline);
    renderProfile();
    renderCompose();
    clearList();
    QList<QWidget *> rows;
    if (m_view == View::Thread) {
        const QJsonObject focused = m_thread.value(QStringLiteral("post")).toObject();
        for (const QJsonValue &item : m_thread.value(QStringLiteral("ancestors")).toArray())
            rows.append(postRow(item.toObject(), QString(), QString(), false));
        if (!focused.isEmpty())
            rows.append(postRow(focused, QString(), QString(), true));
        for (const QJsonValue &item : m_thread.value(QStringLiteral("replies")).toArray())
            rows.append(postRow(item.toObject(), QString(), QString(), false));
    } else {
        for (const QJsonValue &value : std::as_const(m_posts)) {
            const QJsonObject entry = value.toObject();
            const bool boost = text(entry, "type") == QLatin1String("boost");
            rows.append(postRow(statusOf(entry), boost ? text(entry, "booster") : QString(),
                                boost ? text(entry, "boosterName") : QString(), false));
        }
    }
    if (rows.isEmpty()) {
        auto *empty = mutedLabel(m_view == View::Thread
            ? QStringLiteral("This post was deleted.")
            : m_view == View::Profile
                ? QStringLiteral("@%1 has not posted to the feed yet.").arg(m_author)
                : QStringLiteral("No posts yet. Be the first to post to this instance."));
        empty->setAlignment(Qt::AlignHCenter);
        empty->setContentsMargins(8, 32, 8, 32);
        m_list->addWidget(empty);
    }
    for (int index = 0; index < rows.size(); ++index) {
        if (index)
            m_list->addWidget(divider());
        m_list->addWidget(rows.at(index));
    }
    m_moreButton->setVisible(m_view != View::Thread && !m_next.isEmpty());
}

void InstanceFeedFeature::renderProfile()
{
    const bool show = m_view == View::Profile && !m_profileData.isEmpty();
    m_profile->setVisible(show);
    if (!show)
        return;
    const QString muted = palette().color(QPalette::PlaceholderText).name();
    const int posts = count(m_profileData, "posts");
    const qint64 since = m_profileData.value(QStringLiteral("since")).toInteger();
    QString line = QStringLiteral("@%1 · %2 %3").arg(escaped(text(m_profileData, "name")))
                       .arg(posts).arg(posts == 1 ? QStringLiteral("post") : QStringLiteral("posts"));
    if (since > 0)
        line += QStringLiteral(" · posting since %1").arg(
            QLocale().toString(QDateTime::fromMSecsSinceEpoch(since).date(), QStringLiteral("MMMM yyyy")));
    m_profile->setText(QStringLiteral("<span style=\"font-size:17px; font-weight:700\">%1</span><br>"
                                      "<span style=\"color:%2\">%3</span>")
                           .arg(escaped(text(m_profileData, "displayName")), muted, line));
}

void InstanceFeedFeature::renderCompose()
{
    m_replyBar->setVisible(!m_replyTo.isEmpty());
    m_replyLabel->setText(QStringLiteral("Replying to @%1").arg(m_replyAuthor));
    const int length = int(m_text->toPlainText().size());
    m_remaining->setText(QString::number(m_maxLength - length));
    m_remaining->setObjectName(length > m_maxLength ? QStringLiteral("errorLabel") : QStringLiteral("mutedLabel"));
    m_remaining->style()->unpolish(m_remaining);
    m_remaining->style()->polish(m_remaining);
    m_postButton->setEnabled(length > 0 && length <= m_maxLength && !m_text->toPlainText().trimmed().isEmpty());
}

QPushButton *InstanceFeedFeature::actionButton(const QString &label, int total, bool active,
                                               const QString &activeColor)
{
    auto *button = ghostButton(total > 0 ? QStringLiteral("%1  %2").arg(label).arg(total) : label);
    if (active)
        button->setStyleSheet(QStringLiteral("color: %1;").arg(activeColor));
    return button;
}

QWidget *InstanceFeedFeature::postRow(const QJsonObject &post, const QString &boostedBy,
                                      const QString &boosterName, bool focused)
{
    const QString id = text(post, "id");
    const QString author = text(post, "author");
    const QString name = text(post, "authorName").isEmpty() ? author : text(post, "authorName");
    const QString link = dark() ? QStringLiteral("#58a6ff") : QStringLiteral("#0969da");
    const QString amber = dark() ? QStringLiteral("#d29922") : QStringLiteral("#9a6700");
    const QString muted = palette().color(QPalette::PlaceholderText).name();
    const QString plain = QStringLiteral(" style=\"color:%1; text-decoration:none\"")
                              .arg(palette().color(QPalette::WindowText).name());

    auto *row = new QWidget;
    row->setObjectName(QStringLiteral("instanceFeedPost"));
    row->setProperty("postId", id);
    auto *column = new QVBoxLayout(row);
    column->setContentsMargins(12, 12, 12, 10);
    column->setSpacing(6);
    const auto linkHandler = [this](const QString &target) { openLink(target); };

    if (!boostedBy.isEmpty()) {
        auto *banner = new QLabel(QStringLiteral("⟲ <a href=\"fm-author:%1\"%2>%3</a> boosted")
                                      .arg(escaped(boostedBy), plain,
                                           escaped(boosterName.isEmpty() ? boostedBy : boosterName)));
        banner->setObjectName(QStringLiteral("mutedLabel"));
        banner->setTextFormat(Qt::RichText);
        banner->setContentsMargins(48, 0, 0, 0);
        connect(banner, &QLabel::linkActivated, this, linkHandler);
        column->addWidget(banner);
    }

    auto *main = new QHBoxLayout;
    main->setSpacing(12);
    auto *avatar = new QLabel(name.left(1).toUpper());
    avatar->setObjectName(QStringLiteral("issueAvatar"));
    avatar->setAlignment(Qt::AlignCenter);
    avatar->setFixedSize(36, 36);
    main->addWidget(avatar, 0, Qt::AlignTop);

    auto *stack = new QVBoxLayout;
    stack->setSpacing(4);
    auto *header = new QHBoxLayout;
    auto *who = new QLabel(QStringLiteral("<a href=\"fm-author:%1\"%2><b>%3</b> <span style=\"color:%4\">@%1</span></a>")
                               .arg(escaped(author), plain, escaped(name), muted));
    who->setTextFormat(Qt::RichText);
    connect(who, &QLabel::linkActivated, this, linkHandler);
    header->addWidget(who);
    header->addStretch();
    const qint64 created = post.value(QStringLiteral("createdAt")).toInteger();
    auto *when = new QLabel(QStringLiteral("<a href=\"fm-thread:%1\" style=\"color:%2; text-decoration:none\">%3</a>")
                                .arg(id, muted, relativeLabel(created, QDateTime::currentMSecsSinceEpoch())));
    when->setTextFormat(Qt::RichText);
    when->setToolTip(QLocale().toString(QDateTime::fromMSecsSinceEpoch(created), QLocale::LongFormat));
    connect(when, &QLabel::linkActivated, this, linkHandler);
    header->addWidget(when);
    stack->addLayout(header);

    if (!text(post, "replyTo").isEmpty()) {
        const QString parent = text(post, "replyToAuthor");
        auto *replying = new QLabel(parent.isEmpty()
            ? QStringLiteral("Replying to a removed post")
            : QStringLiteral("Replying to <a href=\"fm-author:%1\" style=\"color:%2; text-decoration:none\">@%1</a>")
                  .arg(escaped(parent), link));
        replying->setObjectName(QStringLiteral("mutedLabel"));
        replying->setTextFormat(Qt::RichText);
        connect(replying, &QLabel::linkActivated, this, linkHandler);
        stack->addWidget(replying);
    }

    auto *body = new QLabel(richText(text(post, "text"), link));
    body->setTextFormat(Qt::RichText);
    body->setWordWrap(true);
    body->setTextInteractionFlags(Qt::TextBrowserInteraction);
    body->setOpenExternalLinks(false);
    if (focused)
        body->setStyleSheet(QStringLiteral("font-size: 15px;"));
    connect(body, &QLabel::linkActivated, this, linkHandler);

    const QString spoiler = text(post, "spoiler");
    if (!spoiler.isEmpty()) {
        auto *warning = new QHBoxLayout;
        auto *label = new QLabel(spoiler);
        label->setWordWrap(true);
        warning->addWidget(label, 1);
        const bool shown = m_revealed.contains(id);
        auto *reveal = ghostButton(shown ? QStringLiteral("Show less") : QStringLiteral("Show more"));
        warning->addWidget(reveal, 0, Qt::AlignTop);
        stack->addLayout(warning);
        body->setVisible(shown);
        connect(reveal, &QPushButton::clicked, this, [this, id, body, reveal] {
            const bool show = !m_revealed.contains(id);
            if (show)
                m_revealed.insert(id);
            else
                m_revealed.remove(id);
            body->setVisible(show);
            reveal->setText(show ? QStringLiteral("Show less") : QStringLiteral("Show more"));
        });
    }
    stack->addWidget(body);

    auto *actions = new QHBoxLayout;
    actions->setSpacing(4);
    auto *reply = actionButton(QStringLiteral("↩ Reply"), count(post, "replies"), false, QString());
    const bool boosted = flag(post, "boosted");
    auto *boost = actionButton(boosted ? QStringLiteral("⟲ Boosted") : QStringLiteral("⟲ Boost"),
                               count(post, "boosts"), boosted, link);
    const bool favourited = flag(post, "favourited");
    auto *favourite = actionButton(favourited ? QStringLiteral("★ Favourited") : QStringLiteral("☆ Favourite"),
                                   count(post, "favourites"), favourited, amber);
    actions->addWidget(reply);
    actions->addWidget(boost);
    actions->addWidget(favourite);
    actions->addStretch();
    connect(reply, &QPushButton::clicked, this, [this, post] { startReply(post); });
    connect(boost, &QPushButton::clicked, this, [this, id, boosted] { toggle(id, QStringLiteral("boost"), boosted); });
    connect(favourite, &QPushButton::clicked, this,
            [this, id, favourited] { toggle(id, QStringLiteral("favourite"), favourited); });
    if (flag(post, "canDelete")) {
        auto *remove = ghostButton(QStringLiteral("Delete"));
        remove->setStyleSheet(QStringLiteral("color: %1;").arg(dark() ? QStringLiteral("#f85149") : QStringLiteral("#cf222e")));
        actions->addWidget(remove);
        connect(remove, &QPushButton::clicked, this, [this, id] { deletePost(id); });
    }
    stack->addLayout(actions);
    main->addLayout(stack, 1);
    column->addLayout(main);
    return row;
}

void InstanceFeedFeature::openLink(const QString &link)
{
    if (link.startsWith(QStringLiteral("fm-author:")))
        return showProfile(link.mid(10));
    if (link.startsWith(QStringLiteral("fm-thread:")))
        return showThread(link.mid(10));
    const QUrl url(link);
    if (url.scheme() == QLatin1String("https"))
        QDesktopServices::openUrl(url);
}

void InstanceFeedFeature::startReply(const QJsonObject &post)
{
    m_replyTo = text(post, "id");
    m_replyAuthor = text(post, "author");
    const QString mention = QLatin1Char('@') + m_replyAuthor;
    if (!m_text->toPlainText().startsWith(mention))
        m_text->setPlainText((mention + QLatin1Char(' ') + m_text->toPlainText()).trimmed() + QLatin1Char(' '));
    m_text->moveCursor(QTextCursor::End);
    m_text->setFocus();
    renderCompose();
}

void InstanceFeedFeature::submit()
{
    const QString value = m_text->toPlainText().trimmed();
    if (value.isEmpty() || value.size() > m_maxLength)
        return;
    m_postButton->setEnabled(false);
    const QString parent = m_replyTo;
    const QJsonObject body{
        {QStringLiteral("text"), value},
        {QStringLiteral("spoiler"), m_spoilerToggle->isChecked() ? m_spoiler->text().trimmed() : QString()},
        {QStringLiteral("replyTo"), parent},
    };
    request(QByteArrayLiteral("POST"), kApi + QStringLiteral("/posts"), body,
            [this, parent](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok) {
            renderCompose();
            return setStatus(QStringLiteral("Your post was not published: %1").arg(error), true);
        }
        m_text->clear();
        m_spoiler->clear();
        m_spoilerToggle->setChecked(false);
        m_replyTo.clear();
        m_replyAuthor.clear();
        setStatus(QString());
        if (!parent.isEmpty() && !(m_view == View::Thread && m_threadId == parent))
            return showThread(parent);
        if (m_view == View::Timeline && parent.isEmpty()) {
            m_posts.prepend(payload.value(QStringLiteral("post")).toObject());
            return render();
        }
        load();
    });
}

void InstanceFeedFeature::replacePost(const QJsonObject &post)
{
    const QString id = text(post, "id");
    for (int index = 0; index < m_posts.size(); ++index) {
        QJsonObject entry = m_posts.at(index).toObject();
        if (text(entry, "type") == QLatin1String("boost")) {
            if (text(entry.value(QStringLiteral("post")).toObject(), "id") == id) {
                entry.insert(QStringLiteral("post"), post);
                m_posts.replace(index, entry);
            }
        } else if (text(entry, "id") == id) {
            m_posts.replace(index, post);
        }
    }
    if (text(m_thread.value(QStringLiteral("post")).toObject(), "id") == id)
        m_thread.insert(QStringLiteral("post"), post);
    for (const char *key : {"ancestors", "replies"}) {
        QJsonArray list = m_thread.value(QLatin1String(key)).toArray();
        for (int index = 0; index < list.size(); ++index) {
            if (text(list.at(index).toObject(), "id") == id)
                list.replace(index, post);
        }
        if (m_thread.contains(QLatin1String(key)))
            m_thread.insert(QLatin1String(key), list);
    }
}

void InstanceFeedFeature::toggle(const QString &postId, const QString &kind, bool active)
{
    request(active ? QByteArrayLiteral("DELETE") : QByteArrayLiteral("POST"),
            kApi + QStringLiteral("/posts/%1/%2").arg(postId, kind), {},
            [this, kind](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("The post was not updated: %1").arg(error), true);
        if (kind == QLatin1String("boost") && m_view == View::Timeline)
            return load();
        replacePost(payload.value(QStringLiteral("post")).toObject());
        render();
    });
}

void InstanceFeedFeature::deletePost(const QString &postId)
{
    if (QMessageBox::warning(this, QStringLiteral("Delete post"),
                             QStringLiteral("Delete this post? Its favourites and boosts are removed with it."),
                             QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    request(QByteArrayLiteral("DELETE"), kApi + QStringLiteral("/posts/") + postId, {},
            [this, postId](bool ok, const QJsonObject &, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("The post was not deleted: %1").arg(error), true);
        setStatus(QStringLiteral("Post deleted."));
        if (m_view == View::Thread && m_threadId == postId)
            return showTimeline();
        load();
    });
}

void InstanceFeedFeature::exportMine()
{
    request(QByteArrayLiteral("GET"), kApi + QStringLiteral("/export"), {},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("The export failed: %1").arg(error), true);
        const QString suggested = text(payload, "filename").isEmpty()
            ? QStringLiteral("forkmesh-instance-feed.json") : text(payload, "filename");
        const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save the Feed export"),
                                                          suggested, QStringLiteral("JSON files (*.json)"));
        if (path.isEmpty())
            return;
        QSaveFile file(path);
        const QByteArray bytes = QJsonDocument(payload.value(QStringLiteral("export")).toObject())
                                     .toJson(QJsonDocument::Indented);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
            return setStatus(QStringLiteral("The export file could not be written."), true);
        setStatus(QStringLiteral("Exported to %1.").arg(path));
    });
}

bool InstanceFeedFeature::exportData(forkmesh::market::NativePackageReplyHandler done)
{
    if (m_checked && !m_installed)
        return false;
    request(QByteArrayLiteral("GET"), kApi + QStringLiteral("/export"), {},
            [done = std::move(done)](bool ok, const QJsonObject &payload, const QString &error) {
        QJsonObject result = payload;
        result.insert(QStringLiteral("hasData"), exportHasData(payload));
        done(ok, result, error);
    });
    return true;
}

void InstanceFeedFeature::setStatus(const QString &value, bool error)
{
    m_status->setText(value);
    m_status->setVisible(!value.isEmpty());
    m_status->setObjectName(error ? QStringLiteral("errorLabel") : QStringLiteral("mutedLabel"));
    m_status->style()->unpolish(m_status);
    m_status->style()->polish(m_status);
}

} // namespace forkmesh::instancefeed

namespace {
const forkmesh::market::NativePackageRegistration instanceFeedRegistration({
    QStringLiteral("builtin:instance-feed"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Feed"),
    QStringLiteral("comment"),
    QStringLiteral("/api/instance-feed"),
    QStringLiteral("forkmesh-instance-feed-request-v1"),
    [](const forkmesh::market::NativePackageHost &host, QWidget *parent) {
        return new forkmesh::instancefeed::InstanceFeedFeature(host, parent);
    },
});
} // namespace
