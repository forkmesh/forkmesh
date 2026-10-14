#include "MessageRow.h"
#include "DesignTokens.h"
#include "Theme.h"
#include "ChatRecipients.h"

#include "ChatBlocks.h"
#include "ChatFormat.h"
#include "ChatLinkText.h"
#include "ReactionEmoji.h"

#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QCursor>
#include <QDateTime>
#include <QEnterEvent>
#include <QEvent>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QMovie>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QStyleHints>
#include <QTimer>
#include <QToolTip>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

constexpr int kAvatarSize = 40;
constexpr int kGroupGapPx = 14;
constexpr int kContinuationGapPx = 1;
constexpr int kMaxMediaWidth = 360;
constexpr int kPickerColumns = 6;

QString humanSize(qint64 bytes)
{
    if (bytes < 1024)
        return QString::number(bytes) + " B";
    if (bytes < 1024 * 1024)
        return QString::number(bytes / 1024.0, 'f', 1) + " KB";
    return QString::number(bytes / (1024.0 * 1024.0), 'f', 1) + " MB";
}

// Stand-in first frame for a video attachment: a dark plate with a play badge.
// Decoding the real frame would need Qt Multimedia, which is optional here, so
// every build shows the same "this is playable" affordance.
QPixmap videoPosterPixmap(const QSize &size, qreal dpr)
{
    QPixmap poster(qMax(1, qRound(size.width() * dpr)),
                   qMax(1, qRound(size.height() * dpr)));
    poster.setDevicePixelRatio(dpr);
    poster.fill(Qt::transparent);
    QPainter painter(&poster);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF frame(0.5, 0.5, size.width() - 1.0, size.height() - 1.0);
    painter.setPen(QPen(QColor(0x2a, 0x2f, 0x37)));
    painter.setBrush(QColor(0x0c, 0x0e, 0x12));
    painter.drawRoundedRect(frame, 10, 10);

    const qreal badgeSide = qMin(frame.width(), frame.height()) * 0.32;
    const QRectF badge(frame.center().x() - badgeSide / 2,
                       frame.center().y() - badgeSide / 2, badgeSide, badgeSide);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0xff, 0xff, 0xff, 0x2e));
    painter.drawEllipse(badge);

    const qreal glyph = badgeSide * 0.38;
    QPainterPath triangle;
    triangle.moveTo(badge.center().x() - glyph * 0.42, badge.center().y() - glyph);
    triangle.lineTo(badge.center().x() + glyph * 0.86, badge.center().y());
    triangle.lineTo(badge.center().x() - glyph * 0.42, badge.center().y() + glyph);
    triangle.closeSubpath();
    painter.setBrush(QColor(0xf4, 0xf6, 0xfa));
    painter.drawPath(triangle);
    return poster;
}

QString mentionDisplayName(const MemberInfo &member)
{
    const QString owner = member.ownerUser.trimmed();
    if (!owner.isEmpty())
        return owner;
    const QString name = member.name.trimmed();
    if (!name.isEmpty())
        return name;
    return member.nodeName.trimmed();
}

QString mentionHrefKey(const QString &href)
{
    static const QString prefix = QStringLiteral("forkmesh-mention:");
    if (!href.startsWith(prefix))
        return {};
    return href.mid(prefix.size()).trimmed().toLower();
}

QString mentionCardHtml(const MemberInfo &member)
{
    const QString name = mentionDisplayName(member).toHtmlEscaped();
    QString detail;
    const QString note = member.note.trimmed();
    const QString nodes = member.nodeName.trimmed();
    if (!note.isEmpty()) {
        detail = note;
    } else if (!nodes.isEmpty()) {
        detail = QStringLiteral("Nodes: ") + nodes;
    } else {
        detail = member.online ? QStringLiteral("Online")
                               : QStringLiteral("User profile");
    }

    return QStringLiteral(
               "<div style='white-space:nowrap'>"
               "<b>%1</b><br>"
               "<span style='color:#8b949e'>%2</span><br>"
               "<span style='color:%3'>Click to open full profile</span>"
               "</div>")
        .arg(name, detail.toHtmlEscaped(),
             Theme::nextOrHex(Theme::Role::AccentFg, QStringLiteral("#58a6ff")));
}

// A message body as rich text: @mentions of people in this conversation, the
// links written into the sentence, and escaped plain text everywhere else.
bool themeIsDark();

QString renderMentionedText(const QString &text,
                            const QHash<QString, MemberInfo> &profiles)
{
    return forkmesh::chatformat::toHtml(
        text,
        [&profiles](const QString &handle) {
            const QString key = handle.toLower();
            return profiles.contains(key) ? key : QString();
        },
        themeIsDark());
}

// What a link's tooltip says: where it actually goes. A [label](target) link
// hides its destination inside the sentence, so the target is worth seeing
// before the click rather than after it.
QString linkTargetTooltip(const QString &href)
{
    constexpr int kMaxTooltipChars = 120;
    const QString target = href.trimmed();
    if (target.size() <= kMaxTooltipChars)
        return target;
    return target.left(kMaxTooltipChars - 1) + QString::fromUtf8("\xE2\x80\xA6");
}

// -1 unknown, 0 light, 1 dark. Every ExpiryRing paint used to build a
// QSettings to read the theme preference; with a ring per message row that
// was one settings-file parse per repaint per row. A theme change reaches
// every widget as a style/palette change event, which is where the cache is
// dropped (see ExpiryRing::event).
static int g_messageRowThemeDark = -1;

bool themeIsDark()
{
    if (g_messageRowThemeDark >= 0)
        return g_messageRowThemeDark == 1;
    const QString pref =
        QSettings().value(QStringLiteral("app/theme"), "system").toString();
    bool dark;
    if (pref == "light")
        dark = false;
    else if (pref == "dark")
        dark = true;
    else {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
        dark = QGuiApplication::styleHints()->colorScheme() != Qt::ColorScheme::Light;
#else
        dark = true;
#endif
    }
    g_messageRowThemeDark = dark ? 1 : 0;
    return dark;
}

QTimer *expiryRingTicker()
{
    static QTimer *timer = [] {
        auto *t = new QTimer;
        t->start(60 * 1000);
        return t;
    }();
    return timer;
}

class ExpiryRing : public QWidget
{
public:
    ExpiryRing(qint64 timestampMs, const QString &conversation,
               QWidget *parent = nullptr)
        : QWidget(parent), m_timestampMs(timestampMs),
          m_conversation(conversation)
    {
        setObjectName(QStringLiteral("messageExpiryRing"));
        setFixedSize(kDiameter, kDiameter);
        refreshTooltip();
        connect(expiryRingTicker(), &QTimer::timeout, this, [this] {
            refreshTooltip();
            update();
        });
    }

protected:
    bool event(QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::StyleChange:
        case QEvent::PaletteChange:
        case QEvent::ApplicationPaletteChange:
        case QEvent::ThemeChange:
            g_messageRowThemeDark = -1; // re-read the preference on next paint
            break;
        default:
            break;
        }
        return QWidget::event(event);
    }

    void paintEvent(QPaintEvent *) override
    {
        const qint64 elapsed =
            QDateTime::currentMSecsSinceEpoch() - m_timestampMs;
        const double frac = qBound(
            0.0, double(elapsed) /
                     double(forkmesh::chatretention::retentionMsFor(
                         m_conversation)), 1.0);

        const bool dark = themeIsDark();
        QRectF box(1, 1, kDiameter - 2, kDiameter - 2);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(Theme::isNext()
                                ? Theme::chatColor(QStringLiteral("chatFaint"))
                                : Theme::remapColor(QColor(dark ? "#30363d" : "#d0d7de")),
                            1.2));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(box);
        if (frac > 0.004) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(Theme::isNext()
                                 ? Theme::chatColor(QStringLiteral("chatMuted"))
                                 : Theme::remapColor(QColor(dark ? "#8b949e" : "#9a6700")));
            painter.drawPie(box, 90 * 16, -int(frac * 360.0 * 16));
        }
    }

private:
    void refreshTooltip()
    {
        const qint64 remainingMs =
            qMax<qint64>(0, m_timestampMs +
                                forkmesh::chatretention::retentionMsFor(
                                    m_conversation) -
                                QDateTime::currentMSecsSinceEpoch());
        const qint64 days = remainingMs / (24 * 60 * 60 * 1000);
        const qint64 hours = remainingMs / (60 * 60 * 1000);
        const qint64 minutes = remainingMs / (60 * 1000);
        QString text;
        if (days >= 1)
            text = QString("Disappears in %1 day%2")
                       .arg(days)
                       .arg(days == 1 ? "" : "s");
        else if (hours >= 1)
            text = QString("Disappears in %1 hour%2")
                       .arg(hours)
                       .arg(hours == 1 ? "" : "s");
        else if (minutes >= 1)
            text = QString("Disappears in %1 minute%2")
                       .arg(minutes)
                       .arg(minutes == 1 ? "" : "s");
        else
            text = QStringLiteral("Disappears soon");
        setToolTip(text);
    }

    qint64 m_timestampMs;
    QString m_conversation;
    static constexpr int kDiameter = 10;
};

} // namespace

QPixmap MessageRow::initialsAvatar(const QString &name, const QString &color,
                                   int side)
{
    side = qMax(8, side);
    const qreal dpr = qGuiApp ? qGuiApp->devicePixelRatio() : 1.0;
    QPixmap pixmap(qMax(1, qRound(side * dpr)), qMax(1, qRound(side * dpr)));
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(QColor(color));
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(0, 0, side, side);
    painter.setPen(Qt::white);
    QFont font = painter.font();
    font.setBold(true);
    font.setPixelSize(qMax(8, qRound(side * (16.0 / 36.0))));
    painter.setFont(font);
    const QString initial = name.isEmpty() ? "?" : name.left(1).toUpper();
    painter.drawText(QRect(0, 0, side, side), Qt::AlignCenter, initial);
    return pixmap;
}

namespace {

// The repliers' faces as one overlapping pile, newest on the left, drawn on a
// QPushButton icon because a button carries only a single pixmap.
QPixmap threadFacePile(const QList<QPixmap> &faces, int side, int overlap)
{
    if (faces.isEmpty())
        return QPixmap();
    const qreal dpr = qGuiApp ? qGuiApp->devicePixelRatio() : 1.0;
    const int step = qMax(1, side - overlap);
    const int width = side + step * (faces.size() - 1);
    QPixmap pile(qMax(1, qRound(width * dpr)), qMax(1, qRound(side * dpr)));
    pile.setDevicePixelRatio(dpr);
    pile.fill(Qt::transparent);
    QPainter painter(&pile);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    // Right to left, so each face laps over the one after it rather than
    // being clipped by it.
    for (int index = faces.size() - 1; index >= 0; --index) {
        const QPixmap &face = faces.at(index);
        if (face.isNull())
            continue;
        painter.drawPixmap(QRectF(index * step, 0, side, side), face,
                           QRectF(QPointF(0, 0), face.size()));
    }
    return pile;
}

QString threadCountLabel(int count)
{
    return QString::number(count) + (count == 1 ? " reply" : " replies");
}

} // namespace

MessageRow::MessageRow(const ChatMessage &message, const QString &nameColor,
                       const QHash<QString, MemberInfo> &mentionProfiles,
                       bool canModerate, QWidget *parent,
                       const ThreadReplySummary &threadSummary,
                       bool threadContext)
    : QFrame(parent), m_message(message), m_nameColor(nameColor),
      m_mentionProfiles(mentionProfiles)
{
    setObjectName("messageRow");

    auto *outer = new QHBoxLayout(this);
    m_outer = outer;
    if (Theme::isNext()) {
        outer->setContentsMargins(16, kGroupGapPx, 16, 2);
        outer->setSpacing(16);
    } else {
        outer->setContentsMargins(18, 7, 18, 7);
        outer->setSpacing(10);
    }

    m_avatarLabel = new QLabel;
    m_avatarLabel->setFixedSize(kAvatarSize, kAvatarSize);
    m_avatarLabel->setPixmap(initialsAvatar(message.senderName, nameColor));
    m_avatarLabel->setCursor(Qt::PointingHandCursor);
    m_avatarLabel->setToolTip("View user profile");
    m_avatarLabel->installEventFilter(this);
    outer->addWidget(m_avatarLabel, 0, Qt::AlignTop);

    auto *column = new QVBoxLayout;
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(3);

    auto *header = new QLabel;
    header->setTextFormat(Qt::RichText);
    header->setObjectName(QStringLiteral("messageSender"));
    header->setCursor(Qt::PointingHandCursor);
    header->installEventFilter(this);
    m_senderLabel = header;
    renderSenderHeader();
    auto *headerRow = new QHBoxLayout;
    headerRow->setContentsMargins(0, 0, 0, 0);
    headerRow->setSpacing(6);
    headerRow->addWidget(header);
    m_headerRow = headerRow;
    if (message.recipientCount >= 0) {
        m_message.recipientCount = -1;
        setRecipientCount(message.recipientCount, message.recipientNames);
    }
    if (!message.deleted) {
        m_expiryRing = new ExpiryRing(message.timestampMs, message.conversation);
        headerRow->addWidget(m_expiryRing, 0, Qt::AlignVCenter);
    }
    headerRow->addStretch();
    if (!message.deleted) {
        const bool discordReply =
            message.conversation.startsWith(QStringLiteral("#discord/"));
        const bool showThreadReply =
            !threadContext &&
            (message.threadRootId.isEmpty() || discordReply) &&
            message.conversation.startsWith(QLatin1Char('#'));
        const bool showCopy = !message.text.isEmpty();
        const bool showCreateIssue = !message.text.isEmpty();
        const bool showEdit = message.self && !message.text.isEmpty();
        const bool showModerateDelete = canModerate;
        const bool showSelfDelete = !canModerate && message.self;
        if (showThreadReply || showCopy || showCreateIssue || showEdit ||
            showModerateDelete || showSelfDelete) {
            auto *menuButton = new QToolButton;
            menuButton->setObjectName("messageAction");
            menuButton->setText(QString::fromUtf8("\xE2\x8B\xAF")); // "⋯"
            menuButton->setCursor(Qt::PointingHandCursor);
            menuButton->setToolTip("More actions");
            menuButton->setPopupMode(QToolButton::InstantPopup);
            auto *menu = new QMenu(menuButton);
            if (showThreadReply) {
                QAction *replyAction = menu->addAction(
                    discordReply ? QStringLiteral("Reply on Discord")
                                 : QStringLiteral("Reply in thread"));
                connect(replyAction, &QAction::triggered, this,
                        [this] { emit threadRequested(m_message.id); });
            }
            if (showCopy) {
                QAction *copyAction = menu->addAction("Copy");
                connect(copyAction, &QAction::triggered, this,
                        [this] { QApplication::clipboard()->setText(m_message.text); });
                QAction *sendToPromptAction = menu->addAction("Send to Prompt");
                connect(sendToPromptAction, &QAction::triggered, this,
                        [this] { emit sendToPromptRequested(m_message.text); });
            }
            if (showCreateIssue) {
                QAction *issueAction = menu->addAction("Create issue\xE2\x80\xA6");
                connect(issueAction, &QAction::triggered, this, [this] {
                    emit createIssueRequested(m_message.text,
                                              m_message.senderName,
                                              m_message.timestampMs);
                });
            }
            if (showEdit) {
                QAction *editAction = menu->addAction("Edit");
                connect(editAction, &QAction::triggered, this, [this] {
                    emit editRequested(m_message.id, m_message.text);
                });
            }
            if (showModerateDelete) {
                QAction *delAction = menu->addAction("Delete (admin)");
                connect(delAction, &QAction::triggered, this,
                        [this] { emit moderateDeleteRequested(m_message.id); });
            } else if (showSelfDelete) {
                QAction *delAction = menu->addAction("Delete");
                connect(delAction, &QAction::triggered, this,
                        [this] { emit deleteRequested(m_message.id); });
            }
            menuButton->setMenu(menu);
            headerRow->addWidget(menuButton);
            m_menuButton = menuButton;
        }
    }
    column->addLayout(headerRow);

    if (message.deleted) {
        auto *body = new QLabel(
            QStringLiteral("<span style='color:%1'><i>Message deleted</i></span>")
                .arg(Theme::nextOrHex(Theme::Role::FgMuted,
                                      QStringLiteral("#9ca3af"))));
        body->setObjectName("messageText");
        body->setTextFormat(Qt::RichText);
        column->addWidget(body);
    } else if (!message.text.isEmpty()) {
        auto *body = new QLabel(
            message.blocks.isEmpty() ||
                    !forkmesh::chatblocks::senderMayFormat(message.senderId)
                ? renderMentionedText(message.text, m_mentionProfiles)
                : forkmesh::chatblocks::html(message.blocks, themeIsDark()));
        body->setObjectName("messageText");
        body->setTextFormat(Qt::RichText);
        body->setWordWrap(true);
        body->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                      Qt::LinksAccessibleByMouse);
        body->setOpenExternalLinks(false);
        connect(body, &QLabel::linkHovered, this, [this, body](const QString &href) {
            if (!href.isEmpty() && !ChatLinkText::isMentionHref(href)) {
                QToolTip::showText(QCursor::pos(), linkTargetTooltip(href), body);
                return;
            }
            const QString key = mentionHrefKey(href);
            if (key.isEmpty() || !m_mentionProfiles.contains(key)) {
                QToolTip::hideText();
                return;
            }
            QToolTip::showText(QCursor::pos(),
                               mentionCardHtml(m_mentionProfiles.value(key)), body);
        });
        connect(body, &QLabel::linkActivated, this, [this](const QString &href) {
            if (!href.isEmpty() && !ChatLinkText::isMentionHref(href)) {
                // Re-checked rather than trusted: the anchor was built from a
                // sanitized target, but only this signal reaches the browser.
                const QString target = ChatLinkText::sanitizedHref(href);
                if (!target.isEmpty())
                    emit linkActivated(target);
                return;
            }
            const QString key = mentionHrefKey(href);
            if (key.isEmpty() || !m_mentionProfiles.contains(key))
                return;
            const MemberInfo member = m_mentionProfiles.value(key);
            emit mentionClicked(member.id, mentionDisplayName(member));
        });
        column->addWidget(body);
    }

    if (!message.deleted && message.hasFile())
        buildAttachment(this, column);

    if (!threadContext && !threadSummary.isEmpty()) {
        // Lead with who replied — the faces of the repliers and the name on the
        // newest reply — and keep the count as the trailing detail.
        const QString count = threadCountLabel(threadSummary.count);
        auto *thread = new QPushButton(
            threadSummary.latestSender.isEmpty()
                ? count
                : threadSummary.latestSender + "  " + count);
        thread->setObjectName("threadSummary");
        thread->setCursor(Qt::PointingHandCursor);
        const QPixmap pile =
            threadFacePile(threadSummary.faces, kThreadSummaryFaceSide, 6);
        if (!pile.isNull()) {
            thread->setIcon(QIcon(pile));
            thread->setIconSize(pile.deviceIndependentSize().toSize());
        }
        thread->setToolTip(
            threadSummary.latestSender.isEmpty()
                ? QStringLiteral("Open thread")
                : QString::fromUtf8("Open thread \xE2\x80\x94 %1, last reply from %2")
                      .arg(count, threadSummary.latestSender));
        connect(thread, &QPushButton::clicked, this,
                [this] { emit threadRequested(m_message.id); });
        column->addWidget(thread, 0, Qt::AlignLeft);
    }

    m_reactionsBar = nullptr;
    if (!message.deleted) {
        auto *reactionsRow = new QWidget;
        reactionsRow->setObjectName(QStringLiteral("messageReactions"));
        m_reactionsBar = new QHBoxLayout(reactionsRow);
        m_reactionsBar->setContentsMargins(0, 2, 0, 0);
        m_reactionsBar->setSpacing(4);
        auto *addReaction = new QPushButton;
        addReaction->setObjectName("reactionAdd");
        addReaction->setIcon(QIcon(reactions::addGlyph(
            16, devicePixelRatio(), QColor(0x8b, 0x94, 0x9e))));
        addReaction->setIconSize(QSize(16, 16));
        addReaction->setCursor(Qt::PointingHandCursor);
        addReaction->setToolTip("Add reaction");
        connect(addReaction, &QPushButton::clicked, this,
                &MessageRow::showReactionPicker);
        m_reactionsBar->addWidget(addReaction);
        m_reactionsBar->addStretch();
        column->addWidget(reactionsRow);
        m_reactionsRow = reactionsRow;
        if (Theme::isNext()) {
            reactionsRow->hide();
            m_hoverReactButton = new QPushButton(this);
            m_hoverReactButton->setObjectName(QStringLiteral("messageAction"));
            m_hoverReactButton->setIcon(addReaction->icon());
            m_hoverReactButton->setIconSize(QSize(16, 16));
            m_hoverReactButton->setCursor(Qt::PointingHandCursor);
            m_hoverReactButton->setToolTip(QStringLiteral("Add reaction"));
            m_hoverReactButton->setFixedSize(24, 22);
            m_hoverReactButton->hide();
            connect(m_hoverReactButton, &QPushButton::clicked, this,
                    &MessageRow::showReactionPicker);
        }
    }

    outer->addLayout(column, 1);
}

void MessageRow::buildAttachment(QWidget *, QVBoxLayout *layout)
{
    const QString mime = m_message.fileMime;
    const QByteArray &data = m_message.fileData;

    auto makeClickable = [this](QLabel *label) {
        m_imageLabel = label;
        label->setCursor(Qt::PointingHandCursor);
        label->setToolTip(QStringLiteral("Click to view full size"));
        label->installEventFilter(this);
    };

    if (mime == "image/gif") {
        auto *label = new QLabel;
        auto *buffer = new QBuffer(label);
        buffer->setData(data);
        buffer->open(QIODevice::ReadOnly);
        auto *movie = new QMovie(buffer, QByteArray(), label);
        if (movie->isValid()) {
            movie->jumpToFrame(0);
            QSize size = movie->currentImage().size();
            if (size.width() > kMaxMediaWidth)
                size.scale(kMaxMediaWidth, kMaxMediaWidth * size.height() /
                                               qMax(1, size.width()),
                           Qt::KeepAspectRatio);
            movie->setScaledSize(size);
            label->setMovie(movie);
            movie->start();
            makeClickable(label);
            layout->addWidget(label);
            return;
        }
    } else if (mime.startsWith("image/")) {
        QPixmap pixmap;
        if (pixmap.loadFromData(data)) {
            if (pixmap.width() > kMaxMediaWidth)
                pixmap = pixmap.scaledToWidth(kMaxMediaWidth,
                                              Qt::SmoothTransformation);
            auto *label = new QLabel;
            label->setPixmap(pixmap);
            makeClickable(label);
            layout->addWidget(label);
            return;
        }
    } else if (mime.startsWith("video/") && !data.isEmpty()) {
        // Videos open in a player rather than decoding inline: a transcript can
        // hold dozens of them, and only the one someone actually clicks should
        // cost a decoder.
        auto *card = new QWidget;
        card->setObjectName("videoCard");
        auto *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(0, 0, 0, 0);
        cardLayout->setSpacing(6);

        const QSize posterSize(kMaxMediaWidth, kMaxMediaWidth * 9 / 16);
        auto *poster = new QLabel;
        poster->setObjectName("videoPoster");
        poster->setFixedSize(posterSize);
        poster->setPixmap(videoPosterPixmap(posterSize, devicePixelRatio()));
        poster->setCursor(Qt::PointingHandCursor);
        poster->setToolTip(QStringLiteral("Click to play"));
        poster->installEventFilter(this);
        m_videoLabel = poster;
        cardLayout->addWidget(poster);

        auto *footer = new QWidget;
        auto *footerLayout = new QHBoxLayout(footer);
        footerLayout->setContentsMargins(2, 0, 2, 0);
        footerLayout->setSpacing(8);
        auto *name = new QLabel(m_message.fileName.toHtmlEscaped() + "  <span "
                                "style='color:" +
                                Theme::nextOrHex(Theme::Role::FgMuted,
                                                 QStringLiteral("#9ca3af")) +
                                "'>(" +
                                humanSize(data.size()) + ")</span>");
        name->setTextFormat(Qt::RichText);
        auto *play = new QPushButton(QStringLiteral("Play"));
        play->setObjectName("videoPlayButton");
        play->setCursor(Qt::PointingHandCursor);
        connect(play, &QPushButton::clicked, this, [this] {
            emit videoActivated(m_message.fileName, m_message.fileMime,
                                m_message.fileData);
        });
        auto *saveVideo = new QPushButton(QStringLiteral("Save"));
        saveVideo->setObjectName("ghostButton");
        saveVideo->setCursor(Qt::PointingHandCursor);
        connect(saveVideo, &QPushButton::clicked, this, [this] {
            emit saveFileRequested(m_message.fileName, m_message.fileData);
        });
        footerLayout->addWidget(name, 1);
        footerLayout->addWidget(play);
        footerLayout->addWidget(saveVideo);
        cardLayout->addWidget(footer);

        layout->addWidget(card);
        return;
    }

    auto *chip = new QWidget;
    chip->setObjectName("fileChip");
    auto *chipLayout = new QHBoxLayout(chip);
    chipLayout->setContentsMargins(10, 8, 10, 8);
    chipLayout->setSpacing(8);
    auto *icon = new QLabel("File");
    auto *name = new QLabel(m_message.fileName.toHtmlEscaped() + "  <span "
                            "style='color:" +
                            Theme::nextOrHex(Theme::Role::FgMuted,
                                             QStringLiteral("#9ca3af")) +
                            "'>(" +
                            humanSize(data.size()) + ")</span>");
    name->setTextFormat(Qt::RichText);
    auto *save = new QPushButton("Save");
    save->setObjectName("ghostButton");
    save->setCursor(Qt::PointingHandCursor);
    connect(save, &QPushButton::clicked, this, [this] {
        emit saveFileRequested(m_message.fileName, m_message.fileData);
    });
    chipLayout->addWidget(icon);
    chipLayout->addWidget(name, 1);
    chipLayout->addWidget(save);
    layout->addWidget(chip);
}

void MessageRow::setSenderStatus(const QString &emoji, const QString &tooltip)
{
    if (emoji == m_senderStatusEmoji && tooltip == m_senderStatusTooltip)
        return;
    m_senderStatusEmoji = emoji;
    m_senderStatusTooltip = tooltip;
    renderSenderHeader();
}

void MessageRow::setDeliveryNote(const QString &note, const QString &tooltip)
{
    if (note == m_deliveryNote && tooltip == m_deliveryTooltip)
        return;
    m_deliveryNote = note;
    m_deliveryTooltip = tooltip;
    renderSenderHeader();
}

void MessageRow::setRecipientCount(int count, const QStringList &names)
{
    if (count < 0 || !m_headerRow ||
        (count == m_message.recipientCount &&
         (names.isEmpty() || names == m_message.recipientNames)))
        return;
    m_message.recipientCount = count;
    if (!names.isEmpty())
        m_message.recipientNames = names;
    if (!forkmesh::recipients::showsRecipientCount(m_message.conversation))
        return;
    if (!m_recipientsLabel) {
        m_recipientsLabel = new QLabel;
        m_recipientsLabel->setObjectName(QStringLiteral("messageRecipients"));
        m_recipientsLabel->setTextFormat(Qt::RichText);
        m_headerRow->insertWidget(m_headerRow->indexOf(m_senderLabel) + 1,
                                  m_recipientsLabel, 0, Qt::AlignBottom);
    }
    const QString tooltip =
        forkmesh::recipients::tooltip(count, m_message.recipientNames);
    m_recipientsLabel->setText(
        QStringLiteral("<span style='color:%1; font-size:11px'>")
                .arg(Theme::nextOrHex(Theme::Role::FgMuted,
                                      QStringLiteral("#6b7280"))) +
        forkmesh::recipients::label(count) + QStringLiteral("</span>"));
    m_recipientsLabel->setToolTip(tooltip);
    m_recipientsLabel->setAccessibleName(tooltip);
}

QString MessageRow::chatStampLabel(const QDateTime &sent)
{
    const QDate today = QDate::currentDate();
    const QString time = sent.toString(QStringLiteral("h:mm AP"));
    if (sent.date() == today)
        return time;
    if (sent.date() == today.addDays(-1))
        return QStringLiteral("Yesterday at ") + time;
    return sent.toString(QStringLiteral("M/d/yy, h:mm AP"));
}

QString MessageRow::chatDayLabel(const QDate &day)
{
    return day.toString(QStringLiteral("MMMM d, yyyy"));
}

qint64 MessageRow::timestampMs() const
{
    return m_message.timestampMs;
}

bool MessageRow::continues(const MessageRow *previous) const
{
    if (!previous || m_message.deleted || previous->m_message.deleted ||
        !m_message.fileName.isEmpty() || !previous->m_message.fileName.isEmpty())
        return false;
    if (m_message.senderId != previous->m_message.senderId ||
        m_message.senderName != previous->m_message.senderName)
        return false;
    const QDateTime mine = QDateTime::fromMSecsSinceEpoch(m_message.timestampMs);
    const QDateTime theirs =
        QDateTime::fromMSecsSinceEpoch(previous->m_message.timestampMs);
    return mine.date() == theirs.date() &&
           m_message.timestampMs - previous->m_message.timestampMs >= 0 &&
           m_message.timestampMs - previous->m_message.timestampMs <=
               kContinuationWindowMs;
}

void MessageRow::setContinuation(bool continuation)
{
    if (!Theme::isNext() || m_continuation == continuation)
        return;
    m_continuation = continuation;
    if (m_outer)
        m_outer->setContentsMargins(16, continuation ? kContinuationGapPx : kGroupGapPx,
                                    16, 2);
    m_avatarLabel->setFixedHeight(continuation ? 0 : kAvatarSize);
    if (m_senderLabel)
        m_senderLabel->setVisible(!continuation);
    if (m_expiryRing)
        m_expiryRing->setVisible(!continuation);
    if (m_menuButton) {
        if (continuation) {
            m_headerRow->removeWidget(m_menuButton);
            m_menuButton->setParent(this);
            m_menuButton->hide();
        } else {
            m_headerRow->addWidget(m_menuButton);
            m_menuButton->show();
        }
    }
    setToolTip(continuation
                   ? QDateTime::fromMSecsSinceEpoch(m_message.timestampMs)
                         .toString(QStringLiteral("dddd, MMMM d, yyyy  h:mm AP"))
                   : QString());
}

void MessageRow::enterEvent(QEnterEvent *event)
{
    int right = width() - 16;
    int top = 0;
    if (m_continuation && m_menuButton) {
        m_menuButton->adjustSize();
        m_menuButton->move(width() - m_menuButton->width() - 16, 0);
        m_menuButton->raise();
        m_menuButton->show();
    }
    if (m_menuButton) {
        const QPoint at = m_menuButton->mapTo(this, QPoint(0, 0));
        right = at.x() - 4;
        top = at.y() + (m_menuButton->height() - 22) / 2;
    }
    if (m_hoverReactButton && m_reactionsRow && m_reactionsRow->isHidden()) {
        m_hoverReactButton->move(right - m_hoverReactButton->width(), qMax(0, top));
        m_hoverReactButton->raise();
        m_hoverReactButton->show();
    }
    QFrame::enterEvent(event);
}

void MessageRow::leaveEvent(QEvent *event)
{
    if (m_continuation && m_menuButton && !m_menuButton->isDown() &&
        !(m_menuButton->menu() && m_menuButton->menu()->isVisible()))
        m_menuButton->hide();
    if (m_hoverReactButton)
        m_hoverReactButton->hide();
    QFrame::leaveEvent(event);
}

// The author line: name, the sender's status emoji right after it, then the
// time. Hovering it names the status (and when it clears) above the full
// timestamp.
void MessageRow::renderSenderHeader()
{
    if (!m_senderLabel)
        return;
    const QDateTime sent = QDateTime::fromMSecsSinceEpoch(m_message.timestampMs);
    const QString time = Theme::isNext() ? chatStampLabel(sent)
                                         : sent.toString("h:mm ap");
    const QString status =
        m_senderStatusEmoji.isEmpty()
            ? QString()
            : QStringLiteral("&nbsp;") + m_senderStatusEmoji.toHtmlEscaped();
    m_senderLabel->setText(
        "<span style='color:" +
        (Theme::isNext() ? Theme::chatColor(QStringLiteral("chatTextStrong")).name()
                         : m_nameColor) +
        "; font-weight:" + (Theme::isNext() ? "600" : "700") + "'>" +
        m_message.senderName.toHtmlEscaped() + "</span>" + status +
        "&nbsp;&nbsp;<span style='color:" +
        (Theme::isNext() ? Theme::chatColor(QStringLiteral("chatFaint")).name()
                         : QStringLiteral("#6b7280")) +
        "; font-size:" + (Theme::isNext() ? "12px" : "11px") + "'>" + time +
        (m_message.edited ? " (edited)" : "") + "</span>" +
        (m_deliveryNote.isEmpty()
             ? QString()
             : "&nbsp;&nbsp;<span style='color:#d97706; font-size:11px'>" +
                   m_deliveryNote.toHtmlEscaped() + "</span>"));
    const QString stamp = sent.toString("dddd, MMMM d, yyyy  h:mm:ss AP");
    QString tooltip = m_senderStatusTooltip.isEmpty()
                          ? stamp
                          : m_senderStatusTooltip + QStringLiteral("\n\n") +
                                stamp;
    if (!m_deliveryTooltip.isEmpty())
        tooltip = m_deliveryTooltip + QStringLiteral("\n\n") + tooltip;
    m_senderLabel->setToolTip(tooltip);
}

bool MessageRow::eventFilter(QObject *watched, QEvent *event)
{
    if ((watched == m_avatarLabel || watched == m_senderLabel) &&
        event->type() == QEvent::MouseButtonRelease) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton) {
            emit senderClicked(m_message.senderId, m_message.senderName);
            return true;
        }
    }
    if (watched == m_imageLabel &&
        event->type() == QEvent::MouseButtonRelease) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton) {
            emit imageActivated(m_message.fileName, m_message.fileData);
            return true;
        }
    }
    if (watched == m_videoLabel &&
        event->type() == QEvent::MouseButtonRelease) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton) {
            emit videoActivated(m_message.fileName, m_message.fileMime,
                                m_message.fileData);
            return true;
        }
    }
    return QFrame::eventFilter(watched, event);
}

void MessageRow::setAvatar(const QPixmap &pixmap)
{
    if (pixmap.isNull())
        return;
    const qreal dpr = devicePixelRatio();
    QPixmap rounded(qMax(1, qRound(kAvatarSize * dpr)),
                    qMax(1, qRound(kAvatarSize * dpr)));
    rounded.setDevicePixelRatio(dpr);
    rounded.fill(Qt::transparent);
    QPainter painter(&rounded);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    QPainterPath clip;
    clip.addEllipse(0, 0, kAvatarSize, kAvatarSize);
    painter.setClipPath(clip);
    QPixmap scaled = pixmap.scaled(rounded.width(), rounded.height(),
                                   Qt::KeepAspectRatioByExpanding,
                                   Qt::SmoothTransformation);
    scaled.setDevicePixelRatio(dpr);
    painter.drawPixmap(0, 0, scaled);
    m_avatarLabel->setPixmap(rounded);
}

void MessageRow::setReactions(const QMap<QString, QStringList> &reactionMap)
{
    if (!m_reactionsBar)
        return;
    if (m_reactionsRow && Theme::isNext())
        m_reactionsRow->setVisible(!reactionMap.isEmpty());
    while (m_reactionsBar->count() > 2) {
        QLayoutItem *item = m_reactionsBar->takeAt(0);
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }
    int insertAt = 0;
    for (auto it = reactionMap.constBegin(); it != reactionMap.constEnd();
         ++it) {
        const QString reaction = it.key();
        const int count = it.value().size();
        auto *chip = new QPushButton(QString::number(count));
        chip->setObjectName("reactionChip");
        const QPixmap emoji =
            reactions::emojiPixmap(reaction, 16, devicePixelRatio());
        if (!emoji.isNull()) {
            chip->setIcon(QIcon(emoji));
            chip->setIconSize(QSize(16, 16));
        } else {
            chip->setText(reactions::displayName(reaction) + " " +
                          QString::number(count));
        }
        chip->setCursor(Qt::PointingHandCursor);
        chip->setToolTip(reactions::displayName(reaction) +
                         QString::fromUtf8(" \xC2\xB7 ") +
                         it.value().join(", "));
        connect(chip, &QPushButton::clicked, this,
                [this, reaction] { emit reactionToggled(m_message.id, reaction); });
        m_reactionsBar->insertWidget(insertAt++, chip);
    }
}

void MessageRow::showReactionPicker()
{
    auto *popup = new QFrame(this, Qt::Popup);
    popup->setObjectName("reactionPicker");
    popup->setAttribute(Qt::WA_DeleteOnClose);
    auto *grid = new QGridLayout(popup);
    grid->setContentsMargins(8, 8, 8, 8);
    grid->setSpacing(2);
    int index = 0;
    for (const reactions::CatalogEntry &choice : reactions::catalog()) {
        auto *button = new QToolButton(popup);
        button->setObjectName("reactionPickerButton");
        button->setAutoRaise(true);
        button->setCursor(Qt::PointingHandCursor);
        button->setToolTip(choice.label);
        button->setIcon(QIcon(
            reactions::emojiPixmap(choice.value, 22, devicePixelRatio())));
        button->setIconSize(QSize(22, 22));
        button->setFixedSize(34, 34);
        const QString value = choice.value;
        connect(button, &QToolButton::clicked, this, [this, popup, value] {
            emit reactionToggled(m_message.id, value);
            popup->close();
        });
        grid->addWidget(button, index / kPickerColumns,
                        index % kPickerColumns);
        ++index;
    }
    popup->adjustSize();
    const QPoint cursor = QCursor::pos();
    QPoint pos = cursor - QPoint(10, popup->height() + 6);
    if (QScreen *screen = QGuiApplication::screenAt(cursor)) {
        const QRect avail = screen->availableGeometry();
        pos.setX(qBound(avail.left(), pos.x(),
                        avail.right() - popup->width()));
        if (pos.y() < avail.top())
            pos.setY(cursor.y() + 6);
    }
    popup->move(pos);
    popup->show();
}

QPixmap MessageRow::receiptMarkPixmap(forkmesh::recipients::ReceiptMark mark,
                                      const QColor &muted, const QColor &accent,
                                      qreal dpr)
{
    using forkmesh::recipients::ReceiptMark;
    if (mark == ReceiptMark::None)
        return QPixmap();
    constexpr int kWidth = 17;
    constexpr int kHeight = 11;
    dpr = qMax<qreal>(1.0, dpr);
    QPixmap pixmap(qRound(kWidth * dpr), qRound(kHeight * dpr));
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const qreal scale = kWidth / 22.0;
    painter.translate(0, (kHeight - 14.0 * scale) / 2.0);
    painter.scale(scale, scale);

    const QPointF front(14.0, 7.0);
    const QPointF back(8.5, 7.0);
    constexpr qreal kRadius = 5.2;
    constexpr qreal kStroke = 1.4;
    constexpr qreal kOuter = kRadius + kStroke / 2.0;
    QPainterPath frontTick;
    frontTick.moveTo(11.7, 7.2);
    frontTick.lineTo(13.4, 8.9);
    frontTick.lineTo(16.4, 5.6);
    QPainterPath backTick;
    backTick.moveTo(6.2, 7.2);
    backTick.lineTo(7.9, 8.9);
    backTick.lineTo(9.3, 7.4);
    QPainterPath box;
    box.addRect(QRectF(0, 0, 22, 14));
    const auto disc = [](const QPointF &centre, qreal radius) {
        QPainterPath path;
        path.addEllipse(centre, radius, radius);
        return path;
    };
    QPen pen(mark == ReceiptMark::Read ? accent : muted, kStroke, Qt::SolidLine,
             Qt::RoundCap, Qt::RoundJoin);
    painter.setBrush(Qt::NoBrush);

    if (mark == ReceiptMark::Sending) {
        painter.setOpacity(0.7);
        pen.setDashPattern({0.01, 2.1});
        painter.setPen(pen);
        painter.drawEllipse(front, kRadius, kRadius);
        return pixmap;
    }
    if (mark == ReceiptMark::Read) {
        const QPainterPath backDisc =
            disc(back, kOuter).subtracted(disc(front, kOuter + 0.9));
        painter.setPen(Qt::NoPen);
        painter.setBrush(accent);
        painter.drawPath(backDisc);
        painter.drawPath(disc(front, kOuter));
        painter.setCompositionMode(QPainter::CompositionMode_Clear);
        painter.setBrush(Qt::NoBrush);
        pen.setColor(Qt::black);
        painter.setPen(pen);
        painter.drawPath(frontTick);
        painter.setClipPath(backDisc);
        painter.drawPath(backTick);
        return pixmap;
    }
    painter.setPen(pen);
    if (mark == ReceiptMark::Delivered) {
        painter.save();
        painter.setClipPath(box.subtracted(disc(front, kOuter)));
        painter.drawEllipse(back, kRadius, kRadius);
        painter.drawPath(backTick);
        painter.restore();
    }
    painter.drawEllipse(front, kRadius, kRadius);
    painter.drawPath(frontTick);
    return pixmap;
}

void MessageRow::setReceiptMark(forkmesh::recipients::ReceiptMark mark)
{
    using forkmesh::recipients::ReceiptMark;
    if (!m_headerRow || mark == m_receiptMark)
        return;
    m_receiptMark = mark;
    if (mark == ReceiptMark::None) {
        if (m_receiptLabel)
            m_receiptLabel->hide();
        return;
    }
    if (!m_receiptLabel) {
        m_receiptLabel = new QLabel;
        m_receiptLabel->setObjectName(QStringLiteral("messageReceipt"));
        QWidget *anchor = m_recipientsLabel ? static_cast<QWidget *>(m_recipientsLabel)
                                            : static_cast<QWidget *>(m_senderLabel);
        m_headerRow->insertWidget(m_headerRow->indexOf(anchor) + 1,
                                  m_receiptLabel, 0, Qt::AlignVCenter);
    }
    const QColor muted =
        Theme::nextOr(Theme::Role::FgMuted, QColor(QStringLiteral("#6b7280")));
    const QColor accent = Theme::nextOr(
        Theme::Role::AccentFg,
        QColor(themeIsDark() ? QStringLiteral("#58a6ff")
                             : QStringLiteral("#0969da")));
    const qreal dpr = qGuiApp ? qGuiApp->devicePixelRatio() : 1.0;
    m_receiptLabel->setPixmap(receiptMarkPixmap(mark, muted, accent, dpr));
    const QString label = forkmesh::recipients::receiptLabel(mark);
    m_receiptLabel->setToolTip(label);
    m_receiptLabel->setAccessibleName(label);
    m_receiptLabel->show();
}
