#include "TerminalTabsPage.h"

#include "TerminalBridge.h"
#include "TerminalWidget.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QTabBar>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace forkmesh::ui {

namespace bridge = forkmesh::terminalbridge;

TerminalTabsPage::TerminalTabsPage(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("terminalsPage"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 4, 6, 6);
    layout->setSpacing(4);

    const auto headerButton = [](const char *name, const QString &text, const QString &tip) {
        auto *button = new QPushButton(text);
        button->setObjectName(QString::fromLatin1(name));
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::NoFocus);
        button->setFixedSize(26, 24);
        button->setToolTip(tip);
        button->setAccessibleName(tip);
        return button;
    };

    auto *header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(4);
    auto *title = new QLabel(QStringLiteral("Terminals"));
    title->setObjectName(QStringLiteral("terminalsTitle"));
    header->addWidget(title);
    header->addStretch(1);
    m_share = new QPushButton(QStringLiteral("Shared with agents"));
    m_share->setObjectName(QStringLiteral("terminalsShareButton"));
    m_share->setCheckable(true);
    m_share->setCursor(Qt::PointingHandCursor);
    m_share->setFocusPolicy(Qt::NoFocus);
    m_share->setToolTip(QStringLiteral(
        "When on, agents this desktop launched (and Claude Code or Codex started in a "
        "ForkMesh terminal) can read this terminal and type into it through the ForkMesh "
        "MCP terminal tools. Turn it off to keep this tab to yourself."));
    header->addWidget(m_share);
    m_new = headerButton("terminalsNewButton", QStringLiteral("+"),
                         QStringLiteral("New terminal"));
    header->addWidget(m_new);
    m_kinds = new QToolButton;
    m_kinds->setObjectName(QStringLiteral("terminalsNewKindButton"));
    m_kinds->setText(QStringLiteral("▾"));
    m_kinds->setCursor(Qt::PointingHandCursor);
    m_kinds->setFocusPolicy(Qt::NoFocus);
    m_kinds->setFixedSize(26, 24);
    m_kinds->setPopupMode(QToolButton::InstantPopup);
    m_kinds->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_kinds->setToolTip(QStringLiteral("Open Claude Code, Codex or a command in a new tab"));
    m_kinds->setAccessibleName(m_kinds->toolTip());
    auto *kindsMenu = new QMenu(m_kinds);
    kindsMenu->setObjectName(QStringLiteral("terminalsNewKindMenu"));
    for (const QString &kind : bridge::kinds()) {
        if (kind == bridge::CommandKind)
            continue;
        QAction *action = kindsMenu->addAction(bridge::kindLabel(kind));
        action->setData(kind);
        connect(action, &QAction::triggered, this, [this, kind] {
            if (m_newTab)
                m_newTab(kind);
        });
    }
    kindsMenu->addSeparator();
    connect(kindsMenu->addAction(QStringLiteral("Command or folder…")), &QAction::triggered,
            this, [this] {
                if (m_newTab)
                    m_newTab(QString());
            });
    m_kinds->setMenu(kindsMenu);
    header->addWidget(m_kinds);
    m_hide = headerButton("terminalsHideButton", QStringLiteral("×"),
                          QStringLiteral("Hide terminals; they keep running"));
    m_hide->hide();
    header->addWidget(m_hide);
    layout->addLayout(header);

    m_status = new QLabel;
    m_status->setObjectName(QStringLiteral("terminalsAgentStatus"));
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_status->setWordWrap(true);
    m_status->hide();
    layout->addWidget(m_status);

    m_tabs = new QTabWidget;
    m_tabs->setObjectName(QStringLiteral("terminalsTabs"));
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_tabs->setDocumentMode(true);
    m_tabs->setUsesScrollButtons(true);
    m_tabs->setElideMode(Qt::ElideRight);
    layout->addWidget(m_tabs, 1);

    m_empty = new QLabel(QStringLiteral(
        "No terminals open. Press + for a shell, or pick Claude Code or Codex from the menu "
        "beside it."));
    m_empty->setObjectName(QStringLiteral("terminalsEmpty"));
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setWordWrap(true);
    layout->addWidget(m_empty, 1);

    connect(m_new, &QPushButton::clicked, this, [this] {
        if (m_newTab)
            m_newTab(bridge::ShellKind);
    });
    connect(m_hide, &QPushButton::clicked, this, [this] {
        if (m_hideHandler)
            m_hideHandler();
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

void TerminalTabsPage::setNewTabHandler(std::function<void(const QString &kind)> handler)
{
    m_newTab = std::move(handler);
}

void TerminalTabsPage::setClosedHandler(std::function<void(const QString &)> handler)
{
    m_closed = std::move(handler);
}

void TerminalTabsPage::setHideHandler(std::function<void()> handler)
{
    m_hideHandler = std::move(handler);
    m_hide->setVisible(bool(m_hideHandler));
}

void TerminalTabsPage::setIcons(const Icons &icons)
{
    const auto apply = [](QAbstractButton *button, const QIcon &icon, const QString &text) {
        button->setIcon(icon);
        button->setIconSize(QSize(14, 14));
        button->setText(icon.isNull() ? text : QString());
    };
    apply(m_new, icons.add, QStringLiteral("+"));
    apply(m_kinds, icons.more, QStringLiteral("▾"));
    apply(m_hide, icons.hide, QStringLiteral("×"));
}

void TerminalTabsPage::focusCurrent()
{
    if (QWidget *page = m_tabs->currentWidget())
        page->setFocus(Qt::OtherFocusReason);
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
    m_share->setVisible(any);
    m_share->setText(found && !it->info.shared ? QStringLiteral("Private")
                                               : QStringLiteral("Shared with agents"));
    m_status->setText(found ? it->agentAction : QString());
    m_status->setVisible(!m_status->text().isEmpty());
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
