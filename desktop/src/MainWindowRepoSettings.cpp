// Repository settings page, repository remotes and relay identity. Split
// out of MainWindowRepos.cpp.

#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "SafeDelete.h"
#include "MainWindowReposInternal.h"
#include "KebabHeaderView.h"
#include "NodeEventSocket.h"
#include "PrivateMirrorRuntime.h"
#include "PublicMirrorRuntime.h"
#include "RepoStatsStore.h"

using namespace forkmesh::ui;
using namespace mainwindowrepos;

namespace {

QString normalizedRelayOrigin(const QUrl &url)
{
    return forkmesh::relayscope::normalizedOrigin(url);
}

QString repoRemoteGitDir(const RepositoryRecord &repo)
{
    const QString local = repo.localPath.trimmed();
    if (!local.isEmpty() && QDir(local).exists(QStringLiteral(".git")))
        return local;
    const QString mirror = repo.mirrorPath.trimmed();
    if (!mirror.isEmpty() && QDir(mirror).exists())
        return mirror;
    return {};
}

} // namespace

QString MainWindow::repositoryWebUrl(const QString &owner,
                                     const QString &name) const
{
    QUrl url = catalogApiUrl();
    url.setPath("/" + repoSegment(owner, QStringLiteral("owner")) + "/" +
                repoSegment(name, QStringLiteral("repository")));
    url.setFragment(QString());
    return url.toString();
}

QString MainWindow::repositoryWebUrl(const RepositoryRecord &repo) const
{
    return repositoryWebUrl(repo.owner, repo.name);
}

void MainWindow::updateRepoActionMenus()
{
    if (!m_mirrorMenu || !m_sourceMenu)
        return;
    m_mirrorMenu->clear();
    m_sourceMenu->clear();

    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size()) {
        for (QMenu *menu : {m_mirrorMenu, m_sourceMenu}) {
            QAction *empty = menu->addAction("No repository selected");
            empty->setEnabled(false);
        }
        return;
    }

    const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
    const bool hasMirror = !repo.mirrorPath.isEmpty() && QDir(repo.mirrorPath).exists();
    const QString webUrl = repositoryWebUrl(repo);

    if (repo.previewOnly) {
        m_mirrorMenu->addSection(hasMirror ? "PREVIEW CACHED" : "PREVIEW PENDING");
        QAction *keep = m_mirrorMenu->addAction("Keep as a local mirror");
        connect(keep, &QAction::triggered, this, [this] {
            if (m_repoDetailIndex >= 0)
                mirrorPreviewRepository(m_repoDetailIndex);
        });
    } else {
        m_mirrorMenu->addSection("MIRROR STATUS");
        if (repo.publishToNetwork && hasMirror) {
            QAction *browse = m_mirrorMenu->addAction(
                QStringLiteral("Managed mirror enabled ") + QChar(0x00B7) +
                " open " + webUrl);
            browse->setToolTip(webUrl);
            connect(browse, &QAction::triggered, this,
                    [webUrl] { QDesktopServices::openUrl(QUrl(webUrl)); });
        } else {
            QAction *status = m_mirrorMenu->addAction(
                repo.publishToNetwork ? QStringLiteral("Not published yet")
                                      : QStringLiteral("Local only"));
            status->setEnabled(false);
        }
        m_mirrorMenu->addSection("MIRROR LOCATION");
        QAction *location = m_mirrorMenu->addAction(
            hasMirror ? repo.mirrorPath : "Mirror has not been created yet");
        location->setEnabled(false);
        QAction *sync = m_mirrorMenu->addAction(hasMirror ? "Sync mirror now"
                                                           : "Create mirror now");
        connect(sync, &QAction::triggered, this, [this] {
            if (m_repoDetailIndex >= 0)
                syncRepository(m_repoDetailIndex);
        });
        if (hasMirror) {
            QAction *copy = m_mirrorMenu->addAction("Copy mirror location");
            connect(copy, &QAction::triggered, this, [this] {
                if (m_repoDetailIndex < 0 ||
                    m_repoDetailIndex >= m_repositories.size())
                    return;
                QApplication::clipboard()->setText(
                    m_repositories.at(m_repoDetailIndex).mirrorPath);
                setRepoDetailNotice("Copied mirror location.");
            });
        }
        if (hasMirror) {
            m_mirrorMenu->addSeparator();
            QAction *remove = m_mirrorMenu->addAction("Delete mirror...");
            connect(remove, &QAction::triggered, this,
                    &MainWindow::deleteCurrentMirror);
        }
    }

    const bool hasWorktree = !repo.localPath.isEmpty() && QDir(repo.localPath).exists();

    m_sourceMenu->addSection("LOCAL REMOTE");
    QAction *remote = m_sourceMenu->addAction(
        hasMirror ? repo.mirrorPath : "Sync first to create a local remote");
    remote->setEnabled(false);
    if (hasMirror) {
        QAction *copyRemote = m_sourceMenu->addAction("Copy local remote");
        connect(copyRemote, &QAction::triggered, this, [this] {
            if (m_repoDetailIndex < 0 ||
                m_repoDetailIndex >= m_repositories.size())
                return;
            const QString path = m_repositories.at(m_repoDetailIndex).mirrorPath;
            QApplication::clipboard()->setText(path);
            setRepoDetailNotice("Copied local remote path.");
            logSystem("Copied local remote path to clipboard: " + path);
        });
        QAction *copyCommand = m_sourceMenu->addAction(
            "Copy git remote add command");
        connect(copyCommand, &QAction::triggered, this, [this] {
            if (m_repoDetailIndex < 0 ||
                m_repoDetailIndex >= m_repositories.size())
                return;
            const QString path = m_repositories.at(m_repoDetailIndex).mirrorPath;
            QApplication::clipboard()->setText(
                QStringLiteral("git remote add forkmesh \"") + path + "\"");
            setRepoDetailNotice("Copied git remote add command.");
        });
    }
    m_sourceMenu->addSeparator();
    QAction *zip = m_sourceMenu->addAction("Download ZIP");
    zip->setEnabled(hasMirror || hasWorktree);
    connect(zip, &QAction::triggered, this, &MainWindow::downloadCurrentRepoZip);
}

void MainWindow::deleteCurrentMirror()
{
    deleteRepositoryAt(m_repoDetailIndex, true);
}

void MainWindow::deleteRepositoryAt(int index, bool reopenRepoDetail)
{
    if (index < 0 || index >= m_repositories.size())
        return;
    const RepositoryRecord repo = m_repositories.at(index);
    const QString rawPath = repo.mirrorPath.trimmed();
    const QString rawWorktree = repo.localPath.trimmed();
    const QString path = rawPath.isEmpty() ? QString() : QDir::cleanPath(rawPath);
    const QString worktree = rawWorktree.isEmpty()
                                 ? QString()
                                 : QDir::cleanPath(rawWorktree);

    QString message = QStringLiteral(
        "Delete repository %1/%2 from this machine?\n\nMirror: %3")
                          .arg(repo.owner, repo.name,
                               path.isEmpty() ? QStringLiteral("not created") : path);
    if (!repo.localPath.isEmpty())
        message += QStringLiteral("\n\nYour working directory will be kept on disk "
                                  "but no longer tracked by ForkMesh:\n%1")
                       .arg(repo.localPath);
    if (!repo.previewOnly && repo.publishToNetwork)
        message += QStringLiteral("\n\nThe repository will also be removed from "
                                  "the public ForkMesh catalog.");
    message += QStringLiteral(
        "\n\nThis frees up the name so you can create a new repository "
        "called \"%1\" again.").arg(repo.name);

    QMessageBox box(QMessageBox::Warning, "Delete repository", message,
                     QMessageBox::Yes | QMessageBox::Cancel, this);
    box.setDefaultButton(QMessageBox::Cancel);
    QCheckBox *keepFilesCheck = nullptr;
    if (!path.isEmpty()) {
        keepFilesCheck = new QCheckBox(
            QStringLiteral("Keep the repository files on disk (only remove from ForkMesh)"));
        keepFilesCheck->setChecked(false);
        box.setCheckBox(keepFilesCheck); // QMessageBox takes ownership
    }
    if (box.exec() != QMessageBox::Yes)
        return;
    const bool keepFiles = keepFilesCheck && keepFilesCheck->isChecked();
    index = findNetworkRepoIndex(repo.owner, repo.name, true);
    if (index < 0)
        return;

    if (!keepFiles && !path.isEmpty() && path == worktree) {
        QMessageBox::warning(
            this, "Delete repository",
            "The mirror path matches the working directory, so nothing was deleted.");
        return;
    }

    stopNodeEventSocket();
    // Forks created here borrow this mirror's objects instead of copying them
    // (forkCurrentRepo), so give every borrower its own copy before the files
    // go away - otherwise deleting an upstream silently breaks its forks.
    if (!keepFiles && !path.isEmpty() && QDir(path).exists())
        detachForksBorrowingMirror(path);
    QString refused;
    if (!keepFiles && !path.isEmpty() && QDir(path).exists() &&
        !forkmesh::safedelete::removeTree(path, QFileInfo(path).absolutePath(),
                                          &refused)) {
        startNodeEventSocket();
        QMessageBox::warning(this, "Delete repository",
                             "Could not delete the mirror at:\n" + path +
                                 (refused.isEmpty() ? QString() : "\n\n" + refused));
        return;
    }
    if (PublicMirrorRuntime::isArchiveId(repo.publicArchiveId)) {
        m_publicMirrorMaterializations.remove(repo.publicArchiveId);
        if (!keepFiles) {
            QFile::remove(
                PublicMirrorRuntime::ciphertextPath(
                    publicArchiveRoot(), repo.publicArchiveId));
            QFile::remove(
                QDir(publicArchiveRoot())
                    .filePath(repo.publicArchiveId +
                              QStringLiteral(".json")));
        }
    }
    if (PrivateMirrorStore::isOpaqueId(repo.privateReplicaId)) {
        m_privateMirrorMaterializations.remove(repo.privateReplicaId);
        if (!keepFiles) {
            QFile::remove(
                QDir(privateReplicaRoot())
                    .filePath(repo.privateReplicaId +
                              QStringLiteral(".fm-private")));
        }
    }
    m_repositories.removeAt(index);
    saveRepositories();
    startNodeEventSocket();
    refreshRepositoryList();
    if (m_repoDetailIndex == index)
        m_repoDetailIndex = -1;
    else if (m_repoDetailIndex > index)
        --m_repoDetailIndex;
    if (reopenRepoDetail) {
        if (!m_repositories.isEmpty())
            openRepoDetail(qMin(index, m_repositories.size() - 1));
        else if (m_repoDetailStack)
            m_repoDetailStack->setCurrentIndex(0); // Code (empty)
    }
    logSystem("Deleted repository " + repo.owner + "/" + repo.name + ".");
    setRepoDetailNotice(keepFiles ? "Removed repository from ForkMesh. Files kept on disk."
                                   : "Deleted repository.");
}

QWidget *MainWindow::buildRepoSettingsTab()
{
    auto *page = new QWidget;
    auto *outer = new QVBoxLayout(page);
    outer->setContentsMargins(24, 22, 24, 22);
    outer->setSpacing(10);

    auto *heading = new QLabel("Repository settings");
    heading->setObjectName("channelTitle");
    outer->addWidget(heading);

    auto *tabs = new QTabWidget;
    tabs->setObjectName("repoSettingsTabs");
    tabs->setDocumentMode(true);
    auto addTab = [tabs](QWidget *body, const QString &name) {
        auto *scroll = new QScrollArea;
        scroll->setObjectName("repoSettingsTabScroll");
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setWidget(body);
        tabs->addTab(scroll, name);
    };
    outer->addWidget(tabs, 1);

    auto *generalTab = new QWidget;
    auto *generalCol = new QVBoxLayout(generalTab);
    generalCol->setContentsMargins(2, 12, 2, 12);
    generalCol->setSpacing(8);

    auto *aboutHeading = new QLabel("About");
    aboutHeading->setObjectName("sectionLabel");
    generalCol->addWidget(aboutHeading);

    auto *aboutHint = new QLabel(
        "The description, website link and topics shown on this repository's "
        "overview (and on its public ForkMesh page).");
    aboutHint->setObjectName("statusLine");
    aboutHint->setWordWrap(true);
    generalCol->addWidget(aboutHint);

    auto *aboutEditBtn = new QPushButton("Edit description & website\xE2\x80\xA6");
    aboutEditBtn->setProperty("buttonSize", "sm");
    aboutEditBtn->setCursor(Qt::PointingHandCursor);
    connect(aboutEditBtn, &QPushButton::clicked, this, &MainWindow::editRepoAbout);
    auto *aboutRow = new QHBoxLayout;
    aboutRow->setContentsMargins(0, 0, 0, 0);
    aboutRow->addWidget(aboutEditBtn);
    aboutRow->addStretch();
    generalCol->addLayout(aboutRow);

    generalCol->addSpacing(6);

    auto *visHeading = new QLabel("Visibility");
    visHeading->setObjectName("sectionLabel");
    generalCol->addWidget(visHeading);

    m_repoPrivateCheck = new QCheckBox("Private repository");
    m_repoPrivateCheck->setCursor(Qt::PointingHandCursor);
    m_repoPrivateCheck->setToolTip(
        "Hide this repo from the public catalog and require your node's key to "
        "browse or clone it through the mainnode. Only you can read it.");
    connect(m_repoPrivateCheck, &QCheckBox::toggled, this, [this](bool on) {
        if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
            return;
        if (m_repositories[m_repoDetailIndex].isPrivate == on)
            return;
        m_repositories[m_repoDetailIndex].isPrivate = on;
        saveRepositories();
        QString gatewayError;
        rebuildDirectMirrorGatewayConfiguration(
            &gatewayError, true);
        logSystem(QStringLiteral("%1 %2/%3.")
                      .arg(on ? "Made private" : "Made public",
                           m_repositories.at(m_repoDetailIndex).owner,
                           m_repositories.at(m_repoDetailIndex).name));
        if (on)
            syncPrivateRepository(m_repoDetailIndex, /*quiet=*/false);
        else
            syncRepository(m_repoDetailIndex, /*quiet=*/false);
        startNodeEventSocket();
        refreshRepositoryList();
        refreshRepoSettings();
    });
    generalCol->addWidget(m_repoPrivateCheck);

    m_repoServeCheck = new QCheckBox("Serve on the network");
    m_repoServeCheck->setCursor(Qt::PointingHandCursor);
    m_repoServeCheck->setToolTip(
        "Publish this repository through the relay so your other nodes and "
        "collaborators can reach it. Turn it off to keep the repository "
        "entirely on this machine: a local-only repository makes no relay "
        "request at all, not even to check its collaborator keys, so its "
        "owner/name never appears in a request URL.");
    connect(m_repoServeCheck, &QCheckBox::toggled, this, [this](bool on) {
        const int index = m_repoDetailIndex;
        if (index < 0 || index >= m_repositories.size())
            return;
        if (m_repositories.at(index).publishToNetwork == on) {
            refreshRepoSettings();
            return;
        }
        if (!setRepositoryNetworkServing(index, on)) {
            // Refused (not the owner): put the box back where it was.
            refreshRepoSettings();
            return;
        }
        flashMessage(on ? QString::fromUtf8(
                              "Serving on the network. Publishing now\xE2\x80\xA6")
                        : QStringLiteral(
                              "Local only. This repository no longer contacts "
                              "the network."));
        refreshRepoSettings();
    });
    generalCol->addWidget(m_repoServeCheck);

    m_repoVisibilityHint = new QLabel;
    m_repoVisibilityHint->setObjectName("statusLine");
    m_repoVisibilityHint->setWordWrap(true);
    generalCol->addWidget(m_repoVisibilityHint);

    generalCol->addSpacing(6);

    m_collabSection = new QWidget;
    auto *collabLayout = new QVBoxLayout(m_collabSection);
    collabLayout->setContentsMargins(0, 0, 0, 0);
    collabLayout->setSpacing(8);

    auto *collabHeading = new QLabel("Collaborators");
    collabHeading->setObjectName("sectionLabel");
    collabLayout->addWidget(collabHeading);

    auto *collabHint = new QLabel(
        "Accounts you share this private repository with. They can see it in "
        "their catalog (once signed in) and clone it with their own key. The "
        "repo stays hidden from the public website.");
    collabHint->setObjectName("statusLine");
    collabHint->setWordWrap(true);
    collabLayout->addWidget(collabHint);

    m_collabList = new QListWidget;
    m_collabList->setObjectName("collabList");
    m_collabList->setMaximumHeight(140);
    collabLayout->addWidget(m_collabList);

    m_collabEmptyHint = new QLabel("No collaborators yet.");
    m_collabEmptyHint->setObjectName("statusLine");
    collabLayout->addWidget(m_collabEmptyHint);

    auto *collabRow = new QHBoxLayout;
    m_collabEdit = new QLineEdit;
    m_collabEdit->setPlaceholderText("account name to add");
    collabRow->addWidget(m_collabEdit, 1);
    auto *collabAddBtn = new QPushButton("Add");
    collabAddBtn->setProperty("buttonSize", "sm");
    collabAddBtn->setCursor(Qt::PointingHandCursor);
    collabRow->addWidget(collabAddBtn);
    auto *collabRemoveBtn = new QPushButton("Remove selected");
    collabRemoveBtn->setProperty("buttonSize", "sm");
    collabRemoveBtn->setCursor(Qt::PointingHandCursor);
    collabRow->addWidget(collabRemoveBtn);
    collabLayout->addLayout(collabRow);

    auto addCollab = [this] {
        if (m_collabEdit)
            addRepoCollaborator(m_collabEdit->text());
    };
    connect(collabAddBtn, &QPushButton::clicked, this, addCollab);
    connect(m_collabEdit, &QLineEdit::returnPressed, this, addCollab);
    connect(collabRemoveBtn, &QPushButton::clicked, this, [this] {
        if (m_collabList && m_collabList->currentItem())
            removeRepoCollaborator(m_collabList->currentItem()->text());
    });

    generalCol->addWidget(m_collabSection);
    generalCol->addStretch();
    addTab(generalTab, "General");

    auto *gitTab = new QWidget;
    auto *gitCol = new QVBoxLayout(gitTab);
    gitCol->setContentsMargins(2, 12, 2, 12);
    gitCol->setSpacing(8);

    auto *sourceHeading = new QLabel("Source");
    sourceHeading->setObjectName("sectionLabel");
    gitCol->addWidget(sourceHeading);

    m_repoSourceEdit = new QLineEdit;
    m_repoSourceEdit->setPlaceholderText(
        "https://forkmesh.com/<node>/<owner>/<name>");
    m_repoSourceEdit->setToolTip(
        "The upstream clone URL this mirror was forked from. Edit it to repoint "
        "the mirror at a live node when the original location goes stale.");
    auto *sourceUpdateBtn = new QPushButton("Update");
    sourceUpdateBtn->setProperty("buttonSize", "sm");
    sourceUpdateBtn->setCursor(Qt::PointingHandCursor);
    connect(sourceUpdateBtn, &QPushButton::clicked, this,
            &MainWindow::updateRepoSource);
    connect(m_repoSourceEdit, &QLineEdit::returnPressed, this,
            &MainWindow::updateRepoSource);
    auto *sourceRow = new QHBoxLayout;
    sourceRow->addWidget(m_repoSourceEdit, 1);
    sourceRow->addWidget(sourceUpdateBtn);
    gitCol->addLayout(sourceRow);

    m_repoSourceHint = new QLabel;
    m_repoSourceHint->setObjectName("statusLine");
    m_repoSourceHint->setWordWrap(true);
    gitCol->addWidget(m_repoSourceHint);

    m_repoForkLocation = new QLabel;
    m_repoForkLocation->setObjectName("statusLine");
    m_repoForkLocation->setWordWrap(true);
    m_repoForkLocation->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *forkLocationButton = new QPushButton("Set");
    forkLocationButton->setProperty("buttonSize", "sm");
    forkLocationButton->setCursor(Qt::PointingHandCursor);
    forkLocationButton->setToolTip(
        "Choose an existing local Git working copy to use as this repo's fork "
        "location.");
    connect(forkLocationButton, &QPushButton::clicked, this,
            &MainWindow::promptSetRepoForkLocation);
    auto *forkLocationRow = new QHBoxLayout;
    forkLocationRow->setContentsMargins(0, 0, 0, 0);
    forkLocationRow->addWidget(m_repoForkLocation, 1);
    forkLocationRow->addWidget(forkLocationButton);
    gitCol->addLayout(forkLocationRow);

    m_repoMirrorLocation = new QLabel;
    m_repoMirrorLocation->setObjectName("statusLine");
    m_repoMirrorLocation->setWordWrap(true);
    m_repoMirrorLocation->setTextInteractionFlags(Qt::TextSelectableByMouse);
    gitCol->addWidget(m_repoMirrorLocation);

    auto *remotesHeading = new QLabel("Git remotes");
    remotesHeading->setObjectName("sectionLabel");
    gitCol->addWidget(remotesHeading);

    m_repoRemotesTable = new QTableWidget(0, 3);
    installColumnHeaderMenu(m_repoRemotesTable);
    m_repoRemotesTable->setHorizontalHeaderLabels({"Name", "Fetch URL", "Push URL"});
    m_repoRemotesTable->horizontalHeader()->setStretchLastSection(true);
    m_repoRemotesTable->verticalHeader()->setVisible(false);
    m_repoRemotesTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_repoRemotesTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_repoRemotesTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_repoRemotesTable->setMaximumHeight(150);
    makeColumnsResizable(m_repoRemotesTable);
    gitCol->addWidget(m_repoRemotesTable);

    auto *remoteAddButton = new QPushButton("Add");
    auto *remoteEditButton = new QPushButton("Edit");
    auto *remoteDeleteButton = new QPushButton("Delete");
    for (QPushButton *b : {remoteAddButton, remoteEditButton, remoteDeleteButton}) {
        b->setProperty("buttonSize", "sm");
        b->setCursor(Qt::PointingHandCursor);
    }
    connect(remoteAddButton, &QPushButton::clicked, this,
            &MainWindow::promptAddRepoRemote);
    connect(remoteEditButton, &QPushButton::clicked, this,
            &MainWindow::promptEditRepoRemote);
    connect(remoteDeleteButton, &QPushButton::clicked, this,
            &MainWindow::deleteSelectedRepoRemote);
    connect(m_repoRemotesTable, &QTableWidget::cellDoubleClicked, this,
            [this](int, int) { promptEditRepoRemote(); });
    auto *remoteButtonRow = new QHBoxLayout;
    remoteButtonRow->setContentsMargins(0, 0, 0, 0);
    remoteButtonRow->addWidget(remoteAddButton);
    remoteButtonRow->addWidget(remoteEditButton);
    remoteButtonRow->addWidget(remoteDeleteButton);
    remoteButtonRow->addStretch();
    gitCol->addLayout(remoteButtonRow);

    auto *gitIdentityBtn = new QPushButton(
        QString::fromUtf8("Set Git identity\xE2\x80\xA6"));
    gitIdentityBtn->setProperty("buttonSize", "sm");
    gitIdentityBtn->setCursor(Qt::PointingHandCursor);
    gitIdentityBtn->setToolTip(
        "Review and set this repository's local git user.name and user.email");
    connect(gitIdentityBtn, &QPushButton::clicked, this,
            &MainWindow::setRepoGitIdentityFromForkMesh);
    auto *gitIdentityRow = new QHBoxLayout;
    gitIdentityRow->setContentsMargins(0, 0, 0, 0);
    gitIdentityRow->addWidget(gitIdentityBtn);
    gitIdentityRow->addStretch();
    gitCol->addLayout(gitIdentityRow);

    m_repoStatsFileCheck = new QCheckBox(
        QStringLiteral("Write .forkmesh/stats/repository.json"));
    m_repoStatsFileCheck->setCursor(Qt::PointingHandCursor);
    m_repoStatsFileCheck->setToolTip(QStringLiteral(
        "Persist daily repository statistics inside the working tree. Disabled "
        "by default; viewing repository statistics never enables it."));
    connect(m_repoStatsFileCheck, &QCheckBox::toggled, this, [this](bool enabled) {
        if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
            return;
        const QString dir = m_repositories.at(m_repoDetailIndex).localPath.trimmed();
        QString error;
        if (dir.isEmpty() ||
            !RepoStatsStore::setRepositoryJsonEnabled(dir, enabled, &error)) {
            QSignalBlocker blocker(m_repoStatsFileCheck);
            m_repoStatsFileCheck->setChecked(!enabled);
            flashMessage(QStringLiteral("Could not update repository stats setting: %1")
                             .arg(error),
                         true);
            return;
        }
        if (enabled &&
            RepoStatsStore::captureDaily(dir, &error).isEmpty() &&
            !error.isEmpty()) {
            flashMessage(QStringLiteral("Stats enabled, but repository.json could not be written: %1")
                             .arg(error),
                         true);
            return;
        }
        flashMessage(enabled
                         ? QStringLiteral("repository.json generation enabled.")
                         : QStringLiteral("repository.json generation disabled."));
    });
    gitCol->addWidget(m_repoStatsFileCheck);

    gitCol->addStretch();
    addTab(gitTab, "Git");

    auto *agentsTab = new QWidget;
    auto *agentsCol = new QVBoxLayout(agentsTab);
    agentsCol->setContentsMargins(2, 12, 2, 12);
    agentsCol->setSpacing(8);

    auto *guardrailsHeading = new QLabel("Agent filesystem guardrails");
    guardrailsHeading->setObjectName("sectionLabel");
    agentsCol->addWidget(guardrailsHeading);

    auto *guardrailsIntro = new QLabel(
        "Limit every AI agent for this repository to an explicit set of "
        "repository-relative directories. ForkMesh checks structured tool "
        "calls and confines the provider process, shell commands, and all of "
        "their subprocesses to the same operating-system-enforced allowlist. "
        "If a tool crosses it, ForkMesh blocks the call and shows an Approve "
        "or Deny alert.");
    guardrailsIntro->setObjectName("statusLine");
    guardrailsIntro->setWordWrap(true);
    agentsCol->addWidget(guardrailsIntro);

    m_agentGuardrailsCheck =
        new QCheckBox("Enable guardrails for this repository");
    m_agentGuardrailsCheck->setObjectName("repoAgentGuardrailsCheck");
    m_agentGuardrailsCheck->setCursor(Qt::PointingHandCursor);
    m_agentGuardrailsCheck->setToolTip(
        "Fail closed before an agent starts if the host cannot enforce the "
        "allowlist or if an allowed directory is missing or escapes through a "
        "symbolic link.");
    connect(m_agentGuardrailsCheck, &QCheckBox::toggled, this,
            [this](bool enabled) {
                if (m_repoDetailIndex < 0 ||
                    m_repoDetailIndex >= m_repositories.size())
                    return;
                RepositoryRecord &repo = m_repositories[m_repoDetailIndex];
                if (repo.agentGuardrailsEnabled == enabled)
                    return;
                repo.agentGuardrailsEnabled = enabled;
                // The first enable establishes the useful least-privilege
                // baseline (repository only, never host HOME). The owner can
                // remove '.' and add narrower subdirectories immediately.
                if (enabled && repo.agentAllowedDirectories.isEmpty())
                    repo.agentAllowedDirectories = {QStringLiteral(".")};
                saveRepositories();
                const int stopped =
                    enabled
                        ? stopRepositoryAgentsForGuardrailChange(repo.owner,
                                                                 repo.name)
                        : 0;
#ifdef FORKMESH_WINDOW_TESTS
                setProperty("forkmeshTestGuardrailStoppedCount", stopped);
#endif
                logSystem(QStringLiteral("Agent directory guardrails %1 for %2/%3.")
                              .arg(enabled ? QStringLiteral("enabled")
                                           : QStringLiteral("disabled"),
                                   repo.owner, repo.name));
                if (stopped > 0)
                    flashMessage(
                        QStringLiteral(
                            "Stopped %1 active agent process%2 so the new "
                            "repository boundary takes effect immediately.")
                            .arg(stopped)
                            .arg(stopped == 1 ? QString()
                                              : QStringLiteral("s")));
                scheduleAgentGuardrailRefresh();
                refreshRepoSettings();
            });
    agentsCol->addWidget(m_agentGuardrailsCheck);

    auto *allowlistLabel = new QLabel("Allowed directories");
    allowlistLabel->setObjectName("sectionLabel");
    agentsCol->addWidget(allowlistLabel);

    auto *allowlistIntro = new QLabel(
        "Check every directory agents may read and change. Common source, "
        "test, and documentation folders are listed first and marked "
        "Recommended; dependencies, build output, CI configuration, and "
        "secrets are labeled so they can stay blocked. Changes save "
        "immediately.");
    allowlistIntro->setObjectName("statusLine");
    allowlistIntro->setWordWrap(true);
    agentsCol->addWidget(allowlistIntro);

    m_agentAllowedDirectoriesList = new QListWidget;
    m_agentAllowedDirectoriesList->setObjectName(
        "repoAgentAllowedDirectoriesList");
    m_agentAllowedDirectoriesList->setSelectionMode(
        QAbstractItemView::NoSelection);
    m_agentAllowedDirectoriesList->setMinimumHeight(180);
    m_agentAllowedDirectoriesList->setMaximumHeight(320);
    connect(m_agentAllowedDirectoriesList, &QListWidget::itemChanged, this,
            [this](QListWidgetItem *changed) {
                if (!changed || !m_agentAllowedDirectoriesList)
                    return;
                // "Whole repository" wins; otherwise every checked row counts,
                // including rows it covered, so unchecking it keeps what the
                // list showed as checked.
                QStringList directories;
                for (int i = 0; i < m_agentAllowedDirectoriesList->count(); ++i) {
                    const QListWidgetItem *item =
                        m_agentAllowedDirectoriesList->item(i);
                    const QString path = item->data(Qt::UserRole).toString();
                    if (path == QLatin1String(".")) {
                        if (item->checkState() == Qt::Checked) {
                            directories = {path};
                            break;
                        }
                        continue;
                    }
                    if (item->checkState() == Qt::Checked)
                        directories.append(path);
                }
                // Saving rebuilds the list, which would delete the item this
                // signal is still delivering.
                QTimer::singleShot(0, this, [this, directories] {
                    setRepoAgentAllowedDirectories(directories);
                });
            });
    agentsCol->addWidget(m_agentAllowedDirectoriesList);

    auto *addDirectory = new QPushButton("Add directory\xE2\x80\xA6");
    addDirectory->setObjectName("repoAgentGuardrailAddButton");
    m_agentGuardrailRecommendedButton = new QPushButton("Recommended");
    m_agentGuardrailRecommendedButton->setObjectName(
        "repoAgentGuardrailRecommendedButton");
    m_agentGuardrailRecommendedButton->setToolTip(
        "Allow only the common source, test, and documentation folders found "
        "in this checkout.");
    auto *selectAll = new QPushButton("Select all");
    selectAll->setObjectName("repoAgentGuardrailSelectAllButton");
    auto *clearAll = new QPushButton("Clear");
    clearAll->setObjectName("repoAgentGuardrailClearButton");
    for (QPushButton *button : {m_agentGuardrailRecommendedButton, selectAll,
                                clearAll, addDirectory}) {
        button->setProperty("buttonSize", "sm");
        button->setCursor(Qt::PointingHandCursor);
    }
    connect(m_agentGuardrailRecommendedButton, &QPushButton::clicked, this,
            [this] {
                if (m_repoDetailIndex < 0 ||
                    m_repoDetailIndex >= m_repositories.size())
                    return;
                setRepoAgentAllowedDirectories(
                    forkmesh::agentguard::recommendedDirectories(
                        m_repositories.at(m_repoDetailIndex).localPath.trimmed()));
            });
    connect(selectAll, &QPushButton::clicked, this, [this] {
        setRepoAgentAllowedDirectories({QStringLiteral(".")});
    });
    connect(clearAll, &QPushButton::clicked, this, [this] {
        setRepoAgentAllowedDirectories({});
    });
    connect(addDirectory, &QPushButton::clicked, this, [this] {
        if (m_repoDetailIndex < 0 ||
            m_repoDetailIndex >= m_repositories.size())
            return;
        RepositoryRecord &repo = m_repositories[m_repoDetailIndex];
        const QString root = repo.localPath.trimmed();
        if (root.isEmpty() || !QFileInfo(root).isDir()) {
            QMessageBox::information(
                this, "Agent guardrails",
                "Set a local fork location before choosing allowed directories.");
            return;
        }
        const QString selected = QFileDialog::getExistingDirectory(
            this, "Choose an agent-accessible directory", root,
            QFileDialog::ShowDirsOnly);
        if (selected.isEmpty())
            return;
        if (!forkmesh::agentguard::pathAllowed(
                root, {QStringLiteral(".")}, selected)) {
            QMessageBox::warning(
                this, "Agent guardrails",
                "Choose a directory inside this repository. Symbolic links "
                "that lead outside it cannot be allowed.");
            return;
        }
        const QString rootCanonical = QFileInfo(root).canonicalFilePath();
        const QString selectedCanonical = QFileInfo(selected).canonicalFilePath();
        const QString relative =
            rootCanonical == selectedCanonical
                ? QStringLiteral(".")
                : QDir(rootCanonical).relativeFilePath(selectedCanonical);
        setRepoAgentAllowedDirectories(repo.agentAllowedDirectories +
                                       QStringList{relative});
    });
    auto *guardrailButtons = new QHBoxLayout;
    guardrailButtons->setContentsMargins(0, 0, 0, 0);
    guardrailButtons->addWidget(m_agentGuardrailRecommendedButton);
    guardrailButtons->addWidget(selectAll);
    guardrailButtons->addWidget(clearAll);
    guardrailButtons->addWidget(addDirectory);
    guardrailButtons->addStretch();
    agentsCol->addLayout(guardrailButtons);

    auto *summaryLabel = new QLabel("Policy summary");
    summaryLabel->setObjectName("sectionLabel");
    agentsCol->addWidget(summaryLabel);

    m_agentGuardrailHint = new QLabel;
    m_agentGuardrailHint->setObjectName("repoAgentGuardrailHint");
    m_agentGuardrailHint->setWordWrap(true);
    agentsCol->addWidget(m_agentGuardrailHint);
    agentsCol->addStretch();
    addTab(agentsTab, "Agents");

    auto *automationTab = new QWidget;
    auto *automationCol = new QVBoxLayout(automationTab);
    automationCol->setContentsMargins(2, 12, 2, 12);
    automationCol->setSpacing(8);

    auto *actionsHeading = new QLabel("Actions");
    actionsHeading->setObjectName("sectionLabel");
    automationCol->addWidget(actionsHeading);

    m_settingsActionsCheck = new QCheckBox("Run actions on push");
    m_settingsActionsCheck->setCursor(Qt::PointingHandCursor);
    m_settingsActionsCheck->setToolTip(
        "When a fork pushes to this repo's local mirror, run its .forkmesh/ "
        "workflows. Off by default for mirrored repos. Changed workflows still "
        "require your approval unless automatic approval is enabled below.");
    connect(m_settingsActionsCheck, &QCheckBox::toggled, this,
            [this](bool on) { setRepoActionsEnabled(on); });
    automationCol->addWidget(m_settingsActionsCheck);

    m_settingsAutoApproveCheck =
        new QCheckBox("Automatically approve workflow runs");
    m_settingsAutoApproveCheck->setCursor(Qt::PointingHandCursor);
    m_settingsAutoApproveCheck->setToolTip(
        "Approve the exact pushed repository snapshot and start the workflow "
        "without waiting for an approval click. Turn this off to review each "
        "new snapshot before it can run.");
    connect(m_settingsAutoApproveCheck, &QCheckBox::toggled, this,
            [this](bool on) { setRepoActionsAutoApprove(on); });
    automationCol->addWidget(m_settingsAutoApproveCheck);

    auto *actionsHint = new QLabel(
        "Mirrored repositories start with actions disabled. Enable this only for "
        "repos whose workflows you trust to run on this machine.");
    actionsHint->setObjectName("statusLine");
    actionsHint->setWordWrap(true);
    automationCol->addWidget(actionsHint);

    automationCol->addSpacing(10);

    auto *pullHeading = new QLabel("Pull requests");
    pullHeading->setObjectName("sectionLabel");
    automationCol->addWidget(pullHeading);

    m_settingsRequirePeerApprovalCheck =
        new QCheckBox("Require peer approval before merge");
    m_settingsRequirePeerApprovalCheck->setObjectName(
        QStringLiteral("repoRequirePeerApprovalCheck"));
    m_settingsRequirePeerApprovalCheck->setCursor(Qt::PointingHandCursor);
    m_settingsRequirePeerApprovalCheck->setToolTip(
        "Require at least one approval from someone other than the pull-request "
        "author, with no unresolved request for changes. Turn this off when "
        "peer review is optional for this repository.");
    connect(m_settingsRequirePeerApprovalCheck, &QCheckBox::toggled, this,
            [this](bool on) { setRepoRequirePeerApproval(on); });
    automationCol->addWidget(m_settingsRequirePeerApprovalCheck);

    auto *pullHint = new QLabel(
        "When disabled, pull requests can merge without a peer review. Conflicts "
        "and the repository's other merge safety checks still apply.");
    pullHint->setObjectName("statusLine");
    pullHint->setWordWrap(true);
    automationCol->addWidget(pullHint);

    m_settingsMergeQueueCheck = new QCheckBox("Enable the merge queue");
    m_settingsMergeQueueCheck->setObjectName(
        QStringLiteral("repoMergeQueueCheck"));
    m_settingsMergeQueueCheck->setCursor(Qt::PointingHandCursor);
    m_settingsMergeQueueCheck->setToolTip(
        "Let pull requests be handed to a queue instead of merged by hand. "
        "ForkMesh works through the queue in order: each pull request is brought "
        "up to date with the base branch first, then merged, so they land one "
        "after the next even though every merge moves the base.");
    connect(m_settingsMergeQueueCheck, &QCheckBox::toggled, this,
            [this](bool on) { setRepoMergeQueueEnabled(on); });
    automationCol->addWidget(m_settingsMergeQueueCheck);

    auto *mergeQueueHint = new QLabel(
        "The queue appears under the pull-request list, where it can be "
        "reordered, paused, and have entries taken back out. It keeps every "
        "other merge rule: a pull request that conflicts, or that still needs "
        "the peer approval required above, waits in the queue instead of "
        "merging. The queue advances while this repository is open.");
    mergeQueueHint->setObjectName("statusLine");
    mergeQueueHint->setWordWrap(true);
    automationCol->addWidget(mergeQueueHint);

    automationCol->addSpacing(10);

    // --- Secret scanning --------------------------------------------------
    auto *secretHeading = new QLabel("Secret scanning");
    secretHeading->setObjectName("sectionLabel");
    automationCol->addWidget(secretHeading);

    m_secretScanCheck = new QCheckBox("Block push if secrets are detected");
    m_secretScanCheck->setCursor(Qt::PointingHandCursor);
    m_secretScanCheck->setToolTip(
        "Before each push ForkMesh scans the commits being pushed for API keys, "
        "private keys, and other high-confidence secrets. If any are found you "
        "will be warned and can cancel or push anyway.");
    connect(m_secretScanCheck, &QCheckBox::toggled, this,
            [this](bool on) { setRepoSecretScanningEnabled(on); });
    automationCol->addWidget(m_secretScanCheck);

    auto *secretHint = new QLabel(
        "Detects GitHub tokens, AWS access keys, Slack tokens, and PEM private "
        "keys. Rotate any exposed credentials immediately.");
    secretHint->setObjectName("statusLine");
    secretHint->setWordWrap(true);
    automationCol->addWidget(secretHint);

    automationCol->addStretch();
    addTab(automationTab, "Automation");

    auto *covesTab = new QWidget;
    auto *covesCol = new QVBoxLayout(covesTab);
    covesCol->setContentsMargins(2, 12, 2, 12);
    covesCol->setSpacing(8);
    covesCol->addWidget(buildCoveSection());
    covesCol->addStretch();
    addTab(covesTab, "Coves");

    auto *dangerTab = new QWidget;
    auto *dangerCol = new QVBoxLayout(dangerTab);
    dangerCol->setContentsMargins(2, 12, 2, 12);
    dangerCol->setSpacing(8);

    auto *dangerHeading = new QLabel("Danger zone");
    dangerHeading->setObjectName("sectionLabel");
    dangerCol->addWidget(dangerHeading);

    auto *deleteHint = new QLabel(
        "Delete this repository from this machine completely. Your working "
        "directory, if any, is kept on disk but no longer tracked by "
        "ForkMesh. Published repos are also removed from the public "
        "ForkMesh catalog, freeing up the name for reuse. You can also "
        "choose to keep the mirror files on disk and just untrack the "
        "repository from ForkMesh.");
    deleteHint->setObjectName("statusLine");
    deleteHint->setWordWrap(true);
    dangerCol->addWidget(deleteHint);

    auto *deleteBtn = new QPushButton("Delete repository");
    deleteBtn->setObjectName("dangerButton");
    deleteBtn->setProperty("buttonSize", "sm");
    deleteBtn->setCursor(Qt::PointingHandCursor);
    setOcticon(deleteBtn, "trash", 16);
    connect(deleteBtn, &QPushButton::clicked, this, &MainWindow::deleteCurrentMirror);
    auto *deleteRow = new QHBoxLayout;
    deleteRow->addWidget(deleteBtn);
    deleteRow->addStretch();
    dangerCol->addLayout(deleteRow);

    dangerCol->addStretch();
    addTab(dangerTab, "Danger zone");

    return page;
}

void MainWindow::setRepoActionsEnabled(bool on)
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    if (m_repositories[m_repoDetailIndex].actionsEnabled == on)
        return;
    m_repositories[m_repoDetailIndex].actionsEnabled = on;
    saveRepositories();
    ensurePushHook(m_repositories.at(m_repoDetailIndex));
    logSystem(QStringLiteral("Actions %1 for %2/%3.")
                  .arg(on ? "enabled" : "disabled",
                       m_repositories.at(m_repoDetailIndex).owner,
                       m_repositories.at(m_repoDetailIndex).name));
    if (m_actionsEnabledCheck) {
        QSignalBlocker block(m_actionsEnabledCheck);
        m_actionsEnabledCheck->setChecked(on);
    }
    if (m_settingsActionsCheck) {
        QSignalBlocker block(m_settingsActionsCheck);
        m_settingsActionsCheck->setChecked(on);
    }
}

void MainWindow::setRepoActionsAutoApprove(bool on)
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    if (m_repositories[m_repoDetailIndex].actionsAutoApprove == on)
        return;
    m_repositories[m_repoDetailIndex].actionsAutoApprove = on;
    saveRepositories();
    const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
    logSystem(QStringLiteral("Actions automatic approval %1 for %2/%3.")
                  .arg(on ? "enabled" : "disabled", repo.owner, repo.name));
    if (m_actionsAutoApproveCheck) {
        QSignalBlocker block(m_actionsAutoApproveCheck);
        m_actionsAutoApproveCheck->setChecked(on);
    }
    if (m_settingsAutoApproveCheck) {
        QSignalBlocker block(m_settingsAutoApproveCheck);
        m_settingsAutoApproveCheck->setChecked(on);
    }
    if (!on)
        return;

    const int resumed = autoApproveAwaitingRuns(repo);
    if (resumed == 0)
        return;
    logSystem(QStringLiteral("Actions: queued %1 automatically approved pending "
                             "run%2 for %3/%4.")
                  .arg(resumed)
                  .arg(resumed == 1 ? QString() : QStringLiteral("s"),
                       repo.owner, repo.name));
    refreshActionsTable();
    updateNotificationButton();
    processActionQueue();
}

void MainWindow::setRepoRequirePeerApproval(bool on)
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    if (m_repositories[m_repoDetailIndex].requirePeerApproval == on)
        return;
    m_repositories[m_repoDetailIndex].requirePeerApproval = on;
    saveRepositories();
    const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
    logSystem(QStringLiteral("Peer approval before merge %1 for %2/%3.")
                  .arg(on ? "required" : "optional", repo.owner, repo.name));
    updatePullActionState();
}

void MainWindow::setRepoSecretScanningEnabled(bool on)
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    if (m_repositories[m_repoDetailIndex].secretScanningEnabled == on)
        return;
    m_repositories[m_repoDetailIndex].secretScanningEnabled = on;
    saveRepositories();
    logSystem(QStringLiteral("Secret scanning push protection %1 for %2/%3.")
                  .arg(on ? "enabled" : "disabled",
                       m_repositories.at(m_repoDetailIndex).owner,
                       m_repositories.at(m_repoDetailIndex).name));
    if (m_secretScanCheck) {
        QSignalBlocker block(m_secretScanCheck);
        m_secretScanCheck->setChecked(on);
    }
}

bool MainWindow::isWorkflowDisabled(const QString &path) const
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return false;
    return m_repositories.at(m_repoDetailIndex).disabledWorkflows.contains(path);
}

void MainWindow::promptSetRepoForkLocation()
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;

    const RepositoryRecord current = m_repositories.at(m_repoDetailIndex);
    const QString startDir = current.localPath.trimmed().isEmpty()
                                 ? QDir::homePath()
                                 : current.localPath.trimmed();
    const QString path = QFileDialog::getExistingDirectory(
        this, "Choose existing fork location", startDir);
    if (path.isEmpty())
        return;

    const bool looksLikeGit =
        QDir(path).exists(QStringLiteral(".git")) ||
        QDir(path).exists(QStringLiteral("HEAD"));
    if (!looksLikeGit) {
        QMessageBox::warning(
            this, "Fork location",
            "That folder is not a Git working copy. Choose a folder created by "
            "\"git clone\" or \"git init\".");
        return;
    }

    const QString cleanPath = QDir::cleanPath(path);
    const QString mirrorPath =
        QDir::cleanPath(m_repositories.at(m_repoDetailIndex).mirrorPath.trimmed());
    if (!mirrorPath.isEmpty() && cleanPath == mirrorPath) {
        QMessageBox::warning(
            this, "Fork location",
            "The fork location cannot be the same folder as the bare mirror.");
        return;
    }

    RepositoryRecord &repo = m_repositories[m_repoDetailIndex];
    if (QDir::cleanPath(repo.localPath.trimmed()) == cleanPath) {
        refreshRepoSettings();
        return;
    }
    repo.localPath = cleanPath;
    saveRepositories();
    ensurePushHook(repo);
    refreshRepositoryList();
    refreshRepoSettings();
    refreshSourceControl();
    logSystem(QStringLiteral("Fork location for %1/%2 set to %3.")
                  .arg(repo.owner, repo.name, cleanPath));
    syncRepository(m_repoDetailIndex);
}

void MainWindow::setRepoGitIdentityFromForkMesh()
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
    const QString dir = repo.localPath.trimmed();
    if (dir.isEmpty() || !QDir(dir).exists(QStringLiteral(".git"))) {
        QMessageBox::warning(
            this, "Git identity",
            "Set a local fork location first. Git identity is stored in the "
            "working copy's local config.");
        return;
    }

    if (!promptAndSetGitIdentity(this, dir, settingsAccountName().trimmed()))
        return;

    updateFooterGitIdentity();
    logSystem(QStringLiteral("Git identity for %1/%2 was explicitly confirmed.")
                  .arg(repo.owner, repo.name));
    flashMessage(QStringLiteral("Repository Git identity updated."));
}

void MainWindow::setWorkflowDisabled(const QString &path, bool disabled)
{
    setRepoWorkflowDisabled(m_repoDetailIndex, path, disabled);
}

void MainWindow::setRepoWorkflowDisabled(int index, const QString &path, bool disabled)
{
    if (path.isEmpty() || index < 0 || index >= m_repositories.size())
        return;
    QStringList &off = m_repositories[index].disabledWorkflows;
    if (disabled == off.contains(path))
        return;
    if (disabled)
        off.append(path);
    else
        off.removeAll(path);
    saveRepositories();
    logSystem(QStringLiteral("Actions: workflow %1 %2 for %3/%4.")
                  .arg(path, disabled ? QStringLiteral("disabled")
                                      : QStringLiteral("enabled"),
                       m_repositories.at(index).owner,
                       m_repositories.at(index).name));
    ringLocalControlRepos();
    if (index == m_repoDetailIndex)
        updateManualRunBar();
}

void MainWindow::refreshRepoSettings()
{
    rebuildRepoCovesList();
    const bool haveRepo =
        m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size();
    if (m_repoPrivateCheck) {
        QSignalBlocker block(m_repoPrivateCheck);
        m_repoPrivateCheck->setEnabled(haveRepo);
        m_repoPrivateCheck->setChecked(
            haveRepo && m_repositories.at(m_repoDetailIndex).isPrivate);
    }
    if (m_repoServeCheck) {
        QSignalBlocker block(m_repoServeCheck);
        const RepositoryRecord blank;
        const RepositoryRecord &current =
            haveRepo ? m_repositories.at(m_repoDetailIndex) : blank;
        // Serving is an owner-signed transition, and a cached preview has
        // nothing of its own to publish.
        m_repoServeCheck->setEnabled(
            haveRepo && !current.previewOnly &&
            hasOwnerSigningCapability(catalogOwner(current)));
        m_repoServeCheck->setChecked(haveRepo && current.publishToNetwork);
    }
    if (m_repoStatsFileCheck) {
        QSignalBlocker block(m_repoStatsFileCheck);
        const QString dir = haveRepo
                                ? m_repositories.at(m_repoDetailIndex).localPath.trimmed()
                                : QString();
        m_repoStatsFileCheck->setEnabled(!dir.isEmpty());
        m_repoStatsFileCheck->setChecked(
            !dir.isEmpty() && RepoStatsStore::repositoryJsonEnabled(dir));
    }
    if (m_settingsActionsCheck) {
        QSignalBlocker block(m_settingsActionsCheck);
        m_settingsActionsCheck->setEnabled(haveRepo);
        m_settingsActionsCheck->setChecked(
            haveRepo && m_repositories.at(m_repoDetailIndex).actionsEnabled);
    }
    if (m_settingsAutoApproveCheck) {
        QSignalBlocker block(m_settingsAutoApproveCheck);
        m_settingsAutoApproveCheck->setEnabled(haveRepo);
        m_settingsAutoApproveCheck->setChecked(
            haveRepo && m_repositories.at(m_repoDetailIndex).actionsAutoApprove);
    }
    if (m_agentGuardrailsCheck) {
        QSignalBlocker block(m_agentGuardrailsCheck);
        m_agentGuardrailsCheck->setEnabled(haveRepo);
        m_agentGuardrailsCheck->setChecked(
            haveRepo &&
            m_repositories.at(m_repoDetailIndex).agentGuardrailsEnabled);
    }
    if (m_agentAllowedDirectoriesList) {
        QSignalBlocker block(m_agentAllowedDirectoriesList);
        m_agentAllowedDirectoriesList->clear();
        if (haveRepo) {
            const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
            const QList<forkmesh::agentguard::DirectoryChoice> choices =
                forkmesh::agentguard::directoryChoices(
                    repo.localPath.trimmed(), repo.agentAllowedDirectories);
            const bool wholeRepository =
                !choices.isEmpty() && choices.first().allowed;
            for (const forkmesh::agentguard::DirectoryChoice &choice : choices) {
                const bool root = choice.path == QLatin1String(".");
                QStringList details;
                if (root) {
                    details.append(QStringLiteral(
                        "includes root files such as README and build files"));
                } else {
                    if (!choice.description.isEmpty())
                        details.append(choice.description);
                    if (choice.recommended)
                        details.append(QStringLiteral("Recommended"));
                    if (!choice.exists)
                        details.append(QStringLiteral("missing"));
                }
                auto *item = new QListWidgetItem(
                    (root ? QStringLiteral("Whole repository") : choice.path) +
                    QStringLiteral(" · ") + details.join(QStringLiteral(" · ")));
                item->setData(Qt::UserRole, choice.path);
                item->setToolTip(choice.description.isEmpty() ? choice.path
                                                              : choice.description);
                Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsUserCheckable;
                // Rows the whole repository already covers read as checked but
                // cannot be toggled on their own.
                if (wholeRepository && !root)
                    flags &= ~Qt::ItemIsEnabled;
                item->setFlags(flags);
                item->setCheckState(choice.allowed ? Qt::Checked : Qt::Unchecked);
                m_agentAllowedDirectoriesList->addItem(item);
            }
        }
        m_agentAllowedDirectoriesList->setEnabled(haveRepo);
    }
    if (m_agentGuardrailRecommendedButton)
        m_agentGuardrailRecommendedButton->setEnabled(
            haveRepo &&
            QFileInfo(m_repositories.at(m_repoDetailIndex).localPath.trimmed())
                .isDir());
    if (m_agentGuardrailHint) {
        if (!haveRepo) {
            m_agentGuardrailHint->clear();
        } else {
            const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
            const QStringList &allowed = repo.agentAllowedDirectories;
            QStringList lines;
            if (!repo.agentGuardrailsEnabled)
                lines.append(QStringLiteral(
                    "Status: Off. Agents keep their normal provider sandbox; "
                    "this selection is saved but not applied."));
            else if (allowed.isEmpty())
                lines.append(QStringLiteral(
                    "Status: Nothing allowed. ForkMesh refuses to start agents "
                    "for this repository until a directory is checked."));
            else
                lines.append(QStringLiteral(
                    "Status: Active for every agent working in this repository."));
            lines.append(QStringLiteral("Allowed: %1.").arg(
                forkmesh::agentguard::policyDescription(allowed)));
            if (!allowed.isEmpty() && !allowed.contains(QStringLiteral(".")))
                lines.append(QStringLiteral(
                    "Not covered: files at the repository root, such as README "
                    "and build files."));
            const QString root = repo.localPath.trimmed();
            QStringList missing;
            if (QFileInfo(root).isDir())
                for (const QString &path : allowed)
                    if (path != QLatin1String(".") &&
                        !QFileInfo(QDir(root).filePath(path)).isDir())
                        missing.append(path);
            if (!missing.isEmpty())
                lines.append(QStringLiteral(
                                 "Missing: %1 (agents will not start until it "
                                 "exists).")
                                 .arg(missing.join(QStringLiteral(", "))));
            lines.append(QStringLiteral(
                "Always available: Git objects, refs, and index (hooks and "
                "config stay read-only), provider login state, private scratch "
                "space, and read-only system runtime files."));
            lines.append(QStringLiteral(
                "Enforcement: the operating system confines the agent and every "
                "subprocess; a blocked tool call opens an Approve or Deny "
                "alert."));
            m_agentGuardrailHint->setText(lines.join(QLatin1Char('\n')));
        }
    }
    if (m_settingsRequirePeerApprovalCheck) {
        QSignalBlocker block(m_settingsRequirePeerApprovalCheck);
        m_settingsRequirePeerApprovalCheck->setEnabled(haveRepo);
        m_settingsRequirePeerApprovalCheck->setChecked(
            !haveRepo ||
            m_repositories.at(m_repoDetailIndex).requirePeerApproval);
    }
    if (m_settingsMergeQueueCheck) {
        QSignalBlocker block(m_settingsMergeQueueCheck);
        m_settingsMergeQueueCheck->setEnabled(haveRepo);
        m_settingsMergeQueueCheck->setChecked(
            haveRepo && m_repositories.at(m_repoDetailIndex).mergeQueueEnabled);
    }
    if (m_secretScanCheck) {
        QSignalBlocker block(m_secretScanCheck);
        m_secretScanCheck->setEnabled(haveRepo);
        m_secretScanCheck->setChecked(
            !haveRepo || m_repositories.at(m_repoDetailIndex).secretScanningEnabled);
    }
    refreshRepoCollaborators();
    if (m_repoSourceEdit) {
        QSignalBlocker block(m_repoSourceEdit);
        m_repoSourceEdit->setEnabled(haveRepo);
        m_repoSourceEdit->setText(
            haveRepo ? m_repositories.at(m_repoDetailIndex).cloneUrl.trimmed()
                     : QString());
    }
    if (m_repoSourceHint) {
        if (!haveRepo) {
            m_repoSourceHint->clear();
        } else {
            const RepositoryRecord &r = m_repositories.at(m_repoDetailIndex);
            if (!r.localPath.trimmed().isEmpty())
                m_repoSourceHint->setText(
                    "This repo is backed by a local working copy at " +
                    r.localPath.trimmed() +
                    "; syncs read from there. The clone URL above is the "
                    "published/fork location.");
            else if (r.cloneUrl.trimmed().isEmpty())
                m_repoSourceHint->setText(
                    "No upstream set — this machine hosts the repo directly.");
            else
                m_repoSourceHint->setText(
                    "The mirror fetches from this URL. Update it to repoint the "
                    "fork at a different node, then sync to pull from it.");
        }
    }
    if (m_repoForkLocation) {
        if (!haveRepo) {
            m_repoForkLocation->clear();
        } else {
            const QString path =
                m_repositories.at(m_repoDetailIndex).localPath.trimmed();
            m_repoForkLocation->setText(
                QStringLiteral("Fork location: %1")
                    .arg(path.isEmpty()
                             ? QStringLiteral("No local working copy")
                             : QDir::toNativeSeparators(path)));
        }
    }
    if (m_repoMirrorLocation) {
        if (!haveRepo) {
            m_repoMirrorLocation->clear();
        } else {
            const QString path =
                m_repositories.at(m_repoDetailIndex).mirrorPath.trimmed();
            m_repoMirrorLocation->setText(
                QStringLiteral("Mirror location: %1")
                    .arg(path.isEmpty()
                             ? QStringLiteral("Mirror has not been created yet")
                             : QDir::toNativeSeparators(path)));
        }
    }
    reloadRepoRemotesTable();
    if (!m_repoVisibilityHint)
        return;
    if (!haveRepo) {
        m_repoVisibilityHint->clear();
        return;
    }
    const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
    if (!repo.publishToNetwork)
        m_repoVisibilityHint->setText(
            "Local only \xE2\x80\x94 nothing about this repository leaves this "
            "machine, not even its name in a request URL. The visibility "
            "choice above applies once you serve it on the network.");
    else if (repo.isPrivate)
        m_repoVisibilityHint->setText(
            "Private: hidden from the public catalog. Only this machine's key "
            "can browse or clone it through the mainnode.");
    else
        m_repoVisibilityHint->setText(
            "Public: listed in the catalog and anyone can browse or clone it "
            "through the mainnode.");
}

void MainWindow::updateRepoSource()
{
    if (!m_repoSourceEdit)
        return;
    applyRepositorySource(m_repoDetailIndex, m_repoSourceEdit->text().trimmed());
}

void MainWindow::applyRepositorySource(int index, const QString &newUrl)
{
    if (index < 0 || index >= m_repositories.size())
        return;
    RepositoryRecord &repo = m_repositories[index];
    const bool sourceChanged = newUrl != repo.cloneUrl.trimmed();
    if (sourceChanged) {
        repo.cloneUrl = newUrl;
        saveRepositories();
    }
    if (!repo.localPath.trimmed().isEmpty() && !newUrl.isEmpty())
        configureForkSource(index, newUrl);
    bool mirrorReattached = false;
    if (repo.localPath.trimmed().isEmpty() && !newUrl.isEmpty() &&
        !repo.mirrorPath.isEmpty() && QDir(repo.mirrorPath).exists()) {
        mirrorReattached = runGitCapture(
            repo.mirrorPath,
            {QStringLiteral("remote"), QStringLiteral("set-url"),
             QStringLiteral("origin"), newUrl}, nullptr, nullptr);
        if (!mirrorReattached) {
            mirrorReattached = runGitCapture(
                repo.mirrorPath,
                {QStringLiteral("remote"), QStringLiteral("add"),
                 QStringLiteral("origin"), newUrl}, nullptr, nullptr);
        }
    }
    logSystem(QStringLiteral("Source for %1/%2 %3 %4.")
                  .arg(repo.owner, repo.name,
                       sourceChanged ? QStringLiteral("set to")
                                     : QStringLiteral("reattached to"),
                       newUrl.isEmpty() ? QStringLiteral("(none)") : newUrl));
    if (!sourceChanged && !mirrorReattached)
        logSystem(QStringLiteral("Source reattach did not change the local "
                                 "mirror configuration."));
    refreshRepoSettings();
}

#ifdef FORKMESH_WINDOW_TESTS
void MainWindow::testReattachRepositorySource(int index, const QString &source)
{
    applyRepositorySource(index, source);
}
#endif

void MainWindow::reloadRepoRemotesTable()
{
    if (!m_repoRemotesTable)
        return;
    const bool haveRepo =
        m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size();
    const QString gitDir = haveRepo ? repoRemoteGitDir(m_repositories.at(m_repoDetailIndex))
                                    : QString();
    const quint64 generation = ++m_repoRemotesLoadGen;
    m_repoRemotesTableDir.clear();
    m_repoRemotesTable->setRowCount(0);
    m_repoRemotesTable->setEnabled(false);
    if (gitDir.isEmpty())
        return;
    runOffThread<QList<RepoRemoteRow>>(
        [gitDir] {
            const forkmesh::BackgroundScope activity(
                QStringLiteral("repositories"),
                QStringLiteral("read git remotes of %1").arg(gitDir),
                forkmesh::ActionTelemetry::Execution::Worker);
            return readRepoRemotes(gitDir);
        },
        [this, generation, gitDir](QList<RepoRemoteRow> remotes) {
            if (generation != m_repoRemotesLoadGen || !m_repoRemotesTable)
                return;
            m_repoRemotesTableDir = gitDir;
            m_repoRemotesTable->setEnabled(true);
            QSignalBlocker block(m_repoRemotesTable);
            TableRepaintGuard repaintGuard(m_repoRemotesTable);
            m_repoRemotesTable->setRowCount(0);
            for (const RepoRemoteRow &remote : remotes) {
                const int row = m_repoRemotesTable->rowCount();
                m_repoRemotesTable->insertRow(row);
                m_repoRemotesTable->setItem(row, 0,
                                            new QTableWidgetItem(remote.name));
                m_repoRemotesTable->setItem(
                    row, 1, new QTableWidgetItem(remote.fetchUrl));
                m_repoRemotesTable->setItem(
                    row, 2, new QTableWidgetItem(remote.pushUrl));
            }
        });
}

bool MainWindow::repoRemoteRowsAreCurrent(const QString &gitDir) const
{
    return m_repoRemotesTable && m_repoRemotesTable->isEnabled() &&
           !gitDir.isEmpty() && m_repoRemotesTableDir == gitDir;
}

void MainWindow::runRepoRemoteWrite(
    const QString &gitDir, const QList<QPair<QStringList, QString>> &steps,
    const QString &doneLog)
{
    m_repoRemoteWriteInFlight = true;
    runOffThread<QString>(
        [gitDir, steps] {
            const forkmesh::BackgroundScope activity(
                QStringLiteral("repositories"),
                QStringLiteral("change git remotes of %1").arg(gitDir),
                forkmesh::ActionTelemetry::Execution::Worker);
            for (const auto &step : steps) {
                QString error;
                if (!runGitCapture(gitDir, step.first, nullptr, &error) &&
                    !step.second.isEmpty())
                    return step.second + error.trimmed().right(240);
            }
            return QString();
        },
        [this, doneLog](QString failure) {
            m_repoRemoteWriteInFlight = false;
            if (!failure.isEmpty()) {
                QMessageBox::warning(this, "Git remotes", failure);
                return;
            }
            reloadRepoRemotesTable();
            logSystem(doneLog);
        });
}

void MainWindow::promptAddRepoRemote()
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    if (m_repoRemoteWriteInFlight) {
        flashMessage(QStringLiteral("A git remote change is still being applied."));
        return;
    }
    const QString gitDir = repoRemoteGitDir(m_repositories.at(m_repoDetailIndex));
    if (gitDir.isEmpty()) {
        QMessageBox::information(
            this, "Git remotes",
            "Create a local fork or mirror before editing git remotes.");
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle("Add git remote");
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *nameEdit = new QLineEdit;
    auto *fetchEdit = new QLineEdit;
    auto *pushEdit = new QLineEdit;
    nameEdit->setPlaceholderText("origin");
    fetchEdit->setPlaceholderText("https://example.com/owner/repo.git");
    pushEdit->setPlaceholderText("leave blank to match fetch URL");
    form->addRow("Name", nameEdit);
    form->addRow("Fetch URL", fetchEdit);
    form->addRow("Push URL", pushEdit);
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
                                         QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const QString name = nameEdit->text().trimmed();
    const QString fetchUrl = fetchEdit->text().trimmed();
    const QString pushUrl = pushEdit->text().trimmed();
    if (name.isEmpty() || fetchUrl.isEmpty()) {
        QMessageBox::warning(this, "Git remotes",
                             "Remote name and fetch URL are required.");
        return;
    }

    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size() ||
        repoRemoteGitDir(m_repositories.at(m_repoDetailIndex)) != gitDir)
        return;
    QList<QPair<QStringList, QString>> steps{
        {{QStringLiteral("remote"), QStringLiteral("add"), name, fetchUrl},
         QStringLiteral("Could not add remote: ")}};
    if (!pushUrl.isEmpty() && pushUrl != fetchUrl)
        steps.append({{QStringLiteral("remote"), QStringLiteral("set-url"),
                       QStringLiteral("--push"), name, pushUrl},
                      QString()});
    runRepoRemoteWrite(gitDir, steps,
                       QStringLiteral("Added git remote %1 for %2/%3.")
                           .arg(name, m_repositories.at(m_repoDetailIndex).owner,
                                m_repositories.at(m_repoDetailIndex).name));
}

void MainWindow::promptEditRepoRemote()
{
    if (!m_repoRemotesTable || m_repoDetailIndex < 0 ||
        m_repoDetailIndex >= m_repositories.size())
        return;
    if (m_repoRemoteWriteInFlight) {
        flashMessage(QStringLiteral("A git remote change is still being applied."));
        return;
    }
    const int row = m_repoRemotesTable->currentRow();
    if (row < 0 || !m_repoRemotesTable->item(row, 0)) {
        QMessageBox::information(this, "Git remotes",
                                 "Select a remote to edit.");
        return;
    }
    const QString gitDir = repoRemoteGitDir(m_repositories.at(m_repoDetailIndex));
    if (!repoRemoteRowsAreCurrent(gitDir))
        return;
    const QString oldName = m_repoRemotesTable->item(row, 0)->text();
    const QString oldFetch = m_repoRemotesTable->item(row, 1)
                                 ? m_repoRemotesTable->item(row, 1)->text()
                                 : QString();
    const QString oldPush = m_repoRemotesTable->item(row, 2)
                                ? m_repoRemotesTable->item(row, 2)->text()
                                : QString();

    QDialog dialog(this);
    dialog.setWindowTitle("Edit git remote");
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *nameEdit = new QLineEdit(oldName);
    auto *fetchEdit = new QLineEdit(oldFetch);
    auto *pushEdit = new QLineEdit(oldPush);
    pushEdit->setPlaceholderText("leave blank to match fetch URL");
    form->addRow("Name", nameEdit);
    form->addRow("Fetch URL", fetchEdit);
    form->addRow("Push URL", pushEdit);
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
                                         QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const QString name = nameEdit->text().trimmed();
    const QString fetchUrl = fetchEdit->text().trimmed();
    const QString pushUrl = pushEdit->text().trimmed();
    if (name.isEmpty() || fetchUrl.isEmpty()) {
        QMessageBox::warning(this, "Git remotes",
                             "Remote name and fetch URL are required.");
        return;
    }

    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size() ||
        repoRemoteGitDir(m_repositories.at(m_repoDetailIndex)) != gitDir)
        return;
    QList<QPair<QStringList, QString>> steps;
    if (name != oldName)
        steps.append({{QStringLiteral("remote"), QStringLiteral("rename"),
                       oldName, name},
                      QStringLiteral("Could not rename remote: ")});
    steps.append({{QStringLiteral("remote"), QStringLiteral("set-url"), name,
                   fetchUrl},
                  QStringLiteral("Could not update fetch URL: ")});
    const QString effectivePush = pushUrl.isEmpty() ? fetchUrl : pushUrl;
    steps.append({{QStringLiteral("remote"), QStringLiteral("set-url"),
                   QStringLiteral("--push"), name, effectivePush},
                  QStringLiteral("Could not update push URL: ")});
    runRepoRemoteWrite(gitDir, steps,
                       QStringLiteral("Updated git remote %1 for %2/%3.")
                           .arg(name, m_repositories.at(m_repoDetailIndex).owner,
                                m_repositories.at(m_repoDetailIndex).name));
}

void MainWindow::deleteSelectedRepoRemote()
{
    if (!m_repoRemotesTable || m_repoDetailIndex < 0 ||
        m_repoDetailIndex >= m_repositories.size())
        return;
    if (m_repoRemoteWriteInFlight) {
        flashMessage(QStringLiteral("A git remote change is still being applied."));
        return;
    }
    const int row = m_repoRemotesTable->currentRow();
    if (row < 0 || !m_repoRemotesTable->item(row, 0))
        return;
    const QString gitDir = repoRemoteGitDir(m_repositories.at(m_repoDetailIndex));
    if (!repoRemoteRowsAreCurrent(gitDir))
        return;
    const QString name = m_repoRemotesTable->item(row, 0)->text();
    if (QMessageBox::question(
            this, "Delete git remote",
            QStringLiteral("Remove remote \"%1\" from this repository?").arg(name),
            QMessageBox::Cancel | QMessageBox::Yes,
            QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size() ||
        repoRemoteGitDir(m_repositories.at(m_repoDetailIndex)) != gitDir)
        return;
    runRepoRemoteWrite(
        gitDir,
        {{{QStringLiteral("remote"), QStringLiteral("remove"), name},
          QStringLiteral("Could not remove remote: ")}},
        QStringLiteral("Removed git remote %1 for %2/%3.")
            .arg(name, m_repositories.at(m_repoDetailIndex).owner,
                 m_repositories.at(m_repoDetailIndex).name));
}

void MainWindow::updateRepoDetailStatus()
{
    if (!m_repoDetailStatus)
        return;
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size()) {
        m_repoDetailStatus->clear();
        return;
    }
    const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
    if (repo.previewOnly) {
        const bool cached =
            !repo.mirrorPath.isEmpty() && QDir(repo.mirrorPath).exists();
        QStringList bits;
        bits << (cached ? QStringLiteral("<b>Preview cached</b>")
                        : QStringLiteral("<b>Preview cache pending</b>"));
        bits << QStringLiteral("<b>Temporary</b>");
        QString details = bits.join(QString::fromUtf8(" \xC2\xB7 "));
        details += QString::fromUtf8("<br><span style='color:#8b949e'>Cache %1 "
                                  "\xC2\xB7 Last refresh %2</span>")
                       .arg(repo.mirrorPath.toHtmlEscaped(),
                            formatRepoDate(repo.lastSyncMs));
        m_repoDetailStatus->setText(details);
        return;
    }
    const bool hasMirror =
        !repo.mirrorPath.isEmpty() && QDir(repo.mirrorPath).exists();
    QString details =
        !repo.publishToNetwork ? QStringLiteral("<b>Local only</b>")
        : hasMirror            ? QStringLiteral("<b>Managed mirror enabled</b>")
                               : QStringLiteral("<b>Not published yet</b>");
    details += QString::fromUtf8("<br><span style='color:#8b949e'>Added %1 "
                              "\xC2\xB7 Last sync %2</span>")
                   .arg(formatRepoDate(repo.hostedSinceMs),
                        formatRepoDate(repo.lastSyncMs));
    m_repoDetailStatus->setText(details);
}

QUrl MainWindow::catalogApiUrl() const
{
    QUrl url(canonicalServerUrl(m_serverUrlEdit ? m_serverUrlEdit->text() : QString()));
    if (!url.isValid() || url.host().isEmpty())
        url = QUrl(kDefaultServerUrl);
    if (url.scheme() == "ws")
        url.setScheme(QStringLiteral("http"));
    else if (url.scheme() == "wss")
        url.setScheme(QStringLiteral("https"));
    url.setPath(QStringLiteral("/api/repositories"));
    url.setQuery(QString());
    url.setFragment(QString());
    return url;
}

QString MainWindow::activeRelayOrigin() const
{
    const QString input = m_serverUrlEdit ? m_serverUrlEdit->text() : QString();
    thread_local bool cached = false;
    thread_local QString cachedInput;
    thread_local QString cachedOrigin;
    if (!cached || input != cachedInput) {
        cachedOrigin = normalizedRelayOrigin(catalogApiUrl());
        cachedInput = input;
        cached = true;
    }
    return cachedOrigin;
}

forkmesh::relayscope::Scope MainWindow::relayRepositoryScope() const
{
    forkmesh::relayscope::Scope scope;
    for (const ServerConfig &server : m_servers) {
        const QString origin = forkmesh::relayscope::normalizedOriginText(
            canonicalServerUrl(server.url));
        if (!origin.isEmpty() && !scope.configuredOrigins.contains(origin))
            scope.configuredOrigins.append(origin);
    }
    scope.defaultOrigin =
        forkmesh::relayscope::normalizedOriginText(kDefaultServerUrl);
    // A removed relay's remembered list no longer speaks for any record.
    for (auto it = m_relayRepositoryLists.constBegin();
         it != m_relayRepositoryLists.constEnd(); ++it) {
        if (scope.configuredOrigins.contains(it.key()) ||
            it.key() == scope.defaultOrigin)
            scope.relayLists.insert(it.key(), it.value());
    }
    return scope;
}

QString MainWindow::repositoryRelayOrigin(const RepositoryRecord &repo) const
{
    // A stamped record is final. Older records did not remember their relay:
    // a source URL on a configured relay, then the repository list each relay
    // itself reports, decide before the historical default relay does.
    return forkmesh::relayscope::resolveOrigin(
        relayRepositoryScope(), repo.relayUrl, repo.cloneUrl,
        forkmesh::relayscope::repositoryKeys(repo.owner, catalogOwner(repo),
                                             repo.name));
}

void MainWindow::stampRepositoryRelay(RepositoryRecord &repo) const
{
    const QString stored =
        normalizedRelayOrigin(QUrl(repo.relayUrl.trimmed()));
    repo.relayUrl = stored.isEmpty() ? activeRelayOrigin() : stored;
}

bool MainWindow::repositoryBelongsToActiveRelay(
    const RepositoryRecord &repo) const
{
    const QString active = activeRelayOrigin();
    return !active.isEmpty() &&
           repositoryRelayOrigin(repo).compare(active, Qt::CaseInsensitive) == 0;
}

void MainWindow::loadRelayRepositoryLists()
{
    m_relayRepositoryLists.clear();
    const QJsonObject stored =
        QJsonDocument::fromJson(
            QSettings().value(kRelayRepositoryListsSetting).toByteArray())
            .object();
    for (auto it = stored.constBegin(); it != stored.constEnd(); ++it) {
        const QString origin = normalizedRelayOrigin(QUrl(it.key()));
        if (origin.isEmpty())
            continue;
        QSet<QString> keys;
        for (const QJsonValue &value : it.value().toArray()) {
            const QString key = value.toString().trimmed().toLower();
            if (!key.isEmpty())
                keys.insert(key);
        }
        m_relayRepositoryLists.insert(origin, keys);
    }
}

void MainWindow::saveRelayRepositoryLists() const
{
    QJsonObject stored;
    for (auto it = m_relayRepositoryLists.constBegin();
         it != m_relayRepositoryLists.constEnd(); ++it) {
        QStringList keys(it.value().cbegin(), it.value().cend());
        keys.sort();
        stored.insert(it.key(), QJsonArray::fromStringList(keys));
    }
    QSettings().setValue(
        kRelayRepositoryListsSetting,
        QJsonDocument(stored).toJson(QJsonDocument::Compact));
}

// Ask the active relay for the repository list it stores. The signed viewer
// read carries a weak ETag, so an unchanged list costs the relay one indexed
// read and answers 304.
void MainWindow::refreshRelayRepositoryList()
{
    const QString origin = activeRelayOrigin();
    if (!m_networkAccess || origin.isEmpty() ||
        m_relayRepositoryListInFlight.contains(origin))
        return;
    QNetworkRequest request(catalogListUrl());
    request.setRawHeader("accept", "application/json");
    const QByteArray etag = m_relayRepositoryListEtags.value(origin);
    if (!etag.isEmpty() && m_relayRepositoryLists.contains(origin))
        request.setRawHeader("if-none-match", etag);
    m_relayRepositoryListInFlight.insert(origin);
    QNetworkReply *reply = m_networkAccess->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, origin] {
        m_relayRepositoryListInFlight.remove(origin);
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool ok = reply->error() == QNetworkReply::NoError;
        const QByteArray body = reply->readAll();
        const QByteArray etag = reply->rawHeader("etag");
        reply->deleteLater();
        if (!ok || status != 200)
            return; // 304, offline, or refused: the remembered list stands
        const QJsonObject obj = QJsonDocument::fromJson(body).object();
        if (!obj.value(QStringLiteral("repositories")).isArray())
            return;
        if (!etag.isEmpty())
            m_relayRepositoryListEtags.insert(origin, etag);
        applyRelayRepositoryCatalog(
            origin, obj.value(QStringLiteral("repositories")).toArray());
    });
}

void MainWindow::applyRelayRepositoryCatalog(const QString &origin,
                                             const QJsonArray &catalog)
{
    const QString relay = normalizedRelayOrigin(QUrl(origin));
    if (relay.isEmpty())
        return;
    QSet<QString> localKeys;
    for (const RepositoryRecord &repo : std::as_const(m_repositories)) {
        if (repo.previewOnly)
            continue;
        for (const QString &key : forkmesh::relayscope::repositoryKeys(
                 repo.owner, catalogOwner(repo), repo.name))
            localKeys.insert(key);
    }
    const QSet<QString> listed = forkmesh::relayscope::listedKeys(
        catalog, accountOwner(), localKeys);
    const bool listChanged = !m_relayRepositoryLists.contains(relay) ||
                             m_relayRepositoryLists.value(relay) != listed;
    if (listChanged) {
        m_relayRepositoryLists.insert(relay, listed);
        saveRelayRepositoryLists();
    }

    // Only the relay the desktop is pointed at right now can claim a legacy
    // record; a late answer from the relay just left must not.
    bool stamped = false;
    if (relay == activeRelayOrigin()) {
        const forkmesh::relayscope::Scope scope = relayRepositoryScope();
        for (RepositoryRecord &repo : m_repositories) {
            if (repo.previewOnly)
                continue;
            if (!forkmesh::relayscope::shouldStamp(
                    scope, relay, repo.relayUrl,
                    forkmesh::relayscope::repositoryKeys(
                        repo.owner, catalogOwner(repo), repo.name)))
                continue;
            // Settled by the source URL already; leave that answer alone.
            if (repositoryRelayOrigin(repo) != relay)
                continue;
            repo.relayUrl = relay;
            stamped = true;
        }
    }
    if (stamped)
        saveRepositories();
    if (!listChanged && !stamped)
        return;
    // What this machine serves follows the relay's list, so the gateway
    // configuration and the repository switcher are rebuilt from it.
    QString gatewayError;
    (void)rebuildDirectMirrorGatewayConfiguration(&gatewayError);
    refreshRepositoryList();
}

void MainWindow::migrateReposForProfileName(const QString &oldOwner,
                                            const QString &newOwner)
{
    const QString oldName = repoSegment(oldOwner, QStringLiteral("owner"));
    const QString newName = repoSegment(newOwner, QStringLiteral("owner"));
    if (oldName.isEmpty() || newName.isEmpty() || oldName == newName)
        return;

    bool changed = false;
    for (int i = 0; i < m_repositories.size(); ++i) {
        RepositoryRecord &repo = m_repositories[i];
        if (repo.previewOnly)
            continue;
        if (repo.owner == oldName) {
            repo.owner = newName;
            changed = true;
        }
    }

    if (!changed)
        return;
    saveRepositories();
    installAllPushHooks();
    refreshRepositoryList();
    startNodeEventSocket();
    logSystem("Renamed local repositories from " + oldName + " to " + newName +
              ". The managed mirror service owns catalog reconciliation.");
}
