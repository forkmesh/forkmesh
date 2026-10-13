#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "AppPaths.h"
#include "FlowLayout.h"
#include "TeammateWorkspace.h"

#include <QCheckBox>
#include <QGridLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStyle>

#include <algorithm>

using namespace forkmesh::ui;
using forkmesh::teammates::AgentChoice;
using forkmesh::teammates::ChatEntry;
using forkmesh::teammates::Config;
using forkmesh::teammates::LoadResult;
using forkmesh::teammates::Store;
using forkmesh::teammates::Widget;
using forkmesh::teammates::WidgetItem;
using forkmesh::teammates::WidgetProposal;
using forkmesh::teammates::Workspace;
using forkmesh::teammates::WorkspaceStore;

namespace {

constexpr int kTeammateHomePageIndex = 3;
constexpr int kHomeAvatar = 84;
constexpr int kHomePlay = 28;
constexpr int kHomeRuns = 12;
constexpr int kHomeMessages = 40;
constexpr int kHomeOutputLimit = 4000;
constexpr int kHomeSaid = 30;

bool homeStatusInFlight(const QString &status)
{
    return status == QLatin1String("Working") ||
           status == QLatin1String("Queued") ||
           status == QLatin1String("Waiting");
}

QString homeStatusKind(const QString &status)
{
    if (status == QLatin1String("Working"))
        return QStringLiteral("working");
    if (status == QLatin1String("Scheduled") || status == QLatin1String("Queued"))
        return QStringLiteral("scheduled");
    if (status == QLatin1String("Waiting"))
        return QStringLiteral("waiting");
    return QStringLiteral("idle");
}

QColor homeRunWindowColor(forkmesh::teammates::RunWindowState state, bool dark)
{
    using forkmesh::teammates::RunWindowState;
    switch (state) {
    case RunWindowState::Fresh:
        return dark ? QColor("#3fb950") : QColor("#1a7f37");
    case RunWindowState::DueSoon:
        return dark ? QColor("#f0883e") : QColor("#bc4c00");
    case RunWindowState::Overdue:
        return dark ? QColor("#f85149") : QColor("#cf222e");
    case RunWindowState::Off:
        break;
    }
    return {};
}

QString homeCompact(QString text, int limit)
{
    text = text.simplified();
    if (text.size() <= limit)
        return text;
    return text.left(limit - 1).trimmed() + QString::fromUtf8("\xE2\x80\xA6");
}

QString homeDisplayOutput(QString output)
{
    static const QRegularExpression fence(
        QStringLiteral("```[ \\t]*forkmesh-widget[^\\n]*\\n.*?```"),
        QRegularExpression::DotMatchesEverythingOption);
    output.replace(fence, QStringLiteral("*Widget proposal: see Widgets.*"));
    output = output.trimmed();
    if (output.size() > kHomeOutputLimit)
        output = output.left(kHomeOutputLimit).trimmed() +
                 QString::fromUtf8("\xE2\x80\xA6");
    return output;
}

qint64 homeRunStart(const AgentSession &session)
{
    return qMax(session.createdAtMs, session.startedAtMs);
}

qint64 homeRunKey(const AgentSession &session)
{
    if (AgentStatus::isTerminal(session.status) && session.finishedAtMs > 0)
        return session.finishedAtMs;
    return homeRunStart(session);
}

void clearHomeLayout(QLayout *layout)
{
    if (!layout)
        return;
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        } else if (QLayout *child = item->layout()) {
            clearHomeLayout(child);
        }
        delete item;
    }
}

QFrame *homeSection(const QString &title, QVBoxLayout **contentOut,
                    QHBoxLayout **headerOut = nullptr,
                    QLabel **titleOut = nullptr)
{
    auto *frame = new QFrame;
    frame->setObjectName(QStringLiteral("teammateSection"));
    auto *layout = new QVBoxLayout(frame);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(8);
    auto *header = new QHBoxLayout;
    header->setSpacing(6);
    auto *heading = new QLabel(title);
    heading->setObjectName(QStringLiteral("teammateSectionTitle"));
    header->addWidget(heading);
    header->addStretch(1);
    layout->addLayout(header);
    if (contentOut)
        *contentOut = layout;
    if (headerOut)
        *headerOut = header;
    if (titleOut)
        *titleOut = heading;
    return frame;
}

QLabel *homePill(const QString &text, const QString &kind)
{
    auto *label = new QLabel(text);
    label->setObjectName(QStringLiteral("teammatePill"));
    label->setProperty("kind", kind);
    label->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    return label;
}

QPushButton *homeButton(const QString &text, const QString &objectName,
                        const QString &icon)
{
    auto *button = new QPushButton(text);
    button->setObjectName(objectName);
    button->setCursor(Qt::PointingHandCursor);
    if (!icon.isEmpty())
        setOcticon(button, icon, 13);
    return button;
}

void repolish(QWidget *widget)
{
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

QString checklistText(const Widget &widget)
{
    QStringList lines;
    for (const WidgetItem &item : widget.items)
        lines.append((item.done ? QStringLiteral("[x] ") : QString()) + item.text);
    return lines.join(QLatin1Char('\n'));
}

QList<WidgetItem> checklistFromText(const QString &text)
{
    QList<WidgetItem> items;
    for (QString line : text.split(QLatin1Char('\n'))) {
        line = line.trimmed();
        WidgetItem item;
        if (line.startsWith(QLatin1String("[x]"), Qt::CaseInsensitive)) {
            item.done = true;
            line = line.mid(3).trimmed();
        } else if (line.startsWith(QLatin1String("[ ]"))) {
            line = line.mid(3).trimmed();
        }
        item.text = line;
        if (!item.text.isEmpty())
            items.append(item);
    }
    return items;
}

QString linksText(const Widget &widget)
{
    QStringList lines;
    for (const WidgetItem &item : widget.items)
        lines.append(item.text == item.url
                         ? item.url
                         : QStringLiteral("%1 | %2").arg(item.text, item.url));
    return lines.join(QLatin1Char('\n'));
}

QList<WidgetItem> linksFromText(const QString &text)
{
    QList<WidgetItem> items;
    for (const QString &raw : text.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.isEmpty())
            continue;
        WidgetItem item;
        const int bar = line.lastIndexOf(QLatin1Char('|'));
        if (bar >= 0) {
            item.text = line.left(bar).trimmed();
            item.url = line.mid(bar + 1).trimmed();
        } else {
            item.url = line;
        }
        items.append(item);
    }
    return items;
}

QString widgetSummary(const Widget &widget)
{
    using namespace forkmesh::teammates;
    if (widget.kind == kWidgetChecklist)
        return QStringLiteral("%1 item%2 · %3 open")
            .arg(widget.items.size())
            .arg(widget.items.size() == 1 ? QString() : QStringLiteral("s"))
            .arg(openChecklistItems(widget));
    if (widget.kind == kWidgetGoal)
        return QStringLiteral("%1 of %2%3")
            .arg(QString::number(widget.value, 'g', 10),
                 QString::number(widget.target, 'g', 10),
                 widget.unit.isEmpty() ? QString()
                                       : QStringLiteral(" ") + widget.unit);
    if (widget.kind == kWidgetLinks)
        return QStringLiteral("%1 link%2")
            .arg(widget.items.size())
            .arg(widget.items.size() == 1 ? QString() : QStringLiteral("s"));
    return homeCompact(widget.text, 160);
}

class TeammatePlayBadge : public QPushButton
{
public:
    TeammatePlayBadge(QColor fill, bool inFlight, bool dark, int side,
                      QWidget *parent)
        : QPushButton(parent), m_fill(std::move(fill)), m_inFlight(inFlight),
          m_dark(dark)
    {
        setFixedSize(side, side);
        setAttribute(Qt::WA_Hover);
        setCursor(Qt::PointingHandCursor);
        setFlat(true);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF bounds = QRectF(rect()).adjusted(0.8, 0.8, -0.8, -0.8);
        QColor fill = m_fill;
        if (isDown())
            fill = fill.darker(118);
        else if (underMouse())
            fill = fill.lighter(m_dark ? 125 : 112);
        painter.setPen(QPen(m_dark ? QColor("#0d1117") : QColor("#ffffff"), 1.6));
        painter.setBrush(fill);
        painter.drawEllipse(bounds);
        const QPointF center = bounds.center();
        const qreal side = bounds.width();
        if (m_inFlight) {
            QPen pen(QColor("#ffffff"), qMax(1.2, side * 0.09));
            pen.setCapStyle(Qt::RoundCap);
            pen.setJoinStyle(Qt::RoundJoin);
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            const qreal unit = side * 0.14;
            QPainterPath chevron;
            chevron.moveTo(center + QPointF(-1.9 * unit, -1.2 * unit));
            chevron.lineTo(center + QPointF(-0.7 * unit, 0.0));
            chevron.lineTo(center + QPointF(-1.9 * unit, 1.2 * unit));
            painter.drawPath(chevron);
            painter.drawLine(center + QPointF(0.2 * unit, 1.2 * unit),
                             center + QPointF(1.9 * unit, 1.2 * unit));
            return;
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor("#ffffff"));
        const qreal height = side * 0.42;
        QPainterPath triangle;
        triangle.moveTo(center + QPointF(-height * 0.36, -height / 2));
        triangle.lineTo(center + QPointF(-height * 0.36, height / 2));
        triangle.lineTo(center + QPointF(height * 0.52, 0.0));
        triangle.closeSubpath();
        painter.drawPath(triangle);
    }

private:
    QColor m_fill;
    bool m_inFlight;
    bool m_dark;
};

} // namespace

QString MainWindow::teammateWorkspaceRoot(int repoIndex) const
{
    const QString digest = teammateRepoDigest(repoIndex);
    if (digest.isEmpty())
        return {};
    return QDir(forkmesh::paths::dataRoot())
        .filePath(QStringLiteral("teammate-workspaces/%1").arg(digest));
}

Workspace MainWindow::loadTeammateWorkspace(const QString &id, int repoIndex) const
{
    const QString root = teammateWorkspaceRoot(repoIndex);
    if (root.isEmpty())
        return {};
    return WorkspaceStore(root).load(id);
}

bool MainWindow::saveTeammateWorkspace(const QString &id,
                                       const Workspace &workspace,
                                       int repoIndex) const
{
    const QString root = teammateWorkspaceRoot(repoIndex);
    return !root.isEmpty() && WorkspaceStore(root).save(id, workspace);
}

void MainWindow::updateTeammateWorkspace(
    const QString &id, const std::function<void(Workspace &)> &change,
    bool rebuild)
{
    Workspace workspace = loadTeammateWorkspace(id);
    change(workspace);
    if (!saveTeammateWorkspace(id, workspace))
        flashMessage(QStringLiteral("Could not save the special agent's page."),
                     true);
    if (rebuild && id == m_teammateHomeId)
        refreshTeammateHome(true);
}

QPushButton *MainWindow::makeTeammatePlayButton(const Config &config,
                                                bool inFlight, int sessionId,
                                                int side, QWidget *parent)
{
    const bool dark = currentThemeIsDark();
    forkmesh::teammates::RunWindowState windowState =
        forkmesh::teammates::runWindowState(config);
    if (inFlight && windowState != forkmesh::teammates::RunWindowState::Off)
        windowState = forkmesh::teammates::RunWindowState::Fresh;
    QColor fill;
    if (!config.enabled)
        fill = dark ? QColor("#6e7681") : QColor("#8c959f");
    else if (inFlight)
        fill = dark ? QColor("#2ea043") : QColor("#1a7f37");
    else if (windowState != forkmesh::teammates::RunWindowState::Off)
        fill = homeRunWindowColor(windowState, dark);
    else
        fill = dark ? QColor("#2f81f7") : QColor("#0969da");
    auto *button = new TeammatePlayBadge(fill, inFlight, dark, side, parent);
    button->setObjectName(QStringLiteral("teammatePlayButton"));
    button->setProperty("teammateId", config.id);
    button->setProperty("inFlight", inFlight);
    button->setProperty("runWindowState", static_cast<int>(windowState));
    const AgentChoice primary = forkmesh::teammates::agentChoiceFor(config);
    const QString modelLabel = agentModelLabel(primary.model).isEmpty()
                                   ? agentProviderName(primary.provider)
                                   : agentModelLabel(primary.model);
    QString tip;
    if (inFlight)
        tip = sessionId > 0
                  ? QStringLiteral("%1 is working · open session #%2")
                        .arg(config.name)
                        .arg(sessionId)
                  : QStringLiteral("%1 is working").arg(config.name);
    else if (!config.enabled)
        tip = QStringLiteral("%1 is paused · make it available from the ⋯ menu")
                  .arg(config.name);
    else
        tip = QStringLiteral("Run %1 now on %2").arg(config.name, modelLabel);
    if (config.runWindowHours > 0 && config.enabled)
        tip += QStringLiteral("\nReminder: %1")
                   .arg(forkmesh::teammates::humanRunWindow(config));
    button->setToolTip(tip);
    button->setAccessibleName(
        inFlight ? QStringLiteral("Open %1's running session").arg(config.name)
                 : QStringLiteral("Run %1").arg(config.name));
    connect(button, &QPushButton::clicked, this,
            [this, id = config.id, inFlight, sessionId] {
                if (inFlight) {
                    if (sessionId > 0)
                        switchToAgentsTab(sessionId);
                    return;
                }
                runTeammateFromRail(id);
                scheduleTeammateHomeRefresh();
            });
    return button;
}

void MainWindow::openTeammateHome(const QString &id)
{
    m_teammatesStatusFilter.clear();
    showSection(kTeamSectionIndex);
    QTimer::singleShot(0, this, [this, id] { showTeammateHome(id); });
}

QWidget *MainWindow::buildTeammateHomePage()
{
    auto *page = new QWidget;
    page->setObjectName(QStringLiteral("teammateHomePage"));
    m_teammateHomePage = page;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    auto *header = new QFrame;
    header->setObjectName(QStringLiteral("teammateHero"));
    auto *headerLayout = new QVBoxLayout(header);
    headerLayout->setContentsMargins(15, 12, 15, 12);
    headerLayout->setSpacing(8);
    m_teammateHomeHeader = header;
    layout->addWidget(header);

    auto *split = new QSplitter(Qt::Horizontal);
    split->setObjectName(QStringLiteral("teammateHomeSplit"));
    split->setChildrenCollapsible(false);

    QVBoxLayout *chatContent = nullptr;
    QFrame *chat = homeSection(QStringLiteral("Conversation"), &chatContent,
                               nullptr, &m_teammateHomeChatTitle);
    chat->setProperty("homePart", QStringLiteral("chat"));
    chat->setMinimumWidth(360);
    m_teammateHomeChatScroll = new QScrollArea;
    m_teammateHomeChatScroll->setObjectName(QStringLiteral("teammateHomeChatScroll"));
    m_teammateHomeChatScroll->setWidgetResizable(true);
    m_teammateHomeChatScroll->setFrameShape(QFrame::NoFrame);
    m_teammateHomeChatScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_teammateHomeChatHost = new QWidget;
    m_teammateHomeChatHost->setObjectName(QStringLiteral("teammateHomeChat"));
    auto *chatLayout = new QVBoxLayout(m_teammateHomeChatHost);
    chatLayout->setContentsMargins(0, 0, 6, 0);
    chatLayout->setSpacing(8);
    m_teammateHomeChatScroll->setWidget(m_teammateHomeChatHost);
    chatContent->addWidget(m_teammateHomeChatScroll, 1);

    auto *composerRow = new QHBoxLayout;
    composerRow->setSpacing(6);
    m_teammateHomeComposer = new QPlainTextEdit;
    m_teammateHomeComposer->setObjectName(QStringLiteral("teammateHomeComposer"));
    m_teammateHomeComposer->setFixedHeight(66);
    m_teammateHomeComposer->setTabChangesFocus(true);
    connect(m_teammateHomeComposer, &QPlainTextEdit::textChanged, this, [this] {
        if (m_teammateHomeId.isEmpty())
            return;
        const QString draft = m_teammateHomeComposer->toPlainText();
        if (draft.trimmed().isEmpty())
            m_teammatePromptDrafts.remove(m_teammateHomeId);
        else
            m_teammatePromptDrafts.insert(m_teammateHomeId, draft);
    });
    m_teammateHomeSend = homeButton(QStringLiteral("Send"),
                                    QStringLiteral("primaryButton"),
                                    QStringLiteral("paper-airplane"));
    m_teammateHomeSend->setProperty("teammateHomeSend", true);
    m_teammateHomeSend->setToolTip(QStringLiteral("Send (Ctrl+Enter)"));
    const auto sendTyped = [this] {
        sendTeammateHomeMessage(m_teammateHomeComposer->toPlainText());
    };
    connect(m_teammateHomeSend, &QPushButton::clicked, this, sendTyped);
    for (const Qt::Key key : {Qt::Key_Return, Qt::Key_Enter}) {
        auto *shortcut =
            new QShortcut(QKeySequence(Qt::CTRL | key), m_teammateHomeComposer);
        shortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(shortcut, &QShortcut::activated, this, sendTyped);
    }
    composerRow->addWidget(m_teammateHomeComposer, 1);
    composerRow->addWidget(m_teammateHomeSend, 0, Qt::AlignBottom);
    chatContent->addLayout(composerRow);
    split->addWidget(chat);

    auto *sideScroll = new QScrollArea;
    sideScroll->setObjectName(QStringLiteral("teammateHomeSideScroll"));
    sideScroll->setWidgetResizable(true);
    sideScroll->setFrameShape(QFrame::NoFrame);
    sideScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sideScroll->setMinimumWidth(320);
    auto *side = new QWidget;
    auto *sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(0, 0, 4, 0);
    sideLayout->setSpacing(10);

    QVBoxLayout *saidContent = nullptr;
    QHBoxLayout *saidHeader = nullptr;
    QFrame *said = homeSection(QStringLiteral("What the spider said"), &saidContent, &saidHeader);
    said->setProperty("homePart", QStringLiteral("genieSaid"));
    m_teammateHomeSaidSection = said;
    m_teammateHomeSaidState = new QLabel;
    m_teammateHomeSaidState->setObjectName(QStringLiteral("statusLine"));
    saidHeader->addWidget(m_teammateHomeSaidState);
    m_teammateHomeSaidCheck = homeButton(QStringLiteral("Ask Genie to check"),
                                         QStringLiteral("ghostButton"),
                                         QStringLiteral("check-circle"));
    m_teammateHomeSaidCheck->setProperty("genieSaidCheck", true);
    m_teammateHomeSaidCheck->setToolTip(
        QStringLiteral("Genie checks every message you commented on since its last "
                       "check and marks it accurate or wrong"));
    connect(m_teammateHomeSaidCheck, &QPushButton::clicked, this, [this] {
        sendTeammateHomeMessage(QStringLiteral(
            "Check the spider's messages I commented on and tell me which "
            "were wrong and why."));
    });
    saidHeader->addWidget(m_teammateHomeSaidCheck);
    m_teammateHomeSaidHost = new QWidget;
    m_teammateHomeSaidHost->setObjectName(QStringLiteral("teammateHomeWidgets"));
    auto *saidLayout = new QVBoxLayout(m_teammateHomeSaidHost);
    saidLayout->setContentsMargins(0, 0, 0, 0);
    saidLayout->setSpacing(8);
    saidContent->addWidget(m_teammateHomeSaidHost);
    said->hide();
    sideLayout->addWidget(said);

    QVBoxLayout *notesContent = nullptr;
    QHBoxLayout *notesHeader = nullptr;
    QFrame *notes = homeSection(QStringLiteral("Notes"), &notesContent, &notesHeader);
    notes->setProperty("homePart", QStringLiteral("notes"));
    m_teammateHomeNotesState = new QLabel;
    m_teammateHomeNotesState->setObjectName(QStringLiteral("statusLine"));
    notesHeader->addWidget(m_teammateHomeNotesState);
    m_teammateHomeNotes = new QPlainTextEdit;
    m_teammateHomeNotes->setObjectName(QStringLiteral("teammateHomeNotes"));
    m_teammateHomeNotes->setMinimumHeight(130);
    m_teammateHomeNotes->setTabChangesFocus(true);
    m_teammateHomeNotes->setPlaceholderText(
        QStringLiteral("Notes for working with this special agent: context, "
                       "preferences, decisions. It reads them on every run."));
    m_teammateHomeNotesTimer = new QTimer(this);
    m_teammateHomeNotesTimer->setSingleShot(true);
    m_teammateHomeNotesTimer->setInterval(700);
    connect(m_teammateHomeNotesTimer, &QTimer::timeout, this,
            [this] { saveTeammateHomeNotes(); });
    connect(m_teammateHomeNotes, &QPlainTextEdit::textChanged, this, [this] {
        if (m_teammateHomeNotesId.isEmpty())
            return;
        m_teammateHomeNotesState->setText(QStringLiteral("Editing…"));
        m_teammateHomeNotesTimer->start();
    });
    notesContent->addWidget(m_teammateHomeNotes);
    sideLayout->addWidget(notes);

    QVBoxLayout *widgetsContent = nullptr;
    QHBoxLayout *widgetsHeader = nullptr;
    QFrame *widgets =
        homeSection(QStringLiteral("Widgets"), &widgetsContent, &widgetsHeader);
    widgets->setProperty("homePart", QStringLiteral("widgets"));
    auto *addWidget = homeButton(QStringLiteral("Add"),
                                 QStringLiteral("ghostButton"),
                                 QStringLiteral("plus"));
    addWidget->setProperty("teammateHomeAddWidget", true);
    addWidget->setToolTip(QStringLiteral("Add a widget to this page"));
    connect(addWidget, &QPushButton::clicked, this, [this, addWidget] {
        QMenu menu(this);
        menu.setObjectName(QStringLiteral("teammateHomeAddWidgetMenu"));
        for (const QString &kind : forkmesh::teammates::widgetKinds()) {
            QAction *action = menu.addAction(
                themedOcticon(forkmesh::teammates::widgetKindIcon(kind),
                              currentThemeIsDark() ? QColor("#c9d1d9")
                                                   : QColor("#24292f"),
                              14),
                forkmesh::teammates::widgetKindLabel(kind));
            connect(action, &QAction::triggered, this,
                    [this, kind] { addTeammateHomeWidget(kind); });
        }
        menu.exec(addWidget->mapToGlobal(QPoint(0, addWidget->height())));
    });
    m_teammateHomeBuildWidget = homeButton(QStringLiteral("Build together"),
                                           QStringLiteral("ghostButton"),
                                           QStringLiteral("sparkle"));
    m_teammateHomeBuildWidget->setProperty("teammateHomeBuildWidget", true);
    connect(m_teammateHomeBuildWidget, &QPushButton::clicked, this,
            [this] { askTeammateToBuildWidget(); });
    widgetsHeader->addWidget(addWidget);
    widgetsHeader->addWidget(m_teammateHomeBuildWidget);
    m_teammateHomeWidgetsHost = new QWidget;
    m_teammateHomeWidgetsHost->setObjectName(QStringLiteral("teammateHomeWidgets"));
    auto *widgetsLayout = new QVBoxLayout(m_teammateHomeWidgetsHost);
    widgetsLayout->setContentsMargins(0, 0, 0, 0);
    widgetsLayout->setSpacing(8);
    widgetsContent->addWidget(m_teammateHomeWidgetsHost);
    sideLayout->addWidget(widgets);

    QVBoxLayout *infoContent = nullptr;
    QFrame *info = homeSection(QStringLiteral("About"), &infoContent);
    info->setProperty("homePart", QStringLiteral("about"));
    m_teammateHomeInfoHost = new QWidget;
    m_teammateHomeInfoHost->setObjectName(QStringLiteral("teammateHomeInfo"));
    auto *infoLayout = new QVBoxLayout(m_teammateHomeInfoHost);
    infoLayout->setContentsMargins(0, 0, 0, 0);
    infoLayout->setSpacing(6);
    infoContent->addWidget(m_teammateHomeInfoHost);
    sideLayout->addWidget(info);
    sideLayout->addStretch(1);
    sideScroll->setWidget(side);
    split->addWidget(sideScroll);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    split->setSizes({760, 470});
    layout->addWidget(split, 1);

    m_teammateHomeRefreshTimer = new QTimer(this);
    m_teammateHomeRefreshTimer->setSingleShot(true);
    m_teammateHomeRefreshTimer->setInterval(250);
    connect(m_teammateHomeRefreshTimer, &QTimer::timeout, this,
            [this] { refreshTeammateHome(false); });
    return page;
}

bool MainWindow::teammateHomeVisible() const
{
    return m_teammatesPageStack &&
           m_teammatesPageStack->currentIndex() == kTeammateHomePageIndex &&
           m_sectionStack && m_sectionStack->currentIndex() == kTeamSectionIndex;
}

void MainWindow::scheduleTeammateHomeRefresh()
{
    if (!m_teammateHomeRefreshTimer || m_teammateHomeId.isEmpty() ||
        !teammateHomeVisible())
        return;
    if (!m_teammateHomeRefreshTimer->isActive())
        m_teammateHomeRefreshTimer->start();
}

void MainWindow::showTeammateHome(const QString &id)
{
    if (!m_teammateHomePage || !m_teammatesPageStack)
        return;
    const LoadResult loaded = Store(teammatesDirPath()).load();
    const bool exists = std::any_of(
        loaded.teammates.cbegin(), loaded.teammates.cend(),
        [&id](const Config &candidate) { return candidate.id == id; });
    if (!exists) {
        flashMessage(QStringLiteral("That special agent no longer exists."), true);
        showTeammateGrid();
        return;
    }
    if (m_teammatesPageStack->currentIndex() != kTeammateHomePageIndex)
        m_teammateHomeFromDashboard = m_teammatesDashboardMode;
    if (m_teammateHomeNotesTimer->isActive())
        saveTeammateHomeNotes();
    m_teammateHomeId = id;
    const Workspace workspace = loadTeammateWorkspace(id);
    if (m_teammateHomeNotes->toPlainText() != workspace.notes) {
        const QSignalBlocker blocker(m_teammateHomeNotes);
        m_teammateHomeNotes->setPlainText(workspace.notes);
    }
    if (m_teammateHomeNotesId != id) {
        m_teammateHomeNotesState->setText(
            workspace.notesUpdatedAtMs > 0
                ? QStringLiteral("Saved %1").arg(
                      forkmesh::teammates::humanLastActive(
                          workspace.notesUpdatedAtMs))
                : QString());
        const QSignalBlocker blocker(m_teammateHomeComposer);
        m_teammateHomeComposer->setPlainText(m_teammatePromptDrafts.value(id));
    }
    m_teammateHomeNotesId = id;
    m_teammatesPageStack->setCurrentIndex(kTeammateHomePageIndex);
    refreshTeammateHome(true);
    scheduleNavRecord();
}

void MainWindow::leaveTeammateHome()
{
    if (m_teammateHomeNotesTimer && m_teammateHomeNotesTimer->isActive())
        saveTeammateHomeNotes();
    if (m_teammateHomeFromDashboard)
        showTeammateDashboard();
    else
        showTeammateGrid();
}

void MainWindow::saveTeammateHomeNotes()
{
    if (m_teammateHomeNotesTimer)
        m_teammateHomeNotesTimer->stop();
    if (!m_teammateHomeNotes || m_teammateHomeNotesId.isEmpty())
        return;
    const QString text =
        m_teammateHomeNotes->toPlainText().left(forkmesh::teammates::kWorkspaceMaxNotes);
    Workspace workspace = loadTeammateWorkspace(m_teammateHomeNotesId);
    if (workspace.notes == text) {
        if (m_teammateHomeNotesState->text() == QLatin1String("Editing…"))
            m_teammateHomeNotesState->setText(QStringLiteral("Saved"));
        return;
    }
    workspace.notes = text;
    workspace.notesUpdatedAtMs = QDateTime::currentMSecsSinceEpoch();
    const bool saved = saveTeammateWorkspace(m_teammateHomeNotesId, workspace);
    m_teammateHomeNotesState->setText(saved ? QStringLiteral("Saved")
                                            : QStringLiteral("Not saved"));
}

QList<const AgentSession *> MainWindow::teammateHomeRuns(const QString &id) const
{
    QList<const AgentSession *> runs;
    for (const AgentSession &session : m_agentSessions) {
        if (session.id <= 0 || session.deletedAtMs > 0)
            continue;
        if (session.teammateId != id && !session.teammateIds.contains(id))
            continue;
        runs.append(&session);
    }
    std::stable_sort(runs.begin(), runs.end(),
                     [](const AgentSession *left, const AgentSession *right) {
                         return homeRunStart(*left) < homeRunStart(*right);
                     });
    if (runs.size() > kHomeRuns)
        runs = runs.mid(runs.size() - kHomeRuns);
    return runs;
}

QString MainWindow::teammateHomeRunOutput(const AgentSession &session)
{
    if (!AgentStatus::isTerminal(session.status)) {
        const QString live = m_lastAssistantText.value(session.id).trimmed();
        if (!live.isEmpty())
            return live;
        if (session.status == AgentStatus::Queued)
            return QStringLiteral("Queued and waiting to start.");
        if (session.status == AgentStatus::Waiting)
            return QStringLiteral("Waiting for your reply before continuing.");
        return QStringLiteral("Working on it…");
    }
    const auto cached = m_teammateHomeOutputCache.constFind(session.id);
    if (cached != m_teammateHomeOutputCache.cend() &&
        cached->first == session.finishedAtMs)
        return cached->second;
    const QString output = teammateLatestOutput(&session);
    m_teammateHomeOutputCache.insert(session.id, {session.finishedAtMs, output});
    return output;
}

void MainWindow::refreshTeammateHome(bool force)
{
    if (m_teammateHomeId.isEmpty() || !m_teammateHomeHeader)
        return;
    if (!force && !teammateHomeVisible())
        return;
    const LoadResult loaded = Store(teammatesDirPath()).load();
    const auto it = std::find_if(
        loaded.teammates.cbegin(), loaded.teammates.cend(),
        [this](const Config &candidate) { return candidate.id == m_teammateHomeId; });
    if (it == loaded.teammates.cend()) {
        const bool visible = teammateHomeVisible();
        m_teammateHomeId.clear();
        m_teammateHomeSignature.clear();
        if (visible)
            showTeammateGrid();
        return;
    }
    const Config teammate = teammateWithRuntime(*it);
    const AgentSession *own = ownTeammateSession(teammate);
    const int ownId = own ? own->id : 0;
    const QString status = teammateRosterStatus(teammate, own);
    const bool inFlight = homeStatusInFlight(status);
    const QList<const AgentSession *> runs = teammateHomeRuns(teammate.id);

    QStringList signature{teammate.id,
                          currentThemeIsDark() ? QStringLiteral("dark")
                                               : QStringLiteral("light"),
                          teammate.name, teammate.avatar, teammate.specialty,
                          teammate.focus, teammate.description, status,
                          teammate.enabled ? QStringLiteral("on")
                                           : QStringLiteral("off"),
                          QString::number(teammate.lastRunAtMs),
                          forkmesh::teammates::humanSchedule(teammate.schedule),
                          QString::number(ownId),
                          QString::number(static_cast<int>(
                              forkmesh::teammates::runWindowState(teammate)))};
    for (const AgentSession *run : runs) {
        signature << QString::number(run->id) << run->status
                  << QString::number(run->finishedAtMs)
                  << QString::number(run->taskGrade);
        if (!AgentStatus::isTerminal(run->status))
            signature << QString::number(m_lastAssistantText.value(run->id).size());
    }
    const QString joined = signature.join(QChar(0x1f));
    if (!force && joined == m_teammateHomeSignature)
        return;
    m_teammateHomeSignature = joined;

    const Workspace workspace = loadTeammateWorkspace(teammate.id);
    fillTeammateHomeHeader(teammate, own, status, inFlight);
    fillTeammateHomeChat(teammate, runs, workspace);
    fillTeammateHomeWidgets(teammate, runs, workspace);
    fillTeammateHomeInfo(teammate, runs);
    fillTeammateHomeSaid();
    if (ownId > 0 && !inFlight && teammateHomeVisible() &&
        m_teammateDoneUnseen.contains(ownId))
        clearTeammateDoneUnseen(ownId);
}

void MainWindow::fillTeammateHomeHeader(const Config &teammate,
                                        const AgentSession *session,
                                        const QString &status, bool inFlight)
{
    auto *layout = qobject_cast<QVBoxLayout *>(m_teammateHomeHeader->layout());
    clearHomeLayout(layout);
    m_teammateHomeHeader->setProperty("kind", homeStatusKind(status));
    repolish(m_teammateHomeHeader);
    const QString id = teammate.id;
    const int sessionId = session ? session->id : 0;
    const QString handle = forkmesh::teammates::mentionHandle(teammate);

    auto *topRow = new QHBoxLayout;
    topRow->setSpacing(6);
    auto *back = homeButton(QStringLiteral("Special agents"),
                            QStringLiteral("ghostButton"),
                            QStringLiteral("arrow-left"));
    back->setProperty("teammateHomeBack", true);
    back->setToolTip(QStringLiteral("Back to all special agents"));
    connect(back, &QPushButton::clicked, this, [this] { leaveTeammateHome(); });
    topRow->addWidget(back);
    topRow->addStretch(1);
    if (inFlight) {
        auto *stop = homeButton(QStringLiteral("Stop"),
                                QStringLiteral("teammateStopButton"),
                                QStringLiteral("stop"));
        stop->setProperty("teammateHomeStop", true);
        connect(stop, &QPushButton::clicked, this, [this, id] {
            stopTeammate(id);
            refreshTeammateHome(true);
        });
        topRow->addWidget(stop);
    } else {
        auto *run = homeButton(QStringLiteral("Run now"),
                               QStringLiteral("primaryButton"),
                               QStringLiteral("play"));
        run->setProperty("teammateHomeRun", true);
        run->setToolTip(QStringLiteral("Run %1 on demand").arg(teammate.name));
        connect(run, &QPushButton::clicked, this, [this, id] {
            runTeammateFromRail(id);
            refreshTeammateHome(true);
        });
        topRow->addWidget(run);
    }
    if (sessionId > 0) {
        auto *open = homeButton(QStringLiteral("Session #%1").arg(sessionId),
                                QStringLiteral("teammateAgentLink"),
                                QStringLiteral("terminal"));
        open->setProperty("teammateHomeOpenSession", true);
        connect(open, &QPushButton::clicked, this,
                [this, sessionId] { switchToAgentsTab(sessionId); });
        topRow->addWidget(open);
    }
    auto *briefing = homeButton(QStringLiteral("Briefing"),
                                QStringLiteral("ghostButton"),
                                QStringLiteral("history"));
    connect(briefing, &QPushButton::clicked, this,
            [this, id] { showTeammateBriefing(id); });
    topRow->addWidget(briefing);
    auto *profile = homeButton(QStringLiteral("Profile && settings"),
                               QStringLiteral("ghostButton"),
                               QStringLiteral("gear"));
    profile->setProperty("teammateHomeProfile", true);
    connect(profile, &QPushButton::clicked, this,
            [this, id] { showTeammateDetail(id); });
    topRow->addWidget(profile);
    auto *more = homeButton(QString::fromUtf8("\xE2\x8B\xAF"),
                            QStringLiteral("ghostButton"), QString());
    more->setAccessibleName(QStringLiteral("More for %1").arg(teammate.name));
    connect(more, &QPushButton::clicked, this,
            [this, more, id] { showTeammateRailMenu(id, more); });
    topRow->addWidget(more);
    layout->addLayout(topRow);

    auto *identityRow = new QHBoxLayout;
    identityRow->setSpacing(14);
    auto *avatarHost = new QWidget;
    avatarHost->setObjectName(QStringLiteral("teammateHomeAvatarHost"));
    avatarHost->setFixedSize(kHomeAvatar + 6, kHomeAvatar + 6);
    auto *avatar = new QLabel(avatarHost);
    avatar->setObjectName(QStringLiteral("teammateHomeAvatar"));
    avatar->setPixmap(teammateAvatar(teammate, kHomeAvatar));
    avatar->setFixedSize(kHomeAvatar, kHomeAvatar);
    avatar->move(0, 0);
    QPushButton *play =
        makeTeammatePlayButton(teammate, inFlight, sessionId, kHomePlay, avatarHost);
    play->setObjectName(QStringLiteral("teammateHomePlay"));
    play->move(avatarHost->width() - kHomePlay, avatarHost->height() - kHomePlay);
    identityRow->addWidget(avatarHost, 0, Qt::AlignTop);

    auto *identity = new QVBoxLayout;
    identity->setSpacing(2);
    auto *name = new QLabel(teammate.name);
    name->setObjectName(QStringLiteral("teammateHeroName"));
    auto *mention = new QLabel(QStringLiteral("@%1").arg(handle));
    mention->setObjectName(QStringLiteral("teammateHeroHandle"));
    auto *role = new QLabel(
        teammate.specialty +
        (teammate.focus.isEmpty() ? QString()
                                  : QStringLiteral(" · ") + teammate.focus));
    role->setObjectName(QStringLiteral("teammateHeroRole"));
    role->setWordWrap(true);
    auto *badges = new QHBoxLayout;
    badges->setSpacing(5);
    badges->addWidget(homePill(status, homeStatusKind(status)));
    badges->addWidget(homePill(
        teammate.schedule.active
            ? forkmesh::teammates::humanSchedule(teammate.schedule)
            : QStringLiteral("On demand"),
        QStringLiteral("cadence")));
    badges->addStretch(1);
    QString detail;
    if (inFlight)
        detail = sessionId > 0 ? QStringLiteral("Working now · session #%1").arg(sessionId)
                               : QStringLiteral("Working now");
    else if (!teammate.enabled)
        detail = QStringLiteral("Paused until you make it available again");
    else if (status == QLatin1String("Scheduled"))
        detail = forkmesh::teammates::humanNextInvocation(teammate);
    if (detail.isEmpty())
        detail = QStringLiteral("Last run %1 · press ▶ to run it now")
                     .arg(forkmesh::teammates::humanLastActive(teammate.lastRunAtMs));
    auto *detailLabel = new QLabel(detail);
    detailLabel->setObjectName(QStringLiteral("teammateStatusDetail"));
    detailLabel->setWordWrap(true);
    identity->addWidget(name);
    identity->addWidget(mention);
    identity->addWidget(role);
    identity->addLayout(badges);
    identity->addWidget(detailLabel);
    identityRow->addLayout(identity, 1);
    layout->addLayout(identityRow);

    if (m_teammateHomeChatTitle)
        m_teammateHomeChatTitle->setText(
            QStringLiteral("Talk with %1").arg(teammate.name));
    if (m_teammateHomeBuildWidget)
        m_teammateHomeBuildWidget->setToolTip(
            QStringLiteral("Ask %1 to build a widget with you from its work")
                .arg(teammate.name));
    const bool takesPrompts = teammate.workflow.isEmpty();
    m_teammateHomeComposer->setEnabled(takesPrompts);
    m_teammateHomeSend->setEnabled(takesPrompts);
    m_teammateHomeBuildWidget->setEnabled(takesPrompts);
    m_teammateHomeComposer->setPlaceholderText(
        !takesPrompts
            ? QStringLiteral("%1 runs a built-in workflow and takes no messages. "
                             "Press ▶ to run it.")
                  .arg(teammate.name)
        : inFlight
            ? QStringLiteral("Steer @%1 while it works…").arg(handle)
            : QStringLiteral("Ask @%1 something or give it a task…").arg(handle));
}

void MainWindow::fillTeammateHomeChat(const Config &teammate,
                                      const QList<const AgentSession *> &runs,
                                      const Workspace &workspace)
{
    auto *layout = qobject_cast<QVBoxLayout *>(m_teammateHomeChatHost->layout());
    clearHomeLayout(layout);

    struct Entry {
        qint64 at = 0;
        const ChatEntry *message = nullptr;
        const AgentSession *run = nullptr;
    };
    QList<Entry> entries;
    const int firstMessage = qMax(0, int(workspace.messages.size()) - kHomeMessages);
    for (int index = firstMessage; index < workspace.messages.size(); ++index)
        entries.append({workspace.messages.at(index).atMs,
                        &workspace.messages.at(index), nullptr});
    for (const AgentSession *run : runs)
        entries.append({homeRunKey(*run), nullptr, run});
    std::stable_sort(entries.begin(), entries.end(),
                     [](const Entry &left, const Entry &right) {
                         return left.at < right.at;
                     });

    layout->addStretch(1);
    if (entries.isEmpty()) {
        auto *empty = new QLabel(
            QStringLiteral("No conversation yet. Say hello or give %1 a task "
                           "below: it starts a run and the reply lands here.")
                .arg(teammate.name));
        empty->setObjectName(QStringLiteral("teammateHomeChatEmpty"));
        empty->setWordWrap(true);
        empty->setAlignment(Qt::AlignCenter);
        layout->addWidget(empty);
    }
    const QPixmap face = teammateAvatar(teammate, 22);
    for (const Entry &entry : std::as_const(entries)) {
        if (entry.message) {
            auto *row = new QHBoxLayout;
            row->addStretch(1);
            auto *bubble = new QFrame;
            bubble->setObjectName(QStringLiteral("teammateHomeYou"));
            bubble->setMaximumWidth(520);
            auto *bubbleLayout = new QVBoxLayout(bubble);
            bubbleLayout->setContentsMargins(10, 7, 10, 7);
            bubbleLayout->setSpacing(2);
            auto *text = new QLabel(entry.message->text);
            text->setObjectName(QStringLiteral("teammateHomeYouText"));
            text->setWordWrap(true);
            text->setTextFormat(Qt::PlainText);
            text->setTextInteractionFlags(Qt::TextSelectableByMouse);
            auto *when = new QLabel(QStringLiteral("You · %1").arg(
                forkmesh::teammates::humanLastActive(entry.message->atMs)));
            when->setObjectName(QStringLiteral("teammateHomeWhen"));
            bubbleLayout->addWidget(text);
            bubbleLayout->addWidget(when, 0, Qt::AlignRight);
            row->addWidget(bubble);
            layout->addLayout(row);
            continue;
        }
        const AgentSession &run = *entry.run;
        auto *reply = new QFrame;
        reply->setObjectName(QStringLiteral("teammateHomeReply"));
        reply->setProperty("sessionId", run.id);
        reply->setProperty("status", run.status);
        auto *replyLayout = new QVBoxLayout(reply);
        replyLayout->setContentsMargins(10, 8, 10, 8);
        replyLayout->setSpacing(5);
        auto *head = new QHBoxLayout;
        head->setSpacing(6);
        auto *avatar = new QLabel;
        avatar->setPixmap(face);
        avatar->setFixedSize(22, 22);
        head->addWidget(avatar);
        const QString title = run.issueTitle.trimmed().isEmpty()
                                  ? QStringLiteral("Run #%1").arg(run.id)
                                  : run.issueTitle.trimmed();
        auto *who = new QLabel(QStringLiteral("<b>%1</b> · %2")
                                   .arg(teammate.name.toHtmlEscaped(),
                                        title.toHtmlEscaped()));
        who->setObjectName(QStringLiteral("teammateHomeReplyTitle"));
        who->setTextFormat(Qt::RichText);
        who->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        head->addWidget(who, 1);
        const QString statusText =
            run.status.isEmpty() ? QString()
                                 : run.status.left(1).toUpper() + run.status.mid(1);
        head->addWidget(homePill(
            statusText, run.status == AgentStatus::Running ? QStringLiteral("working")
                        : run.status == AgentStatus::Failed ? QStringLiteral("waiting")
                                                            : QStringLiteral("idle")));
        auto *when = new QLabel(forkmesh::teammates::humanLastActive(entry.at));
        when->setObjectName(QStringLiteral("teammateHomeWhen"));
        head->addWidget(when);
        replyLayout->addLayout(head);
        auto *body = new QLabel(homeDisplayOutput(teammateHomeRunOutput(run)));
        body->setObjectName(QStringLiteral("teammateHomeReplyText"));
        body->setTextFormat(Qt::MarkdownText);
        body->setWordWrap(true);
        body->setTextInteractionFlags(Qt::TextSelectableByMouse);
        replyLayout->addWidget(body);
        auto *open = homeButton(QStringLiteral("Open session #%1").arg(run.id),
                                QStringLiteral("teammateAgentLink"),
                                QStringLiteral("terminal"));
        open->setProperty("teammateHomeReplyOpen", true);
        connect(open, &QPushButton::clicked, this,
                [this, sessionId = run.id] { switchToAgentsTab(sessionId); });
        replyLayout->addWidget(open, 0, Qt::AlignLeft);
        layout->addWidget(reply);
    }
    QPointer<QScrollArea> scroll = m_teammateHomeChatScroll;
    QTimer::singleShot(0, this, [scroll] {
        if (scroll)
            scroll->verticalScrollBar()->setValue(
                scroll->verticalScrollBar()->maximum());
    });
}

void MainWindow::fillTeammateHomeWidgets(const Config &teammate,
                                         const QList<const AgentSession *> &runs,
                                         const Workspace &workspace)
{
    auto *layout = qobject_cast<QVBoxLayout *>(m_teammateHomeWidgetsHost->layout());
    clearHomeLayout(layout);
    const QString id = teammate.id;

    QList<WidgetProposal> proposals;
    QStringList seen;
    for (auto run = runs.crbegin(); run != runs.crend(); ++run) {
        if (!AgentStatus::isTerminal((*run)->status))
            continue;
        for (const WidgetProposal &proposal :
             forkmesh::teammates::pendingProposals(
                 workspace, teammateHomeRunOutput(**run), (*run)->id)) {
            if (seen.contains(proposal.fingerprint))
                continue;
            seen.append(proposal.fingerprint);
            proposals.append(proposal);
        }
    }
    if (!proposals.isEmpty()) {
        auto *eyebrow = new QLabel(
            QStringLiteral("PROPOSED BY %1").arg(teammate.name.toUpper()));
        eyebrow->setObjectName(QStringLiteral("teammateEyebrow"));
        layout->addWidget(eyebrow);
    }
    for (const WidgetProposal &proposal : std::as_const(proposals)) {
        auto *card = new QFrame;
        card->setObjectName(QStringLiteral("teammateHomeProposal"));
        card->setProperty("fingerprint", proposal.fingerprint);
        auto *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(10, 8, 10, 8);
        cardLayout->setSpacing(4);
        auto *title = new QLabel(
            QStringLiteral("%1 · %2%3")
                .arg(forkmesh::teammates::widgetKindLabel(proposal.widget.kind),
                     proposal.widget.title,
                     proposal.updatesExisting ? QStringLiteral(" (update)")
                                              : QString()));
        title->setObjectName(QStringLiteral("teammateWorkTitle"));
        title->setWordWrap(true);
        auto *summary = new QLabel(widgetSummary(proposal.widget));
        summary->setObjectName(QStringLiteral("statusLine"));
        summary->setWordWrap(true);
        auto *actions = new QHBoxLayout;
        auto *accept = homeButton(proposal.updatesExisting
                                      ? QStringLiteral("Apply update")
                                      : QStringLiteral("Add widget"),
                                  QStringLiteral("primaryButton"),
                                  QStringLiteral("plus"));
        accept->setProperty("teammateHomeProposalAccept", true);
        connect(accept, &QPushButton::clicked, this, [this, id, proposal] {
            updateTeammateWorkspace(id, [&proposal, &id](Workspace &target) {
                Widget widget = proposal.widget;
                widget.createdBy = proposal.updatesExisting ? QString() : id;
                if (!forkmesh::teammates::upsertWidget(&target, widget))
                    return;
                forkmesh::teammates::markProposalHandled(&target,
                                                         proposal.fingerprint);
            });
        });
        auto *dismiss = homeButton(QStringLiteral("Dismiss"),
                                   QStringLiteral("ghostButton"), QString());
        dismiss->setProperty("teammateHomeProposalDismiss", true);
        connect(dismiss, &QPushButton::clicked, this,
                [this, id, fingerprint = proposal.fingerprint] {
                    updateTeammateWorkspace(id, [&fingerprint](Workspace &target) {
                        forkmesh::teammates::markProposalHandled(&target,
                                                                 fingerprint);
                    });
                });
        actions->addWidget(accept);
        actions->addWidget(dismiss);
        actions->addStretch(1);
        cardLayout->addWidget(title);
        cardLayout->addWidget(summary);
        cardLayout->addLayout(actions);
        layout->addWidget(card);
    }

    if (workspace.widgets.isEmpty() && proposals.isEmpty()) {
        auto *empty = new QLabel(
            QStringLiteral("No widgets yet. Add a checklist, goal tracker, "
                           "pinned note, quick request or links, or build one "
                           "together with %1. It sees them on every run.")
                .arg(teammate.name));
        empty->setObjectName(QStringLiteral("teammateHomeWidgetsEmpty"));
        empty->setWordWrap(true);
        layout->addWidget(empty);
    }
    for (const Widget &widget : workspace.widgets)
        layout->addWidget(buildTeammateHomeWidgetCard(teammate, widget));
}

QWidget *MainWindow::buildTeammateHomeWidgetCard(const Config &teammate,
                                                 const Widget &widget)
{
    using namespace forkmesh::teammates;
    const QString id = teammate.id;
    const QString widgetId = widget.id;
    const bool dark = currentThemeIsDark();
    auto *card = new QFrame;
    card->setObjectName(QStringLiteral("teammateHomeWidget"));
    card->setProperty("kind", widget.kind);
    card->setProperty("widgetId", widget.id);
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(6);

    auto *head = new QHBoxLayout;
    head->setSpacing(6);
    auto *icon = new QLabel;
    icon->setPixmap(themedOcticon(widgetKindIcon(widget.kind),
                                  dark ? QColor("#a371f7") : QColor("#8250df"), 14)
                        .pixmap(14, 14));
    head->addWidget(icon);
    auto *title = new QLabel(widget.title);
    title->setObjectName(QStringLiteral("teammateWorkTitle"));
    title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    head->addWidget(title, 1);
    if (!widget.createdBy.isEmpty() && widget.createdBy != kWorkspaceFromYou) {
        auto *origin = new QLabel(QStringLiteral("by %1").arg(teammate.name));
        origin->setObjectName(QStringLiteral("teammateHomeWhen"));
        head->addWidget(origin);
    }
    auto *menuButton = homeButton(QString::fromUtf8("\xE2\x8B\xAF"),
                                  QStringLiteral("ghostButton"), QString());
    menuButton->setProperty("teammateHomeWidgetMenu", true);
    menuButton->setAccessibleName(QStringLiteral("Widget options"));
    connect(menuButton, &QPushButton::clicked, this,
            [this, menuButton, id, widgetId, title = widget.title,
             name = teammate.name, takesPrompts = teammate.workflow.isEmpty()] {
                QMenu menu(this);
                connect(menu.addAction(QStringLiteral("Edit…")), &QAction::triggered,
                        this, [this, widgetId] { editTeammateHomeWidget(widgetId); });
                connect(menu.addAction(QStringLiteral("Rename…")),
                        &QAction::triggered, this, [this, id, widgetId, title] {
                            bool ok = false;
                            const QString renamed = QInputDialog::getText(
                                this, QStringLiteral("Rename widget"),
                                QStringLiteral("Title"), QLineEdit::Normal, title,
                                &ok);
                            if (!ok || renamed.trimmed().isEmpty())
                                return;
                            updateTeammateWorkspace(
                                id, [&widgetId, &renamed](Workspace &target) {
                                    for (Widget &existing : target.widgets)
                                        if (existing.id == widgetId)
                                            existing.title = renamed.trimmed();
                                });
                        });
                QAction *ask = menu.addAction(
                    QStringLiteral("Ask %1 to update it").arg(name));
                ask->setEnabled(takesPrompts);
                connect(ask, &QAction::triggered, this,
                        [this, widgetId] { askTeammateToBuildWidget(widgetId); });
                menu.addSeparator();
                connect(menu.addAction(QStringLiteral("Remove")), &QAction::triggered,
                        this, [this, id, widgetId, title] {
                            if (QMessageBox::question(
                                    this, QStringLiteral("Remove widget"),
                                    QStringLiteral("Remove the \"%1\" widget?")
                                        .arg(title)) != QMessageBox::Yes)
                                return;
                            updateTeammateWorkspace(id, [&widgetId](Workspace &target) {
                                removeWidget(&target, widgetId);
                            });
                        });
                menu.exec(menuButton->mapToGlobal(QPoint(0, menuButton->height())));
            });
    head->addWidget(menuButton);
    layout->addLayout(head);

    const auto progressBar = [](double fraction, int height) {
        auto *bar = new QProgressBar;
        bar->setObjectName(QStringLiteral("teammateBoxProgress"));
        bar->setRange(0, 1000);
        bar->setValue(int(qBound(0.0, fraction, 1.0) * 1000));
        bar->setTextVisible(false);
        bar->setFixedHeight(height);
        return bar;
    };

    if (widget.kind == kWidgetChecklist) {
        for (int index = 0; index < widget.items.size(); ++index) {
            auto *box = new QCheckBox(widget.items.at(index).text);
            box->setObjectName(QStringLiteral("teammateHomeChecklistItem"));
            box->setChecked(widget.items.at(index).done);
            connect(box, &QCheckBox::toggled, this,
                    [this, id, widgetId, index](bool checked) {
                        updateTeammateWorkspace(
                            id, [&widgetId, index, checked](Workspace &target) {
                                for (Widget &existing : target.widgets)
                                    if (existing.id == widgetId &&
                                        index < existing.items.size())
                                        existing.items[index].done = checked;
                            });
                    });
            layout->addWidget(box);
        }
        if (!widget.items.isEmpty()) {
            layout->addWidget(progressBar(widgetProgress(widget), 4));
            auto *count = new QLabel(
                QStringLiteral("%1 of %2 done")
                    .arg(widget.items.size() - openChecklistItems(widget))
                    .arg(widget.items.size()));
            count->setObjectName(QStringLiteral("teammateHomeWhen"));
            layout->addWidget(count);
        }
        auto *addItem = homeButton(QStringLiteral("Add item"),
                                   QStringLiteral("ghostButton"),
                                   QStringLiteral("plus"));
        connect(addItem, &QPushButton::clicked, this, [this, id, widgetId] {
            bool ok = false;
            const QString text = QInputDialog::getText(
                this, QStringLiteral("Add item"), QStringLiteral("Item"),
                QLineEdit::Normal, QString(), &ok);
            if (!ok || text.trimmed().isEmpty())
                return;
            updateTeammateWorkspace(id, [&widgetId, &text](Workspace &target) {
                for (Widget &existing : target.widgets)
                    if (existing.id == widgetId &&
                        existing.items.size() < kWorkspaceMaxItems)
                        existing.items.append({text.trimmed(), {}, false});
            });
        });
        layout->addWidget(addItem, 0, Qt::AlignLeft);
    } else if (widget.kind == kWidgetGoal) {
        layout->addWidget(progressBar(widgetProgress(widget), 8));
        auto *row = new QHBoxLayout;
        auto *value = new QLabel(widgetSummary(widget));
        value->setObjectName(QStringLiteral("teammateHomeGoalValue"));
        row->addWidget(value, 1);
        const auto step = [this, id, widgetId](double delta) {
            updateTeammateWorkspace(id, [&widgetId, delta](Workspace &target) {
                for (Widget &existing : target.widgets)
                    if (existing.id == widgetId)
                        existing.value = qMax(0.0, existing.value + delta);
            });
        };
        auto *minus = homeButton(QString::fromUtf8("\xE2\x88\x92"),
                                 QStringLiteral("ghostButton"), QString());
        minus->setProperty("teammateHomeGoalStep", -1);
        connect(minus, &QPushButton::clicked, this, [step] { step(-1.0); });
        auto *plus = homeButton(QStringLiteral("+"), QStringLiteral("ghostButton"),
                                QString());
        plus->setProperty("teammateHomeGoalStep", 1);
        connect(plus, &QPushButton::clicked, this, [step] { step(1.0); });
        auto *set = homeButton(QStringLiteral("Set…"), QStringLiteral("ghostButton"),
                               QString());
        connect(set, &QPushButton::clicked, this,
                [this, id, widgetId, current = widget.value] {
                    bool ok = false;
                    const double next = QInputDialog::getDouble(
                        this, QStringLiteral("Set progress"),
                        QStringLiteral("Current value"), current, 0.0, 1e12, 2,
                        &ok);
                    if (!ok)
                        return;
                    updateTeammateWorkspace(id, [&widgetId, next](Workspace &target) {
                        for (Widget &existing : target.widgets)
                            if (existing.id == widgetId)
                                existing.value = next;
                    });
                });
        row->addWidget(minus);
        row->addWidget(plus);
        row->addWidget(set);
        layout->addLayout(row);
    } else if (widget.kind == kWidgetNote) {
        auto *text = new QLabel(widget.text.isEmpty()
                                    ? QStringLiteral("Empty note · choose Edit to write it.")
                                    : widget.text);
        text->setObjectName(QStringLiteral("teammateHomeWidgetText"));
        text->setTextFormat(widget.text.isEmpty() ? Qt::PlainText : Qt::MarkdownText);
        text->setWordWrap(true);
        text->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(text);
    } else if (widget.kind == kWidgetPrompt) {
        const QString request = widget.text.isEmpty() ? widget.title : widget.text;
        auto *text = new QLabel(homeCompact(request, 280));
        text->setObjectName(QStringLiteral("teammateHomeWidgetText"));
        text->setWordWrap(true);
        layout->addWidget(text);
        auto *send = homeButton(QStringLiteral("Send to %1").arg(teammate.name),
                                QStringLiteral("ghostButton"),
                                QStringLiteral("paper-airplane"));
        send->setProperty("teammateHomeWidgetSend", true);
        send->setEnabled(teammate.workflow.isEmpty());
        connect(send, &QPushButton::clicked, this,
                [this, request] { sendTeammateHomeMessage(request); });
        layout->addWidget(send, 0, Qt::AlignLeft);
    } else if (widget.kind == kWidgetLinks) {
        for (const WidgetItem &item : widget.items) {
            auto *link = new QLabel(QStringLiteral("<a href=\"%1\">%2</a>")
                                        .arg(item.url.toHtmlEscaped(),
                                             item.text.toHtmlEscaped()));
            link->setObjectName(QStringLiteral("teammateHomeLink"));
            link->setTextFormat(Qt::RichText);
            link->setOpenExternalLinks(true);
            link->setToolTip(item.url);
            layout->addWidget(link);
        }
        auto *addLink = homeButton(QStringLiteral("Add link"),
                                   QStringLiteral("ghostButton"),
                                   QStringLiteral("plus"));
        connect(addLink, &QPushButton::clicked, this, [this, id, widgetId] {
            bool ok = false;
            const QString url = QInputDialog::getText(
                this, QStringLiteral("Add link"), QStringLiteral("Web address"),
                QLineEdit::Normal, QStringLiteral("https://"), &ok);
            if (!ok)
                return;
            const QString safe = safeUrl(url);
            if (safe.isEmpty()) {
                flashMessage(QStringLiteral("Links must be http or https addresses."),
                             true);
                return;
            }
            const QString label = QInputDialog::getText(
                this, QStringLiteral("Add link"), QStringLiteral("Label"),
                QLineEdit::Normal, QString(), &ok);
            updateTeammateWorkspace(id, [&widgetId, &safe, &label](Workspace &target) {
                for (Widget &existing : target.widgets)
                    if (existing.id == widgetId &&
                        existing.items.size() < kWorkspaceMaxItems)
                        existing.items.append({label.trimmed(), safe, false});
            });
        });
        layout->addWidget(addLink, 0, Qt::AlignLeft);
    }
    return card;
}

void MainWindow::addTeammateHomeWidget(const QString &kind)
{
    const QString id = m_teammateHomeId;
    if (id.isEmpty())
        return;
    bool ok = false;
    const QString title = QInputDialog::getText(
        this, QStringLiteral("New %1").arg(forkmesh::teammates::widgetKindLabel(kind)),
        QStringLiteral("Title"), QLineEdit::Normal,
        forkmesh::teammates::widgetKindLabel(kind), &ok);
    if (!ok)
        return;
    const Widget widget = forkmesh::teammates::newWidget(kind, title);
    bool added = false;
    updateTeammateWorkspace(id, [&widget, &added](Workspace &target) {
        added = forkmesh::teammates::upsertWidget(&target, widget);
    });
    if (!added) {
        flashMessage(QStringLiteral("This page already holds the most widgets it can."),
                     true);
        return;
    }
    if (kind != forkmesh::teammates::kWidgetGoal)
        editTeammateHomeWidget(widget.id);
}

void MainWindow::editTeammateHomeWidget(const QString &widgetId)
{
    using namespace forkmesh::teammates;
    const QString id = m_teammateHomeId;
    if (id.isEmpty())
        return;
    const Workspace workspace = loadTeammateWorkspace(id);
    const auto it = std::find_if(workspace.widgets.cbegin(), workspace.widgets.cend(),
                                 [&widgetId](const Widget &widget) {
                                     return widget.id == widgetId;
                                 });
    if (it == workspace.widgets.cend())
        return;
    Widget edited = *it;
    bool ok = false;
    if (edited.kind == kWidgetChecklist) {
        const QString text = QInputDialog::getMultiLineText(
            this, edited.title,
            QStringLiteral("One item per line. Start a line with [x] when it is done."),
            checklistText(edited), &ok);
        if (!ok)
            return;
        edited.items = checklistFromText(text);
    } else if (edited.kind == kWidgetLinks) {
        const QString text = QInputDialog::getMultiLineText(
            this, edited.title,
            QStringLiteral("One link per line: Label | https://address"),
            linksText(edited), &ok);
        if (!ok)
            return;
        edited.items = linksFromText(text);
    } else if (edited.kind == kWidgetGoal) {
        const double target = QInputDialog::getDouble(
            this, edited.title, QStringLiteral("Target"), edited.target, 0.01, 1e12,
            2, &ok);
        if (!ok)
            return;
        edited.target = target;
        const QString unit = QInputDialog::getText(
            this, edited.title, QStringLiteral("Unit (optional)"), QLineEdit::Normal,
            edited.unit, &ok);
        if (ok)
            edited.unit = unit;
    } else {
        const QString text = QInputDialog::getMultiLineText(
            this, edited.title,
            edited.kind == kWidgetPrompt
                ? QStringLiteral("The request this button sends")
                : QStringLiteral("Note"),
            edited.text, &ok);
        if (!ok)
            return;
        edited.text = text;
    }
    updateTeammateWorkspace(id, [&edited](Workspace &target) {
        upsertWidget(&target, edited);
    });
}

void MainWindow::askTeammateToBuildWidget(const QString &widgetId)
{
    const QString id = m_teammateHomeId;
    if (id.isEmpty())
        return;
    QString shown;
    QString assignment;
    if (widgetId.isEmpty()) {
        bool ok = false;
        const QString request = QInputDialog::getMultiLineText(
            this, QStringLiteral("Build a widget together"),
            QStringLiteral("What should the widget help with? Leave it empty "
                           "to let the special agent suggest one."),
            QString(), &ok);
        if (!ok)
            return;
        shown = request.trimmed().isEmpty()
                    ? QStringLiteral("Suggest a widget for this page.")
                    : QStringLiteral("Build a widget: %1").arg(request.trimmed());
        assignment = forkmesh::teammates::widgetBuildAssignment(request);
    } else {
        const Workspace workspace = loadTeammateWorkspace(id);
        const auto it = std::find_if(
            workspace.widgets.cbegin(), workspace.widgets.cend(),
            [&widgetId](const Widget &widget) { return widget.id == widgetId; });
        if (it == workspace.widgets.cend())
            return;
        shown = QStringLiteral("Update the \"%1\" widget.").arg(it->title);
        assignment = forkmesh::teammates::widgetBuildAssignment(
            QStringLiteral("Update the existing widget below with what you know "
                           "now, keeping its id.\n%1")
                .arg(QString::fromUtf8(
                    QJsonDocument(forkmesh::teammates::widgetToJson(*it))
                        .toJson(QJsonDocument::Compact))));
    }
    sendTeammateHomeMessage(shown, assignment);
}

void MainWindow::sendTeammateHomeMessage(const QString &text,
                                         const QString &assignment)
{
    const QString shown = text.trimmed();
    const QString id = m_teammateHomeId;
    if (id.isEmpty())
        return;
    if (shown.isEmpty()) {
        flashMessage(QStringLiteral("Type a message first."), true);
        return;
    }
    const LoadResult loaded = Store(teammatesDirPath()).load();
    const auto it = std::find_if(
        loaded.teammates.cbegin(), loaded.teammates.cend(),
        [&id](const Config &candidate) { return candidate.id == id; });
    if (it == loaded.teammates.cend()) {
        flashMessage(QStringLiteral("That special agent no longer exists."), true);
        return;
    }
    const Config teammate = teammateWithRuntime(*it);
    if (!teammate.workflow.isEmpty()) {
        flashMessage(QStringLiteral("%1 runs a built-in workflow and takes no "
                                    "messages. Press ▶ to run it.")
                         .arg(teammate.name),
                     true);
        return;
    }
    if (!teammate.enabled) {
        flashMessage(QStringLiteral("%1 is paused. Choose Make available from "
                                    "the ⋯ menu first.")
                         .arg(teammate.name),
                     true);
        return;
    }
    const bool steering = homeStatusInFlight(
        teammateRosterStatus(teammate, ownTeammateSession(teammate)));
    QString prompt = assignment.trimmed().isEmpty() ? shown : assignment.trimmed();
    if (!steering) {
        const Workspace workspace = loadTeammateWorkspace(id);
        QStringList earlier;
        for (int index = qMax(0, int(workspace.messages.size()) - 4);
             index < workspace.messages.size(); ++index) {
            const ChatEntry &entry = workspace.messages.at(index);
            earlier.append(QStringLiteral("- You (%1): %2")
                               .arg(forkmesh::teammates::humanLastActive(entry.atMs),
                                    homeCompact(entry.text, 400)));
        }
        QString latest;
        const QList<const AgentSession *> runs = teammateHomeRuns(id);
        for (auto run = runs.crbegin(); run != runs.crend(); ++run) {
            if (AgentStatus::isTerminal((*run)->status)) {
                latest = homeCompact(homeDisplayOutput(teammateHomeRunOutput(**run)),
                                     1500);
                break;
            }
        }
        if (!earlier.isEmpty() || !latest.isEmpty()) {
            prompt += QStringLiteral("\n\nEarlier on your page (context, oldest first)");
            if (!earlier.isEmpty())
                prompt += QLatin1Char('\n') + earlier.join(QLatin1Char('\n'));
            if (!latest.isEmpty())
                prompt += QStringLiteral("\nYour last reply: %1").arg(latest);
        }
    }
    const bool fromComposer =
        m_teammateHomeComposer &&
        m_teammateHomeComposer->toPlainText().trimmed() == shown;
    const QString draft = m_teammatePromptDrafts.value(id);
    sendTeammatePrompt(id, prompt);
    if (m_teammatePromptDrafts.value(id) == prompt.trimmed()) {
        if (draft.isEmpty())
            m_teammatePromptDrafts.remove(id);
        else
            m_teammatePromptDrafts.insert(id, draft);
        refreshTeammateHome(true);
        return;
    }
    const LoadResult after = Store(teammatesDirPath()).load();
    int sessionId = 0;
    for (const Config &candidate : after.teammates) {
        if (candidate.id != id)
            continue;
        if (const AgentSession *session =
                ownTeammateSession(teammateWithRuntime(candidate)))
            sessionId = session->id;
    }
    updateTeammateWorkspace(
        id,
        [&shown, sessionId](Workspace &target) {
            forkmesh::teammates::appendMessage(
                &target, {forkmesh::teammates::kWorkspaceFromYou, shown, 0, sessionId});
        },
        false);
    if (fromComposer) {
        const QSignalBlocker blocker(m_teammateHomeComposer);
        m_teammateHomeComposer->clear();
        m_teammatePromptDrafts.remove(id);
    } else if (!draft.isEmpty()) {
        m_teammatePromptDrafts.insert(id, draft);
    }
    refreshTeammateHome(true);
}

void MainWindow::fillTeammateHomeInfo(const Config &teammate,
                                      const QList<const AgentSession *> &runs)
{
    auto *layout = qobject_cast<QVBoxLayout *>(m_teammateHomeInfoHost->layout());
    clearHomeLayout(layout);
    if (!teammate.description.isEmpty()) {
        auto *description = new QLabel(teammate.description);
        description->setObjectName(QStringLiteral("teammateDescription"));
        description->setWordWrap(true);
        layout->addWidget(description);
    }
    const AgentChoice primary = forkmesh::teammates::agentChoiceFor(teammate);
    const QString model = agentModelLabel(primary.model).isEmpty()
                              ? agentProviderName(primary.provider)
                              : agentModelLabel(primary.model);
    int succeeded = 0;
    int failed = 0;
    int graded = 0;
    int gradeTotal = 0;
    for (const AgentSession *run : runs) {
        succeeded += run->status == AgentStatus::Success ? 1 : 0;
        failed += run->status == AgentStatus::Failed ? 1 : 0;
        if (run->taskGrade > 0) {
            ++graded;
            gradeTotal += run->taskGrade;
        }
    }
    QList<QPair<QString, QString>> rows{
        {QStringLiteral("Schedule"),
         teammate.schedule.active
             ? forkmesh::teammates::humanSchedule(teammate.schedule)
             : QStringLiteral("On demand")},
        {QStringLiteral("Model"), model},
        {QStringLiteral("Last run"),
         teammate.lastRunAtMs > 0
             ? forkmesh::teammates::humanLastActive(teammate.lastRunAtMs)
             : QStringLiteral("Never")},
        {QStringLiteral("Recent runs"),
         QStringLiteral("%1 · %2 succeeded · %3 failed")
             .arg(runs.size())
             .arg(succeeded)
             .arg(failed)},
        {QStringLiteral("Average grade"),
         graded > 0 ? QString::number(double(gradeTotal) / graded, 'f', 1) +
                          QStringLiteral(" / 5")
                    : QStringLiteral("Not graded yet")}};
    if (teammate.runWindowHours > 0)
        rows.append({QStringLiteral("Reminder"),
                     forkmesh::teammates::humanRunWindow(teammate)});
    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(4);
    for (int index = 0; index < rows.size(); ++index) {
        auto *key = new QLabel(rows.at(index).first);
        key->setObjectName(QStringLiteral("statusLine"));
        auto *value = new QLabel(rows.at(index).second);
        value->setWordWrap(true);
        grid->addWidget(key, index, 0, Qt::AlignTop);
        grid->addWidget(value, index, 1);
    }
    grid->setColumnStretch(1, 1);
    layout->addLayout(grid);
    if (!teammate.skills.isEmpty()) {
        auto *skillHost = new QWidget;
        skillHost->setObjectName(QStringLiteral("teammateProfileSkillsHost"));
        auto *flow = new FlowLayout(skillHost, 0, 5, 5);
        for (const QString &skill : teammate.skills)
            flow->addWidget(homePill(skill, QStringLiteral("skill")));
        layout->addWidget(skillHost);
    }
    for (int index = 0; index < qMin(5, int(teammate.activity.size())); ++index) {
        auto *line = new QLabel(teammate.activity.at(index));
        line->setObjectName(QStringLiteral("teammateActivityLine"));
        line->setWordWrap(true);
        layout->addWidget(line);
    }
}

void MainWindow::fillTeammateHomeSaid()
{
    if (!m_teammateHomeSaidSection || !m_teammateHomeSaidHost)
        return;
    const bool genie = m_teammateHomeId == QLatin1String(GenieLessons::teammateId);
    m_teammateHomeSaidSection->setVisible(genie);
    auto *layout = qobject_cast<QVBoxLayout *>(m_teammateHomeSaidHost->layout());
    clearHomeLayout(layout);
    if (!genie)
        return;
    const GenieSaidLog &log = genieSaidLog();
    const int awaiting = log.awaitingCheck();
    m_teammateHomeSaidCheck->setVisible(awaiting > 0);
    m_teammateHomeSaidCheck->setText(
        QStringLiteral("Ask Genie to check (%1)").arg(awaiting));
    m_teammateHomeSaidState->setText(
        log.said.isEmpty() ? QString()
                           : QStringLiteral("%1 kept").arg(log.said.size()));
    if (log.said.isEmpty()) {
        auto *empty = new QLabel(QStringLiteral(
            "Nothing yet. Every tip the spider shows is kept here, word for "
            "word, with the readings it was based on, so you can comment on "
            "it."));
        empty->setObjectName(QStringLiteral("teammateHomeWidgetsEmpty"));
        empty->setWordWrap(true);
        layout->addWidget(empty);
        return;
    }
    const auto stamp = [](qint64 ms) {
        return QDateTime::fromMSecsSinceEpoch(ms).toString(
            QStringLiteral("MMM d, HH:mm"));
    };
    int shown = 0;
    int hidden = 0;
    for (const GenieSaid &entry : log.said) {
        if (shown >= kHomeSaid && entry.comments.isEmpty()) {
            ++hidden;
            continue;
        }
        ++shown;
        auto *card = new QFrame;
        card->setObjectName(QStringLiteral("genieSaid"));
        card->setProperty("saidId", entry.id);
        card->setProperty("verdict", entry.awaitingCheck() ? QString() : entry.verdict);
        auto *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(10, 8, 10, 8);
        cardLayout->setSpacing(4);

        auto *top = new QHBoxLayout;
        top->setSpacing(6);
        QString when = QStringLiteral("%1 · %2").arg(
            GenieSaidLog::kindLabel(entry.kind), stamp(entry.lastAtMs));
        if (entry.times > 1)
            when += QStringLiteral(" · said %1 times since %2")
                        .arg(entry.times)
                        .arg(stamp(entry.firstAtMs));
        auto *whenLabel = new QLabel(when);
        whenLabel->setObjectName(QStringLiteral("teammateHomeWhen"));
        whenLabel->setWordWrap(true);
        whenLabel->setToolTip(QStringLiteral("Message %1").arg(entry.id));
        top->addWidget(whenLabel, 1);
        auto *comment = homeButton(QStringLiteral("Comment"),
                                   QStringLiteral("ghostButton"),
                                   QStringLiteral("comment"));
        comment->setProperty("genieSaidComment", true);
        comment->setToolTip(QStringLiteral("Say what was wrong, missing or right about it"));
        connect(comment, &QPushButton::clicked, this, [this, id = entry.id] {
            QTimer::singleShot(0, this, [this, id] { commentOnGenieSaid(id); });
        });
        top->addWidget(comment, 0, Qt::AlignTop);
        cardLayout->addLayout(top);

        auto *text = new QLabel(entry.text);
        text->setObjectName(QStringLiteral("teammateHomeReplyText"));
        text->setTextFormat(Qt::PlainText);
        text->setWordWrap(true);
        text->setTextInteractionFlags(Qt::TextSelectableByMouse);
        cardLayout->addWidget(text);
        if (!entry.task.isEmpty()) {
            auto *task = new QLabel(QStringLiteral("An agent can %1.").arg(entry.task));
            task->setObjectName(QStringLiteral("statusLine"));
            task->setTextFormat(Qt::PlainText);
            task->setWordWrap(true);
            cardLayout->addWidget(task);
        }
        if (!entry.basis.isEmpty()) {
            auto *basis = new QLabel(QStringLiteral("Based on: %1").arg(entry.basis));
            basis->setObjectName(QStringLiteral("statusLine"));
            basis->setTextFormat(Qt::PlainText);
            basis->setWordWrap(true);
            basis->setTextInteractionFlags(Qt::TextSelectableByMouse);
            cardLayout->addWidget(basis);
        }
        QString check;
        if (entry.awaitingCheck())
            check = QStringLiteral("Waiting for Genie to check it.");
        else if (!entry.verdict.isEmpty())
            check = QStringLiteral("Genie checked it %1: %2%3")
                        .arg(stamp(entry.checkedAtMs),
                             entry.verdict == QLatin1String(GenieSaidLog::verdictWrong)
                                 ? QStringLiteral("wrong")
                                 : QStringLiteral("accurate"),
                             entry.verdictNote.isEmpty()
                                 ? QStringLiteral(".")
                                 : QStringLiteral(". ") + entry.verdictNote);
        if (!check.isEmpty()) {
            auto *verdict = new QLabel(check);
            verdict->setObjectName(QStringLiteral("teammateWorkTitle"));
            verdict->setTextFormat(Qt::PlainText);
            verdict->setWordWrap(true);
            cardLayout->addWidget(verdict);
        }
        for (int index = 0; index < entry.comments.size(); ++index) {
            const GenieSaidComment &note = entry.comments.at(index);
            auto *bubble = new QFrame;
            bubble->setObjectName(QStringLiteral("teammateHomeYou"));
            auto *row = new QHBoxLayout(bubble);
            row->setContentsMargins(8, 6, 6, 6);
            row->setSpacing(6);
            auto *column = new QVBoxLayout;
            column->setSpacing(2);
            auto *noteWhen = new QLabel(QStringLiteral("You · %1").arg(stamp(note.atMs)));
            noteWhen->setObjectName(QStringLiteral("teammateHomeWhen"));
            auto *noteText = new QLabel(note.text);
            noteText->setObjectName(QStringLiteral("teammateHomeYouText"));
            noteText->setTextFormat(Qt::PlainText);
            noteText->setWordWrap(true);
            noteText->setTextInteractionFlags(Qt::TextSelectableByMouse);
            column->addWidget(noteWhen);
            column->addWidget(noteText);
            row->addLayout(column, 1);
            auto *remove = homeButton(QString(), QStringLiteral("ghostButton"),
                                      QStringLiteral("x"));
            remove->setProperty("genieSaidUncomment", true);
            remove->setToolTip(QStringLiteral("Remove this comment"));
            remove->setAccessibleName(QStringLiteral("Remove this comment"));
            connect(remove, &QPushButton::clicked, this, [this, id = entry.id, index] {
                removeGenieSaidComment(id, index);
            });
            row->addWidget(remove, 0, Qt::AlignTop);
            cardLayout->addWidget(bubble);
        }
        layout->addWidget(card);
    }
    if (hidden > 0) {
        auto *more = new QLabel(
            QStringLiteral("%1 older messages without comments are kept and not "
                           "shown.")
                .arg(hidden));
        more->setObjectName(QStringLiteral("statusLine"));
        more->setWordWrap(true);
        layout->addWidget(more);
    }
}
