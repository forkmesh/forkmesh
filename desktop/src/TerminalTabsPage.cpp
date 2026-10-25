#include "TerminalTabsPage.h"

#include "TerminalBridge.h"
#include "TerminalWidget.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTabBar>
#include <QTabWidget>
#include <QVBoxLayout>

namespace forkmesh::ui {

namespace bridge = forkmesh::terminalbridge;

TerminalTabsPage::TerminalTabsPage(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("terminalsPage"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(8);

    auto *header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(8);
    auto *title = new QLabel(QStringLiteral("Terminals"));
    title->setObjectName(QStringLiteral("terminalsTitle"));
    header->addWidget(title);
    m_status = new QLabel;
    m_status->setObjectName(QStringLiteral("terminalsAgentStatus"));
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    header->addWidget(m_status, 1);
    m_share = new QPushButton(QStringLiteral("Shared with agents"));
    m_share->setObjectName(QStringLiteral("terminalsShareButton"));
    m_share->setCheckable(true);
    m_share->setCursor(Qt::PointingHandCursor);
    m_share->setToolTip(QStringLiteral(
        "When on, agents this desktop launched (and Claude Code started in a ForkMesh "
        "terminal) can read this terminal and type into it through the ForkMesh MCP "
        "terminal tools. Turn it off to keep this tab to yourself."));
    header->addWidget(m_share);
    m_new = new QPushButton(QStringLiteral("New terminal"));
    m_new->setObjectName(QStringLiteral("terminalsNewButton"));
    m_new->setCursor(Qt::PointingHandCursor);
    m_new->setToolTip(QStringLiteral("Open the terminal launcher"));
    header->addWidget(m_new);
    layout->addLayout(header);

    m_tabs = new QTabWidget;
    m_tabs->setObjectName(QStringLiteral("terminalsTabs"));
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_tabs->setDocumentMode(true);
    layout->addWidget(m_tabs, 1);

    m_empty = new QLabel(QStringLiteral(
        "No terminals open. Use New terminal, or the terminal button in the top bar."));
    m_empty->setObjectName(QStringLiteral("terminalsEmpty"));
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setWordWrap(true);
    layout->addWidget(m_empty, 1);

    connect(m_new, &QPushButton::clicked, this, [this] {
        if (m_newTab)
            m_newTab();
    });
    connect(m_share, &QPushButton::toggled, this, [this](bool on) {
        const QString id = currentId();
        if (!id.isEmpty())
            setShared(id, on);
    });
    connect(m_tabs, &QTabWidget::tabCloseRequested, this,
            [this](int index) { closeTab(idAt(index)); });
    connect(m_tabs, &QTabWidget::currentChanged, this, [this] {
        syncChrome();
        if (QWidget *page = m_tabs->currentWidget())
            page->setFocus(Qt::OtherFocusReason);
    });
    syncChrome();
}

TerminalTabsPage::~TerminalTabsPage()
{
    for (const Tab &tab : std::as_const(m_byId))
        if (tab.terminal)
            tab.terminal->stop();
}

void TerminalTabsPage::setNewTabHandler(std::function<void()> handler)
{
    m_newTab = std::move(handler);
}

void TerminalTabsPage::setClosedHandler(std::function<void(const QString &)> handler)
{
    m_closed = std::move(handler);
}

void TerminalTabsPage::setEmptyText(const QString &text)
{
    m_empty->setText(text);
}

QString TerminalTabsPage::openTab(const Spec &spec)
{
    const QString id = bridge::validTerminalId(spec.id) ? spec.id : bridge::newTerminalId();
    auto *terminal = new TerminalWidget;
    terminal->setObjectName(QStringLiteral("terminalTab"));
    terminal->setProperty("terminalId", id);
    Tab tab;
    tab.terminal = terminal;
    tab.info.id = id;
    tab.info.kind = spec.kind.isEmpty() ? bridge::ShellKind : spec.kind;
    tab.info.cwd = spec.cwd;
    tab.info.command = spec.command;
    tab.info.openedBy = spec.openedBy;
    tab.info.shared = spec.shared;
    tab.info.startedAtMs = QDateTime::currentMSecsSinceEpoch();
    QString title = spec.title.simplified().left(bridge::kMaxTitleChars);
    if (title.isEmpty()) {
        const QString folder = QFileInfo(spec.cwd).fileName();
        title = bridge::kindLabel(tab.info.kind);
        if (!folder.isEmpty() && QDir(spec.cwd) != QDir::home())
            title += QStringLiteral(" · ") + folder;
    }
    tab.info.title = title;
    m_byId.insert(id, tab);

    connect(terminal, &TerminalWidget::outputArrived, this, [this, id] {
        auto it = m_byId.find(id);
        if (it != m_byId.end())
            it->info.lastOutputMs = QDateTime::currentMSecsSinceEpoch();
    });
    connect(terminal, &TerminalWidget::started, this, [this, id] {
        auto it = m_byId.find(id);
        if (it == m_byId.end())
            return;
        it->info.running = true;
        retitle(id);
    });
    connect(terminal, &TerminalWidget::finished, this, [this, id](int exitCode) {
        auto it = m_byId.find(id);
        if (it == m_byId.end())
            return;
        it->info.running = false;
        it->info.exitCode = exitCode;
        retitle(id);
        syncChrome();
    });

    const int index = m_tabs->addTab(terminal, title);
    m_tabs->setCurrentIndex(index);
    terminal->runCommand(spec.command, spec.cwd, spec.env, spec.keepShell);
    auto it = m_byId.find(id);
    if (it != m_byId.end())
        it->info.running = terminal->isRunning();
    retitle(id);
    syncChrome();
    terminal->setFocus(Qt::OtherFocusReason);
    return id;
}

bool TerminalTabsPage::closeTab(const QString &id)
{
    const int index = indexOf(id);
    auto it = m_byId.find(id);
    if (index < 0 || it == m_byId.end())
        return false;
    QPointer<TerminalWidget> terminal = it->terminal;
    m_byId.erase(it);
    m_tabs->removeTab(index);
    if (terminal) {
        terminal->disconnect(this);
        terminal->stop();
        terminal->deleteLater();
    }
    syncChrome();
    if (m_closed)
        m_closed(id);
    return true;
}

bool TerminalTabsPage::focusTab(const QString &id)
{
    const int index = indexOf(id);
    if (index < 0)
        return false;
    m_tabs->setCurrentIndex(index);
    return true;
}

void TerminalTabsPage::setShared(const QString &id, bool shared)
{
    auto it = m_byId.find(id);
    if (it == m_byId.end() || it->info.shared == shared)
        return;
    it->info.shared = shared;
    retitle(id);
    syncChrome();
}

void TerminalTabsPage::noteAgentAction(const QString &id, const QString &text)
{
    auto it = m_byId.find(id);
    if (it == m_byId.end())
        return;
    it->agentAction = text.simplified().left(160);
    syncChrome();
}

int TerminalTabsPage::count() const
{
    return m_tabs->count();
}

QStringList TerminalTabsPage::ids() const
{
    QStringList ids;
    for (int i = 0; i < m_tabs->count(); ++i)
        ids << idAt(i);
    return ids;
}

QList<TerminalTabsPage::Info> TerminalTabsPage::tabs() const
{
    QList<Info> rows;
    const QString current = currentId();
    for (const QString &id : ids()) {
        Info row;
        if (info(id, &row)) {
            row.current = id == current;
            rows.append(row);
        }
    }
    return rows;
}

bool TerminalTabsPage::info(const QString &id, Info *out) const
{
    const auto it = m_byId.constFind(id);
    if (it == m_byId.constEnd())
        return false;
    if (out) {
        *out = it->info;
        if (it->terminal) {
            out->running = it->terminal->isRunning();
            out->cols = it->terminal->gridColumns();
            out->rows = it->terminal->gridRows();
        }
        out->current = id == currentId();
    }
    return true;
}

TerminalWidget *TerminalTabsPage::terminal(const QString &id) const
{
    const auto it = m_byId.constFind(id);
    return it == m_byId.constEnd() ? nullptr : it->terminal.data();
}

QString TerminalTabsPage::currentId() const
{
    return idAt(m_tabs->currentIndex());
}

int TerminalTabsPage::indexOf(const QString &id) const
{
    if (id.isEmpty())
        return -1;
    for (int i = 0; i < m_tabs->count(); ++i)
        if (idAt(i) == id)
            return i;
    return -1;
}

QString TerminalTabsPage::idAt(int index) const
{
    QWidget *page = index >= 0 ? m_tabs->widget(index) : nullptr;
    return page ? page->property("terminalId").toString() : QString();
}

void TerminalTabsPage::syncChrome()
{
    const bool any = m_tabs->count() > 0;
    m_tabs->setVisible(any);
    m_empty->setVisible(!any);
    const auto it = m_byId.constFind(currentId());
    const bool found = it != m_byId.constEnd();
    m_share->setEnabled(found);
    {
        const QSignalBlocker blocker(m_share);
        m_share->setChecked(found && it->info.shared);
    }
    m_share->setText(found && !it->info.shared ? QStringLiteral("Private")
                                               : QStringLiteral("Shared with agents"));
    m_status->setText(found ? it->agentAction : QString());
}

void TerminalTabsPage::retitle(const QString &id)
{
    const int index = indexOf(id);
    const auto it = m_byId.constFind(id);
    if (index < 0 || it == m_byId.constEnd())
        return;
    QString text = it->info.title;
    const bool running = it->terminal && it->terminal->isRunning();
    if (!running && it->info.exitCode >= 0)
        text += QStringLiteral(" (exited)");
    m_tabs->setTabText(index, text);
    QStringList tip{it->info.title};
    if (!it->info.cwd.isEmpty())
        tip << QDir::toNativeSeparators(it->info.cwd);
    tip << (it->info.shared ? QStringLiteral("Agents can read and type here")
                            : QStringLiteral("Private: agents cannot see this tab"));
    m_tabs->setTabToolTip(index, tip.join(QLatin1Char('\n')));
}

}
