#include "SessionSidePanels.h"

#include "DesignTokens.h"
#include "DesktopUi.h"

#include <QAbstractButton>
#include <QApplication>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace forkmesh::ui {

namespace {

constexpr char kOrderSetting[] = "sessions/panelOrder";
constexpr char kHiddenSetting[] = "sessions/panelHidden";
constexpr char kCollapsedSetting[] = "sessions/panelCollapsed";
constexpr char kMime[] = "application/x-forkmesh-session-panel";
constexpr int kVisibleRows = 8;
constexpr int kEnvironmentRows = 5;
constexpr int kContextRows = 6;
constexpr int kChangesRows = 4;

class ElidedLabel : public QLabel
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

class RemoteSwitch : public QAbstractButton
{
public:
    explicit RemoteSwitch(QWidget *parent) : QAbstractButton(parent)
    {
        setCheckable(true);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        setFixedSize(sizeHint());
    }

    QSize sizeHint() const override { return QSize(32, 18); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF track = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        const qreal radius = track.height() / 2.0;
        QColor fill = isChecked() ? Theme::color(Theme::Role::Accent)
                                  : Theme::color(Theme::Role::BorderStrong);
        if (!isEnabled())
            fill.setAlphaF(0.45);
        painter.setPen(hasFocus() ? QPen(Theme::color(Theme::Role::AccentHover), 1.0)
                                  : QPen(Qt::NoPen));
        painter.setBrush(fill);
        painter.drawRoundedRect(track, radius, radius);
        const qreal knob = track.height() - 4.0;
        const qreal x = isChecked() ? track.right() - 2.0 - knob : track.left() + 2.0;
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(Qt::white));
        painter.drawEllipse(QRectF(x, track.top() + 2.0, knob, knob));
    }
};

enum class Swatch { Used, Free, Input, Output, Turns, Cost };

QColor swatchColor(Swatch swatch)
{
    const bool dark = currentThemeIsDark();
    switch (swatch) {
    case Swatch::Used:
        return Theme::color(Theme::Role::Accent);
    case Swatch::Free:
        return Theme::color(Theme::Role::BorderStrong);
    case Swatch::Input:
        return QColor(dark ? QStringLiteral("#f0883e") : QStringLiteral("#d1661f"));
    case Swatch::Output:
        return QColor(dark ? QStringLiteral("#3fb9a7") : QStringLiteral("#1a8f7f"));
    case Swatch::Turns:
        return QColor(dark ? QStringLiteral("#a371f7") : QStringLiteral("#8250df"));
    case Swatch::Cost:
        return QColor(dark ? QStringLiteral("#d29922") : QStringLiteral("#b07d0a"));
    }
    return QColor();
}

QString shareText(qint64 part, qint64 whole)
{
    if (whole <= 0)
        return QString();
    return QString::number(part * 100.0 / whole, 'f', 1) + QLatin1Char('%');
}

void setPanelIcon(QLabel *label, const QString &name, int size)
{
    label->setProperty("forkmeshOcticon", name);
    label->setProperty("forkmeshOcticonSize", size);
    const QColor color(currentThemeIsDark() ? QStringLiteral("#8b949e")
                                            : QStringLiteral("#656d76"));
    label->setPixmap(tintedOcticonPixmap(name, color, size));
    label->setFixedSize(size, size);
}

QString compactCount(qint64 value)
{
    if (value < 1000)
        return QString::number(value);
    QString number = value < 1000000 ? QString::number(value / 1000.0, 'f', 1)
                                     : QString::number(value / 1000000.0, 'f', 1);
    if (number.endsWith(QLatin1String(".0")))
        number.chop(2);
    return number + QLatin1Char(value < 1000000 ? 'k' : 'M');
}

QString homePath(const QString &path)
{
    const QString home = QDir::homePath();
    if (!home.isEmpty() && (path == home || path.startsWith(home + QLatin1Char('/'))))
        return QLatin1Char('~') + path.mid(home.size());
    return path;
}

QString cardTitle(const QString &key)
{
    static const QHash<QString, QString> titles{
        {QStringLiteral("progress"), QStringLiteral("Progress")},
        {QStringLiteral("subsessions"), QStringLiteral("Sub-sessions")},
        {QStringLiteral("outputs"), QStringLiteral("Outputs")},
        {QStringLiteral("sources"), QStringLiteral("Sources")},
        {QStringLiteral("background"), QStringLiteral("Background")},
        {QStringLiteral("changes"), QStringLiteral("Changes")},
        {QStringLiteral("context"), QStringLiteral("Context")},
        {QStringLiteral("environment"), QStringLiteral("Environment")},
        {QStringLiteral("tools"), QStringLiteral("Tools")},
    };
    return titles.value(key, key);
}

QString cardHint(const QString &key)
{
    static const QHash<QString, QString> hints{
        {QStringLiteral("progress"),
         QStringLiteral("The agent's steps show here once it plans its work.")},
        {QStringLiteral("subsessions"),
         QStringLiteral("Subagents, background shells and workflows show here while they "
                        "run, with Stop.")},
        {QStringLiteral("outputs"),
         QStringLiteral("Files the agent creates or edits show here.")},
        {QStringLiteral("sources"),
         QStringLiteral("Files, web pages, skills and connectors the agent reads show here.")},
        {QStringLiteral("background"),
         QStringLiteral("Subagents and background shells show here while they run.")},
        {QStringLiteral("changes"),
         QStringLiteral("The branch's changed files show here.")},
        {QStringLiteral("context"),
         QStringLiteral("Context use, tokens and cost show once the agent replies.")},
        {QStringLiteral("environment"),
         QStringLiteral("Where the session runs shows here once it starts.")},
        {QStringLiteral("tools"),
         QStringLiteral("Every tool call the agent makes is counted here.")},
    };
    return hints.value(key);
}

QString statusLabel(const QString &status)
{
    if (status == QLatin1String("running"))
        return QStringLiteral("running");
    if (status == QLatin1String("completed"))
        return QStringLiteral("done");
    return status;
}

QString lineStats(int added, int removed)
{
    if (added <= 0 && removed <= 0)
        return QString();
    return QStringLiteral("+%1 −%2").arg(qMax(0, added)).arg(qMax(0, removed));
}

}

SessionSidePanels::SessionSidePanels(QWidget *parent) : QScrollArea(parent)
{
    setObjectName(QStringLiteral("sessionsDetails"));
    setWidgetResizable(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_inner = new QWidget;
    m_inner->setObjectName(QStringLiteral("sessionsDetailsInner"));
    m_inner->setAttribute(Qt::WA_StyledBackground, true);
    m_inner->setAcceptDrops(true);
    m_inner->installEventFilter(this);
    m_layout = new QVBoxLayout(m_inner);
    m_layout->setContentsMargins(10, 10, 10, 10);
    m_layout->setSpacing(8);

    for (const QString &key : sessioninsights::defaultCardOrder())
        makeCard(key, cardTitle(key), cardHint(key));

    Card &env = m_cards[QStringLiteral("environment")];
    for (int row = 0; row < kEnvironmentRows; ++row)
        addRow(env);
    m_remoteRow = new QWidget(env.body);
    m_remoteRow->setObjectName(QStringLiteral("sessionsDetailRow"));
    m_remoteRow->setFixedHeight(30);
    auto *remoteLayout = new QHBoxLayout(m_remoteRow);
    remoteLayout->setContentsMargins(8, 0, 8, 0);
    remoteLayout->setSpacing(10);
    auto *remoteIcon = new QLabel(m_remoteRow);
    setPanelIcon(remoteIcon, QStringLiteral("device-mobile"), 14);
    auto *remoteText = new QLabel(QStringLiteral("Remote control"), m_remoteRow);
    remoteText->setObjectName(QStringLiteral("sessionsDetailText"));
    m_remoteSwitch = new RemoteSwitch(m_remoteRow);
    m_remoteSwitch->setObjectName(QStringLiteral("sessionsRemoteSwitch"));
    m_remoteSwitch->setAccessibleName(QStringLiteral("Remote control"));
    m_remoteSwitch->setToolTip(QStringLiteral("Let your paired phone follow and steer this "
                                              "desktop's sessions"));
    remoteLayout->addWidget(remoteIcon);
    remoteLayout->addWidget(remoteText, 1);
    remoteLayout->addWidget(m_remoteSwitch);
    env.body->layout()->addWidget(m_remoteRow);
    m_remoteStatus = new QLabel(env.body);
    m_remoteStatus->setObjectName(QStringLiteral("sessionsRemoteStatus"));
    m_remoteStatus->setTextFormat(Qt::RichText);
    m_remoteStatus->setWordWrap(true);
    m_remoteStatus->setContentsMargins(32, 0, 8, 4);
    m_remoteStatus->setTextInteractionFlags(Qt::LinksAccessibleByMouse |
                                            Qt::LinksAccessibleByKeyboard);
    env.body->layout()->addWidget(m_remoteStatus);
    QObject::connect(m_remoteSwitch, &QAbstractButton::clicked, this, [this](bool on) {
        if (m_remoteToggle)
            m_remoteToggle(on);
    });
    QObject::connect(m_remoteStatus, &QLabel::linkActivated, this, [this](const QString &) {
        if (m_remoteConnect)
            m_remoteConnect();
    });

    Card &context = m_cards[QStringLiteral("context")];
    m_contextBarRow = new QWidget(context.body);
    m_contextBarRow->setObjectName(QStringLiteral("sessionsDetailRow"));
    auto *barLayout = new QVBoxLayout(m_contextBarRow);
    barLayout->setContentsMargins(8, 0, 8, 8);
    barLayout->setSpacing(6);
    auto *summary = new QHBoxLayout;
    summary->setContentsMargins(0, 0, 0, 0);
    m_contextUsed = new QLabel(m_contextBarRow);
    m_contextUsed->setObjectName(QStringLiteral("sessionsContextUsed"));
    m_contextWindow = new QLabel(m_contextBarRow);
    m_contextWindow->setObjectName(QStringLiteral("sessionsContextWindow"));
    summary->addWidget(m_contextUsed);
    summary->addStretch(1);
    summary->addWidget(m_contextWindow);
    barLayout->addLayout(summary);
    m_contextBar = new QProgressBar(m_contextBarRow);
    m_contextBar->setObjectName(QStringLiteral("sessionsContextBar"));
    m_contextBar->setTextVisible(false);
    m_contextBar->setRange(0, 1000);
    m_contextBar->setFixedHeight(6);
    barLayout->addWidget(m_contextBar);
    context.body->layout()->addWidget(m_contextBarRow);
    for (int row = 0; row < kContextRows; ++row)
        addRow(context);

    Card &changes = m_cards[QStringLiteral("changes")];
    changes.meta->setTextFormat(Qt::RichText);
    for (int row = 0; row < kChangesRows; ++row)
        addRow(changes);

    m_layout->addStretch(1);
    setWidget(m_inner);
    reloadLayout();
    refreshActivity();
    refreshDetails();
}

SessionSidePanels::Card &SessionSidePanels::makeCard(const QString &key, const QString &title,
                                                     const QString &hint)
{
    Card card;
    card.key = key;
    card.frame = new QFrame(m_inner);
    card.frame->setObjectName(QStringLiteral("sessionsDetailCard"));
    card.frame->setProperty("sessionsPanel", key);
    card.frame->setAttribute(Qt::WA_StyledBackground, true);
    auto *layout = new QVBoxLayout(card.frame);
    layout->setContentsMargins(0, 0, 0, 6);
    layout->setSpacing(0);
    card.head = new QPushButton(card.frame);
    card.head->setObjectName(QStringLiteral("sessionsDetailHeader"));
    card.head->setCursor(Qt::OpenHandCursor);
    card.head->setFixedHeight(40);
    card.head->setProperty("sessionsPanel", key);
    card.head->setToolTip(QStringLiteral("Click to fold %1, drag to move it, right-click for "
                                         "more")
                              .arg(title));
    card.head->setAccessibleName(title);
    card.head->setContextMenuPolicy(Qt::DefaultContextMenu);
    card.head->installEventFilter(this);
    auto *headLayout = new QHBoxLayout(card.head);
    headLayout->setContentsMargins(14, 0, 14, 0);
    headLayout->setSpacing(6);
    auto *titleLabel = new QLabel(title, card.head);
    titleLabel->setObjectName(QStringLiteral("sessionsDetailTitle"));
    titleLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    card.chevron = new QLabel(card.head);
    card.chevron->setObjectName(QStringLiteral("sessionsDetailChevron"));
    card.chevron->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    card.meta = new QLabel(card.head);
    card.meta->setObjectName(QStringLiteral("sessionsDetailMeta"));
    card.meta->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    headLayout->addWidget(titleLabel);
    headLayout->addWidget(card.chevron);
    headLayout->addStretch(1);
    headLayout->addWidget(card.meta);
    card.body = new QWidget(card.frame);
    card.body->setObjectName(QStringLiteral("sessionsDetailBody"));
    auto *bodyLayout = new QVBoxLayout(card.body);
    bodyLayout->setContentsMargins(6, 0, 6, 0);
    bodyLayout->setSpacing(0);
    card.hint = new QLabel(hint, card.body);
    card.hint->setObjectName(QStringLiteral("sessionsDetailHint"));
    card.hint->setWordWrap(true);
    card.hint->setContentsMargins(8, 2, 8, 6);
    bodyLayout->addWidget(card.hint);
    layout->addWidget(card.head);
    layout->addWidget(card.body);
    m_layout->addWidget(card.frame);
    const QString cardKey = key;
    QObject::connect(card.head, &QPushButton::clicked, this, [this, cardKey] {
        setCardCollapsed(cardKey, !m_collapsed.contains(cardKey));
    });
    m_cards.insert(key, card);
    return m_cards[key];
}

SessionSidePanels::Row &SessionSidePanels::addRow(Card &card)
{
    Row row;
    row.row = new QWidget(card.body);
    row.row->setObjectName(QStringLiteral("sessionsDetailRow"));
    row.row->setFixedHeight(30);
    row.row->installEventFilter(this);
    auto *layout = new QHBoxLayout(row.row);
    layout->setContentsMargins(8, 0, 8, 0);
    layout->setSpacing(10);
    row.swatch = new QLabel(row.row);
    row.swatch->setObjectName(QStringLiteral("sessionsDetailSwatch"));
    row.swatch->setFixedSize(10, 10);
    row.swatch->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    row.swatch->hide();
    layout->addWidget(row.swatch);
    row.icon = new QLabel(row.row);
    row.icon->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    row.icon->hide();
    layout->addWidget(row.icon);
    row.text = new ElidedLabel(row.row);
    row.text->setObjectName(QStringLiteral("sessionsDetailText"));
    row.text->setTextFormat(Qt::PlainText);
    row.text->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    layout->addWidget(row.text, 1);
    row.trailing = new QLabel(row.row);
    row.trailing->setObjectName(QStringLiteral("sessionsDetailTrailing"));
    row.trailing->setTextFormat(Qt::PlainText);
    row.trailing->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    layout->addWidget(row.trailing);
    row.share = new QLabel(row.row);
    row.share->setObjectName(QStringLiteral("sessionsDetailShare"));
    row.share->setTextFormat(Qt::PlainText);
    row.share->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row.share->setMinimumWidth(42);
    row.share->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    row.share->hide();
    layout->addWidget(row.share);
    if (card.key == QLatin1String("subsessions")) {
        row.stop = new QToolButton(row.row);
        row.stop->setObjectName(QStringLiteral("sessionsSubSessionStop"));
        row.stop->setText(QStringLiteral("Stop"));
        row.stop->setCursor(Qt::PointingHandCursor);
        row.stop->setToolTip(QStringLiteral("Stop this sub-session; the main session keeps going"));
        row.stop->hide();
        layout->addWidget(row.stop);
        const QPointer<QWidget> owner = row.row;
        QObject::connect(row.stop, &QToolButton::clicked, this, [this, owner] {
            const QString task = owner ? owner->property("sessionsTask").toString() : QString();
            if (!task.isEmpty() && m_stopSubSession)
                m_stopSubSession(task);
        });
    }
    auto *body = qobject_cast<QVBoxLayout *>(card.body->layout());
    const int at = card.more ? body->indexOf(card.more) : body->count();
    body->insertWidget(at, row.row);
    card.rows.append(row);
    return card.rows.last();
}

void SessionSidePanels::setRow(const Row &row, const Item &item)
{
    if (!row.row || !row.text || !row.trailing || !row.icon)
        return;
    row.row->setVisible(!item.text.isEmpty());
    if (row.swatch) {
        row.swatch->setVisible(item.swatch.isValid());
        if (item.swatch.isValid()) {
            const QString style = QStringLiteral("background-color:%1;border-radius:3px;")
                                      .arg(item.swatch.name());
            if (row.swatch->styleSheet() != style)
                row.swatch->setStyleSheet(style);
        }
    }
    if (row.share) {
        row.share->setText(item.share);
        row.share->setVisible(item.swatch.isValid() || !item.share.isEmpty());
    }
    if (item.icon.isEmpty()) {
        row.icon->hide();
    } else {
        if (row.icon->property("forkmeshOcticon").toString() != item.icon)
            setPanelIcon(row.icon, item.icon, 14);
        row.icon->show();
    }
    if (row.text->text() != item.text)
        row.text->setText(item.text);
    row.trailing->setText(item.trailing);
    row.trailing->setVisible(!item.trailing.isEmpty());
    row.row->setToolTip(item.tip);
    row.row->setProperty("sessionsOpen", item.open);
    row.row->setCursor(item.open.isEmpty() ? Qt::ArrowCursor : Qt::PointingHandCursor);
}

void SessionSidePanels::fillFixed(Card &card, const QList<Item> &items, const QString &meta)
{
    bool any = false;
    for (int index = 0; index < card.rows.size(); ++index) {
        const Item item = index < items.size() ? items.at(index) : Item();
        setRow(card.rows.at(index), item);
        any |= !item.text.isEmpty();
    }
    if (card.meta && card.meta->text() != meta)
        card.meta->setText(meta);
    if (card.hint)
        card.hint->setVisible(!any);
}

void SessionSidePanels::fillList(Card &card, const QList<Item> &items, const QString &meta)
{
    const int shown = card.expanded ? int(items.size()) : qMin(int(items.size()), kVisibleRows);
    while (card.rows.size() < shown)
        addRow(card);
    for (int index = 0; index < card.rows.size(); ++index)
        setRow(card.rows.at(index), index < shown ? items.at(index) : Item());
    if (card.meta && card.meta->text() != meta)
        card.meta->setText(meta);
    if (card.hint)
        card.hint->setVisible(items.isEmpty());
    const bool needMore = items.size() > kVisibleRows;
    if (needMore && !card.more) {
        card.more = new QPushButton(card.body);
        card.more->setObjectName(QStringLiteral("sessionsDetailMore"));
        card.more->setCursor(Qt::PointingHandCursor);
        card.more->setFlat(true);
        card.body->layout()->addWidget(card.more);
        const QString key = card.key;
        QObject::connect(card.more, &QPushButton::clicked, this, [this, key] {
            auto it = m_cards.find(key);
            if (it == m_cards.end())
                return;
            it->expanded = !it->expanded;
            refreshActivity();
        });
    }
    if (card.more) {
        card.more->setVisible(needMore);
        card.more->setText(card.expanded ? QStringLiteral("Show less")
                                         : QStringLiteral("Show %1 more")
                                               .arg(items.size() - kVisibleRows));
    }
}

void SessionSidePanels::refreshActivity()
{
    namespace si = sessioninsights;
    {
        Card &card = m_cards[QStringLiteral("progress")];
        QList<Item> items;
        const agenttasks::Tracker &tracker = m_collector.tasks();
        for (const agenttasks::TaskGroup &group : tracker.groups()) {
            for (const agenttasks::Task &task : group.tasks) {
                Item item;
                item.icon = task.completed()    ? QStringLiteral("check-circle")
                            : task.inProgress() ? QStringLiteral("sync")
                                                : QStringLiteral("clock");
                item.text = task.inProgress() && !task.activeForm.isEmpty() ? task.activeForm
                                                                            : task.subject;
                if (group.subagent())
                    item.trailing = group.title;
                item.tip = task.subject;
                items << item;
            }
        }
        const agenttasks::Counts counts = tracker.counts();
        fillList(card, items,
                 counts.total > 0 ? QStringLiteral("%1 of %2").arg(counts.completed).arg(counts.total)
                                  : QString());
    }
    {
        Card &card = m_cards[QStringLiteral("outputs")];
        QList<Item> items;
        for (const si::Entry &entry : m_collector.outputs()) {
            Item item;
            item.icon = entry.icon;
            item.text = entry.label;
            item.trailing = entry.created ? QStringLiteral("new")
                                          : lineStats(entry.added, entry.removed);
            if (entry.created && entry.added > 0)
                item.trailing = QStringLiteral("new · %1 lines").arg(entry.added);
            item.tip = entry.count > 1 ? QStringLiteral("%1\n%2 edits").arg(entry.detail).arg(entry.count)
                                       : entry.detail;
            item.open = entry.key;
            items << item;
        }
        fillList(card, items, items.isEmpty() ? QString() : QString::number(items.size()));
    }
    {
        Card &card = m_cards[QStringLiteral("sources")];
        QList<Item> items;
        for (const si::Entry &entry : m_collector.sources()) {
            Item item;
            item.icon = entry.icon;
            item.text = entry.label;
            item.trailing = entry.count > 1 ? QStringLiteral("×%1").arg(entry.count) : QString();
            item.tip = entry.detail;
            if (entry.icon == QLatin1String("file") || entry.icon == QLatin1String("link"))
                item.open = entry.key;
            items << item;
        }
        fillList(card, items, items.isEmpty() ? QString() : QString::number(items.size()));
    }
    {
        Card &card = m_cards[QStringLiteral("background")];
        QList<Item> items;
        for (const si::Background &task : m_collector.background()) {
            Item item;
            item.icon = task.kind == QLatin1String("agent") ? QStringLiteral("smart-toy")
                                                            : QStringLiteral("terminal");
            item.text = task.label;
            item.trailing = statusLabel(task.status);
            item.tip = QStringLiteral("%1 · %2")
                           .arg(task.kind == QLatin1String("agent") ? QStringLiteral("Subagent")
                                                                    : QStringLiteral("Background shell"),
                                statusLabel(task.status));
            items << item;
        }
        const int running = m_collector.runningBackground();
        fillList(card, items,
                 running > 0 ? QStringLiteral("%1 running").arg(running)
                             : items.isEmpty() ? QString() : QString::number(items.size()));
    }
    {
        Card &card = m_cards[QStringLiteral("tools")];
        QList<Item> items;
        for (const si::ToolCount &tool : m_collector.tools()) {
            Item item;
            item.text = tool.name;
            item.trailing = tool.errors > 0
                                ? QStringLiteral("%1 · %2 failed").arg(tool.calls).arg(tool.errors)
                                : QString::number(tool.calls);
            item.tip = item.text;
            items << item;
        }
        const int calls = m_collector.toolCalls();
        fillList(card, items,
                 calls > 0 ? (calls == 1 ? QStringLiteral("1 call") : QStringLiteral("%1 calls").arg(calls))
                           : QString());
    }
}

void SessionSidePanels::refreshDetails()
{
    const Details &d = m_details;
    {
        Card &env = m_cards[QStringLiteral("environment")];
        fillFixed(env,
                  {{d.machineIcon, d.machine, d.machineNote, QString(), QString()},
                   {QStringLiteral("file-directory"), homePath(d.path), QString(), d.pathFull,
                    QFileInfo(d.pathFull).isDir() ? d.pathFull : QString()},
                   {QStringLiteral("git-branch"), d.branch, d.branchNote, d.branch, QString()},
                   {QStringLiteral("smart-toy"), d.agent, QString(), QString(), QString()},
                   {QStringLiteral("shield-check"),
                    d.mode.isEmpty() ? QString() : QStringLiteral("%1 mode").arg(d.mode),
                    QString(), QString(), QString()}},
                  QString());
    }
    {
        Card &context = m_cards[QStringLiteral("context")];
        const bool hasWindow = d.contextWindow > 0;
        if (m_contextBarRow)
            m_contextBarRow->setVisible(hasWindow);
        const qint64 used = qBound<qint64>(0, d.contextUsed, qMax<qint64>(0, d.contextWindow));
        const qint64 room = qMax<qint64>(0, d.contextWindow - used);
        const int percent = hasWindow ? qRound(used * 100.0 / d.contextWindow) : 0;
        const QString level = percent >= 95   ? QStringLiteral("full")
                              : percent >= 80 ? QStringLiteral("high")
                                              : QString();
        if (m_contextBar && hasWindow) {
            m_contextBar->setValue(int(used * 1000 / d.contextWindow));
            if (m_contextBar->property("sessionsLevel").toString() != level) {
                m_contextBar->setProperty("sessionsLevel", level);
                m_contextBar->style()->unpolish(m_contextBar);
                m_contextBar->style()->polish(m_contextBar);
            }
        }
        if (m_contextUsed)
            m_contextUsed->setText(hasWindow ? QStringLiteral("%1% used").arg(percent) : QString());
        if (m_contextWindow)
            m_contextWindow->setText(hasWindow ? QStringLiteral("%1 / %2")
                                                     .arg(compactCount(used),
                                                          compactCount(d.contextWindow))
                                               : QString());
        const QColor usedColor = level == QLatin1String("full")   ? Theme::color(Theme::Role::Danger)
                                 : level == QLatin1String("high") ? Theme::color(Theme::Role::Warning)
                                                                  : swatchColor(Swatch::Used);
        const qint64 tokens = qMax(d.totalTokens, d.promptTokens + d.completionTokens);
        const bool split = d.promptTokens > 0 || d.completionTokens > 0;
        QList<Item> items;
        items << Item{QString(), hasWindow ? QStringLiteral("Used") : QString(), compactCount(used),
                      QStringLiteral("Tokens in the context window now"), QString(), usedColor,
                      shareText(used, d.contextWindow)};
        items << Item{QString(), hasWindow ? QStringLiteral("Free space") : QString(),
                      compactCount(room),
                      QStringLiteral("Room left in the context window before it fills"),
                      QString(), swatchColor(Swatch::Free), shareText(room, d.contextWindow)};
        if (split) {
            items << Item{QString(), d.promptTokens > 0 ? QStringLiteral("Input tokens") : QString(),
                          compactCount(d.promptTokens),
                          QStringLiteral("Tokens the agent read, over the whole session"),
                          QString(), swatchColor(Swatch::Input), shareText(d.promptTokens, tokens)};
            items << Item{QString(),
                          d.completionTokens > 0 ? QStringLiteral("Output tokens") : QString(),
                          compactCount(d.completionTokens),
                          QStringLiteral("Tokens the agent wrote, over the whole session"),
                          QString(), swatchColor(Swatch::Output),
                          shareText(d.completionTokens, tokens)};
        } else {
            items << Item{QString(), d.totalTokens > 0 ? QStringLiteral("Tokens used") : QString(),
                          compactCount(d.totalTokens),
                          QStringLiteral("Tokens used over the whole session"), QString(),
                          swatchColor(Swatch::Input), QString()};
            items << Item();
        }
        items << Item{QString(), d.turns > 0 ? QStringLiteral("Turns") : QString(),
                      QString::number(d.turns), QString(), QString(), swatchColor(Swatch::Turns),
                      QString()};
        items << Item{QString(), d.costUsd > 0.0 ? QStringLiteral("Cost") : QString(),
                      QStringLiteral("$%1").arg(d.costUsd, 0, 'f', 2), QString(), QString(),
                      swatchColor(Swatch::Cost), QString()};
        fillFixed(context, items, hasWindow ? QStringLiteral("%1%").arg(percent) : QString());
        if (context.hint)
            context.hint->setVisible(!hasWindow && tokens <= 0 && d.turns <= 0 &&
                                     d.costUsd <= 0.0);
    }
    {
        Card &changes = m_cards[QStringLiteral("changes")];
        const bool hasStat = d.files >= 0;
        fillFixed(changes,
                  {{QStringLiteral("file-diff"),
                    !hasStat        ? QString()
                    : d.files == 0  ? QStringLiteral("No files changed")
                    : d.files == 1  ? QStringLiteral("1 file changed")
                                    : QStringLiteral("%1 files changed").arg(d.files),
                    QString(), QString(), QString()},
                   {QStringLiteral("pencil"),
                    d.dirty > 0 ? QStringLiteral("%1 uncommitted").arg(d.dirty) : QString(),
                    QString(), QString(), QString()},
                   {QStringLiteral("git-pull-request"),
                    d.prNumber > 0 ? QStringLiteral("Pull request #%1").arg(d.prNumber)
                                   : QString(),
                    QString(), QString(), QString()},
                   {QStringLiteral("alert"),
                    d.conflicted ? QStringLiteral("Conflicts with the base branch") : QString(),
                    QString(), QString(), QString()}},
                  hasStat && d.added >= 0 && d.removed >= 0
                      ? QStringLiteral("<span style=\"color:%1\">+%2</span>&nbsp;"
                                       "<span style=\"color:%3\">−%4</span>")
                            .arg(Theme::hex(Theme::Role::Success))
                            .arg(d.added)
                            .arg(Theme::hex(Theme::Role::Danger))
                            .arg(d.removed)
                      : QString());
    }
    refreshRemote();
}

void SessionSidePanels::refreshRemote()
{
    if (!m_remoteSwitch || !m_remoteStatus)
        return;
    const Details &d = m_details;
    const bool on = d.remoteAvailable && d.remoteOn;
    m_remoteSwitch->setEnabled(d.remoteAvailable && m_remoteToggle != nullptr);
    if (m_remoteSwitch->isChecked() != on)
        m_remoteSwitch->setChecked(on);
    const QString link = QStringLiteral("<a href=\"connect\" style=\"color:%1;"
                                        "text-decoration:none\">%2</a>")
                             .arg(Theme::hex(Theme::Role::Accent));
    QString html;
    if (!d.remoteAvailable) {
        html = QStringLiteral("Your phone can control this desktop once ForkMesh has set up "
                              "its identity here.");
    } else if (!d.remoteOn) {
        html = QStringLiteral("Off. Your phone can't see or steer this desktop's sessions "
                              "until you turn this back on.");
    } else if (!d.remotePhones.isEmpty()) {
        QStringList names;
        for (const QString &phone : d.remotePhones)
            names << QStringLiteral("<b>%1</b>").arg(phone.toHtmlEscaped());
        html = QStringLiteral("<span style=\"color:%1\">&#9679;</span> %2 connected %3.<br>"
                              "From your phone you can follow this session, send it prompts, "
                              "and stop or continue it.")
                   .arg(Theme::hex(Theme::Role::Success), names.join(QStringLiteral(", ")),
                        d.remoteLocal ? QStringLiteral("on this network")
                                      : QStringLiteral("through the relay"));
    } else if (!d.remotePaired.isEmpty()) {
        const QString seen =
            d.remoteLastSeenMs > 0
                ? QStringLiteral(", last seen %1")
                      .arg(QDateTime::fromMSecsSinceEpoch(d.remoteLastSeenMs)
                               .toString(QStringLiteral("d MMM, HH:mm")))
                : QString();
        html = QStringLiteral("<b>%1</b> is paired but not connected%2. Open ForkMesh on the "
                              "phone to reconnect. %3")
                   .arg(d.remotePaired.toHtmlEscaped(), seen,
                        link.arg(QStringLiteral("Phone settings")));
    } else {
        html = QStringLiteral("No phone connected. Get the ForkMesh app, open it on this Wi-Fi "
                              "and choose this desktop to follow and steer sessions from "
                              "anywhere. %1")
                   .arg(link.arg(QStringLiteral("Connect a phone")));
    }
    if (m_remoteStatus->text() != html)
        m_remoteStatus->setText(html);
}

void SessionSidePanels::setDetails(const Details &details)
{
    m_details = details;
    refreshDetails();
}

void SessionSidePanels::setRemoteActions(std::function<void(bool)> toggle,
                                         std::function<void()> connect)
{
    m_remoteToggle = std::move(toggle);
    m_remoteConnect = std::move(connect);
    refreshRemote();
}

void SessionSidePanels::setSubSessions(const QList<SubSession> &subSessions, bool canStop,
                                       std::function<void(const QString &)> stop)
{
    m_stopSubSession = std::move(stop);
    Card &card = m_cards[QStringLiteral("subsessions")];
    while (card.rows.size() < subSessions.size())
        addRow(card);
    for (int index = 0; index < card.rows.size(); ++index) {
        const Row &row = card.rows.at(index);
        const SubSession sub = index < subSessions.size() ? subSessions.at(index) : SubSession();
        setRow(row, {sub.icon, sub.text, sub.trailing, sub.tip, QString()});
        if (!row.row)
            continue;
        row.row->setProperty("sessionsTask", sub.id);
        if (QLayout *layout = row.row->layout())
            layout->setContentsMargins(8 + qBound(0, sub.depth, 4) * 14, 0, 8, 0);
        if (row.stop)
            row.stop->setVisible(canStop && !sub.id.isEmpty());
    }
    const QString meta =
        subSessions.isEmpty() ? QString() : QStringLiteral("%1 running").arg(subSessions.size());
    if (card.meta && card.meta->text() != meta)
        card.meta->setText(meta);
    if (card.hint)
        card.hint->setVisible(subSessions.isEmpty());
}

void SessionSidePanels::clearActivity()
{
    ++m_generation;
    m_queue.clear();
    m_cursor = 0;
    m_sliceQueued = false;
    m_collector.clear();
    for (Card &card : m_cards)
        card.expanded = false;
    refreshActivity();
}

void SessionSidePanels::append(const QJsonObject &event)
{
    m_queue.append(event);
    scheduleSlice();
}

void SessionSidePanels::replaceActivity(const QList<QJsonObject> &events)
{
    clearActivity();
    m_queue = events;
    scheduleSlice();
}

void SessionSidePanels::scheduleSlice()
{
    if (m_sliceQueued || m_cursor >= m_queue.size() || !isVisible())
        return;
    m_sliceQueued = true;
    const quint64 generation = m_generation;
    QTimer::singleShot(0, this, [this, generation] {
        if (generation != m_generation)
            return;
        m_sliceQueued = false;
        runSlice();
    });
}

void SessionSidePanels::runSlice()
{
    if (!isVisible())
        return;
    QElapsedTimer budget;
    budget.start();
    bool changed = false;
    while (m_cursor < m_queue.size() && budget.elapsed() < 8)
        changed |= m_collector.ingest(m_queue.at(m_cursor++));
    if (m_cursor >= m_queue.size()) {
        m_queue.clear();
        m_cursor = 0;
    } else {
        scheduleSlice();
    }
    if (changed)
        refreshActivity();
}

void SessionSidePanels::showEvent(QShowEvent *event)
{
    QScrollArea::showEvent(event);
    reloadLayout();
    scheduleSlice();
}

void SessionSidePanels::reloadLayout()
{
    QSettings settings;
    const QStringList order = sessioninsights::normalizedCardOrder(
        settings.value(QLatin1String(kOrderSetting)).toStringList());
    const QStringList hiddenList = settings.value(QLatin1String(kHiddenSetting)).toStringList();
    const QStringList collapsedList =
        settings.value(QLatin1String(kCollapsedSetting)).toStringList();
    const QSet<QString> hidden(hiddenList.cbegin(), hiddenList.cend());
    const QSet<QString> collapsed(collapsedList.cbegin(), collapsedList.cend());
    if (order == m_order && hidden == m_hidden && collapsed == m_collapsed)
        return;
    m_order = order;
    m_hidden = hidden;
    m_collapsed = collapsed;
    applyOrder();
    for (Card &card : m_cards)
        applyCollapsed(card);
}

void SessionSidePanels::saveLayout() const
{
    QSettings settings;
    settings.setValue(QLatin1String(kOrderSetting), m_order);
    settings.setValue(QLatin1String(kHiddenSetting), QStringList(m_hidden.cbegin(), m_hidden.cend()));
    settings.setValue(QLatin1String(kCollapsedSetting),
                      QStringList(m_collapsed.cbegin(), m_collapsed.cend()));
}

void SessionSidePanels::applyOrder()
{
    for (const QString &key : std::as_const(m_order)) {
        const Card &card = m_cards[key];
        if (card.frame)
            m_layout->removeWidget(card.frame);
    }
    int at = 0;
    for (const QString &key : std::as_const(m_order)) {
        const Card &card = m_cards[key];
        if (!card.frame)
            continue;
        m_layout->insertWidget(at++, card.frame);
        card.frame->setVisible(!m_hidden.contains(key));
    }
}

void SessionSidePanels::applyCollapsed(Card &card)
{
    const bool open = !m_collapsed.contains(card.key);
    if (card.body)
        card.body->setVisible(open);
    if (card.meta && card.key == QLatin1String("context"))
        card.meta->setVisible(!open);
    if (card.chevron)
        setPanelIcon(card.chevron,
                        open ? QStringLiteral("chevron-down") : QStringLiteral("chevron-right"), 12);
}

QStringList SessionSidePanels::visibleCards() const
{
    QStringList shown;
    for (const QString &key : m_order)
        if (!m_hidden.contains(key))
            shown << key;
    return shown;
}

QFrame *SessionSidePanels::cardFrame(const QString &key) const
{
    const auto it = m_cards.constFind(key);
    return it == m_cards.constEnd() ? nullptr : it->frame.data();
}

void SessionSidePanels::moveCard(const QString &key, int toIndex)
{
    const QStringList moved = sessioninsights::movedCard(m_order, key, toIndex);
    if (moved == m_order)
        return;
    m_order = moved;
    applyOrder();
    saveLayout();
}

void SessionSidePanels::setCardHidden(const QString &key, bool hidden)
{
    if (!m_cards.contains(key) || m_hidden.contains(key) == hidden)
        return;
    if (hidden)
        m_hidden.insert(key);
    else
        m_hidden.remove(key);
    applyOrder();
    saveLayout();
}

void SessionSidePanels::setCardCollapsed(const QString &key, bool collapsed)
{
    auto it = m_cards.find(key);
    if (it == m_cards.end() || m_collapsed.contains(key) == collapsed)
        return;
    if (collapsed)
        m_collapsed.insert(key);
    else
        m_collapsed.remove(key);
    applyCollapsed(*it);
    saveLayout();
}

void SessionSidePanels::resetCards()
{
    m_order = sessioninsights::defaultCardOrder();
    m_hidden.clear();
    m_collapsed.clear();
    applyOrder();
    for (Card &card : m_cards)
        applyCollapsed(card);
    saveLayout();
}

void SessionSidePanels::cardMenu(const QString &key, const QPoint &globalPos)
{
    QMenu menu(this);
    const qsizetype index = m_order.indexOf(key);
    QAction *up = menu.addAction(QStringLiteral("Move up"));
    up->setEnabled(index > 0);
    QAction *down = menu.addAction(QStringLiteral("Move down"));
    down->setEnabled(index >= 0 && index < m_order.size() - 1);
    QAction *fold = menu.addAction(m_collapsed.contains(key) ? QStringLiteral("Unfold")
                                                             : QStringLiteral("Fold"));
    QAction *hide = menu.addAction(QStringLiteral("Hide %1").arg(cardTitle(key)));
    menu.addSeparator();
    QMenu *show = menu.addMenu(QStringLiteral("Show panel"));
    for (const QString &other : std::as_const(m_order)) {
        if (!m_hidden.contains(other))
            continue;
        QAction *action = show->addAction(cardTitle(other));
        action->setData(other);
    }
    show->setEnabled(!show->isEmpty());
    QAction *reset = menu.addAction(QStringLiteral("Reset panels"));
    QAction *chosen = menu.exec(globalPos);
    if (!chosen)
        return;
    if (chosen == up)
        moveCard(key, int(index) - 1);
    else if (chosen == down)
        moveCard(key, int(index) + 1);
    else if (chosen == fold)
        setCardCollapsed(key, !m_collapsed.contains(key));
    else if (chosen == hide)
        setCardHidden(key, true);
    else if (chosen == reset)
        resetCards();
    else if (!chosen->data().toString().isEmpty())
        setCardHidden(chosen->data().toString(), false);
}

int SessionSidePanels::dropIndex(int y, const QString &dragging) const
{
    int index = 0;
    for (const QString &key : m_order) {
        if (key == dragging)
            continue;
        const QFrame *frame = cardFrame(key);
        if (!frame || frame->isHidden()) {
            ++index;
            continue;
        }
        if (y < frame->geometry().center().y())
            return index;
        ++index;
    }
    return index;
}

void SessionSidePanels::startDrag(const QString &key)
{
    QFrame *frame = cardFrame(key);
    if (!frame)
        return;
    m_dragKey = key;
    auto *mime = new QMimeData;
    mime->setData(QLatin1String(kMime), key.toUtf8());
    auto *drag = new QDrag(this);
    drag->setMimeData(mime);
    const QPixmap shot = frame->grab();
    if (!shot.isNull()) {
        QPixmap faded(shot.size());
        faded.setDevicePixelRatio(shot.devicePixelRatio());
        faded.fill(Qt::transparent);
        QPainter painter(&faded);
        painter.setOpacity(0.85);
        painter.drawPixmap(0, 0, shot);
        painter.end();
        drag->setPixmap(faded);
        drag->setHotSpot(QPoint(m_pressPos.x(), qMin(m_pressPos.y(), 20)));
    }
    frame->setProperty("sessionsDragging", true);
    frame->style()->unpolish(frame);
    frame->style()->polish(frame);
    drag->exec(Qt::MoveAction);
    if (QFrame *after = cardFrame(key)) {
        after->setProperty("sessionsDragging", false);
        after->style()->unpolish(after);
        after->style()->polish(after);
    }
    const auto it = m_cards.constFind(key);
    if (it != m_cards.constEnd() && it->head)
        it->head->setDown(false);
    m_dragKey.clear();
    m_pressKey.clear();
    saveLayout();
}

bool SessionSidePanels::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_inner) {
        switch (event->type()) {
        case QEvent::DragEnter: {
            auto *drag = static_cast<QDragEnterEvent *>(event);
            if (!drag->mimeData()->hasFormat(QLatin1String(kMime)))
                return false;
            drag->acceptProposedAction();
            return true;
        }
        case QEvent::DragMove: {
            auto *drag = static_cast<QDragMoveEvent *>(event);
            if (!drag->mimeData()->hasFormat(QLatin1String(kMime)))
                return false;
            const QString key = QString::fromUtf8(drag->mimeData()->data(QLatin1String(kMime)));
            if (m_cards.contains(key)) {
                const int y = drag->position().toPoint().y();
                const int target = dropIndex(y, key);
                if (m_order.indexOf(key) != target) {
                    m_order = sessioninsights::movedCard(m_order, key, target);
                    applyOrder();
                    m_layout->activate();
                }
                const int edge = 28;
                const int top = m_inner->mapTo(viewport(), QPoint(0, y)).y();
                if (top < edge)
                    verticalScrollBar()->setValue(verticalScrollBar()->value() - 12);
                else if (top > viewport()->height() - edge)
                    verticalScrollBar()->setValue(verticalScrollBar()->value() + 12);
            }
            drag->acceptProposedAction();
            return true;
        }
        case QEvent::Drop: {
            auto *drop = static_cast<QDropEvent *>(event);
            if (!drop->mimeData()->hasFormat(QLatin1String(kMime)))
                return false;
            drop->acceptProposedAction();
            saveLayout();
            return true;
        }
        default:
            return false;
        }
    }
    const QString key = watched->property("sessionsPanel").toString();
    if (!key.isEmpty() && m_cards.contains(key) && watched == m_cards.value(key).head) {
        switch (event->type()) {
        case QEvent::MouseButtonPress: {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton) {
                m_pressKey = key;
                m_pressPos = mouse->position().toPoint();
            }
            return false;
        }
        case QEvent::MouseMove: {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (m_pressKey != key || !(mouse->buttons() & Qt::LeftButton) ||
                !m_dragKey.isEmpty())
                return false;
            if ((mouse->position().toPoint() - m_pressPos).manhattanLength() <
                QApplication::startDragDistance())
                return false;
            startDrag(key);
            return true;
        }
        case QEvent::MouseButtonRelease:
            m_pressKey.clear();
            return false;
        case QEvent::ContextMenu: {
            auto *menu = static_cast<QContextMenuEvent *>(event);
            cardMenu(key, menu->globalPos());
            return true;
        }
        default:
            return false;
        }
    }
    if (event->type() == QEvent::MouseButtonRelease) {
        auto *widget = qobject_cast<QWidget *>(watched);
        auto *mouse = static_cast<QMouseEvent *>(event);
        const QString target = watched->property("sessionsOpen").toString();
        if (widget && mouse->button() == Qt::LeftButton && !target.isEmpty() &&
            widget->rect().contains(mouse->position().toPoint())) {
            const QUrl url = target.contains(QLatin1String("://"))
                                 ? QUrl(target)
                                 : QUrl::fromLocalFile(target);
            if (url.isLocalFile() && !QFileInfo::exists(url.toLocalFile()))
                return false;
            QDesktopServices::openUrl(url);
            return true;
        }
    }
    return false;
}

}
