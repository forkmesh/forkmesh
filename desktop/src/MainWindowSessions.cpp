#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "MainWindowAgentsInternal.h"
#include "MainWindowNextAccess.h"
#include "AgentCloudSessions.h"
#include "AgentHub.h"
#include "AgentLogText.h"
#include "AppPaths.h"
#include "ClaudePeerMessaging.h"
#include "ClaudeSessionScan.h"
#include "ClaudeTranscriptView.h"
#include "DesignTokens.h"
#include "LocalControl.h"
#include "SessionSidePanels.h"
#include "UiKit.h"

#include <QAbstractItemView>
#include <QAction>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QSysInfo>
#include <QTextCursor>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <climits>
#include <functional>

using namespace forkmesh::ui;

namespace hub = forkmesh::agenthub;

enum class SessionsSource { Local, External, Cloud };

struct SessionsQueued {
    QJsonObject event;
    SessionsSource source = SessionsSource::Local;
    bool statsOnly = false;
};

struct SessionsRelay {
    int from = 0;
    int to = 0;
    qint64 atMs = 0;
};

struct SessionsPageUi : public QObject {
    explicit SessionsPageUi(QObject *parent) : QObject(parent) {}

    QPointer<QWidget> page;
    QPointer<QPushButton> newButton;
    QPointer<QLineEdit> search;
    QPointer<QPushButton> betweenFilter;
    QPointer<QListWidget> list;
    QPointer<QLabel> title;
    QPointer<QLabel> status;
    QPointer<QLabel> meta;
    QPointer<QPushButton> stop;
    QPointer<QPushButton> resume;
    QPointer<QPushButton> openInAgents;
    QPointer<QWidget> relayStrip;
    QHBoxLayout *relayLayout = nullptr;
    QList<QPointer<QPushButton>> relayButtons;
    QStringList relayTexts;
    QPointer<QStackedWidget> centerStack;
    QPointer<QLabel> emptyTitle;
    QPointer<QLabel> emptyBody;
    QPointer<ClaudeTranscriptView> transcript;
    QPointer<QPlainTextEdit> composer;
    QPointer<QWidget> targetRow;
    QPointer<QComboBox> toCombo;
    QPointer<QComboBox> alsoCombo;
    QPointer<QToolButton> forward;
    QPointer<QWidget> newRow;
    QPointer<QComboBox> repoCombo;
    QPointer<QComboBox> providerCombo;
    QPointer<QPushButton> send;
    QPointer<QLabel> composerNote;

    QPointer<QPushButton> filterButton;
    QPointer<QAction> searchIcon;
    QPointer<QWidget> header;
    QPointer<QPushButton> titleMenu;
    QPointer<QFrame> branchBadge;
    QPointer<QLabel> branchText;
    QPointer<QPushButton> panelToggle;
    QPointer<QWidget> column;
    QVBoxLayout *columnLayout = nullptr;
    QPointer<QWidget> emptyColumn;
    QVBoxLayout *composerSlot = nullptr;
    QPointer<QWidget> composerHost;
    QHBoxLayout *composerHostLayout = nullptr;
    QPointer<QWidget> starters;
    QPointer<QLabel> composerModel;
    QPointer<forkmesh::ui::SessionSidePanels> details;
    bool next = false;
    bool panelOpen = true;
    bool groupByRepo = true;
    QSet<QString> collapsedGroups;

    int openId = 0;
    bool newMode = false;
    bool listDirty = true;
    bool refreshQueued = false;
    bool transcriptDirty = false;
    bool syncQueued = false;
    bool sliceQueued = false;
    quint64 generation = 0;
    SessionsSource source = SessionsSource::Local;
    int consumed = 0;
    int renderLimit = 300;
    QList<SessionsQueued> queue;
    int cursor = 0;
    bool bulk = false;
    int skipped = 0;
    bool skippedNoticeAdded = false;
    bool fallbackShown = false;
    qint64 externalOffset = -1;
    bool externalReading = false;
    bool externalAgain = false;
    int cloudTurns = -1;
    qint64 cloudTail = -1;
    QList<SessionsRelay> relays;
    QHash<int, int> relayCounts;
    QHash<int, QString> lastReply;
    QList<int> listedIds;
};

namespace {

constexpr int kSessionsHeaderActive = INT_MIN;
constexpr int kSessionsHeaderRecent = INT_MIN + 1;
constexpr int kSessionsHeaderGroup = INT_MIN + 2;
constexpr int kSessionsRecentLimit = 300;
constexpr int kSessionsPickerLimit = 40;
constexpr int kSessionsRelayLimit = 200;
constexpr int kSessionsRelayChips = 6;
constexpr qint64 kSessionsRelayDedupMs = 3000;

enum SessionsRole {
    kSessionIdRole = Qt::UserRole,
    kSessionHeaderRole,
    kSessionSubtitleRole,
    kSessionAgeRole,
    kSessionDotRole,
    kSessionUnreadRole,
    kSessionRelaysRole,
    kSessionStateRole,
    kSessionGroupRole,
    kSessionCollapsedRole,
    kSessionAddableRole,
};

bool sessionsHeaderId(int id)
{
    return id == kSessionsHeaderActive || id == kSessionsHeaderRecent ||
           id == kSessionsHeaderGroup;
}

const QString kSessionsEnvelopeLead = QStringLiteral("Message from ForkMesh agent #");

int sessionsEnvelopeSender(const QString &text)
{
    if (!text.startsWith(kSessionsEnvelopeLead))
        return 0;
    static const QRegularExpression lead(
        QStringLiteral("^Message from ForkMesh agent #(-?\\d+)"));
    const QRegularExpressionMatch match = lead.match(text);
    return match.hasMatch() ? match.captured(1).toInt() : 0;
}

QString sessionsAssistantText(const QJsonObject &event)
{
    const QString type = event.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("_codex_agent_complete"))
        return event.value(QStringLiteral("text")).toString().trimmed();
    if (type != QLatin1String("assistant"))
        return QString();
    QString text;
    const QJsonArray content = event.value(QStringLiteral("message"))
                                   .toObject()
                                   .value(QStringLiteral("content"))
                                   .toArray();
    for (const QJsonValue &value : content) {
        const QJsonObject block = value.toObject();
        if (block.value(QStringLiteral("type")).toString() == QLatin1String("text"))
            text += block.value(QStringLiteral("text")).toString();
    }
    return text.trimmed();
}

void appendSessionsEvent(ClaudeTranscriptView *view, const QJsonObject &event,
                         SessionsSource source)
{
    const QString type = event.value(QStringLiteral("type")).toString();
    if (source == SessionsSource::Local) {
        if (type == QLatin1String("_local_user")) {
            view->addUserTurn(event.value(QStringLiteral("text")).toString(),
                              ClaudeTranscriptView::eventTime(event));
            return;
        }
        view->handleEvent(event);
        return;
    }
    if (source == SessionsSource::Cloud) {
        if (type == QLatin1String("user")) {
            view->addUserTurn(event.value(QStringLiteral("message"))
                                  .toObject()
                                  .value(QStringLiteral("content"))
                                  .toString(),
                              ClaudeTranscriptView::eventTime(event));
            return;
        }
        view->handleEvent(event, false);
        return;
    }
    if (event.value(QStringLiteral("isSidechain")).toBool() ||
        event.value(QStringLiteral("isMeta")).toBool())
        return;
    if (type == QLatin1String("user")) {
        const QJsonValue content = event.value(QStringLiteral("message"))
                                       .toObject()
                                       .value(QStringLiteral("content"));
        if (content.isString()) {
            const QString text = content.toString();
            if (!text.trimmed().isEmpty() && !AgentLogText::isInjectedPrompt(text))
                view->addUserTurn(text, ClaudeTranscriptView::eventTime(event));
        } else {
            for (const QJsonValue &value : content.toArray()) {
                const QJsonObject block = value.toObject();
                if (block.value(QStringLiteral("type")).toString() !=
                    QLatin1String("text"))
                    continue;
                const QString text = block.value(QStringLiteral("text")).toString();
                if (!text.trimmed().isEmpty() &&
                    !AgentLogText::isInjectedPrompt(text))
                    view->addUserTurn(text, ClaudeTranscriptView::eventTime(event));
            }
        }
    }
    view->handleEvent(event);
}

qint64 sessionsActivityMs(const AgentSession &session)
{
    return std::max({session.createdAtMs, session.startedAtMs,
                     session.finishedAtMs, session.runningSinceMs});
}

int sessionsActiveRank(const AgentSession &session)
{
    if (session.merged || session.deletedAtMs > 0)
        return -1;
    if (session.status == AgentStatus::Waiting)
        return 0;
    if (session.status == AgentStatus::Running)
        return 1;
    if (session.status == AgentStatus::Queued)
        return 2;
    return -1;
}

class SessionsEventHook : public QObject
{
public:
    SessionsEventHook(std::function<bool(QObject *, QEvent *)> handler,
                      QObject *parent)
        : QObject(parent), m_handler(std::move(handler))
    {
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        return m_handler ? m_handler(watched, event) : false;
    }

private:
    std::function<bool(QObject *, QEvent *)> m_handler;
};

class SessionsPickerCombo : public QComboBox
{
public:
    std::function<void()> beforePopup;

    void showPopup() override
    {
        if (beforePopup)
            beforePopup();
        QComboBox::showPopup();
    }
};

class SessionsRowDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        return QSize(option.rect.width(),
                     index.data(kSessionHeaderRole).toBool() ? 28 : 52);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const QRect rect = option.rect;
        const QPalette &palette = option.palette;
        const QColor muted = palette.color(QPalette::PlaceholderText);
        if (index.data(kSessionHeaderRole).toBool()) {
            QFont font = option.font;
            font.setPixelSize(11);
            font.setWeight(QFont::DemiBold);
            painter->setFont(font);
            painter->setPen(muted);
            painter->drawText(rect.adjusted(12, 6, -8, 0),
                              Qt::AlignLeft | Qt::AlignVCenter,
                              index.data(Qt::DisplayRole).toString());
            painter->restore();
            return;
        }
        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;
        if (selected || hovered) {
            QColor fill = palette.color(QPalette::Highlight);
            fill.setAlphaF(selected ? 0.24 : 0.10);
            painter->setPen(Qt::NoPen);
            painter->setBrush(fill);
            painter->drawRoundedRect(rect.adjusted(4, 2, -4, -2), 7, 7);
        }
        const QColor dot = index.data(kSessionDotRole).value<QColor>();
        painter->setPen(Qt::NoPen);
        painter->setBrush(dot.isValid() ? dot : muted);
        painter->drawEllipse(QPointF(rect.left() + 17, rect.top() + 18), 4, 4);

        const bool unread = index.data(kSessionUnreadRole).toBool();
        const QString age = index.data(kSessionAgeRole).toString();
        QFont small = option.font;
        small.setPixelSize(11);
        const QFontMetrics smallMetrics(small);
        const int right = rect.right() - 12;
        int titleRight = right;
        if (unread) {
            painter->setBrush(palette.color(QPalette::Link));
            painter->drawEllipse(QPointF(right - 3, rect.top() + 18), 3.5, 3.5);
            titleRight -= 12;
        }
        if (!age.isEmpty()) {
            const int ageWidth = smallMetrics.horizontalAdvance(age);
            painter->setFont(small);
            painter->setPen(muted);
            painter->drawText(QRect(titleRight - ageWidth, rect.top() + 8,
                                    ageWidth, 20),
                              Qt::AlignRight | Qt::AlignVCenter, age);
            titleRight -= ageWidth + 8;
        }

        QFont titleFont = option.font;
        titleFont.setPixelSize(13);
        titleFont.setWeight(unread ? QFont::DemiBold : QFont::Medium);
        painter->setFont(titleFont);
        painter->setPen(palette.color(QPalette::Text));
        const int left = rect.left() + 30;
        const QString title = QFontMetrics(titleFont).elidedText(
            index.data(Qt::DisplayRole).toString(), Qt::ElideRight,
            qMax(0, titleRight - left));
        painter->drawText(QRect(left, rect.top() + 8, qMax(0, titleRight - left), 20),
                          Qt::AlignLeft | Qt::AlignVCenter, title);

        int subtitleRight = right;
        const int relays = index.data(kSessionRelaysRole).toInt();
        if (relays > 0) {
            const QString badge =
                QString::fromUtf8("\xE2\x87\x84 %1").arg(relays);
            const int badgeWidth = smallMetrics.horizontalAdvance(badge) + 10;
            const QRect badgeRect(subtitleRight - badgeWidth, rect.top() + 30,
                                  badgeWidth, 16);
            QColor badgeFill = palette.color(QPalette::Link);
            badgeFill.setAlphaF(0.16);
            painter->setPen(Qt::NoPen);
            painter->setBrush(badgeFill);
            painter->drawRoundedRect(badgeRect, 8, 8);
            painter->setFont(small);
            painter->setPen(palette.color(QPalette::Link));
            painter->drawText(badgeRect, Qt::AlignCenter, badge);
            subtitleRight -= badgeWidth + 6;
        }
        painter->setFont(small);
        painter->setPen(muted);
        const QString subtitle = smallMetrics.elidedText(
            index.data(kSessionSubtitleRole).toString(), Qt::ElideRight,
            qMax(0, subtitleRight - left));
        painter->drawText(QRect(left, rect.top() + 28, qMax(0, subtitleRight - left), 20),
                          Qt::AlignLeft | Qt::AlignVCenter, subtitle);
        painter->restore();
    }
};

QString sessionsGroupKey(const AgentSession &session, bool byRepo)
{
    if (!byRepo)
        return sessionsActiveRank(session) >= 0 ? QStringLiteral("@active")
                                                : QStringLiteral("@recent");
    return session.owner.isEmpty() ? QStringLiteral("@other")
                                   : session.owner + QLatin1Char('/') + session.name;
}

qreal sessionsIconRatio()
{
    return qMax<qreal>(2.0, qApp ? qApp->devicePixelRatio() : 1.0);
}

QIcon sessionsPanelIcon(const QColor &color)
{
    const qreal ratio = sessionsIconRatio();
    const int px = 16;
    QPixmap pixmap(qRound(px * ratio), qRound(px * ratio));
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    QPen pen(color, 1.4);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(1.7, 2.7, 12.6, 10.6), 2.4, 2.4);
    painter.drawLine(QPointF(10.0, 2.7), QPointF(10.0, 13.3));
    return QIcon(pixmap);
}

void paintSessionsStatusIcon(QPainter *painter, const QRectF &box, const QString &state)
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    const QPointF c = box.center();
    const qreal r = box.width() / 2.0 - 1.0;
    const QRectF circle(c.x() - r, c.y() - r, 2 * r, 2 * r);
    QPen pen;
    pen.setWidthF(1.5);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter->setBrush(Qt::NoBrush);
    if (state == AgentStatus::Running) {
        pen.setColor(Theme::color(Theme::Role::AccentFg));
        painter->setPen(pen);
        painter->drawArc(circle, 90 * 16, -270 * 16);
    } else if (state == AgentStatus::Waiting) {
        pen.setColor(Theme::color(Theme::Role::Warning));
        painter->setPen(pen);
        painter->drawEllipse(circle);
        QFont font = painter->font();
        font.setPixelSize(qMax(7, int(r * 1.45)));
        font.setWeight(QFont::Bold);
        painter->setFont(font);
        painter->drawText(circle, Qt::AlignCenter, QStringLiteral("?"));
    } else if (state == AgentStatus::Failed) {
        pen.setColor(Theme::color(Theme::Role::Danger));
        painter->setPen(pen);
        const qreal d = r * 0.68;
        painter->drawLine(c + QPointF(-d, -d), c + QPointF(d, d));
        painter->drawLine(c + QPointF(-d, d), c + QPointF(d, -d));
    } else if (state == AgentStatus::Success || state == QLatin1String("merged")) {
        pen.setColor(Theme::color(state == AgentStatus::Success ? Theme::Role::FgMuted
                                                                : Theme::Role::Success));
        painter->setPen(pen);
        QPainterPath path;
        path.moveTo(c.x() - r * 0.8, c.y() + r * 0.02);
        path.lineTo(c.x() - r * 0.22, c.y() + r * 0.6);
        path.lineTo(c.x() + r * 0.85, c.y() - r * 0.58);
        painter->drawPath(path);
    } else if (state == AgentStatus::Stopped || state == AgentStatus::Cleared) {
        pen.setColor(Theme::color(Theme::Role::FgSubtle));
        painter->setPen(pen);
        painter->drawEllipse(circle);
        const qreal d = r * 0.7;
        painter->drawLine(c + QPointF(-d, d), c + QPointF(d, -d));
    } else {
        pen.setColor(Theme::color(Theme::Role::FgSubtle));
        pen.setDashPattern({1.4, 1.9});
        pen.setCapStyle(Qt::FlatCap);
        painter->setPen(pen);
        painter->drawEllipse(circle);
    }
    painter->restore();
}

class SessionsNextRowDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        if (index.data(kSessionHeaderRole).toBool())
            return QSize(option.rect.width(), index.row() == 0 ? 28 : 36);
        return QSize(option.rect.width(), 33);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const QRect rect = option.rect;
        const bool hovered = option.state & QStyle::State_MouseOver;
        if (index.data(kSessionHeaderRole).toBool()) {
            const QRect band(rect.left(), rect.bottom() - 27, rect.width(), 28);
            const QColor tone = Theme::color(hovered ? Theme::Role::FgMuted
                                                     : Theme::Role::FgSubtle);
            QPen pen(tone, 1.4);
            pen.setCapStyle(Qt::RoundCap);
            pen.setJoinStyle(Qt::RoundJoin);
            painter->setPen(pen);
            painter->setBrush(Qt::NoBrush);
            const QPointF mark(band.left() + 10.5, band.center().y() + 0.5);
            QPainterPath chevron;
            if (index.data(kSessionCollapsedRole).toBool()) {
                chevron.moveTo(mark + QPointF(-1.5, -3.0));
                chevron.lineTo(mark + QPointF(1.5, 0.0));
                chevron.lineTo(mark + QPointF(-1.5, 3.0));
            } else {
                chevron.moveTo(mark + QPointF(-3.0, -1.5));
                chevron.lineTo(mark + QPointF(0.0, 1.5));
                chevron.lineTo(mark + QPointF(3.0, -1.5));
            }
            painter->drawPath(chevron);
            const bool addable = index.data(kSessionAddableRole).toBool();
            int textRight = band.right() - 8;
            if (addable && hovered) {
                const QPointF plus(band.right() - 13.5, band.center().y() + 0.5);
                painter->drawLine(plus + QPointF(-4.0, 0.0), plus + QPointF(4.0, 0.0));
                painter->drawLine(plus + QPointF(0.0, -4.0), plus + QPointF(0.0, 4.0));
                textRight = band.right() - 28;
            }
            QFont font = option.font;
            font.setPixelSize(12);
            font.setWeight(QFont::Medium);
            painter->setFont(font);
            painter->setPen(tone);
            const int left = band.left() + 20;
            const QString label = QFontMetrics(font).elidedText(
                index.data(Qt::DisplayRole).toString(), Qt::ElideRight,
                qMax(0, textRight - left));
            painter->drawText(QRect(left, band.top(), qMax(0, textRight - left),
                                    band.height()),
                              Qt::AlignLeft | Qt::AlignVCenter, label);
            painter->restore();
            return;
        }
        const QRect band = rect.adjusted(0, 0, 0, -1);
        const bool selected = option.state & QStyle::State_Selected;
        if (selected || hovered) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(Theme::color(selected ? Theme::Role::SurfaceActive
                                                    : Theme::Role::SurfaceRaised));
            painter->drawRoundedRect(QRectF(band), 8, 8);
        }
        paintSessionsStatusIcon(painter,
                                QRectF(band.left() + 9, band.center().y() - 6.5, 14, 14),
                                index.data(kSessionStateRole).toString());

        const bool unread = index.data(kSessionUnreadRole).toBool();
        QFont small = option.font;
        small.setPixelSize(11);
        const QFontMetrics smallMetrics(small);
        int titleRight = band.right() - 8;
        if (hovered) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(Theme::color(Theme::Role::FgMuted));
            const QPointF dots(band.right() - 14.5, band.center().y() + 0.5);
            for (int step = -1; step <= 1; ++step)
                painter->drawEllipse(dots + QPointF(step * 4.0, 0.0), 1.3, 1.3);
            titleRight = band.right() - 30;
        } else {
            const QString age = index.data(kSessionAgeRole).toString();
            if (!age.isEmpty()) {
                const int ageWidth = smallMetrics.horizontalAdvance(age);
                painter->setFont(small);
                painter->setPen(Theme::color(Theme::Role::FgSubtle));
                painter->drawText(QRect(titleRight - ageWidth, band.top(), ageWidth,
                                        band.height()),
                                  Qt::AlignRight | Qt::AlignVCenter, age);
                titleRight -= ageWidth + 8;
            }
            const int relays = index.data(kSessionRelaysRole).toInt();
            if (relays > 0) {
                const QString badge = QString::fromUtf8("\xE2\x87\x84%1").arg(relays);
                const int badgeWidth = smallMetrics.horizontalAdvance(badge);
                painter->setFont(small);
                painter->setPen(Theme::color(Theme::Role::AccentFg));
                painter->drawText(QRect(titleRight - badgeWidth, band.top(), badgeWidth,
                                        band.height()),
                                  Qt::AlignRight | Qt::AlignVCenter, badge);
                titleRight -= badgeWidth + 8;
            }
            if (unread) {
                painter->setPen(Qt::NoPen);
                painter->setBrush(Theme::color(Theme::Role::Accent));
                painter->drawEllipse(QPointF(titleRight - 3, band.center().y() + 0.5),
                                     3.0, 3.0);
                titleRight -= 12;
            }
        }
        QFont titleFont = option.font;
        titleFont.setPixelSize(13);
        titleFont.setWeight(unread ? QFont::DemiBold : QFont::Normal);
        painter->setFont(titleFont);
        painter->setPen(Theme::color(selected || hovered || unread
                                         ? Theme::Role::Fg
                                         : Theme::Role::FgSecondary));
        const int left = band.left() + 31;
        const QString title = QFontMetrics(titleFont).elidedText(
            index.data(Qt::DisplayRole).toString(), Qt::ElideRight,
            qMax(0, titleRight - left));
        painter->drawText(QRect(left, band.top(), qMax(0, titleRight - left), band.height()),
                          Qt::AlignLeft | Qt::AlignVCenter, title);
        painter->restore();
    }
};

class SessionsElidedLabel : public QLabel
{
public:
    using QLabel::QLabel;

    QSize minimumSizeHint() const override
    {
        const QSize hint = QLabel::minimumSizeHint();
        return QSize(qMin(hint.width(), 36), hint.height());
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const QRect area = contentsRect();
        painter.setFont(font());
        painter.setPen(palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled,
                                       foregroundRole()));
        painter.drawText(area, Qt::AlignLeft | Qt::AlignVCenter,
                         fontMetrics().elidedText(text(), Qt::ElideRight, area.width()));
    }
};

void openSessionsPanels(SessionsPageUi *ui)
{
    if (!ui || !ui->next)
        return;
    ui->panelOpen = true;
    if (ui->panelToggle && !ui->panelToggle->isChecked())
        ui->panelToggle->setChecked(true);
}

void retintSessionsIcons(SessionsPageUi *ui)
{
    if (!ui || !ui->next)
        return;
    if (ui->searchIcon)
        ui->searchIcon->setIcon(QIcon(forkmesh::ui::kit::tintedIcon(
            QStringLiteral("search"), Theme::color(Theme::Role::FgSubtle), 14,
            sessionsIconRatio())));
    if (ui->panelToggle)
        ui->panelToggle->setIcon(sessionsPanelIcon(Theme::color(Theme::Role::FgMuted)));
}

void placeSessionsComposer(SessionsPageUi *ui)
{
    if (!ui || !ui->next || !ui->composerHost || !ui->composerSlot ||
        !ui->columnLayout || !ui->composerHostLayout)
        return;
    const bool centred = ui->newMode;
    QWidget *target = centred ? ui->emptyColumn.data() : ui->column.data();
    if (!target || ui->composerHost->parentWidget() == target)
        return;
    if (centred) {
        ui->composerHostLayout->setContentsMargins(0, 0, 0, 0);
        ui->composerSlot->addWidget(ui->composerHost);
    } else {
        ui->composerHostLayout->setContentsMargins(24, 0, 24, 16);
        ui->columnLayout->addWidget(ui->composerHost);
    }
    ui->composerHost->show();
}

}

QString MainWindow::sessionsTitle(const AgentSession &session) const
{
    QString title = session.issueTitle.simplified();
    if (title.isEmpty())
        title = session.prompt.section(QLatin1Char('\n'), 0, 0).simplified();
    if (title.isEmpty())
        title = session.owner.isEmpty()
                    ? QStringLiteral("Session #%1").arg(session.id)
                    : session.owner + QLatin1Char('/') + session.name;
    if (title.size() > 120)
        title = title.left(119) + QString::fromUtf8("\xE2\x80\xA6");
    return title;
}

QString MainWindow::sessionsModeLabel(const AgentSession &session) const
{
    if (isExternalSession(session.id) && !isCloudSession(session.id))
        return QString();
    return agentEffectiveModeLabel(session);
}

QString MainWindow::sessionsLastReply(int sessionId) const
{
    const QString remembered = m_lastAssistantText.value(sessionId).trimmed();
    if (!remembered.isEmpty())
        return remembered;
    const auto events = m_streamEvents.constFind(sessionId);
    if (events != m_streamEvents.constEnd()) {
        for (auto it = events->crbegin(); it != events->crend(); ++it) {
            const QString text = sessionsAssistantText(*it);
            if (!text.isEmpty())
                return text;
        }
    }
    return m_sessionsUi ? m_sessionsUi->lastReply.value(sessionId) : QString();
}

QWidget *MainWindow::buildSessionsSection()
{
    if (!m_sessionsUi)
        m_sessionsUi = new SessionsPageUi(this);
    SessionsPageUi *ui = m_sessionsUi;
    const bool next = Theme::isNext();
    ui->next = next;

    auto *page = new QWidget;
    page->setObjectName(QStringLiteral("sessionsPage"));
    ui->page = page;

    auto *split = new QSplitter(Qt::Horizontal, page);
    split->setObjectName(QStringLiteral("sessionsSplit"));
    split->setChildrenCollapsible(false);
    split->setHandleWidth(1);

    auto *sidebar = new QWidget;
    sidebar->setObjectName(QStringLiteral("sessionsSidebar"));
    sidebar->setAttribute(Qt::WA_StyledBackground, true);
    sidebar->setMinimumWidth(240);
    auto *side = new QVBoxLayout(sidebar);
    side->setContentsMargins(10, 10, 10, 6);
    side->setSpacing(next ? 6 : 8);

    ui->newButton = new QPushButton(QStringLiteral("New session"), sidebar);
    ui->newButton->setObjectName(QStringLiteral("sessionsNewButton"));
    ui->newButton->setCursor(Qt::PointingHandCursor);
    ui->newButton->setToolTip(
        QStringLiteral("Start a new agent session in a repository"));
    setOcticon(ui->newButton, QStringLiteral("plus"), 14);
    side->addWidget(ui->newButton);

    ui->search = new QLineEdit(sidebar);
    ui->search->setObjectName(QStringLiteral("sessionsSearch"));
    ui->search->setPlaceholderText(QStringLiteral("Search sessions"));
    ui->search->setClearButtonEnabled(true);
    if (next) {
        ui->searchIcon = ui->search->addAction(QIcon(), QLineEdit::LeadingPosition);
        ui->filterButton = new QPushButton(sidebar);
        ui->filterButton->setObjectName(QStringLiteral("sessionsFilterButton"));
        ui->filterButton->setCursor(Qt::PointingHandCursor);
        ui->filterButton->setToolTip(QStringLiteral("Filter and group sessions"));
        ui->filterButton->setFixedSize(32, 32);
        ui->filterButton->setProperty("nextIconRole", int(Theme::Role::FgMuted));
        setOcticon(ui->filterButton, QStringLiteral("filter"), 14);
        auto *searchRow = new QHBoxLayout;
        searchRow->setSpacing(4);
        searchRow->addWidget(ui->search, 1);
        searchRow->addWidget(ui->filterButton);
        side->addLayout(searchRow);
    } else {
        side->addWidget(ui->search);
    }

    ui->betweenFilter =
        new QPushButton(QStringLiteral("Between sessions"), sidebar);
    ui->betweenFilter->setObjectName(QStringLiteral("sessionsBetweenFilter"));
    ui->betweenFilter->setCheckable(true);
    ui->betweenFilter->setCursor(Qt::PointingHandCursor);
    ui->betweenFilter->setToolTip(
        QStringLiteral("Show only sessions that sent or received a message "
                       "from another session"));
    side->addWidget(ui->betweenFilter, 0, Qt::AlignLeft);
    if (next)
        ui->betweenFilter->hide();

    ui->list = new QListWidget(sidebar);
    ui->list->setObjectName(QStringLiteral("sessionsList"));
    ui->list->setFrameShape(QFrame::NoFrame);
    if (next)
        ui->list->setItemDelegate(new SessionsNextRowDelegate(ui->list));
    else
        ui->list->setItemDelegate(new SessionsRowDelegate(ui->list));
    ui->list->setSelectionMode(QAbstractItemView::SingleSelection);
    ui->list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    ui->list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    ui->list->setContextMenuPolicy(Qt::CustomContextMenu);
    ui->list->setMouseTracking(true);
    ui->list->viewport()->setAttribute(Qt::WA_Hover, true);
    side->addWidget(ui->list, 1);

    auto *center = new QWidget;
    center->setObjectName(QStringLiteral("sessionsCenter"));
    center->setAttribute(Qt::WA_StyledBackground, true);
    auto *centerLayout = new QVBoxLayout(center);
    centerLayout->setContentsMargins(0, 0, 0, 0);
    centerLayout->setSpacing(0);

    auto *header = new QWidget(center);
    header->setObjectName(QStringLiteral("sessionsHeader"));
    header->setAttribute(Qt::WA_StyledBackground, true);
    ui->header = header;
    auto *headerLayout = new QHBoxLayout(header);
    if (next) {
        header->setFixedHeight(48);
        headerLayout->setContentsMargins(12, 0, 12, 0);
        headerLayout->setSpacing(6);
        ui->title = new SessionsElidedLabel(header);
        ui->title->setObjectName(QStringLiteral("sessionsTitle"));
        ui->title->setTextFormat(Qt::PlainText);
        ui->titleMenu = new QPushButton(header);
        ui->titleMenu->setObjectName(QStringLiteral("sessionsTitleMenu"));
        ui->titleMenu->setCursor(Qt::PointingHandCursor);
        ui->titleMenu->setToolTip(QStringLiteral("Session actions"));
        ui->titleMenu->setFixedSize(22, 22);
        ui->titleMenu->setProperty("nextIconRole", int(Theme::Role::FgSubtle));
        setOcticon(ui->titleMenu, QStringLiteral("chevron-down"), 12);
        ui->branchBadge = new QFrame(header);
        ui->branchBadge->setObjectName(QStringLiteral("sessionsBranchBadge"));
        ui->branchBadge->setAttribute(Qt::WA_StyledBackground, true);
        ui->branchBadge->setFixedHeight(22);
        auto *badgeLayout = new QHBoxLayout(ui->branchBadge);
        badgeLayout->setContentsMargins(6, 0, 8, 0);
        badgeLayout->setSpacing(4);
        auto *branchIcon = new QLabel(ui->branchBadge);
        setLabelOcticon(branchIcon, QStringLiteral("git-branch"), 12);
        ui->branchText = new QLabel(ui->branchBadge);
        ui->branchText->setObjectName(QStringLiteral("sessionsBranchText"));
        ui->branchText->setTextFormat(Qt::PlainText);
        ui->branchText->setFont(Theme::monospaceFont(ui->branchText->font()));
        badgeLayout->addWidget(branchIcon);
        badgeLayout->addWidget(ui->branchText);
        ui->branchBadge->hide();
        ui->meta = new SessionsElidedLabel(header);
        ui->meta->setObjectName(QStringLiteral("sessionsMeta"));
        ui->meta->setTextFormat(Qt::PlainText);
        ui->meta->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        ui->status = new QLabel(header);
        ui->status->setObjectName(QStringLiteral("sessionsStatus"));
    } else {
        headerLayout->setContentsMargins(16, 10, 16, 10);
        headerLayout->setSpacing(10);
        auto *titles = new QVBoxLayout;
        titles->setSpacing(2);
        ui->title = new QLabel(header);
        ui->title->setObjectName(QStringLiteral("sessionsTitle"));
        ui->title->setTextFormat(Qt::PlainText);
        ui->title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        ui->meta = new QLabel(header);
        ui->meta->setObjectName(QStringLiteral("sessionsMeta"));
        ui->meta->setTextFormat(Qt::PlainText);
        ui->meta->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        titles->addWidget(ui->title);
        titles->addWidget(ui->meta);
        headerLayout->addLayout(titles, 1);
        ui->status = new QLabel(header);
        ui->status->setObjectName(QStringLiteral("sessionsStatus"));
        headerLayout->addWidget(ui->status, 0, Qt::AlignVCenter);
    }
    const auto headerButton = [header, next](const QString &text, const QString &name,
                                             const QString &icon) {
        auto *button = new QPushButton(text, header);
        button->setObjectName(name);
        button->setCursor(Qt::PointingHandCursor);
        if (next)
            button->setProperty("nextIconRole", int(Theme::Role::FgMuted));
        setOcticon(button, icon, 14);
        return button;
    };
    ui->stop = headerButton(QStringLiteral("Stop"),
                            QStringLiteral("sessionsStop"),
                            QStringLiteral("stop"));
    ui->resume = headerButton(QStringLiteral("Continue"),
                              QStringLiteral("sessionsContinue"),
                              QStringLiteral("play"));
    ui->openInAgents = headerButton(QStringLiteral("Open in Agents"),
                                    QStringLiteral("sessionsOpenInAgents"),
                                    QStringLiteral("smart-toy"));
    if (next) {
        headerLayout->addWidget(ui->title);
        headerLayout->addWidget(ui->titleMenu);
        headerLayout->addSpacing(4);
        headerLayout->addWidget(ui->branchBadge);
        headerLayout->addWidget(ui->status, 0, Qt::AlignVCenter);
        headerLayout->addSpacing(2);
        headerLayout->addWidget(ui->meta, 1);
        headerLayout->addWidget(ui->stop);
        headerLayout->addWidget(ui->resume);
        headerLayout->addWidget(ui->openInAgents);
        auto *divider = new QFrame(header);
        divider->setObjectName(QStringLiteral("sessionsHeaderDivider"));
        divider->setAttribute(Qt::WA_StyledBackground, true);
        divider->setFixedSize(1, 16);
        headerLayout->addSpacing(2);
        headerLayout->addWidget(divider);
        headerLayout->addSpacing(2);
        ui->panelToggle = new QPushButton(header);
        ui->panelToggle->setObjectName(QStringLiteral("sessionsPanelToggle"));
        ui->panelToggle->setCursor(Qt::PointingHandCursor);
        ui->panelToggle->setCheckable(true);
        ui->panelToggle->setChecked(ui->panelOpen);
        ui->panelToggle->setToolTip(QStringLiteral("Hide details"));
        ui->panelToggle->setFixedSize(28, 28);
        ui->panelToggle->setIconSize(QSize(16, 16));
        headerLayout->addWidget(ui->panelToggle);
    } else {
        headerLayout->addWidget(ui->stop);
        headerLayout->addWidget(ui->resume);
        headerLayout->addWidget(ui->openInAgents);
    }
    centerLayout->addWidget(header);

    ui->relayStrip = new QWidget(center);
    ui->relayStrip->setObjectName(QStringLiteral("sessionsRelayStrip"));
    ui->relayStrip->setAttribute(Qt::WA_StyledBackground, true);
    ui->relayLayout = new QHBoxLayout(ui->relayStrip);
    ui->relayLayout->setContentsMargins(16, 4, 16, 4);
    ui->relayLayout->setSpacing(6);
    auto *relayCaption =
        new QLabel(QStringLiteral("Between sessions"), ui->relayStrip);
    relayCaption->setObjectName(QStringLiteral("sessionsRelayCaption"));
    ui->relayLayout->addWidget(relayCaption);
    ui->relayLayout->addStretch(1);
    ui->relayStrip->hide();
    centerLayout->addWidget(ui->relayStrip);

    if (next) {
        auto *body = new QWidget(center);
        body->setObjectName(QStringLiteral("sessionsBody"));
        auto *bodyLayout = new QHBoxLayout(body);
        bodyLayout->setContentsMargins(0, 0, 0, 0);
        bodyLayout->setSpacing(0);
        ui->column = new QWidget(body);
        ui->column->setObjectName(QStringLiteral("sessionsColumn"));
        ui->columnLayout = new QVBoxLayout(ui->column);
        ui->columnLayout->setContentsMargins(0, 0, 0, 0);
        ui->columnLayout->setSpacing(0);
        bodyLayout->addWidget(ui->column, 1);
        ui->details = new forkmesh::ui::SessionSidePanels(body);
        ui->details->setFixedWidth(300);
        ui->details->hide();
        bodyLayout->addWidget(ui->details);
        centerLayout->addWidget(body, 1);
    }

    ui->centerStack = new QStackedWidget(center);
    auto *empty = new QWidget(ui->centerStack);
    empty->setObjectName(QStringLiteral("sessionsEmpty"));
    auto *emptyLayout = new QVBoxLayout(empty);
    emptyLayout->addStretch(1);
    ui->emptyTitle = new QLabel(empty);
    ui->emptyTitle->setObjectName(QStringLiteral("sessionsEmptyTitle"));
    ui->emptyTitle->setAlignment(Qt::AlignCenter);
    ui->emptyBody = new QLabel(empty);
    ui->emptyBody->setObjectName(QStringLiteral("sessionsEmptyBody"));
    ui->emptyBody->setAlignment(Qt::AlignCenter);
    ui->emptyBody->setWordWrap(true);
    if (next) {
        emptyLayout->setContentsMargins(32, 24, 32, 24);
        auto *emptyRow = new QHBoxLayout;
        emptyRow->setContentsMargins(0, 0, 0, 0);
        ui->emptyColumn = new QWidget(empty);
        ui->emptyColumn->setObjectName(QStringLiteral("sessionsEmptyColumn"));
        ui->emptyColumn->setMaximumWidth(680);
        auto *emptyColumnLayout = new QVBoxLayout(ui->emptyColumn);
        emptyColumnLayout->setContentsMargins(0, 0, 0, 0);
        emptyColumnLayout->setSpacing(6);
        emptyColumnLayout->addWidget(ui->emptyTitle);
        emptyColumnLayout->addWidget(ui->emptyBody);
        ui->composerSlot = new QVBoxLayout;
        ui->composerSlot->setContentsMargins(0, 22, 0, 0);
        emptyColumnLayout->addLayout(ui->composerSlot);
        ui->starters = new QWidget(ui->emptyColumn);
        ui->starters->setObjectName(QStringLiteral("sessionsStarters"));
        auto *startersLayout = new QHBoxLayout(ui->starters);
        startersLayout->setContentsMargins(0, 18, 0, 0);
        startersLayout->setSpacing(8);
        const QList<QStringList> starters{
            {QStringLiteral("Fix a failing test"),
             QStringLiteral("Find the cause and patch it"),
             QStringLiteral("Run the tests, find the one that fails, work out why "
                            "and fix it.")},
            {QStringLiteral("Review recent changes"),
             QStringLiteral("Look over the latest commits"),
             QStringLiteral("Review the most recent commits on this branch and point "
                            "out bugs or risky changes.")},
            {QStringLiteral("Explore the codebase"),
             QStringLiteral("Map a subsystem for me"),
             QStringLiteral("Explore this repository and explain how it is organised: "
                            "the main parts, where they live and how they connect.")}};
        for (const QStringList &starter : starters) {
            auto *card = new QPushButton(ui->starters);
            card->setObjectName(QStringLiteral("sessionsStarter"));
            card->setCursor(Qt::PointingHandCursor);
            card->setToolTip(starter.at(2));
            card->setMinimumHeight(66);
            card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
            auto *cardLayout = new QVBoxLayout(card);
            cardLayout->setContentsMargins(12, 10, 12, 10);
            cardLayout->setSpacing(2);
            auto *cardTitle = new QLabel(starter.at(0), card);
            cardTitle->setObjectName(QStringLiteral("sessionsStarterTitle"));
            cardTitle->setAttribute(Qt::WA_TransparentForMouseEvents, true);
            auto *cardBody = new QLabel(starter.at(1), card);
            cardBody->setObjectName(QStringLiteral("sessionsStarterBody"));
            cardBody->setWordWrap(true);
            cardBody->setAttribute(Qt::WA_TransparentForMouseEvents, true);
            cardLayout->addWidget(cardTitle);
            cardLayout->addWidget(cardBody);
            cardLayout->addStretch(1);
            startersLayout->addWidget(card, 1);
            const QString prompt = starter.at(2);
            connect(card, &QPushButton::clicked, this, [this, prompt] {
                if (!m_sessionsUi || !m_sessionsUi->composer ||
                    !m_sessionsUi->composer->isEnabled())
                    return;
                m_sessionsUi->composer->setPlainText(prompt);
                m_sessionsUi->composer->moveCursor(QTextCursor::End);
                m_sessionsUi->composer->setFocus(Qt::OtherFocusReason);
            });
        }
        ui->starters->hide();
        emptyColumnLayout->addWidget(ui->starters);
        emptyRow->addStretch(1);
        emptyRow->addWidget(ui->emptyColumn, 100);
        emptyRow->addStretch(1);
        emptyLayout->addLayout(emptyRow);
        emptyLayout->addStretch(1);
    } else {
        emptyLayout->addWidget(ui->emptyTitle);
        emptyLayout->addWidget(ui->emptyBody);
        emptyLayout->addStretch(2);
    }
    ui->centerStack->addWidget(empty);

    ui->transcript = new ClaudeTranscriptView(ui->centerStack);
    ui->transcript->setObjectName(QStringLiteral("sessionsTranscript"));
    ui->transcript->setSplitDiffs(
        QSettings().value(kClaudeDiffSplitSetting, false).toBool());
    ui->transcript->setTerminalOutputMode(
        QSettings().value(kTerminalTranscriptOutputSetting, true).toBool());
    ui->transcript->setTheme(currentThemeIsDark());
    ui->centerStack->addWidget(ui->transcript);
    if (next) {
        ui->transcript->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
        if (QWidget *column = ui->transcript->widget())
            column->setMaximumWidth(808);
        ui->columnLayout->addWidget(ui->centerStack, 1);
    } else {
        centerLayout->addWidget(ui->centerStack, 1);
    }

    auto *composerBox = new QFrame(center);
    composerBox->setObjectName(QStringLiteral("sessionsComposer"));
    auto *composerLayout = new QVBoxLayout(composerBox);
    if (next)
        composerLayout->setContentsMargins(10, 12, 8, 8);
    else
        composerLayout->setContentsMargins(12, 10, 12, 10);
    composerLayout->setSpacing(6);
    ui->composer = new QPlainTextEdit(composerBox);
    ui->composer->setObjectName(QStringLiteral("sessionsComposerInput"));
    ui->composer->setTabChangesFocus(true);
    ui->composer->setFixedHeight(next ? 60 : 84);
    composerLayout->addWidget(ui->composer);

    auto *controls = new QHBoxLayout;
    controls->setSpacing(8);
    ui->targetRow = new QWidget(composerBox);
    auto *targetLayout = new QHBoxLayout(ui->targetRow);
    targetLayout->setContentsMargins(0, 0, 0, 0);
    targetLayout->setSpacing(6);
    auto *toLabel = new QLabel(QStringLiteral("To"), ui->targetRow);
    toLabel->setObjectName(QStringLiteral("sessionsComposerLabel"));
    auto *toCombo = new SessionsPickerCombo;
    toCombo->setParent(ui->targetRow);
    toCombo->setObjectName(QStringLiteral("sessionsToCombo"));
    toCombo->setMinimumWidth(180);
    toCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    toCombo->setMinimumContentsLength(18);
    ui->toCombo = toCombo;
    auto *alsoLabel = new QLabel(QStringLiteral("Also send to"), ui->targetRow);
    alsoLabel->setObjectName(QStringLiteral("sessionsComposerLabel"));
    auto *alsoCombo = new SessionsPickerCombo;
    alsoCombo->setParent(ui->targetRow);
    alsoCombo->setObjectName(QStringLiteral("sessionsAlsoCombo"));
    alsoCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    alsoCombo->setMinimumContentsLength(14);
    ui->alsoCombo = alsoCombo;
    ui->forward = new QToolButton(ui->targetRow);
    ui->forward->setObjectName(QStringLiteral("sessionsForward"));
    ui->forward->setText(QString::fromUtf8("Forward last reply to\xE2\x80\xA6"));
    ui->forward->setToolTip(
        QStringLiteral("Send this session's latest reply to another session "
                       "as a message from this session"));
    ui->forward->setPopupMode(QToolButton::InstantPopup);
    ui->forward->setCursor(Qt::PointingHandCursor);
    auto *forwardMenu = new QMenu(ui->forward);
    ui->forward->setMenu(forwardMenu);
    targetLayout->addWidget(toLabel);
    targetLayout->addWidget(toCombo);
    targetLayout->addWidget(alsoLabel);
    targetLayout->addWidget(alsoCombo);
    targetLayout->addWidget(ui->forward);
    controls->addWidget(ui->targetRow);

    ui->newRow = new QWidget(composerBox);
    auto *newLayout = new QHBoxLayout(ui->newRow);
    newLayout->setContentsMargins(0, 0, 0, 0);
    newLayout->setSpacing(6);
    auto *repoLabel = new QLabel(QStringLiteral("Repository"), ui->newRow);
    repoLabel->setObjectName(QStringLiteral("sessionsComposerLabel"));
    ui->repoCombo = new QComboBox(ui->newRow);
    ui->repoCombo->setObjectName(QStringLiteral("sessionsRepoCombo"));
    ui->repoCombo->setMinimumWidth(180);
    auto *providerLabel = new QLabel(QStringLiteral("Agent"), ui->newRow);
    providerLabel->setObjectName(QStringLiteral("sessionsComposerLabel"));
    ui->providerCombo = new QComboBox(ui->newRow);
    ui->providerCombo->setObjectName(QStringLiteral("sessionsProviderCombo"));
    for (const auto &[id, label] :
         QList<QPair<QString, QString>>{
             {QStringLiteral("claude-code"), QStringLiteral("Claude Code")},
             {QStringLiteral("codex"), QStringLiteral("Codex")},
             {QStringLiteral("claude-api"), QStringLiteral("Claude API")},
             {QStringLiteral("openai"), QStringLiteral("OpenAI")}})
        ui->providerCombo->addItem(label, id);
    if (ui->providerCombo->findData(defaultAgentProvider()) < 0)
        ui->providerCombo->addItem(defaultAgentProvider(), defaultAgentProvider());
    selectDefaultAgentProvider(ui->providerCombo);
    newLayout->addWidget(repoLabel);
    newLayout->addWidget(ui->repoCombo);
    newLayout->addWidget(providerLabel);
    newLayout->addWidget(ui->providerCombo);
    ui->newRow->hide();
    controls->addWidget(ui->newRow);

    ui->composerNote = new QLabel(composerBox);
    ui->composerNote->setObjectName(QStringLiteral("sessionsComposerNote"));
    ui->composerNote->setWordWrap(true);
    ui->composerNote->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    controls->addWidget(ui->composerNote, 1);
    if (next) {
        ui->composerModel = new QLabel(composerBox);
        ui->composerModel->setObjectName(QStringLiteral("sessionsComposerModel"));
        ui->composerModel->setTextFormat(Qt::PlainText);
        ui->composerModel->hide();
        controls->addWidget(ui->composerModel);
        ui->send = new QPushButton(composerBox);
        ui->send->setObjectName(QStringLiteral("sessionsSend"));
        ui->send->setCursor(Qt::PointingHandCursor);
        ui->send->setToolTip(QStringLiteral("Send (Enter)"));
        ui->send->setFixedSize(28, 28);
        ui->send->setProperty("sessionsRound", true);
        ui->send->setProperty("nextIconRole", int(Theme::Role::OnAccent));
        setOcticon(ui->send, QStringLiteral("arrow-left"), 14, 90.0);
    } else {
        ui->send = new QPushButton(QStringLiteral("Send"), composerBox);
        ui->send->setObjectName(QStringLiteral("sessionsSend"));
        ui->send->setCursor(Qt::PointingHandCursor);
        setOcticon(ui->send, QStringLiteral("paper-airplane"), 14);
    }
    controls->addWidget(ui->send);
    composerLayout->addLayout(controls);
    if (next) {
        ui->composerHost = new QWidget(ui->column);
        ui->composerHost->setObjectName(QStringLiteral("sessionsComposerHost"));
        ui->composerHostLayout = new QHBoxLayout(ui->composerHost);
        ui->composerHostLayout->setContentsMargins(24, 0, 24, 16);
        ui->composerHostLayout->setSpacing(0);
        ui->composerHostLayout->addStretch(1);
        ui->composerHostLayout->addWidget(composerBox, 100);
        ui->composerHostLayout->addStretch(1);
        composerBox->setMaximumWidth(760);
        composerBox->setProperty("sessionsHosted", true);
        ui->columnLayout->addWidget(ui->composerHost);
    } else {
        centerLayout->addWidget(composerBox);
    }

    split->addWidget(sidebar);
    split->addWidget(center);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setSizes({next ? 264 : 300, 900});

    auto *pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->setSpacing(0);
    pageLayout->addWidget(split);

    connect(ui->newButton, &QPushButton::clicked, this,
            [this] { startNewSessionInSessionsPage(); });
    connect(ui->search, &QLineEdit::textChanged, this,
            [this] { refreshSessionsList(); });
    connect(ui->betweenFilter, &QPushButton::toggled, this,
            [this] { refreshSessionsList(); });
    connect(ui->list, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *item) {
                const int id = item ? item->data(kSessionIdRole).toInt() : 0;
                if (id == 0 || !m_sessionsUi ||
                    (id == m_sessionsUi->openId && !m_sessionsUi->newMode))
                    return;
                openSessionInSessionsPage(id);
            });
    connect(ui->list, &QListWidget::customContextMenuRequested, this,
            [this](const QPoint &at) {
                if (!m_sessionsUi || !m_sessionsUi->list)
                    return;
                QListWidgetItem *item = m_sessionsUi->list->itemAt(at);
                const int id = item ? item->data(kSessionIdRole).toInt() : 0;
                if (id != 0)
                    showSessionsContextMenu(
                        id, m_sessionsUi->list->viewport()->mapToGlobal(at));
            });
    connect(ui->stop, &QPushButton::clicked, this, [this] {
        if (m_sessionsUi && m_sessionsUi->openId != 0)
            stopAgentSessions({m_sessionsUi->openId});
        scheduleSessionsPageRefresh();
    });
    connect(ui->resume, &QPushButton::clicked, this, [this] {
        if (m_sessionsUi && m_sessionsUi->openId != 0)
            continueAgentSessionInteractive(m_sessionsUi->openId);
        scheduleSessionsPageRefresh();
    });
    connect(ui->openInAgents, &QPushButton::clicked, this, [this] {
        if (m_sessionsUi && m_sessionsUi->openId != 0)
            switchToAgentsTab(m_sessionsUi->openId);
    });
    connect(ui->send, &QPushButton::clicked, this,
            [this] { sendSessionsComposer(); });
    toCombo->beforePopup = [this] {
        if (m_sessionsUi && m_sessionsUi->toCombo)
            fillSessionsPicker(m_sessionsUi->toCombo,
                               m_sessionsUi->toCombo->currentData().toInt(), 0,
                               QString());
    };
    alsoCombo->beforePopup = [this] {
        if (m_sessionsUi && m_sessionsUi->alsoCombo)
            fillSessionsPicker(m_sessionsUi->alsoCombo,
                               m_sessionsUi->alsoCombo->currentData().toInt(), 0,
                               QStringLiteral("No one else"));
    };
    connect(forwardMenu, &QMenu::aboutToShow, this, [this, forwardMenu] {
        forwardMenu->clear();
        const int from = m_sessionsUi ? m_sessionsUi->openId : 0;
        if (from == 0 || sessionsLastReply(from).isEmpty()) {
            forwardMenu->addAction(QStringLiteral("No reply to forward yet"))
                ->setEnabled(false);
            return;
        }
        SessionsPickerCombo picker;
        fillSessionsPicker(&picker, 0, from, QString());
        for (int row = 0; row < picker.count(); ++row) {
            const int to = picker.itemData(row).toInt();
            forwardMenu->addAction(picker.itemText(row), this, [this, from, to] {
                QString why;
                if (forwardLastSessionReply(from, to, &why))
                    flashMessage(QStringLiteral("Forwarded the last reply of "
                                                "session #%1 to session #%2.")
                                     .arg(from)
                                     .arg(to));
                else
                    flashMessage(why, true);
            });
        }
        if (forwardMenu->isEmpty())
            forwardMenu->addAction(QStringLiteral("No other session"))
                ->setEnabled(false);
    });

    ui->composer->installEventFilter(new SessionsEventHook(
        [this](QObject *, QEvent *event) {
            if (event->type() != QEvent::KeyPress)
                return false;
            auto *key = static_cast<QKeyEvent *>(event);
            if ((key->key() != Qt::Key_Return && key->key() != Qt::Key_Enter) ||
                (key->modifiers() & Qt::ShiftModifier))
                return false;
            sendSessionsComposer();
            return true;
        },
        ui->composer));
    page->installEventFilter(new SessionsEventHook(
        [this](QObject *, QEvent *event) {
            if (event->type() == QEvent::Show && m_sessionsUi)
                QTimer::singleShot(0, m_sessionsUi, [this] {
                    if (sessionsPageOnScreen())
                        refreshSessionsPage();
                });
            return false;
        },
        page));

    connect(ui->transcript, &ClaudeTranscriptView::referenceActivated, this,
            &MainWindow::openAgentTranscriptReference);
    connect(ui->transcript, &ClaudeTranscriptView::commandRunRequested, this,
            &MainWindow::openAgentCommandTerminal);
    connect(ui->transcript, &ClaudeTranscriptView::questionAnswered, this,
            [this](const QString &toolUseId, const QString &answer,
                   bool sensitive) {
                if (m_sessionsUi && m_sessionsUi->openId != 0)
                    answerAgentQuestion(m_sessionsUi->openId, toolUseId, answer,
                                        sensitive);
            });
    connect(ui->transcript, &ClaudeTranscriptView::inlineChoiceAnswered, this,
            [this](const QString &answer) {
                if (m_sessionsUi && m_sessionsUi->openId != 0)
                    sendPromptToAgentSession(m_sessionsUi->openId, answer);
            });
    connect(ui->transcript, &ClaudeTranscriptView::loadEarlierRequested, this,
            [this] {
                if (!m_sessionsUi || m_sessionsUi->openId == 0 ||
                    m_sessionsUi->source != SessionsSource::Local)
                    return;
                const int limit = m_sessionsUi->renderLimit + 300;
                const int id = m_sessionsUi->openId;
                prepareSessionsTranscript(id);
                m_sessionsUi->renderLimit = limit;
                syncSessionsTranscript();
            });

    ui->emptyTitle->setText(QStringLiteral("No session open"));
    ui->emptyBody->setText(
        QStringLiteral("Pick a session on the left, or start a new one."));
    ui->centerStack->setCurrentIndex(0);

    if (next) {
        auto *filterMenu = new QMenu(ui->filterButton);
        ui->filterButton->setMenu(filterMenu);
        connect(filterMenu, &QMenu::aboutToShow, this, [this, filterMenu] {
            filterMenu->clear();
            SessionsPageUi *ui = m_sessionsUi;
            if (!ui)
                return;
            filterMenu->addSection(QStringLiteral("Show"));
            QAction *between = filterMenu->addAction(
                QStringLiteral("Between sessions only"), this, [this](bool on) {
                    if (m_sessionsUi && m_sessionsUi->betweenFilter)
                        m_sessionsUi->betweenFilter->setChecked(on);
                });
            between->setCheckable(true);
            between->setChecked(ui->betweenFilter && ui->betweenFilter->isChecked());
            between->setToolTip(ui->betweenFilter ? ui->betweenFilter->toolTip()
                                                  : QString());
            filterMenu->addSection(QStringLiteral("Group by"));
            const auto groupAction = [this, filterMenu](const QString &text, bool byRepo) {
                QAction *action = filterMenu->addAction(text, this, [this, byRepo] {
                    if (!m_sessionsUi || m_sessionsUi->groupByRepo == byRepo)
                        return;
                    m_sessionsUi->groupByRepo = byRepo;
                    refreshSessionsList();
                });
                action->setCheckable(true);
                action->setChecked(m_sessionsUi && m_sessionsUi->groupByRepo == byRepo);
            };
            groupAction(QStringLiteral("Repository"), true);
            groupAction(QStringLiteral("Status"), false);
        });
        connect(ui->betweenFilter, &QPushButton::toggled, this, [this](bool on) {
            if (!m_sessionsUi || !m_sessionsUi->filterButton)
                return;
            QPushButton *button = m_sessionsUi->filterButton;
            button->setProperty("filterActive", on);
            button->style()->unpolish(button);
            button->style()->polish(button);
        });
        connect(ui->titleMenu, &QPushButton::clicked, this, [this] {
            if (!m_sessionsUi || m_sessionsUi->openId == 0 || !m_sessionsUi->titleMenu)
                return;
            QPushButton *button = m_sessionsUi->titleMenu;
            showSessionsContextMenu(m_sessionsUi->openId,
                                    button->mapToGlobal(QPoint(0, button->height() + 4)));
        });
        connect(ui->panelToggle, &QPushButton::toggled, this, [this](bool on) {
            if (!m_sessionsUi)
                return;
            m_sessionsUi->panelOpen = on;
            if (m_sessionsUi->panelToggle)
                m_sessionsUi->panelToggle->setToolTip(on ? QStringLiteral("Hide details")
                                                         : QStringLiteral("Show details"));
            refreshSessionsHeader();
        });
        ui->list->viewport()->installEventFilter(new SessionsEventHook(
            [this](QObject *, QEvent *event) {
                if (event->type() != QEvent::MouseButtonPress || !m_sessionsUi ||
                    !m_sessionsUi->list)
                    return false;
                auto *mouse = static_cast<QMouseEvent *>(event);
                if (mouse->button() != Qt::LeftButton)
                    return false;
                QListWidget *list = m_sessionsUi->list;
                const QPoint at = mouse->position().toPoint();
                QListWidgetItem *item = list->itemAt(at);
                if (!item)
                    return false;
                const QRect rect = list->visualItemRect(item);
                if (item->data(kSessionHeaderRole).toBool()) {
                    const QString key = item->data(kSessionGroupRole).toString();
                    if (key.isEmpty())
                        return true;
                    if (item->data(kSessionAddableRole).toBool() &&
                        at.x() >= rect.right() - 26) {
                        startNewSessionInSessionsPage();
                        if (!m_sessionsUi || !m_sessionsUi->repoCombo)
                            return true;
                        for (int index = 0; index < m_repositories.size(); ++index) {
                            const RepositoryRecord &repo = m_repositories.at(index);
                            if (repo.owner + QLatin1Char('/') + repo.name != key)
                                continue;
                            const int row = m_sessionsUi->repoCombo->findData(index);
                            if (row >= 0) {
                                m_sessionsUi->repoCombo->setCurrentIndex(row);
                                break;
                            }
                        }
                        return true;
                    }
                    if (!m_sessionsUi->collapsedGroups.remove(key))
                        m_sessionsUi->collapsedGroups.insert(key);
                    refreshSessionsList();
                    return true;
                }
                const int id = item->data(kSessionIdRole).toInt();
                if (id != 0 && at.x() >= rect.right() - 30) {
                    showSessionsContextMenu(
                        id, list->viewport()->mapToGlobal(
                                QPoint(rect.right() - 26, rect.bottom() + 2)));
                    return true;
                }
                return false;
            },
            ui->list->viewport()));
        retintSessionsIcons(ui);
    }
    return page;
}

bool MainWindow::sessionsPageOnScreen() const
{
    return m_sessionsUi && m_sessionsUi->page && m_sessionsUi->page->isVisible() &&
           !m_sessionsUi->page->window()->isMinimized();
}

void MainWindow::showSessionsPage(int sessionId)
{
    showSection(kSessionsSectionIndex);
    SessionsPageUi *ui = m_sessionsUi;
    if (!ui || !ui->page)
        return;
    if (sessionId != 0) {
        openSessionInSessionsPage(sessionId);
        return;
    }
    if (ui->openId != 0 || ui->newMode)
        return;
    if (ui->listedIds.isEmpty())
        refreshSessionsList();
    for (int id : std::as_const(ui->listedIds)) {
        if (!sessionsHeaderId(id)) {
            openSessionInSessionsPage(id);
            return;
        }
    }
    startNewSessionInSessionsPage();
}

void MainWindow::scheduleSessionsPageRefresh()
{
    SessionsPageUi *ui = m_sessionsUi;
    if (!ui)
        return;
    ui->listDirty = true;
    if (ui->refreshQueued || !sessionsPageOnScreen())
        return;
    ui->refreshQueued = true;
    QTimer::singleShot(400, ui, [this] {
        if (!m_sessionsUi)
            return;
        m_sessionsUi->refreshQueued = false;
        if (sessionsPageOnScreen())
            refreshSessionsPage();
    });
}

void MainWindow::refreshSessionsPage()
{
    SessionsPageUi *ui = m_sessionsUi;
    if (!ui || !ui->page)
        return;
    if (!sessionsPageOnScreen()) {
        ui->listDirty = true;
        return;
    }
    if (ui->listDirty)
        refreshSessionsList();
    if (ui->openId != 0 && !findAgentSession(ui->openId)) {
        ++ui->generation;
        ui->openId = 0;
        ui->queue.clear();
        ui->cursor = 0;
        if (ui->transcript)
            ui->transcript->clear();
        if (ui->emptyTitle)
            ui->emptyTitle->setText(QStringLiteral("Session closed"));
        if (ui->emptyBody)
            ui->emptyBody->setText(
                QStringLiteral("That session was deleted. Pick another one."));
        if (ui->centerStack)
            ui->centerStack->setCurrentIndex(0);
    }
    refreshSessionsHeader();
    refreshSessionsComposer();
    refreshSessionsRelayStrip();
    syncSessionsTranscript();
    if (ui->openId != 0 && isActiveWindow())
        markAgentSessionRead(ui->openId);
}

void MainWindow::refreshSessionsList()
{
    SessionsPageUi *ui = m_sessionsUi;
    if (!ui || !ui->list)
        return;
    ui->listDirty = false;
    const QString needle =
        ui->search ? ui->search->text().trimmed().toLower() : QString();
    const bool between = ui->betweenFilter && ui->betweenFilter->isChecked();

    struct Row {
        const AgentSession *session;
        int rank;
        qint64 at;
    };
    QList<Row> active;
    QList<Row> recent;
    for (const AgentSession &session : std::as_const(m_agentSessions)) {
        if (session.deletedAtMs > 0 || session.associationOnly || session.id == 0)
            continue;
        if (between && !ui->relayCounts.contains(session.id))
            continue;
        if (!needle.isEmpty()) {
            const QString haystack =
                QStringList{sessionsTitle(session), session.owner, session.name,
                            session.branchName, session.provider, session.model,
                            QStringLiteral("#%1").arg(session.id)}
                    .join(QLatin1Char(' '))
                    .toLower();
            if (!haystack.contains(needle))
                continue;
        }
        const int rank = sessionsActiveRank(session);
        qint64 at = sessionsActivityMs(session);
        if (isExternalSession(session.id) && !isCloudSession(session.id))
            at = std::max(at, m_externalSurfaced.value(session.id).lastActivityMs);
        (rank >= 0 ? active : recent).append({&session, rank, at});
    }
    std::stable_sort(active.begin(), active.end(), [](const Row &a, const Row &b) {
        return a.rank != b.rank ? a.rank < b.rank : a.at > b.at;
    });
    std::stable_sort(recent.begin(), recent.end(),
                     [](const Row &a, const Row &b) { return a.at > b.at; });
    if (recent.size() > kSessionsRecentLimit)
        recent = recent.mid(0, kSessionsRecentLimit);

    struct Header {
        QString label;
        QString key;
        bool collapsed = false;
        bool addable = false;
    };
    QList<int> ids;
    QList<const AgentSession *> sessions;
    QList<Header> headers;
    const auto addHeader = [&ids, &sessions, &headers](int id, const Header &header) {
        ids.append(id);
        sessions.append(nullptr);
        headers.append(header);
    };
    const auto addRow = [&ids, &sessions, &headers](const Row &row) {
        ids.append(row.session->id);
        sessions.append(row.session);
        headers.append(Header());
    };
    if (ui->next && ui->groupByRepo) {
        QSet<QString> localRepos;
        for (const RepositoryRecord &repo : std::as_const(m_repositories))
            if (!repo.previewOnly && !repo.localPath.trimmed().isEmpty())
                localRepos.insert(repo.owner + QLatin1Char('/') + repo.name);
        struct Group {
            QString key;
            QString name;
            QList<Row> rows;
            bool hasActive = false;
            qint64 latest = 0;
        };
        QList<Group> groups;
        QHash<QString, int> groupAt;
        const auto place = [&groups, &groupAt](const Row &row) {
            const QString key = sessionsGroupKey(*row.session, true);
            int at = groupAt.value(key, -1);
            if (at < 0) {
                at = int(groups.size());
                groupAt.insert(key, at);
                Group group;
                group.key = key;
                group.name = row.session->owner.isEmpty() ? QStringLiteral("Other sessions")
                                                          : row.session->name;
                groups.append(group);
            }
            Group &group = groups[at];
            group.rows.append(row);
            group.hasActive = group.hasActive || row.rank >= 0;
            group.latest = std::max(group.latest, row.at);
        };
        for (const Row &row : std::as_const(active))
            place(row);
        for (const Row &row : std::as_const(recent))
            place(row);
        std::stable_sort(groups.begin(), groups.end(), [](const Group &a, const Group &b) {
            if (a.hasActive != b.hasActive)
                return a.hasActive;
            return a.latest > b.latest;
        });
        QHash<QString, int> names;
        for (const Group &group : std::as_const(groups))
            ++names[group.name];
        for (const Group &group : std::as_const(groups)) {
            Header header;
            header.key = group.key;
            header.label = names.value(group.name) > 1 && !group.key.startsWith(QLatin1Char('@'))
                               ? group.key
                               : group.name;
            header.collapsed = ui->collapsedGroups.contains(group.key);
            header.addable = localRepos.contains(group.key);
            addHeader(kSessionsHeaderGroup, header);
            if (!header.collapsed)
                for (const Row &row : group.rows)
                    addRow(row);
        }
    } else {
        if (!active.isEmpty()) {
            Header header;
            header.label = QStringLiteral("Active (%1)").arg(active.size());
            if (ui->next) {
                header.key = QStringLiteral("@active");
                header.collapsed = ui->collapsedGroups.contains(header.key);
            }
            addHeader(kSessionsHeaderActive, header);
            if (!header.collapsed)
                for (const Row &row : std::as_const(active))
                    addRow(row);
        }
        if (!recent.isEmpty()) {
            Header header;
            header.label = QStringLiteral("Recent");
            if (ui->next) {
                header.key = QStringLiteral("@recent");
                header.collapsed = ui->collapsedGroups.contains(header.key);
            }
            addHeader(kSessionsHeaderRecent, header);
            if (!header.collapsed)
                for (const Row &row : std::as_const(recent))
                    addRow(row);
        }
    }

    const auto fill = [this, ui](QListWidgetItem *item, int id,
                                 const AgentSession *session, const Header &header) {
        if (!session) {
            item->setData(kSessionIdRole, 0);
            item->setData(kSessionHeaderRole, true);
            item->setData(Qt::DisplayRole, header.label);
            if (ui->next) {
                item->setData(kSessionGroupRole, header.key);
                item->setData(kSessionCollapsedRole, header.collapsed);
                item->setData(kSessionAddableRole, header.addable);
                item->setToolTip(header.addable
                                     ? QStringLiteral("%1. Click to fold, + starts a new "
                                                      "session here.")
                                           .arg(header.key)
                                     : QString());
            }
            item->setFlags(Qt::NoItemFlags);
            return;
        }
        QStringList subtitle;
        if (!session->owner.isEmpty())
            subtitle << session->owner + QLatin1Char('/') + session->name;
        if (!session->branchName.isEmpty())
            subtitle << session->branchName;
        QString provider = agentModelLabel(session->model);
        if (provider.isEmpty())
            provider = cliProviderLabel(session->provider);
        if (isCloudSession(id))
            provider += QStringLiteral(" (cloud)");
        else if (isExternalSession(id))
            provider = QStringLiteral("Claude Code (outside)");
        if (!provider.trimmed().isEmpty())
            subtitle << provider;
        qint64 at = sessionsActivityMs(*session);
        if (isExternalSession(id) && !isCloudSession(id))
            at = std::max(at, m_externalSurfaced.value(id).lastActivityMs);
        item->setData(kSessionIdRole, id);
        item->setData(kSessionHeaderRole, false);
        item->setData(Qt::DisplayRole, sessionsTitle(*session));
        item->setData(kSessionSubtitleRole,
                      subtitle.join(QString::fromUtf8(" \xC2\xB7 ")));
        item->setData(kSessionAgeRole, formatShortRelativeTime(at / 1000));
        item->setData(kSessionDotRole, agentStatusIconColor(*session));
        item->setData(kSessionUnreadRole, agentSessionUnread(*session));
        item->setData(kSessionRelaysRole, ui->relayCounts.value(id));
        const QString tip = QStringLiteral("#%1 %2 \u2014 %3")
                                .arg(id)
                                .arg(sessionsTitle(*session), agentStatusLabel(*session));
        if (ui->next) {
            item->setData(kSessionStateRole, session->merged ? QStringLiteral("merged")
                                                             : session->status);
            const QString detail = subtitle.join(QString::fromUtf8(" \xC2\xB7 "));
            item->setToolTip(detail.isEmpty() ? tip : tip + QLatin1Char('\n') + detail);
        } else {
            item->setToolTip(tip);
        }
    };

    QSignalBlocker block(ui->list);
    if (ids == ui->listedIds && ui->list->count() == ids.size()) {
        for (int row = 0; row < ids.size(); ++row)
            fill(ui->list->item(row), ids.at(row), sessions.at(row), headers.at(row));
        ui->list->viewport()->update();
        return;
    }
    ui->listedIds = ids;
    ui->list->clear();
    for (int row = 0; row < ids.size(); ++row) {
        auto *item = new QListWidgetItem;
        fill(item, ids.at(row), sessions.at(row), headers.at(row));
        ui->list->addItem(item);
        if (ids.at(row) == ui->openId && ui->openId != 0 && !ui->newMode)
            ui->list->setCurrentItem(item);
    }
}

void MainWindow::refreshSessionsHeader()
{
    SessionsPageUi *ui = m_sessionsUi;
    if (!ui || !ui->title)
        return;
    const AgentSession *session =
        ui->newMode || ui->openId == 0 ? nullptr : findAgentSession(ui->openId);
    ui->stop->setVisible(session && isStoppableAgentSession(session->id));
    const bool watchOnly =
        session && isExternalSession(session->id) && !isCloudSession(session->id);
    ui->resume->setVisible(session && !watchOnly &&
                           !isStoppableAgentSession(session->id) &&
                           session->status != AgentStatus::Queued);
    ui->openInAgents->setVisible(session != nullptr);
    if (ui->next) {
        if (ui->header)
            ui->header->setVisible(!ui->newMode);
        if (ui->titleMenu)
            ui->titleMenu->setVisible(session != nullptr);
        if (ui->panelToggle)
            ui->panelToggle->setVisible(session != nullptr);
        QString branch;
        if (session) {
            branch = session->branchName;
            if (branch.isEmpty() && isExternalSession(session->id) &&
                !isCloudSession(session->id))
                branch = m_externalSurfaced.value(session->id).gitBranch;
        }
        if (ui->branchText)
            ui->branchText->setText(branch);
        if (ui->branchBadge) {
            ui->branchBadge->setVisible(!branch.isEmpty());
            ui->branchBadge->setToolTip(branch);
        }
        const bool showDetails = (session || ui->newMode) && ui->panelOpen && ui->details;
        if (ui->details) {
            ui->details->setVisible(showDetails);
            if (showDetails)
                forkmesh::ui::NextAccess::fillSessionSidePanels(*this, ui->details,
                                                                session ? session->id : 0);
        }
    }
    if (!session) {
        ui->title->setText(ui->newMode ? QStringLiteral("New session")
                                       : QStringLiteral("Sessions"));
        ui->meta->setText(
            ui->newMode
                ? QStringLiteral("Pick a repository and an agent, then write the "
                                 "first prompt below.")
                : QStringLiteral("Every agent session on this desktop, live."));
        ui->status->hide();
        return;
    }
    ui->title->setText(sessionsTitle(*session));
    if (ui->next)
        ui->title->setToolTip(ui->title->text());
    QStringList meta{QStringLiteral("#%1").arg(session->id)};
    if (!session->owner.isEmpty())
        meta << session->owner + QLatin1Char('/') + session->name;
    if (!session->branchName.isEmpty() && !ui->next)
        meta << session->branchName;
    QString provider = cliProviderLabel(session->provider);
    const QString model = agentModelLabel(session->model);
    if (!model.isEmpty())
        provider = provider.isEmpty() ? model : provider + QStringLiteral(" / ") + model;
    if (!provider.trimmed().isEmpty())
        meta << provider;
    const QString mode = sessionsModeLabel(*session);
    if (!mode.isEmpty())
        meta << QStringLiteral("%1 mode").arg(mode);
    ui->meta->setText(meta.join(QString::fromUtf8(" \xC2\xB7 ")));
    ui->status->setText(agentStatusLabel(*session));
    const QString state =
        session->merged ? QStringLiteral("merged") : session->status;
    if (ui->status->property("sessionState").toString() != state) {
        ui->status->setProperty("sessionState", state);
        ui->status->style()->unpolish(ui->status);
        ui->status->style()->polish(ui->status);
    }
    ui->status->show();
}

void forkmesh::ui::NextAccess::sessionStarted(MainWindow &window, int sessionId)
{
    SessionsPageUi *ui = window.m_sessionsUi;
    if (ui && (ui->newMode || ui->openId == 0 || ui->openId == sessionId))
        openSessionsPanels(ui);
    workspaceSessionStarted(window, sessionId);
}

void forkmesh::ui::NextAccess::fillSessionSidePanels(MainWindow &window,
                                                     SessionSidePanels *panels, int sessionId)
{
    if (!panels)
        return;
    SessionSidePanels::Details details;
    const AgentSession *session = sessionId != 0 ? window.findAgentSession(sessionId) : nullptr;
    window.fillAgentSubSessions(panels, session ? sessionId : 0);
    QPointer<MainWindow> self(&window);
    panels->setRemoteActions(
        [self](bool on) {
            if (self)
                self->setRemoteControlOn(on);
        },
        [self] {
            if (self)
                self->openPhoneSettings();
        });
    details.remoteOn = window.remoteControlOn();
    if (LocalControlServer *control = window.m_localControl) {
        details.remoteAvailable = true;
        const auto phoneName = [](const LocalControlServer::Device &device) {
            return device.name.isEmpty() ? QStringLiteral("Phone") : device.name;
        };
        for (const LocalControlServer::Device &device : control->pairedDevices()) {
            if (control->isConnected(device.key)) {
                details.remotePhones << phoneName(device);
                details.remoteLocal |= control->connectedLocally(device.key);
            } else if (device.lastSeenMs >= details.remoteLastSeenMs) {
                details.remotePaired = phoneName(device);
                details.remoteLastSeenMs = device.lastSeenMs;
            }
        }
    }
    if (!session) {
        panels->setDetails(details);
        return;
    }
    details.session = true;
    const bool cloud = window.isCloudSession(session->id);
    const bool outside = window.isExternalSession(session->id) && !cloud;
    const AgentDiffStat stat = window.m_agentDiffStats.value(session->id);
    if (cloud) {
        details.machine = session->cloudNode.isEmpty() ? QStringLiteral("Cloud runner")
                                                       : session->cloudNode;
        details.machineNote = session->cloudHere ? QStringLiteral("this computer")
                                                 : QStringLiteral("cloud");
        details.machineIcon = QStringLiteral("cloud");
    } else {
        details.machine = QSysInfo::machineHostName();
        details.machineNote = outside ? QStringLiteral("outside ForkMesh")
                                      : QStringLiteral("this computer");
        details.machineIcon = QStringLiteral("device-desktop");
    }
    QString branch = session->branchName;
    if (branch.isEmpty() && outside)
        branch = window.m_externalSurfaced.value(session->id).gitBranch;
    QString path = stat.worktree;
    if (path.isEmpty() && outside)
        path = window.m_externalSurfaced.value(session->id).cwd;
    if (path.isEmpty())
        path = session->claudeImportCwd;
    details.pathFull = path;
    details.path = path;
    details.branch = branch;
    if (stat.ahead >= 0 && stat.behind >= 0)
        details.branchNote = QString::fromUtf8("\xE2\x86\x91%1 \xE2\x86\x93%2")
                                 .arg(stat.ahead)
                                 .arg(stat.behind);
    QString agent = cliProviderLabel(session->provider);
    const QString model = agentModelLabel(session->model);
    if (!model.isEmpty())
        agent = agent.isEmpty() ? model : agent + QString::fromUtf8(" \xC2\xB7 ") + model;
    if (outside)
        agent = model.isEmpty() ? QStringLiteral("Claude Code")
                                : QStringLiteral("Claude Code \u00B7 ") + model;
    details.agent = agent.trimmed();
    details.mode = window.sessionsModeLabel(*session);
    details.contextUsed = session->contextTokens;
    details.contextWindow = session->contextWindow;
    details.totalTokens = session->totalTokens;
    details.promptTokens = session->promptTokens;
    details.completionTokens = session->completionTokens;
    details.turns = session->numTurns;
    details.costUsd = session->costUsd;
    details.files = stat.files;
    details.added = stat.added;
    details.removed = stat.removed;
    details.dirty = stat.dirty;
    details.conflicted = stat.conflicted;
    details.prNumber = session->prNumber;
    panels->setDetails(details);
}

void forkmesh::ui::NextAccess::feedSessionSidePanels(MainWindow &window,
                                                     SessionSidePanels *panels, int sessionId)
{
    if (!panels)
        return;
    SessionSidePanels::Feed &feed = panels->feed();
    if (feed.session != sessionId) {
        feed = SessionSidePanels::Feed();
        feed.session = sessionId;
        panels->clearActivity();
    }
    fillSessionSidePanels(window, panels, sessionId);
    if (sessionId == 0)
        return;
    if (window.isCloudSession(sessionId)) {
        const auto cloud = window.m_cloudAgentSessions.constFind(sessionId);
        if (cloud == window.m_cloudAgentSessions.constEnd())
            return;
        const int turns = int(cloud->history.size());
        const qint64 tail = cloud->history.isEmpty() ? 0 : cloud->history.last().text.size();
        if (turns != feed.cloudTurns || tail != feed.cloudTail) {
            feed.cloudTurns = turns;
            feed.cloudTail = tail;
            panels->replaceActivity(forkmesh::agents::cloudAgentTranscriptEvents(
                cloud.value(), window.cloudAgentImageDirectory()));
        }
        return;
    }
    const auto events = window.m_streamEvents.constFind(sessionId);
    if (events == window.m_streamEvents.constEnd()) {
        window.ensureStreamEventsLoadedAsync(sessionId);
        return;
    }
    if (events->size() < feed.consumed) {
        panels->clearActivity();
        feed.consumed = 0;
    }
    for (int index = feed.consumed; index < events->size(); ++index)
        panels->append(events->at(index));
    feed.consumed = int(events->size());
}

void MainWindow::fillSessionsPicker(QComboBox *combo, int keepId, int excludeId,
                                    const QString &noneLabel)
{
    if (!combo)
        return;
    QList<const AgentSession *> picks;
    for (const AgentSession &session : std::as_const(m_agentSessions)) {
        if (session.deletedAtMs > 0 || session.associationOnly ||
            session.id == excludeId || session.id == 0)
            continue;
        picks.append(&session);
    }
    std::stable_sort(picks.begin(), picks.end(),
                     [keepId](const AgentSession *a, const AgentSession *b) {
                         if ((a->id == keepId) != (b->id == keepId))
                             return a->id == keepId;
                         const bool aActive = sessionsActiveRank(*a) >= 0;
                         const bool bActive = sessionsActiveRank(*b) >= 0;
                         if (aActive != bActive)
                             return aActive;
                         return sessionsActivityMs(*a) > sessionsActivityMs(*b);
                     });
    if (picks.size() > kSessionsPickerLimit)
        picks = picks.mid(0, kSessionsPickerLimit);
    QSignalBlocker block(combo);
    combo->clear();
    if (!noneLabel.isEmpty())
        combo->addItem(noneLabel, 0);
    for (const AgentSession *session : std::as_const(picks)) {
        QString title = sessionsTitle(*session);
        if (title.size() > 48)
            title = title.left(47) + QString::fromUtf8("\xE2\x80\xA6");
        combo->addItem(QStringLiteral("#%1 %2").arg(session->id).arg(title),
                       session->id);
    }
    const int at = combo->findData(keepId);
    combo->setCurrentIndex(at >= 0 ? at : 0);
}

void MainWindow::refreshSessionsComposer()
{
    SessionsPageUi *ui = m_sessionsUi;
    if (!ui || !ui->composer)
        return;
    ui->targetRow->setVisible(!ui->newMode);
    ui->newRow->setVisible(ui->newMode);
    placeSessionsComposer(ui);
    if (ui->starters)
        ui->starters->setVisible(ui->newMode);
    if (ui->composerModel)
        ui->composerModel->hide();
    if (ui->newMode) {
        if (ui->repoCombo->property("sessionsRepoCount").toInt() !=
                int(m_repositories.size()) ||
            ui->repoCombo->count() == 0) {
            const int keep = ui->repoCombo->count() > 0
                                 ? ui->repoCombo->currentData().toInt()
                                 : resolveAgentRepositoryIndex(m_repoDetailIndex);
            QSignalBlocker block(ui->repoCombo);
            ui->repoCombo->clear();
            for (int index = 0; index < m_repositories.size(); ++index) {
                const RepositoryRecord &repo = m_repositories.at(index);
                if (repo.previewOnly || repo.localPath.trimmed().isEmpty())
                    continue;
                ui->repoCombo->addItem(repo.owner + QLatin1Char('/') + repo.name,
                                       index);
            }
            const int at = ui->repoCombo->findData(keep);
            ui->repoCombo->setCurrentIndex(at >= 0 ? at : 0);
            ui->repoCombo->setProperty("sessionsRepoCount",
                                       int(m_repositories.size()));
        }
        const bool canStart = ui->repoCombo->count() > 0;
        ui->composer->setEnabled(canStart);
        ui->send->setEnabled(canStart);
        ui->composer->setPlaceholderText(
            QString::fromUtf8("Describe the task for the new session\xE2\x80\xA6"));
        ui->composerNote->setText(
            canStart ? QString()
                     : QStringLiteral("Add a repository with a local checkout "
                                      "to start a session."));
        return;
    }
    const AgentSession *session =
        ui->openId == 0 ? nullptr : findAgentSession(ui->openId);
    QString note;
    bool enabled = session != nullptr;
    if (!session) {
        note = QStringLiteral("Open a session or start a new one.");
    } else if (isExternalSession(session->id) && !isCloudSession(session->id) &&
               !outsideAgentSteerable(session->id)) {
        enabled = false;
        note = QStringLiteral("This Claude Code session runs outside ForkMesh "
                              "without a messaging socket, so it can only be "
                              "watched. Forward its reply to another session "
                              "instead.");
    }
    ui->composer->setEnabled(enabled);
    ui->send->setEnabled(enabled);
    if (ui->composerModel && session) {
        QStringList parts;
        const QString mode = sessionsModeLabel(*session);
        if (!mode.isEmpty())
            parts << mode;
        QString model = agentModelLabel(session->model);
        if (model.isEmpty())
            model = cliProviderLabel(session->provider);
        if (!model.trimmed().isEmpty())
            parts << model.trimmed();
        ui->composerModel->setText(parts.join(QString::fromUtf8(" \xC2\xB7 ")));
        ui->composerModel->setVisible(!parts.isEmpty());
    }
    ui->forward->setEnabled(session != nullptr);
    ui->toCombo->setEnabled(enabled);
    ui->alsoCombo->setEnabled(enabled);
    ui->composerNote->setText(note);
    ui->composer->setPlaceholderText(
        session ? QString::fromUtf8("Message #%1 \xE2\x80\x94 Enter sends, "
                                    "Shift+Enter adds a line")
                      .arg(session->id)
                : QString());
}

void MainWindow::refreshSessionsRelayStrip()
{
    SessionsPageUi *ui = m_sessionsUi;
    if (!ui || !ui->relayStrip || !ui->relayLayout)
        return;
    struct Chip {
        QString text;
        int other = 0;
        bool outgoing = false;
    };
    QList<Chip> chips;
    QStringList texts;
    const int open = ui->newMode ? 0 : ui->openId;
    for (auto it = ui->relays.crbegin();
         open != 0 && it != ui->relays.crend() && chips.size() < kSessionsRelayChips;
         ++it) {
        if (it->from != open && it->to != open)
            continue;
        const bool outgoing = it->from == open;
        const int other = outgoing ? it->to : it->from;
        QString title;
        if (const AgentSession *session = findAgentSession(other)) {
            title = sessionsTitle(*session);
            if (title.size() > 28)
                title = title.left(27) + QString::fromUtf8("\xE2\x80\xA6");
        }
        const QString text =
            QStringLiteral("%1 #%2 %3 \u00B7 %4")
                .arg(outgoing ? QString::fromUtf8("\xE2\x86\x92")
                              : QString::fromUtf8("\xE2\x86\x90"))
                .arg(other)
                .arg(title, formatShortRelativeTime(it->atMs / 1000))
                .simplified();
        chips.append({text, other, outgoing});
        texts << text;
    }
    if (texts == ui->relayTexts && ui->relayButtons.size() == chips.size()) {
        ui->relayStrip->setVisible(!chips.isEmpty());
        return;
    }
    ui->relayTexts = texts;
    for (const QPointer<QPushButton> &button : std::as_const(ui->relayButtons))
        if (button)
            button->deleteLater();
    ui->relayButtons.clear();
    for (const Chip &entry : std::as_const(chips)) {
        const int other = entry.other;
        const bool outgoing = entry.outgoing;
        auto *chip = new QPushButton(entry.text, ui->relayStrip);
        chip->setObjectName(QStringLiteral("sessionsRelayChip"));
        chip->setCursor(Qt::PointingHandCursor);
        chip->setToolTip(outgoing
                             ? QStringLiteral("This session sent a message to "
                                              "session #%1. Click to open it.")
                                   .arg(other)
                             : QStringLiteral("Session #%1 sent this session a "
                                              "message. Click to open it.")
                                   .arg(other));
        connect(chip, &QPushButton::clicked, this,
                [this, other] { openSessionInSessionsPage(other); });
        ui->relayLayout->insertWidget(ui->relayLayout->count() - 1, chip);
        ui->relayButtons.append(chip);
    }
    ui->relayStrip->setVisible(!chips.isEmpty());
}

void MainWindow::refreshSessionsPageTheme(bool dark)
{
    if (m_sessionsUi && m_sessionsUi->transcript)
        m_sessionsUi->transcript->setTheme(dark);
    retintSessionsIcons(m_sessionsUi);
}

void MainWindow::recordSessionsRelay(int fromId, int toId, qint64 atMs)
{
    SessionsPageUi *ui = m_sessionsUi;
    if (!ui || fromId == 0 || toId == 0 || fromId == toId)
        return;
    for (const SessionsRelay &relay : std::as_const(ui->relays))
        if (relay.from == fromId && relay.to == toId &&
            qAbs(relay.atMs - atMs) < kSessionsRelayDedupMs)
            return;
    auto at = std::upper_bound(ui->relays.begin(), ui->relays.end(), atMs,
                               [](qint64 value, const SessionsRelay &relay) {
                                   return value < relay.atMs;
                               });
    ui->relays.insert(at, {fromId, toId, atMs});
    while (ui->relays.size() > kSessionsRelayLimit)
        ui->relays.removeFirst();
    ++ui->relayCounts[fromId];
    ++ui->relayCounts[toId];
    if (sessionsPageOnScreen() && (fromId == ui->openId || toId == ui->openId))
        refreshSessionsRelayStrip();
}

void MainWindow::noteSessionsPageEvent(int sessionId, const QJsonObject &event)
{
    SessionsPageUi *ui = m_sessionsUi;
    if (!ui)
        return;
    if (event.value(QStringLiteral("type")).toString() == QLatin1String("_local_user")) {
        const int from =
            sessionsEnvelopeSender(event.value(QStringLiteral("text")).toString());
        if (from != 0) {
            const QDateTime at = ClaudeTranscriptView::eventTime(event);
            recordSessionsRelay(from, sessionId,
                                at.isValid() ? at.toMSecsSinceEpoch()
                                             : QDateTime::currentMSecsSinceEpoch());
        }
    }
    if (sessionId == ui->openId && !ui->newMode &&
        ui->source == SessionsSource::Local) {
        if (!sessionsPageOnScreen()) {
            ui->transcriptDirty = true;
        } else if (!ui->syncQueued) {
            ui->syncQueued = true;
            QTimer::singleShot(0, ui, [this] {
                if (!m_sessionsUi)
                    return;
                m_sessionsUi->syncQueued = false;
                syncSessionsTranscript();
            });
        }
    }
    scheduleSessionsPageRefresh();
}

void MainWindow::openSessionInSessionsPage(int sessionId)
{
    SessionsPageUi *ui = m_sessionsUi;
    if (!ui || !ui->page) {
        showSessionsPage(sessionId);
        return;
    }
    const AgentSession *opened = findAgentSession(sessionId);
    if (!opened)
        return;
    if (ui->next &&
        ui->collapsedGroups.remove(sessionsGroupKey(*opened, ui->groupByRepo)))
        refreshSessionsList();
    const bool changed = ui->openId != sessionId || ui->newMode;
    ui->newMode = false;
    if (changed) {
        ui->openId = sessionId;
        prepareSessionsTranscript(sessionId);
    }
    if (ui->list) {
        QSignalBlocker block(ui->list);
        ui->list->clearSelection();
        for (int row = 0; row < ui->list->count(); ++row) {
            if (ui->list->item(row)->data(kSessionIdRole).toInt() == sessionId) {
                ui->list->setCurrentRow(row);
                ui->list->scrollToItem(ui->list->item(row));
                break;
            }
        }
    }
    if (changed) {
        fillSessionsPicker(ui->toCombo, sessionId, 0, QString());
        fillSessionsPicker(ui->alsoCombo, 0, 0, QStringLiteral("No one else"));
    }
    refreshSessionsHeader();
    refreshSessionsComposer();
    refreshSessionsRelayStrip();
    if (sessionsPageOnScreen()) {
        syncSessionsTranscript();
        if (isActiveWindow())
            markAgentSessionRead(sessionId);
    } else {
        ui->transcriptDirty = true;
    }
}

void MainWindow::startNewSessionInSessionsPage()
{
    if (!m_sessionsUi || !m_sessionsUi->page)
        showSection(kSessionsSectionIndex);
    SessionsPageUi *ui = m_sessionsUi;
    if (!ui || !ui->page)
        return;
    ui->newMode = true;
    ui->openId = 0;
    openSessionsPanels(ui);
    if (ui->details)
        ui->details->clearActivity();
    ++ui->generation;
    ui->queue.clear();
    ui->cursor = 0;
    ui->sliceQueued = false;
    if (ui->transcript) {
        ui->transcript->setBulkPopulate(false);
        ui->transcript->clear();
    }
    ui->emptyTitle->setText(QStringLiteral("Start a new session"));
    ui->emptyBody->setText(
        QStringLiteral("Choose the repository and the agent below, describe the "
                       "task, and press Enter. The session opens here and its "
                       "transcript streams in as it works."));
    ui->centerStack->setCurrentIndex(0);
    if (ui->list) {
        QSignalBlocker block(ui->list);
        ui->list->clearSelection();
        ui->list->setCurrentRow(-1);
    }
    refreshSessionsHeader();
    refreshSessionsComposer();
    refreshSessionsRelayStrip();
    ui->composer->setFocus(Qt::OtherFocusReason);
}

void MainWindow::prepareSessionsTranscript(int sessionId)
{
    SessionsPageUi *ui = m_sessionsUi;
    if (!ui || !ui->transcript)
        return;
    ++ui->generation;
    ui->queue.clear();
    ui->cursor = 0;
    ui->consumed = 0;
    ui->skipped = 0;
    ui->skippedNoticeAdded = false;
    ui->fallbackShown = false;
    ui->sliceQueued = false;
    ui->bulk = false;
    ui->externalOffset = -1;
    ui->externalAgain = false;
    ui->cloudTurns = -1;
    ui->cloudTail = -1;
    ui->renderLimit = 300;
    if (ui->details)
        ui->details->clearActivity();
    ui->source = isCloudSession(sessionId)      ? SessionsSource::Cloud
                 : isExternalSession(sessionId) ? SessionsSource::External
                                                : SessionsSource::Local;
    ClaudeTranscriptView *view = ui->transcript;
    view->setBulkPopulate(false);
    view->clear();
    view->jumpToBottom();
    if (const AgentSession *session = findAgentSession(sessionId)) {
        const bool cli = session->provider == QLatin1String("claude-code") ||
                         agentIsCodexProvider(session->provider);
        const bool outside = ui->source == SessionsSource::External;
        view->setSessionContext(session->branchName,
                                cli && !outside ? session->mode : QString(),
                                outside ? QString() : session->strength);
        view->setCodexStyle(agentIsCodexProvider(session->provider));
        constexpr int kSpeakerAvatarPx = 26;
        QString modelName = agentModelLabel(session->model);
        if (modelName.isEmpty())
            modelName = cliProviderLabel(session->provider);
        view->setSpeakerAvatars(
            roundedAvatar(agentModelIcon(session->provider, session->model)
                              .pixmap(QSize(64, 64)),
                          kSpeakerAvatarPx),
            modelName, roundedAvatar(effectiveUserAvatar(), kSpeakerAvatarPx),
            topBarUserName());
    }
    if (ui->centerStack)
        ui->centerStack->setCurrentWidget(view);
}

void MainWindow::syncSessionsTranscript()
{
    SessionsPageUi *ui = m_sessionsUi;
    if (!ui || !ui->transcript || ui->openId == 0 || ui->newMode)
        return;
    if (!sessionsPageOnScreen()) {
        ui->transcriptDirty = true;
        return;
    }
    ui->transcriptDirty = false;
    const int id = ui->openId;
    if (ui->source == SessionsSource::External) {
        readSessionsExternalTranscript(id);
        scheduleSessionsTranscriptSlice();
        return;
    }
    if (ui->source == SessionsSource::Cloud) {
        fetchCloudAgentDetail(id);
        const auto it = m_cloudAgentSessions.constFind(id);
        if (it == m_cloudAgentSessions.constEnd())
            return;
        const int turns = int(it->history.size());
        const qint64 tail = it->history.isEmpty() ? 0 : it->history.last().text.size();
        if (turns == ui->cloudTurns && tail == ui->cloudTail) {
            scheduleSessionsTranscriptSlice();
            return;
        }
        ++ui->generation;
        ui->queue.clear();
        ui->cursor = 0;
        ui->sliceQueued = false;
        ui->transcript->clear();
        ui->transcript->setBulkPopulate(true);
        if (ui->details)
            ui->details->clearActivity();
        ui->bulk = true;
        ui->cloudTurns = turns;
        ui->cloudTail = tail;
        const QList<QJsonObject> events = forkmesh::agents::cloudAgentTranscriptEvents(
            it.value(), cloudAgentImageDirectory());
        for (const QJsonObject &event : events)
            ui->queue.append({event, SessionsSource::Cloud, false});
        scheduleSessionsTranscriptSlice();
        return;
    }
    if (!m_streamEvents.contains(id)) {
        if (!ensureStreamEventsLoadedAsync(id) && m_streamEventsLoading.contains(id)) {
            const quint64 generation = ui->generation;
            QTimer::singleShot(120, ui, [this, generation] {
                if (m_sessionsUi && m_sessionsUi->generation == generation)
                    syncSessionsTranscript();
            });
        } else if (!ui->fallbackShown && !m_streamEvents.contains(id)) {
            ui->fallbackShown = true;
            if (const AgentSession *session = findAgentSession(id);
                session && !session->prompt.trimmed().isEmpty())
                ui->queue.append({QJsonObject{{QStringLiteral("type"),
                                               QStringLiteral("_local_user")},
                                              {QStringLiteral("text"),
                                               session->prompt}},
                                  SessionsSource::Local, false});
            const QString reply = sessionsLastReply(id);
            if (!reply.isEmpty())
                ui->queue.append(
                    {QJsonObject{
                         {QStringLiteral("type"), QStringLiteral("assistant")},
                         {QStringLiteral("message"),
                          QJsonObject{{QStringLiteral("content"),
                                       QJsonArray{QJsonObject{
                                           {QStringLiteral("type"),
                                            QStringLiteral("text")},
                                           {QStringLiteral("text"), reply}}}}}}},
                     SessionsSource::Local, false});
            scheduleSessionsTranscriptSlice();
        }
        return;
    }
    if (ui->fallbackShown) {
        const int limit = ui->renderLimit;
        prepareSessionsTranscript(id);
        ui->renderLimit = limit;
    }
    const QList<QJsonObject> events = m_streamEvents.value(id);
    if (events.size() < ui->consumed) {
        const int limit = ui->renderLimit;
        prepareSessionsTranscript(id);
        ui->renderLimit = limit;
    }
    if (events.size() > ui->consumed) {
        if (ui->consumed == 0) {
            ui->skipped = qMax(0, int(events.size()) - ui->renderLimit);
            ui->bulk = true;
            ui->transcript->setBulkPopulate(true);
        }
        for (int index = ui->consumed; index < events.size(); ++index) {
            const QJsonObject &event = events.at(index);
            ui->queue.append({event, SessionsSource::Local, index < ui->skipped});
            if (event.value(QStringLiteral("type")).toString() ==
                QLatin1String("_local_user")) {
                const int from = sessionsEnvelopeSender(
                    event.value(QStringLiteral("text")).toString());
                const QDateTime at = ClaudeTranscriptView::eventTime(event);
                if (from != 0 && at.isValid())
                    recordSessionsRelay(from, id, at.toMSecsSinceEpoch());
            }
        }
        ui->consumed = int(events.size());
    }
    scheduleSessionsTranscriptSlice();
}

void MainWindow::readSessionsExternalTranscript(int sessionId)
{
    SessionsPageUi *ui = m_sessionsUi;
    if (!ui)
        return;
    if (ui->externalReading) {
        ui->externalAgain = true;
        return;
    }
    const auto it = m_externalSurfaced.constFind(sessionId);
    if (it == m_externalSurfaced.constEnd())
        return;
    const ExternalClaudeSession ext = it.value();
    const qint64 offset = ui->externalOffset;
    if (offset >= 0 && QFileInfo(ext.path).size() <= offset)
        return;
    ui->externalReading = true;
    const quint64 generation = ui->generation;
    const QStringList credentials = localProviderCredentialValues();
    struct ExternalRead {
        QList<QJsonObject> events;
        qint64 newOffset = 0;
    };
    runOffThread<ExternalRead>(
        [ext, offset, credentials] {
            ExternalRead read;
            const qint64 start =
                offset < 0 ? ClaudeSessionScan::tailStartOffset(ext.path, 400 * 1024)
                           : offset;
            const QList<QJsonObject> raw =
                ClaudeSessionScan::readEvents(ext.path, start, &read.newOffset);
            read.events.reserve(raw.size());
            for (const QJsonObject &event : raw)
                read.events.append(
                    redactProviderCredentials(QJsonValue(event), credentials)
                        .toObject());
            return read;
        },
        [this, sessionId, generation](ExternalRead read) {
            SessionsPageUi *ui = m_sessionsUi;
            if (!ui)
                return;
            ui->externalReading = false;
            if (ui->generation != generation || ui->openId != sessionId) {
                if (ui->externalAgain) {
                    ui->externalAgain = false;
                    syncSessionsTranscript();
                }
                return;
            }
            ui->externalOffset = read.newOffset;
            if (!ui->transcript)
                return;
            if (ui->consumed == 0 && !read.events.isEmpty()) {
                ui->bulk = true;
                ui->transcript->setBulkPopulate(true);
            }
            ui->consumed += int(read.events.size());
            for (const QJsonObject &event : std::as_const(read.events)) {
                ui->queue.append({event, SessionsSource::External, false});
                const QString text = sessionsAssistantText(event);
                if (!text.isEmpty())
                    ui->lastReply.insert(sessionId, text);
            }
            scheduleSessionsTranscriptSlice();
            if (ui->externalAgain) {
                ui->externalAgain = false;
                readSessionsExternalTranscript(sessionId);
            }
        });
}

void MainWindow::scheduleSessionsTranscriptSlice()
{
    SessionsPageUi *ui = m_sessionsUi;
    if (!ui || ui->sliceQueued || ui->cursor >= ui->queue.size())
        return;
    ui->sliceQueued = true;
    const quint64 generation = ui->generation;
    QTimer::singleShot(0, ui, [this, generation] {
        if (!m_sessionsUi || m_sessionsUi->generation != generation)
            return;
        m_sessionsUi->sliceQueued = false;
        renderSessionsTranscriptSlice(generation);
    });
}

void MainWindow::renderSessionsTranscriptSlice(quint64 generation)
{
    SessionsPageUi *ui = m_sessionsUi;
    if (!ui || !ui->transcript || ui->generation != generation)
        return;
    if (!sessionsPageOnScreen()) {
        ui->transcriptDirty = true;
        return;
    }
    ClaudeTranscriptView *view = ui->transcript;
    QElapsedTimer budget;
    budget.start();
    bool handled = false;
    while (ui->cursor < ui->queue.size() && (!handled || budget.elapsed() < 8)) {
        const SessionsQueued item = ui->queue.at(ui->cursor++);
        handled = true;
        if (ui->details)
            ui->details->append(item.event);
        if (item.statsOnly) {
            view->accumulateStatsOnly(item.event);
            continue;
        }
        if (!ui->skippedNoticeAdded && ui->skipped > 0) {
            ui->skippedNoticeAdded = true;
            view->addSkippedNotice(ui->skipped);
        }
        appendSessionsEvent(view, item.event, item.source);
        if (item.source == SessionsSource::Cloud) {
            const QString text = sessionsAssistantText(item.event);
            if (!text.isEmpty())
                ui->lastReply.insert(ui->openId, text);
        }
    }
    if (ui->cursor < ui->queue.size()) {
        scheduleSessionsTranscriptSlice();
        return;
    }
    ui->queue.clear();
    ui->cursor = 0;
    if (ui->bulk) {
        ui->bulk = false;
        view->setBulkPopulate(false);
    }
}

void MainWindow::sendSessionsComposer()
{
    SessionsPageUi *ui = m_sessionsUi;
    if (!ui || !ui->composer || !ui->composer->isEnabled())
        return;
    const QString text = ui->composer->toPlainText().trimmed();
    if (text.isEmpty())
        return;
    if (ui->newMode) {
        const int repoIndex =
            ui->repoCombo->count() > 0 ? ui->repoCombo->currentData().toInt() : -1;
        const QString provider = ui->providerCombo->currentData().toString();
        const int id = startAdHocAgentForRepo(repoIndex, text, provider,
                                              /*createPr=*/false, QString(),
                                              QString(), /*genie=*/false,
                                              /*switchToTab=*/false);
        if (id == 0)
            return;
        ui->composer->clear();
        openSessionsPanels(ui);
        refreshSessionsList();
        openSessionInSessionsPage(id);
        return;
    }
    int to = ui->toCombo ? ui->toCombo->currentData().toInt() : 0;
    if (to == 0)
        to = ui->openId;
    const int also = ui->alsoCombo ? ui->alsoCombo->currentData().toInt() : 0;
    if (to == 0)
        return;
    const auto deliver = [this](int target, const QString &message) {
        if (isExternalSession(target) && !isCloudSession(target))
            deliverToExternalSession(target, message);
        else
            sendPromptToAgentSession(target, message);
    };
    deliver(to, text);
    if (also != 0 && also != to)
        deliver(also, text);
    ui->composer->clear();
    if (ui->alsoCombo)
        ui->alsoCombo->setCurrentIndex(0);
    if (to == ui->openId && ui->transcript)
        ui->transcript->jumpToBottom();
    scheduleSessionsPageRefresh();
}

void MainWindow::deliverToExternalSession(int sessionId, const QString &text)
{
    const auto surfaced = m_externalSurfaced.constFind(sessionId);
    if (surfaced == m_externalSurfaced.constEnd() ||
        !outsideAgentSteerable(sessionId)) {
        flashMessage(QStringLiteral("That Claude Code session runs outside "
                                    "ForkMesh without a messaging socket, so it "
                                    "can only be watched."),
                     true);
        return;
    }
    const QString uuid = surfaced->uuid;
    const QString cwd = surfaced->cwd;
    const QString transcript = surfaced->path;
    const QString home = QDir::homePath();
    const QString accountsRoot =
        forkmesh::paths::dataPath(QStringLiteral("agent-accounts"));
    struct Sent {
        bool ok = false;
        bool held = false;
        QString error;
    };
    runOffThread<Sent>(
        [text, uuid, cwd, transcript, home, accountsRoot] {
            Sent sent;
            const QList<qint64> pids = ClaudeSessionScan::findSessionPids(uuid, cwd);
            const QList<ClaudePeerSession> peers = ClaudePeerMessaging::liveSessions(
                ClaudePeerMessaging::configDirs(home, accountsRoot, {}));
            const ClaudePeerSession *session =
                ClaudePeerMessaging::find(peers, pids, uuid);
            if (!session) {
                sent.error = QStringLiteral("it has no live messaging socket");
                return sent;
            }
            QByteArrayList argv;
            QFile cmdline(QStringLiteral("/proc/%1/cmdline").arg(session->pid));
            if (cmdline.open(QIODevice::ReadOnly))
                argv = cmdline.readAll().split('\0');
            const ClaudeInboundPolicy policy = ClaudePeerMessaging::predictInbound(
                *session, argv, ClaudePeerMessaging::permissionClass(argv, transcript),
                ClaudePeerMessaging::managedSettingsPath());
            if (policy.policy == QLatin1String("refuse")) {
                sent.error = QStringLiteral("it refuses messages from other "
                                            "sessions (%1)")
                                 .arg(policy.cause);
                return sent;
            }
            sent.held = policy.policy == QLatin1String("hold");
            if (sent.held && session->entrypoint != QLatin1String("cli")) {
                sent.error = QStringLiteral("it would hold the message for "
                                            "approval and has no terminal to "
                                            "approve it in");
                return sent;
            }
            sent.ok = ClaudePeerMessaging::deliver(*session, text, &sent.error);
            return sent;
        },
        [this, cwd](Sent sent) {
            if (!sent.ok)
                flashMessage(QStringLiteral("Could not send to the Claude Code "
                                            "session in %1: %2.")
                                 .arg(cwd, sent.error),
                             true);
            else if (sent.held)
                flashMessage(QStringLiteral("Claude Code in %1 holds the message "
                                            "until you approve it in its "
                                            "terminal.")
                                 .arg(cwd));
            else
                logSystem(QStringLiteral("Sent a message to the Claude Code "
                                         "session in %1.")
                              .arg(cwd));
            scheduleSessionsPageRefresh();
        });
}

bool MainWindow::relayBetweenSessions(int fromId, int toId, const QString &text,
                                      QString *why)
{
    const auto fail = [why](const QString &reason) {
        if (why)
            *why = reason;
        return false;
    };
    if (fromId == toId)
        return fail(QStringLiteral("A session cannot message itself."));
    const AgentSession *sender = findAgentSession(fromId);
    const AgentSession *target = findAgentSession(toId);
    if (!sender || !target || target->deletedAtMs > 0 || target->associationOnly)
        return fail(QStringLiteral("Session #%1 is not on this desktop.").arg(toId));
    if (target->merged)
        return fail(QStringLiteral("Session #%1 finished and its work was merged; "
                                   "start a new session instead.")
                        .arg(toId));
    const QString body = text.trimmed().left(hub::kMaxMessageChars);
    if (body.isEmpty())
        return fail(QStringLiteral("There is nothing to send."));
    const bool targetOutside = isExternalSession(toId) && !isCloudSession(toId);
    if (targetOutside && !outsideAgentSteerable(toId))
        return fail(QStringLiteral("Session #%1 runs outside ForkMesh without a "
                                   "messaging socket, so it cannot be messaged.")
                        .arg(toId));
    const QString senderMode = sessionsModeLabel(*sender);
    const QString targetMode = sessionsModeLabel(*target);
    if (!hub::messageAllowed(senderMode, targetMode))
        return fail(QStringLiteral("Session #%1 runs in %2 mode, which allows more "
                                   "than session #%3's %4 mode, so #%3 cannot "
                                   "direct it.")
                        .arg(toId)
                        .arg(targetMode.isEmpty() ? QStringLiteral("an unknown")
                                                  : targetMode)
                        .arg(fromId)
                        .arg(senderMode.isEmpty() ? QStringLiteral("unknown")
                                                  : senderMode));
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!m_agentMessageLimiter.allow(now))
        return fail(QStringLiteral("Sessions may send at most %1 messages every "
                                   "%2 minutes on this desktop.")
                        .arg(hub::kMessageLimit)
                        .arg(hub::kMessageWindowMs / 60000));
    const QString envelope =
        hub::messageEnvelope(fromId, sessionsTitle(*sender), body);
    if (!isExternalSession(fromId))
        noteAgentSessionNotice(fromId,
                               QStringLiteral("Sent a message to agent #%1.").arg(toId));
    if (m_agentStore && !isExternalSession(toId))
        if (const AgentSession *receiver = findAgentSession(toId))
            m_agentStore->appendLog(*receiver,
                                    QStringLiteral("==> Message from agent #%1.\n")
                                        .arg(fromId));
    recordSessionsRelay(fromId, toId, now);
    if (targetOutside)
        deliverToExternalSession(toId, envelope);
    else
        sendPromptToAgentSession(toId, envelope);
    notifyLocalControlAgentsChanged();
    scheduleSessionsPageRefresh();
    return true;
}

bool MainWindow::forwardLastSessionReply(int fromId, int toId, QString *why)
{
    const QString reply = sessionsLastReply(fromId);
    if (reply.isEmpty()) {
        if (why)
            *why = QStringLiteral("Session #%1 has no reply to forward yet.")
                       .arg(fromId);
        return false;
    }
    return relayBetweenSessions(fromId, toId, reply, why);
}

void MainWindow::promptSessionsMessage(int fromId, int toId)
{
    const AgentSession *sender = findAgentSession(fromId);
    if (!sender || !findAgentSession(toId))
        return;
    bool ok = false;
    const QString text = QInputDialog::getMultiLineText(
        this, QStringLiteral("Message to session #%1").arg(toId),
        QStringLiteral("Sent as session #%1 (%2). Session #%3 reads it as a "
                       "message from another agent, not from you.")
            .arg(fromId)
            .arg(sessionsTitle(*sender))
            .arg(toId),
        sessionsLastReply(fromId), &ok);
    if (!ok || text.trimmed().isEmpty())
        return;
    QString why;
    if (relayBetweenSessions(fromId, toId, text, &why))
        flashMessage(QStringLiteral("Session #%1 sent a message to session #%2.")
                         .arg(fromId)
                         .arg(toId));
    else
        flashMessage(why, true);
}

void MainWindow::showSessionsContextMenu(int sessionId, const QPoint &globalPos)
{
    const AgentSession *session = findAgentSession(sessionId);
    if (!session)
        return;
    const bool outside = isExternalSession(sessionId) && !isCloudSession(sessionId);
    QMenu menu(this);
    menu.addAction(QStringLiteral("Open in Agents"), this,
                   [this, sessionId] { switchToAgentsTab(sessionId); });
    QAction *resume = menu.addAction(QStringLiteral("Continue"), this, [this, sessionId] {
        continueAgentSessionInteractive(sessionId);
        scheduleSessionsPageRefresh();
    });
    resume->setEnabled(!outside && !isStoppableAgentSession(sessionId));
    QAction *stop = menu.addAction(QStringLiteral("Stop"), this, [this, sessionId] {
        stopAgentSessions({sessionId});
        scheduleSessionsPageRefresh();
    });
    stop->setEnabled(isStoppableAgentSession(sessionId));
    QAction *toClaude = menu.addAction(QStringLiteral("Open in Claude app"), this,
                                       [this, sessionId] {
                                           addAgentToClaudeDesktop(sessionId, true);
                                       });
    toClaude->setEnabled(claudeDesktopBlocker(sessionId).isEmpty());
    QMenu *message =
        menu.addMenu(QString::fromUtf8("Send message to another session\xE2\x80\xA6"));
    SessionsPickerCombo picker;
    fillSessionsPicker(&picker, 0, sessionId, QString());
    for (int row = 0; row < picker.count(); ++row) {
        const int to = picker.itemData(row).toInt();
        message->addAction(picker.itemText(row), this,
                           [this, sessionId, to] { promptSessionsMessage(sessionId, to); });
    }
    message->setEnabled(!message->isEmpty());
    menu.addSeparator();
    menu.addAction(QString::fromUtf8("Delete\xE2\x80\xA6"), this, [this, sessionId] {
        deleteAgentSessionFromRow(sessionId);
        scheduleSessionsPageRefresh();
    });
    menu.exec(globalPos);
}

#ifdef FORKMESH_WINDOW_TESTS
QList<int> MainWindow::testSessionsListIds() const
{
    QList<int> ids;
    if (!m_sessionsUi)
        return ids;
    for (int id : std::as_const(m_sessionsUi->listedIds))
        if (!sessionsHeaderId(id))
            ids.append(id);
    return ids;
}

int MainWindow::testSessionsOpenId() const
{
    return m_sessionsUi && !m_sessionsUi->newMode ? m_sessionsUi->openId : 0;
}

bool MainWindow::testSessionsTranscriptIdle() const
{
    const SessionsPageUi *ui = m_sessionsUi;
    return ui && ui->cursor >= ui->queue.size() && !ui->externalReading &&
           !ui->sliceQueued && !ui->syncQueued &&
           !m_streamEventsLoading.contains(ui->openId);
}

int MainWindow::testSessionsTranscriptMatches(const QString &text)
{
    if (!m_sessionsUi || !m_sessionsUi->transcript)
        return -1;
    const int matches = m_sessionsUi->transcript->search(text);
    m_sessionsUi->transcript->clearSearch();
    return matches;
}

void MainWindow::testSessionsComposerSend(const QString &text, int alsoTo)
{
    SessionsPageUi *ui = m_sessionsUi;
    if (!ui || !ui->composer)
        return;
    if (alsoTo != 0 && ui->alsoCombo) {
        fillSessionsPicker(ui->alsoCombo, alsoTo, 0, QStringLiteral("No one else"));
    }
    ui->composer->setPlainText(text);
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QCoreApplication::sendEvent(ui->composer, &press);
}

QString MainWindow::testSessionsComposerState() const
{
    const SessionsPageUi *ui = m_sessionsUi;
    if (!ui || !ui->composer)
        return QString();
    return QStringLiteral("%1|%2|%3")
        .arg(ui->composer->isEnabled() ? 1 : 0)
        .arg(ui->toCombo ? ui->toCombo->currentData().toInt() : 0)
        .arg(ui->composerNote ? ui->composerNote->text() : QString());
}

QStringList MainWindow::testSessionsRelayStrip() const
{
    QStringList chips;
    if (!m_sessionsUi || !m_sessionsUi->relayStrip ||
        m_sessionsUi->relayStrip->isHidden())
        return chips;
    for (const QPointer<QPushButton> &chip : std::as_const(m_sessionsUi->relayButtons))
        if (chip)
            chips << chip->text();
    return chips;
}
#endif
