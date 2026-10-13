#include "AgentAccessEditor.h"
#include "CommandVariables.h"
#include "DestructiveCommandReview.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

namespace {

QLabel *sectionLabel(const QString &text)
{
    auto *label = new QLabel(text);
    label->setObjectName("sectionLabel");
    return label;
}

QLabel *hint(const QString &text)
{
    auto *label = new QLabel(text);
    label->setObjectName("modeHint");
    label->setWordWrap(true);
    return label;
}

QPushButton *ghostButton(const QString &text)
{
    auto *button = new QPushButton(text);
    button->setObjectName("ghostButton");
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

} // namespace

AgentAccessEditor::AgentAccessEditor(QWidget *parent)
    : QWidget(parent)
{
    auto *column = new QVBoxLayout(this);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(8);

    column->addWidget(sectionLabel(QStringLiteral("AGENT FOLDERS")));
    m_locked = new QRadioButton(QStringLiteral("Only this project’s folder (recommended)"));
    m_locked->setToolTip(QStringLiteral(
        "Agents can read and change their worktree and the project folder, apart from the "
        "project's own data, settings, logs and keys. Everything else of yours is off "
        "limits unless you add it below."));
    m_open = new QRadioButton(QStringLiteral("Anywhere my account can reach"));
    m_open->setToolTip(QStringLiteral(
        "No folder boundary beyond any repository guardrails. Agents can read and change "
        "any file you can."));
    auto *folderGroup = new QButtonGroup(this);
    folderGroup->addButton(m_locked);
    folderGroup->addButton(m_open);
    column->addWidget(m_locked);
    column->addWidget(m_open);

    m_sharedTools =
        new QCheckBox(QStringLiteral("Let agents use installed tools, caches and temp folders"));
    m_sharedTools->setToolTip(QStringLiteral(
        "Read-only: ~/.local/bin, ~/.cargo/bin, ~/.rustup, ~/.nvm, /opt, /usr/local and "
        "similar toolchains.\nRead and write: ~/.cache, /tmp, /var/tmp and /dev/shm.\n"
        "Off: only the system's own programs, the project and the folders below."));
    column->addWidget(m_sharedTools);

    column->addWidget(hint(QStringLiteral("Extra folders agents may use:")));
    m_folders = new QListWidget;
    m_folders->setMaximumHeight(110);
    column->addWidget(m_folders);
    auto *folderButtons = new QHBoxLayout;
    m_addReadOnly = ghostButton(QStringLiteral("Add read-only folder…"));
    m_addWritable = ghostButton(QStringLiteral("Add writable folder…"));
    m_removeFolder = ghostButton(QStringLiteral("Remove"));
    folderButtons->addWidget(m_addReadOnly);
    folderButtons->addWidget(m_addWritable);
    folderButtons->addStretch(1);
    folderButtons->addWidget(m_removeFolder);
    column->addLayout(folderButtons);
    connect(m_addReadOnly, &QPushButton::clicked, this, [this] { chooseFolder(false); });
    connect(m_addWritable, &QPushButton::clicked, this, [this] { chooseFolder(true); });
    connect(m_removeFolder, &QPushButton::clicked, this,
            [this] { delete m_folders->takeItem(m_folders->currentRow()); });
    if (!forkmesh::projects::folderLockSupported())
        column->addWidget(hint(QStringLiteral(
            "The folder lock uses Linux Landlock. On this system Claude Code agents are told "
            "the boundary, and Codex agents run in Codex's own workspace sandbox.")));

    column->addWidget(sectionLabel(QStringLiteral("EXTERNAL CALLS")));
    m_ask = new QRadioButton(QStringLiteral("Ask me before each external call (recommended)"));
    m_ask->setToolTip(QStringLiteral(
        "Web fetches and searches, and commands such as curl, git fetch or push, ssh, "
        "package installs and scripts that use the network, wait for your answer. Approved "
        "destinations below go through."));
    m_block = new QRadioButton(QStringLiteral("Block external calls, except approved destinations"));
    m_allow = new QRadioButton(QStringLiteral("Allow all external calls"));
    auto *networkGroup = new QButtonGroup(this);
    networkGroup->addButton(m_ask);
    networkGroup->addButton(m_block);
    networkGroup->addButton(m_allow);
    column->addWidget(m_ask);
    column->addWidget(m_block);
    column->addWidget(m_allow);

    column->addWidget(hint(QStringLiteral(
        "Approved destinations (“*.example.com” covers its subdomains; "
        "“web-search” is the web search tool):")));
    m_hosts = new QListWidget;
    m_hosts->setMaximumHeight(100);
    column->addWidget(m_hosts);
    auto *hostRow = new QHBoxLayout;
    m_hostEdit = new QLineEdit;
    m_hostEdit->setPlaceholderText(QStringLiteral("github.com"));
    m_addHost = ghostButton(QStringLiteral("Add"));
    m_removeHost = ghostButton(QStringLiteral("Remove"));
    hostRow->addWidget(m_hostEdit, 1);
    hostRow->addWidget(m_addHost);
    hostRow->addWidget(m_removeHost);
    column->addLayout(hostRow);

    column->addWidget(sectionLabel(QStringLiteral("DESTRUCTIVE COMMANDS ALLOWED WITHOUT ASKING")));
    column->addWidget(hint(QStringLiteral(
        "Checked kinds run without the “Destructive command” prompt. The prompt’s "
        "“Always allow” adds to this list. Blocked destructive commands stay blocked.")));
    m_destructive = new QListWidget;
    m_destructive->setObjectName(QStringLiteral("agentAccessAllowedDestructive"));
    m_destructive->setMaximumHeight(150);
    column->addWidget(m_destructive);

    column->addWidget(sectionLabel(QStringLiteral("FOLDERS WHERE RECURSIVE DELETES RUN WITHOUT ASKING")));
    column->addWidget(hint(QStringLiteral(
        "A recursive delete runs without the prompt when every path it deletes, with its "
        "variables filled in, matches one of these patterns. “*” matches one name and “**” "
        "any number of folders; start with /, ~ or $FORKMESH_AGENT_SCRATCH (each agent’s own "
        "scratch folder). ForkMesh data, .git, worktree folders themselves, your home folder "
        "and / are never covered.")));
    m_deletePaths = new QListWidget;
    m_deletePaths->setObjectName(QStringLiteral("agentAccessDeletePaths"));
    m_deletePaths->setMaximumHeight(100);
    column->addWidget(m_deletePaths);
    auto *deleteRow = new QHBoxLayout;
    m_deletePathEdit = new QLineEdit;
    m_deletePathEdit->setObjectName(QStringLiteral("agentAccessDeletePathEdit"));
    m_deletePathEdit->setPlaceholderText(QStringLiteral("$FORKMESH_AGENT_SCRATCH/**"));
    auto *addDeletePathButton = ghostButton(QStringLiteral("Add"));
    auto *removeDeletePathButton = ghostButton(QStringLiteral("Remove"));
    deleteRow->addWidget(m_deletePathEdit, 1);
    deleteRow->addWidget(addDeletePathButton);
    deleteRow->addWidget(removeDeletePathButton);
    column->addLayout(deleteRow);
    connect(addDeletePathButton, &QPushButton::clicked, this, [this] { addDeletePath(); });
    connect(m_deletePathEdit, &QLineEdit::returnPressed, this, [this] { addDeletePath(); });
    connect(removeDeletePathButton, &QPushButton::clicked, this,
            [this] { delete m_deletePaths->takeItem(m_deletePaths->currentRow()); });

    m_error = new QLabel;
    m_error->setStyleSheet("color:#ff6b6b; background:transparent;");
    m_error->setWordWrap(true);
    m_error->hide();
    column->addWidget(m_error);
    connect(m_addHost, &QPushButton::clicked, this, [this] { addHost(); });
    connect(m_hostEdit, &QLineEdit::returnPressed, this, [this] { addHost(); });
    connect(m_removeHost, &QPushButton::clicked, this,
            [this] { delete m_hosts->takeItem(m_hosts->currentRow()); });

    connect(m_locked, &QRadioButton::toggled, this, [this] { syncControls(); });
    connect(m_allow, &QRadioButton::toggled, this, [this] { syncControls(); });
    setAccess(forkmesh::projects::AgentAccess());
}

void AgentAccessEditor::setAccess(const forkmesh::projects::AgentAccess &access)
{
    using Access = forkmesh::projects::AgentAccess;
    (access.folders == Access::Folders::Project ? m_locked : m_open)->setChecked(true);
    m_sharedTools->setChecked(access.sharedTools);
    m_folders->clear();
    for (const QString &path : access.readWriteFolders)
        addFolder(path, true);
    for (const QString &path : access.readOnlyFolders)
        addFolder(path, false);
    (access.network == Access::Network::Block   ? m_block
     : access.network == Access::Network::Allow ? m_allow
                                                : m_ask)
        ->setChecked(true);
    m_hosts->clear();
    m_hosts->addItems(access.allowedHosts);
    m_destructive->clear();
    QStringList kinds = forkmesh::destructive::kinds();
    for (const QString &kind : access.allowedDestructive)
        if (!kinds.contains(kind))
            kinds.append(kind);
    for (const QString &kind : kinds) {
        const QString example = forkmesh::destructive::kindExample(kind);
        auto *item = new QListWidgetItem(
            example.isEmpty() ? kind : QStringLiteral("%1 — %2").arg(kind, example),
            m_destructive);
        item->setData(Qt::UserRole, kind);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(access.allowedDestructive.contains(kind) ? Qt::Checked
                                                                     : Qt::Unchecked);
    }
    m_deletePaths->clear();
    m_deletePaths->addItems(access.allowedDeletePaths);
    syncControls();
}

forkmesh::projects::AgentAccess AgentAccessEditor::access() const
{
    using Access = forkmesh::projects::AgentAccess;
    Access access;
    access.folders = m_locked->isChecked() ? Access::Folders::Project
                                           : Access::Folders::Unrestricted;
    access.sharedTools = m_sharedTools->isChecked();
    for (int i = 0; i < m_folders->count(); ++i) {
        const QListWidgetItem *item = m_folders->item(i);
        (item->data(Qt::UserRole + 1).toBool() ? access.readWriteFolders
                                               : access.readOnlyFolders)
            .append(item->data(Qt::UserRole).toString());
    }
    access.network = m_block->isChecked()   ? Access::Network::Block
                     : m_allow->isChecked() ? Access::Network::Allow
                                            : Access::Network::Ask;
    for (int i = 0; i < m_hosts->count(); ++i)
        access.allowedHosts.append(m_hosts->item(i)->text());
    for (int i = 0; i < m_destructive->count(); ++i)
        if (m_destructive->item(i)->checkState() == Qt::Checked)
            access.allowedDestructive.append(m_destructive->item(i)->data(Qt::UserRole).toString());
    for (int i = 0; i < m_deletePaths->count(); ++i)
        access.allowedDeletePaths.append(m_deletePaths->item(i)->text());
    return access;
}

void AgentAccessEditor::setChoicesEnabled(bool enabled)
{
    m_choicesEnabled = enabled;
    syncControls();
}

void AgentAccessEditor::syncControls()
{
    for (QWidget *widget : {static_cast<QWidget *>(m_locked), static_cast<QWidget *>(m_open),
                            static_cast<QWidget *>(m_ask), static_cast<QWidget *>(m_block),
                            static_cast<QWidget *>(m_allow)})
        widget->setEnabled(m_choicesEnabled);
    const bool locked = m_locked->isChecked() || !m_choicesEnabled;
    m_sharedTools->setEnabled(m_choicesEnabled && m_locked->isChecked());
    for (QWidget *widget : {static_cast<QWidget *>(m_folders),
                            static_cast<QWidget *>(m_addReadOnly),
                            static_cast<QWidget *>(m_addWritable),
                            static_cast<QWidget *>(m_removeFolder)})
        widget->setEnabled(locked);
    const bool gated = !m_allow->isChecked() || !m_choicesEnabled;
    for (QWidget *widget :
         {static_cast<QWidget *>(m_hosts), static_cast<QWidget *>(m_hostEdit),
          static_cast<QWidget *>(m_addHost), static_cast<QWidget *>(m_removeHost)})
        widget->setEnabled(gated);
}

void AgentAccessEditor::addFolder(const QString &path, bool writable)
{
    auto *item = new QListWidgetItem(
        QStringLiteral("%1  —  %2")
            .arg(QDir::toNativeSeparators(path),
                 writable ? QStringLiteral("read and write") : QStringLiteral("read only")),
        m_folders);
    item->setData(Qt::UserRole, path);
    item->setData(Qt::UserRole + 1, writable);
}

void AgentAccessEditor::chooseFolder(bool writable)
{
    const QString chosen = QFileDialog::getExistingDirectory(
        this,
        writable ? QStringLiteral("Folder agents may change")
                 : QStringLiteral("Folder agents may read"),
        QDir::homePath());
    if (chosen.isEmpty())
        return;
    const QString path = QDir::cleanPath(chosen);
    for (int i = m_folders->count() - 1; i >= 0; --i)
        if (m_folders->item(i)->data(Qt::UserRole).toString() == path)
            delete m_folders->takeItem(i);
    addFolder(path, writable);
}

void AgentAccessEditor::addHost()
{
    const QString host = forkmesh::projects::normalizeHost(m_hostEdit->text());
    if (host.isEmpty()) {
        m_error->setText(QStringLiteral("Enter a host name such as github.com."));
        m_error->show();
        return;
    }
    m_error->hide();
    if (m_hosts->findItems(host, Qt::MatchExactly).isEmpty())
        m_hosts->addItem(host);
    m_hostEdit->clear();
}

void AgentAccessEditor::addDeletePath()
{
    const QString pattern = m_deletePathEdit->text().trimmed();
    const QString problem = forkmesh::commandvars::deletePatternProblem(pattern);
    if (!problem.isEmpty()) {
        m_error->setText(problem);
        m_error->show();
        return;
    }
    m_error->hide();
    if (m_deletePaths->findItems(pattern, Qt::MatchExactly).isEmpty())
        m_deletePaths->addItem(pattern);
    m_deletePathEdit->clear();
}
