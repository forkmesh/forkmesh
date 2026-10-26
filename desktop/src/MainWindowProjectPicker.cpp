// The project picker at the far left of the top bar. A project is the top-level thing ForkMesh operates on: a folder in
// ~/projects with its own data, settings, logs and keys (see AppPaths.h).
//
// One process serves one project. Picking another project starts a process
// for it, and this one steps aside unless it is doing work that must not stop
// (a system service, or a running agent).

#include "MainWindow.h"
#include "ProcessReaper.h"
#include "MainWindowInternal.h"
#include "InstanceTheme.h"
#include "AgentAccessEditor.h"
#include "ClaudeTranscriptView.h"
#include "ForkMeshPackageRegistry.h"
#include "NetworkReplyError.h"
#include "OfficeChannelMirror.h"
#include "ProjectAgentAccess.h"
#include "ProjectInvite.h"
#include "ProjectRegistry.h"
#include "SingleInstance.h"

#include <QNetworkInformation>
#include <tuple>

using namespace forkmesh::ui;

namespace {

// A project logo drawn to `side`, contained rather than cropped so a wordmark
// keeps its ends, with the letter mark as the fallback. Decoded at draw size:
// logos are routinely more than a thousand pixels square.
QPixmap projectLogoPixmap(const forkmesh::projects::Project &project, int side)
{
    if (project.logoPath.isEmpty())
        return letterFavicon(project.name, side);
    static QHash<QString, QPixmap> cache;
    const QFileInfo info(project.logoPath);
    const qreal dpr = iconDevicePixelRatio();
    const QString key = project.logoPath + QLatin1Char('|') +
                        QString::number(info.lastModified().toMSecsSinceEpoch()) +
                        QLatin1Char('|') + QString::number(side) + QLatin1Char('|') +
                        QString::number(dpr);
    const auto cached = cache.constFind(key);
    if (cached != cache.constEnd())
        return *cached;

    const int px = qMax(1, qRound(side * dpr));
    QImageReader reader(project.logoPath);
    reader.setAutoTransform(true);
    const QSize native = reader.size();
    if (native.isValid() && !native.isEmpty() &&
        (native.width() > px || native.height() > px))
        reader.setScaledSize(native.scaled(px, px, Qt::KeepAspectRatio));
    const QImage image = reader.read();
    if (image.isNull())
        return letterFavicon(project.name, side);
    QPixmap scaled = QPixmap::fromImage(
        image.scaled(px, px, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    scaled.setDevicePixelRatio(dpr);

    QPixmap mark = crispIconPixmap(side, dpr);
    QPainter painter(&mark);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    QPainterPath clip;
    clip.addRoundedRect(0, 0, side, side, side * 0.25, side * 0.25);
    painter.setClipPath(clip);
    painter.drawPixmap(QPointF((side - scaled.width() / dpr) / 2.0,
                               (side - scaled.height() / dpr) / 2.0),
                       scaled);
    painter.end();
    if (cache.size() > 64)
        cache.clear();
    cache.insert(key, mark);
    return mark;
}

// Under a service manager the process is someone's node, not just a window.
bool runningAsService()
{
    return !qEnvironmentVariableIsEmpty("INVOCATION_ID");
}

} // namespace

QPixmap forkmesh::ui::currentProjectLogo(int side, QString *name)
{
    const QString slug = forkmesh::paths::projectSlug();
    forkmesh::projects::Project current;
    for (const forkmesh::projects::Project &project : forkmesh::projects::list(
             forkmesh::paths::projectsRoot(), forkmesh::paths::homeDir())) {
        if (project.slug == slug)
            current = project;
    }
    if (current.slug.isEmpty()) {
        current.slug = slug;
        current.name = slug;
    }
    if (name)
        *name = current.name;
    return projectLogoPixmap(current, side);
}

QPushButton *MainWindow::buildProjectMenuButton()
{
    m_projectMenuButton = new QPushButton;
    m_projectMenuButton->setObjectName("projectMenuButton");
    m_projectMenuButton->setCursor(Qt::PointingHandCursor);
    m_projectMenuButton->setIconSize(QSize(32, 32));
    m_projectMenuButton->setFixedWidth(railItemWidth());
    connect(m_projectMenuButton, &QPushButton::clicked, this,
            &MainWindow::showProjectMenu);
    updateProjectSwitcher();
    return m_projectMenuButton;
}

void MainWindow::updateProjectSwitcher()
{
    if (!m_projectMenuButton)
        return;
    const QString slug = forkmesh::paths::projectSlug();
    forkmesh::projects::Project current;
    for (const forkmesh::projects::Project &project : forkmesh::projects::list(
             forkmesh::paths::projectsRoot(), forkmesh::paths::homeDir())) {
        if (project.slug == slug)
            current = project;
    }
    if (current.slug.isEmpty()) {
        current.slug = slug;
        current.name = slug;
    }
    m_projectMenuButton->setIcon(QIcon(projectLogoPixmap(current, 40)));
    m_projectMenuButton->setToolTip(
        QStringLiteral("Project: %1\n%2\nSwitch project or add a new one")
            .arg(current.slug, QDir::toNativeSeparators(forkmesh::paths::projectDir())));
}

void MainWindow::showProjectMenu()
{
    if (!m_projectMenuButton)
        return;
    const QVector<forkmesh::projects::Project> projects = forkmesh::projects::list(
        forkmesh::paths::projectsRoot(), forkmesh::paths::homeDir());
    const QString currentSlug = forkmesh::paths::projectSlug();

    QMenu menu(this);
    menu.setStyleSheet(Theme::remapSheet(QStringLiteral(
        "QPushButton#projectMenuPick { text-align:left; padding:5px 8px; }"
        "QPushButton#projectMenuContext { text-align:left; padding:3px 8px; }")));
    QAction *header = menu.addAction(QStringLiteral("Projects"));
    header->setEnabled(false);
    menu.addSeparator();

    for (const forkmesh::projects::Project &project : projects) {
        auto *row = new QWidget(&menu);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(4, 1, 4, 1);
        rowLayout->setSpacing(2);
        const bool isCurrent = project.slug == currentSlug;
        const QString folder = project.slug;
        auto *pick = new QPushButton(
            QIcon(projectLogoPixmap(project, 36)),
            isCurrent ? folder + QStringLiteral("  ✓") : folder, row);
        pick->setObjectName(QStringLiteral("projectMenuPick"));
        pick->setFlat(true);
        pick->setIconSize(QSize(28, 28));
        pick->setCursor(Qt::PointingHandCursor);
        pick->setMinimumWidth(220);
        pick->setToolTip(
            (project.description.isEmpty() ? QString()
                                           : project.description + QLatin1Char('\n')) +
            QDir::toNativeSeparators(project.dir));
        const QString slug = project.slug;
        connect(pick, &QPushButton::clicked, &menu, [this, slug, isCurrent, &menu] {
            menu.close();
            if (!isCurrent)
                openProject(slug, /*keepThisOneOpen=*/false);
        });
        rowLayout->addWidget(pick, 1);
        // The two ways to open another project, spelled out: take over this
        // window, or open it in a window of its own and keep this one.
        if (!isCurrent) {
            auto *switchHere = new QPushButton(QStringLiteral("Switch"), row);
            switchHere->setObjectName(QStringLiteral("ghostButton"));
            switchHere->setFlat(true);
            switchHere->setCursor(Qt::PointingHandCursor);
            switchHere->setToolTip(
                QStringLiteral("Switch this window to %1").arg(folder));
            switchHere->setAccessibleName(QStringLiteral("Switch to %1").arg(folder));
            connect(switchHere, &QPushButton::clicked, &menu, [this, slug, &menu] {
                menu.close();
                openProject(slug, /*keepThisOneOpen=*/false);
            });
            rowLayout->addWidget(switchHere);

            auto *beside = new QPushButton(QStringLiteral("New window"), row);
            beside->setObjectName(QStringLiteral("ghostButton"));
            beside->setFlat(true);
            beside->setCursor(Qt::PointingHandCursor);
            beside->setToolTip(QStringLiteral("Open %1 in a new window and keep this one open")
                                   .arg(folder));
            beside->setAccessibleName(
                QStringLiteral("Open %1 in a new window").arg(folder));
            setOcticon(beside, QStringLiteral("screen-full"), 14);
            connect(beside, &QPushButton::clicked, &menu, [this, slug, &menu] {
                menu.close();
                openProject(slug, /*keepThisOneOpen=*/true);
            });
            rowLayout->addWidget(beside);
        } else {
            auto *invite = new QPushButton(row);
            invite->setObjectName(QStringLiteral("ghostButton"));
            invite->setFlat(true);
            invite->setCursor(Qt::PointingHandCursor);
            invite->setToolTip(QStringLiteral("Invite someone to %1").arg(folder));
            invite->setAccessibleName(QStringLiteral("Invite someone to %1").arg(folder));
            setOcticon(invite, QStringLiteral("people"), 14);
            connect(invite, &QPushButton::clicked, &menu, [this, &menu] {
                menu.close();
                QTimer::singleShot(0, this, [this] { promptInviteToProject(); });
            });
            rowLayout->addWidget(invite);
        }
        auto *reveal = new QPushButton(row);
        reveal->setObjectName(QStringLiteral("ghostButton"));
        reveal->setFlat(true);
        reveal->setCursor(Qt::PointingHandCursor);
        reveal->setToolTip(QStringLiteral("Open %1 in the file manager")
                               .arg(QDir::toNativeSeparators(project.dir)));
        reveal->setAccessibleName(QStringLiteral("Open the folder of %1").arg(folder));
        setOcticon(reveal, QStringLiteral("file-directory"), 14);
        connect(reveal, &QPushButton::clicked, &menu, [dir = project.dir, &menu] {
            menu.close();
            QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
        });
        rowLayout->addWidget(reveal);
        auto *settings = new QPushButton(row);
        settings->setObjectName(QStringLiteral("ghostButton"));
        settings->setFlat(true);
        settings->setCursor(Qt::PointingHandCursor);
        settings->setToolTip(QStringLiteral("Project settings: name, agent folders and "
                                            "external calls"));
        settings->setAccessibleName(QStringLiteral("Settings for %1").arg(folder));
        setOcticon(settings, QStringLiteral("gear"), 14);
        connect(settings, &QPushButton::clicked, &menu, [this, slug, &menu] {
            menu.close();
            // After the menu's own event loop has unwound.
            QTimer::singleShot(0, this, [this, slug] { showProjectSettings(slug); });
        });
        rowLayout->addWidget(settings);
        auto *action = new QWidgetAction(&menu);
        action->setDefaultWidget(row);
        menu.addAction(action);
        if (isCurrent)
            addProjectContextRows(menu);
    }

    menu.addSeparator();
    QAction *addAction = menu.addAction(QStringLiteral("Add project…"));
    connect(addAction, &QAction::triggered, this, &MainWindow::promptAddProject);
    QAction *inviteAction = menu.addAction(QStringLiteral("Invite someone to this project…"));
    connect(inviteAction, &QAction::triggered, this, &MainWindow::promptInviteToProject);
    QAction *folderAction = menu.addAction(QStringLiteral("Open this project’s folder"));
    connect(folderAction, &QAction::triggered, this, [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(forkmesh::paths::projectDir()));
    });
    menu.exec(m_projectMenuButton->mapToGlobal(QPoint(0, m_projectMenuButton->height())));
}

void MainWindow::addProjectContextRows(QMenu &menu)
{
    auto *context = new QWidget(&menu);
    auto *column = new QVBoxLayout(context);
    column->setContentsMargins(40, 0, 4, 4);
    column->setSpacing(0);
    const auto later = [this, &menu](std::function<void()> run) {
        menu.close();
        QTimer::singleShot(0, this, std::move(run));
    };
    const auto addRow = [this, context, column, later](QPushButton *source,
                                                       const QString &text,
                                                       const QString &octicon) {
        auto *row = new QWidget(context);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(2);
        const bool live = source && !source->isHidden();
        auto *chip = new QPushButton(live ? source->icon() : QIcon(), text, row);
        chip->setObjectName(QStringLiteral("projectMenuContext"));
        chip->setFlat(true);
        chip->setIconSize(QSize(20, 20));
        chip->setCursor(Qt::PointingHandCursor);
        chip->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        if (live) {
            chip->setToolTip(source->toolTip());
            chip->setAccessibleName(source->accessibleName().isEmpty()
                                        ? text
                                        : source->accessibleName());
            chip->setEnabled(source->isEnabled());
            QPointer<QPushButton> guarded(source);
            connect(chip, &QPushButton::clicked, row, [later, guarded] {
                later([guarded] {
                    if (guarded)
                        guarded->click();
                });
            });
        } else {
            chip->setEnabled(false);
        }
        if (chip->icon().isNull())
            setOcticon(chip, octicon, 16);
        rowLayout->addWidget(chip, 1);
        const auto addTool = [row, rowLayout, later](
                                 const QString &icon, const QString &tip,
                                 std::function<void()> run) {
            auto *tool = new QPushButton(row);
            tool->setObjectName(QStringLiteral("ghostButton"));
            tool->setFlat(true);
            tool->setCursor(Qt::PointingHandCursor);
            tool->setToolTip(tip);
            tool->setAccessibleName(tip);
            setOcticon(tool, icon, 14);
            connect(tool, &QPushButton::clicked, row,
                    [later, run] { later(run); });
            rowLayout->addWidget(tool);
        };
        column->addWidget(row);
        return addTool;
    };

    const int active = m_activeServer;
    const bool instancePending = projectInstancePending();
    {
        auto tool = addRow(m_relayMenuButton,
                           m_relayMenuButton ? m_relayMenuButton->text() : QString(),
                           QStringLiteral("server"));
        tool(QStringLiteral("plus"), QStringLiteral("Add an instance"),
             [this, instancePending] {
                 if (instancePending)
                     promptChooseProjectInstance();
                 else
                     promptAddServer();
             });
        if (instancePending || (active >= 0 && active < m_servers.size()))
            tool(QStringLiteral("pencil"), QStringLiteral("Edit this instance's name"),
                 [this, instancePending, active] {
                     if (instancePending)
                         promptChooseProjectInstance();
                     else
                         renameServer(active);
                 });
    }
    {
        const bool signedIn =
            m_accountSwitcherButton && !m_accountSwitcherButton->isHidden();
        QPushButton *source = signedIn ? m_accountSwitcherButton : m_navSignInButton;
        const QString text =
            signedIn ? topBarUserName()
            : source && !source->isHidden() ? source->text()
                                            : QStringLiteral("Not signed in");
        auto tool = addRow(source, text, QStringLiteral("person"));
        tool(QStringLiteral("plus"),
             signedIn ? QStringLiteral("Sign in as another user")
                      : QStringLiteral("Sign in"),
             [this] { loginToUserAccount(); });
        if (signedIn)
            tool(QStringLiteral("pencil"), QStringLiteral("Edit your account"),
                 [this] { openAccountManagement(); });
    }
    {
        const bool hasRepo = m_repoMenuButton && !m_repoMenuButton->isHidden();
        auto tool = addRow(m_repoMenuButton,
                           hasRepo ? m_repoMenuButton->text()
                                   : QStringLiteral("No repository"),
                           QStringLiteral("repo"));
        tool(QStringLiteral("plus"), QStringLiteral("Add a repository"), [this] {
            QMenu add(this);
            QAction *create = add.addAction(QStringLiteral("New repository") +
                                            QString::fromUtf8("\xE2\x80\xA6"));
            QAction *local = add.addAction(QStringLiteral("Add local repo") +
                                           QString::fromUtf8("\xE2\x80\xA6"));
            QAction *chosen = add.exec(topBarMenuAnchor(nullptr));
            if (chosen == create)
                createNewRepository();
            else if (chosen == local)
                promptAddRepository();
        });
        const int repoIndex = m_repoDetailIndex;
        if (repoIndex >= 0 && repoIndex < m_repositories.size())
            tool(QStringLiteral("pencil"), QStringLiteral("Edit this repository's settings"),
                 [this, repoIndex] {
                     if (repoIndex < 0 || repoIndex >= m_repositories.size())
                         return;
                     openRepoDetail(repoIndex);
                     if (m_repoDetailTabs && m_settingsTabIndex >= 0 &&
                         m_repoDetailTabs->button(m_settingsTabIndex))
                         m_repoDetailTabs->button(m_settingsTabIndex)->click();
                 });
    }

    auto *action = new QWidgetAction(&menu);
    action->setDefaultWidget(context);
    menu.addAction(action);
}

QPoint MainWindow::topBarMenuAnchor(QWidget *button) const
{
    QWidget *anchor = button && button->isVisible() ? button : m_projectMenuButton;
    if (!anchor)
        return QCursor::pos();
    return anchor->mapToGlobal(QPoint(0, anchor->height()));
}

void MainWindow::promptAddProject()
{
    namespace projects = forkmesh::projects;
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("addProjectDialog"));
    dialog.setWindowTitle(QStringLiteral("Add project"));
    dialog.setMinimumWidth(480);
    auto *column = new QVBoxLayout(&dialog);
    column->setSpacing(10);

    auto *intro = new QLabel(
        QStringLiteral("Each project has its own folder, data and keys in %1.")
            .arg(QDir::toNativeSeparators(forkmesh::paths::projectsRoot())));
    intro->setObjectName("modeHint");
    intro->setWordWrap(true);
    column->addWidget(intro);

    auto *nameEdit = new QLineEdit;
    nameEdit->setObjectName(QStringLiteral("addProjectName"));
    nameEdit->setPlaceholderText(QStringLiteral("Project name"));
    nameEdit->setMaxLength(80);
    auto *folderLabel = new QLabel;
    folderLabel->setObjectName("statusLine");

    auto *instanceEdit = new QLineEdit;
    instanceEdit->setObjectName(QStringLiteral("addProjectInstance"));
    instanceEdit->setPlaceholderText(QStringLiteral("Instance address (optional): relay.example.com"));
    instanceEdit->setClearButtonEnabled(true);
    auto *checkButton = new QPushButton(QStringLiteral("Check"));
    checkButton->setObjectName(QStringLiteral("ghostButton"));
    checkButton->setCursor(Qt::PointingHandCursor);
    checkButton->setToolTip(
        QStringLiteral("Check that a ForkMesh instance answers there and use its name and logo"));
    auto *instanceRow = new QHBoxLayout;
    instanceRow->addWidget(instanceEdit, 1);
    instanceRow->addWidget(checkButton);
    auto *instanceStatus = new QLabel;
    instanceStatus->setObjectName(QStringLiteral("statusLine"));
    instanceStatus->setWordWrap(true);
    instanceStatus->hide();

    auto logoPath = std::make_shared<QString>();
    auto logoFromInstance = std::make_shared<bool>(false);
    auto logoDir = std::make_shared<QTemporaryDir>();
    auto *logoPreview = new QLabel;
    logoPreview->setFixedSize(48, 48);
    auto *logoChoose = new QPushButton(QStringLiteral("Choose logo…"));
    logoChoose->setObjectName("ghostButton");
    logoChoose->setCursor(Qt::PointingHandCursor);
    auto *logoClear = new QPushButton(QStringLiteral("Remove"));
    logoClear->setObjectName("ghostButton");
    logoClear->setCursor(Qt::PointingHandCursor);
    auto *logoRow = new QHBoxLayout;
    logoRow->addWidget(logoPreview);
    logoRow->addWidget(logoChoose);
    logoRow->addWidget(logoClear);
    logoRow->addStretch(1);

    auto refreshPreview = [nameEdit, logoPreview, logoClear, logoPath, folderLabel] {
        projects::Project preview;
        preview.name = nameEdit->text().trimmed().isEmpty() ? QStringLiteral("?")
                                                            : nameEdit->text().trimmed();
        preview.logoPath = *logoPath;
        logoPreview->setPixmap(projectLogoPixmap(preview, 48));
        logoClear->setVisible(!logoPath->isEmpty());
        const QString slug = forkmesh::paths::slugify(nameEdit->text());
        folderLabel->setText(
            slug.isEmpty()
                ? QString()
                : QDir::toNativeSeparators(
                      QDir(forkmesh::paths::projectsRoot()).filePath(slug)));
        folderLabel->setVisible(!slug.isEmpty());
    };
    connect(nameEdit, &QLineEdit::textChanged, &dialog, refreshPreview);
    connect(logoChoose, &QPushButton::clicked, &dialog,
            [&dialog, logoPath, logoFromInstance, refreshPreview] {
                const QString chosen = QFileDialog::getOpenFileName(
                    &dialog, QStringLiteral("Choose a logo"), QDir::homePath(),
                    QStringLiteral("Images (*.png *.webp *.jpg *.jpeg)"));
                if (!chosen.isEmpty()) {
                    *logoPath = chosen;
                    *logoFromInstance = false;
                    refreshPreview();
                }
            });
    connect(logoClear, &QPushButton::clicked, &dialog,
            [logoPath, logoFromInstance, refreshPreview] {
                logoPath->clear();
                *logoFromInstance = false;
                refreshPreview();
            });

    auto *identityCheck = new QCheckBox(QStringLiteral("Use my account in this project"));
    identityCheck->setObjectName(QStringLiteral("addProjectShareIdentity"));
    identityCheck->setChecked(false);
    identityCheck->setToolTip(QStringLiteral(
        "Copies your identity key into the project. Off: the project gets an identity of "
        "its own and is a separate account everywhere."));

    QStringList offered;
    QStringList defaultInstalled;
    if (m_packageRegistry) {
        for (const forkmesh::market::PackageManifest &manifest :
             m_packageRegistry->packages()) {
            if (manifest.managedExternally)
                continue;
            if (manifest.bundled &&
                (manifest.installedByDefault || manifest.installedBefore.isValid()))
                defaultInstalled.append(manifest.id);
            if (!manifest.bundled || !m_packageRegistry->marketParent(manifest.id).isEmpty())
                continue;
            offered.append(manifest.id);
            for (const QString &nested : m_packageRegistry->nestedPackages(manifest.id)) {
                const forkmesh::market::PackageManifest *child =
                    m_packageRegistry->package(nested);
                if (child && child->bundled && !child->managedExternally)
                    offered.append(child->id);
            }
        }
    }
    auto *pluginsLabel = new QLabel(QStringLiteral("PLUGINS TO START WITH"));
    pluginsLabel->setObjectName("sectionLabel");
    auto *packGroup = new QButtonGroup(&dialog);
    auto *packColumn = new QVBoxLayout;
    packColumn->setSpacing(2);
    const QList<std::tuple<projects::StarterPack, QString, QString>> packs{
        {projects::StarterPack::Minimal, QStringLiteral("Minimal"),
         QStringLiteral("Just your code. Add more from the Market later.")},
        {projects::StarterPack::Standard, QStringLiteral("Standard"),
         QStringLiteral("Agents, Chat, Inbox, Code and the tour. A good place to start.")},
        {projects::StarterPack::Everything, QStringLiteral("Everything"),
         QStringLiteral("Every plugin that ships with ForkMesh.")}};
    for (const auto &[pack, label, summary] : packs) {
        auto *option = new QRadioButton(label);
        option->setObjectName(QStringLiteral("addProjectPack%1").arg(label));
        option->setChecked(pack == projects::StarterPack::Standard);
        packGroup->addButton(option, int(pack));
        auto *detail = new QLabel(summary);
        detail->setObjectName(QStringLiteral("statusLine"));
        detail->setWordWrap(true);
        detail->setContentsMargins(24, 0, 0, 4);
        packColumn->addWidget(option);
        packColumn->addWidget(detail);
    }

    auto *directoryLabel = new QLabel(QStringLiteral("INSTANCES ON FORKMESH"));
    directoryLabel->setObjectName("sectionLabel");
    auto *directoryList = new QListWidget;
    directoryList->setObjectName(QStringLiteral("addProjectDirectory"));
    directoryList->setSelectionMode(QAbstractItemView::NoSelection);
    directoryLabel->hide();
    directoryList->hide();

    auto *errorLabel = new QLabel;
    errorLabel->setStyleSheet(Theme::remapSheet("color:#ff6b6b; background:transparent;"));
    errorLabel->setWordWrap(true);
    errorLabel->hide();

    auto *buttons = new QDialogButtonBox;
    QPushButton *createButton =
        buttons->addButton(QStringLiteral("Create and open"), QDialogButtonBox::AcceptRole);
    createButton->setObjectName("primaryButton");
    buttons->addButton(QDialogButtonBox::Cancel);

    column->addWidget(nameEdit);
    column->addWidget(folderLabel);
    column->addLayout(instanceRow);
    column->addWidget(instanceStatus);
    column->addLayout(logoRow);
    column->addWidget(identityCheck);
    column->addWidget(pluginsLabel);
    column->addLayout(packColumn);
    column->addWidget(directoryLabel);
    column->addWidget(directoryList);
    column->addWidget(errorLabel);
    column->addWidget(buttons);
    refreshPreview();

    auto checked = std::make_shared<projects::InstanceChoice>();
    auto autoName = std::make_shared<QString>();
    auto generation = std::make_shared<int>(0);
    const auto showStatus = [instanceStatus](const QString &text, bool error) {
        instanceStatus->setStyleSheet(
            error ? Theme::remapSheet("color:#ff6b6b; background:transparent;") : QString());
        instanceStatus->setText(text);
        instanceStatus->setVisible(!text.isEmpty());
    };
    const auto originOf = [](const QString &serverUrl) {
        QUrl origin = faviconUrl(serverUrl);
        origin.setPath(QString());
        return origin;
    };

    auto fetchInstanceLogo = std::make_shared<std::function<void(QList<QUrl>, int)>>();
    *fetchInstanceLogo = [this, &dialog, fetchInstanceLogo, generation, logoPath,
                          logoFromInstance, logoDir, refreshPreview](QList<QUrl> candidates,
                                                                     int current) {
        if (candidates.isEmpty() || !m_networkAccess || *generation != current ||
            (!logoPath->isEmpty() && !*logoFromInstance))
            return;
        QNetworkRequest request(candidates.takeFirst());
        forkmesh::markFaviconProbe(request);
        request.setTransferTimeout(10000);
        QNetworkReply *reply = m_networkAccess->get(request);
        connect(reply, &QNetworkReply::finished, &dialog,
                [reply, candidates, current, fetchInstanceLogo, generation, logoPath,
                 logoFromInstance, logoDir, refreshPreview] {
                    reply->deleteLater();
                    if (*generation != current)
                        return;
                    QImage image;
                    if (reply->error() != QNetworkReply::NoError ||
                        !image.loadFromData(reply->read(4 * 1024 * 1024)) || image.isNull()) {
                        (*fetchInstanceLogo)(candidates, current);
                        return;
                    }
                    if ((!logoPath->isEmpty() && !*logoFromInstance) || !logoDir->isValid())
                        return;
                    if (image.width() > 512 || image.height() > 512)
                        image = image.scaled(512, 512, Qt::KeepAspectRatio,
                                             Qt::SmoothTransformation);
                    const QString path =
                        logoDir->filePath(QStringLiteral("logo-%1.png").arg(current));
                    if (!image.save(path, "PNG"))
                        return;
                    *logoPath = path;
                    *logoFromInstance = true;
                    refreshPreview();
                });
    };

    const auto forgetInstanceLogo = [logoPath, logoFromInstance, refreshPreview] {
        if (!*logoFromInstance)
            return;
        logoPath->clear();
        *logoFromInstance = false;
        refreshPreview();
    };

    const auto checkInstance = [=, &dialog]() {
        const QString typed = instanceEdit->text().trimmed();
        ++*generation;
        const int current = *generation;
        *checked = {};
        if (typed.isEmpty()) {
            showStatus(QString(), false);
            forgetInstanceLogo();
            return;
        }
        const QString canonical = canonicalServerUrl(typed);
        const QUrl origin = originOf(canonical);
        if (origin.host().isEmpty()) {
            showStatus(QStringLiteral("Enter the instance's address."), true);
            return;
        }
        const QString owner = otherProjectForInstance(canonical);
        if (!owner.isEmpty()) {
            showStatus(QStringLiteral("%1 already belongs to the project %2. An instance can "
                                      "only be in one project.")
                           .arg(serverHost(canonical), owner),
                       true);
            return;
        }
        if (!m_networkAccess) {
            showStatus(QStringLiteral("Could not check %1.").arg(serverHost(canonical)), true);
            return;
        }
        showStatus(QStringLiteral("Checking %1…").arg(serverHost(canonical)), false);
        checkButton->setEnabled(false);
        QUrl versionUrl(origin);
        versionUrl.setPath(QStringLiteral("/api/version"));
        QNetworkRequest versionRequest(versionUrl);
        versionRequest.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                                    QNetworkRequest::AlwaysNetwork);
        versionRequest.setRawHeader("accept", "application/json");
        versionRequest.setTransferTimeout(10000);
        QNetworkReply *version = m_networkAccess->get(versionRequest);
        connect(version, &QNetworkReply::finished, &dialog, [=, &dialog] {
            version->deleteLater();
            if (*generation != current)
                return;
            const int status =
                version->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const bool identified = projects::isForkMeshVersionReply(
                status, version->hasRawHeader("x-forkmesh-worker"),
                QJsonDocument::fromJson(version->read(512 * 1024)).object());
            if (!identified) {
                checkButton->setEnabled(true);
                forgetInstanceLogo();
                showStatus(version->error() == QNetworkReply::NoError ||
                                   status > 0
                               ? QStringLiteral("%1 answered, but it is not a ForkMesh "
                                                "instance.")
                                     .arg(serverHost(canonical))
                               : QStringLiteral("Could not reach %1.")
                                     .arg(serverHost(canonical)),
                           true);
                return;
            }
            QUrl instanceUrl(origin);
            instanceUrl.setPath(QStringLiteral("/api/instance"));
            QNetworkRequest instanceRequest(instanceUrl);
            instanceRequest.setRawHeader("accept", "application/json");
            instanceRequest.setTransferTimeout(10000);
            QNetworkReply *described = m_networkAccess->get(instanceRequest);
            connect(described, &QNetworkReply::finished, &dialog, [=] {
                described->deleteLater();
                if (*generation != current)
                    return;
                checkButton->setEnabled(true);
                const QJsonObject document =
                    described->error() == QNetworkReply::NoError
                        ? QJsonDocument::fromJson(described->read(512 * 1024)).object()
                        : QJsonObject();
                const QString host = serverHost(canonical);
                const QString name =
                    document.value(QStringLiteral("name")).toString().trimmed().left(80);
                checked->url = canonical;
                checked->name = name.isEmpty() ? host : name;
                if (!name.isEmpty() && (nameEdit->text().trimmed().isEmpty() ||
                                        nameEdit->text().trimmed() == *autoName)) {
                    *autoName = name;
                    nameEdit->setText(name);
                }
                showStatus(name.isEmpty()
                               ? QStringLiteral("Found a ForkMesh instance at %1.").arg(host)
                               : QStringLiteral("Found %1 at %2.").arg(name, host),
                           false);
                QList<QUrl> candidates;
                QUrl logoUrl(origin);
                const QString logo =
                    document.value(QStringLiteral("logo")).toString().trimmed();
                if (logo.startsWith(QLatin1Char('/')) && !logo.startsWith(QStringLiteral("//"))) {
                    logoUrl.setPath(logo);
                    candidates.append(logoUrl);
                }
                logoUrl.setPath(faviconRootPath());
                candidates.append(logoUrl);
                logoUrl.setPath(faviconAssetPath());
                candidates.append(logoUrl);
                (*fetchInstanceLogo)(candidates, current);
            });
        });
    };
    connect(checkButton, &QPushButton::clicked, &dialog, checkInstance);
    connect(instanceEdit, &QLineEdit::textEdited, &dialog,
            [checked, generation, checkButton, showStatus, forgetInstanceLogo] {
                ++*generation;
                *checked = {};
                checkButton->setEnabled(true);
                showStatus(QString(), false);
                forgetInstanceLogo();
            });

    const QString directoryOrigin = originOf(kDefaultServerUrl).toString();
    auto populateDirectory = [=, &dialog](const QJsonArray &rows) {
        QVector<projects::DirectoryInstance> entries;
        projects::DirectoryInstance flagship;
        flagship.host = serverHost(kDefaultServerUrl);
        flagship.name = QStringLiteral("ForkMesh");
        flagship.online = true;
        entries.append(flagship);
        for (const projects::DirectoryInstance &entry :
             projects::directoryInstances(rows, directoryOrigin))
            if (entry.host != flagship.host)
                entries.append(entry);
        directoryList->clear();
        const QVector<projects::Project> known =
            projects::list(forkmesh::paths::projectsRoot(), forkmesh::paths::homeDir());
        for (const projects::DirectoryInstance &entry : std::as_const(entries)) {
            const projects::Project *ownerProject = projects::instanceOwner(
                known, canonicalServerUrl(entry.host), forkmesh::paths::projectDir());
            const QString owner = ownerProject ? ownerProject->name : QString();
            auto *item = new QListWidgetItem(directoryList);
            item->setData(Qt::UserRole, entry.host);
            auto *row = new QWidget;
            auto *rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(6, 4, 6, 4);
            rowLayout->setSpacing(10);
            auto *logo = new QLabel;
            logo->setFixedSize(28, 28);
            logo->setPixmap(letterFavicon(entry.name, 28));
            auto *title = new QLabel(entry.name);
            auto *subtitle = new QLabel(
                entry.online ? entry.host : QStringLiteral("%1 · checking").arg(entry.host));
            subtitle->setObjectName(QStringLiteral("statusLine"));
            auto *text = new QVBoxLayout;
            text->setSpacing(0);
            text->addWidget(title);
            text->addWidget(subtitle);
            rowLayout->addWidget(logo);
            rowLayout->addLayout(text, 1);
            if (!owner.isEmpty()) {
                auto *inProject = new QLabel(QStringLiteral("In %1").arg(owner));
                inProject->setObjectName(QStringLiteral("statusLine"));
                rowLayout->addWidget(inProject);
            } else {
                auto *add = new QPushButton(QStringLiteral("Add"));
                add->setObjectName(QStringLiteral("ghostButton"));
                add->setCursor(Qt::PointingHandCursor);
                add->setAccessibleName(QStringLiteral("Add %1").arg(entry.name));
                setOcticon(add, QStringLiteral("plus"), 14);
                const QString host = entry.host;
                connect(add, &QPushButton::clicked, &dialog,
                        [instanceEdit, host, checkInstance] {
                            instanceEdit->setText(host);
                            checkInstance();
                        });
                rowLayout->addWidget(add);
            }
            item->setSizeHint(row->sizeHint());
            directoryList->setItemWidget(item, row);
            if (!entry.logoUrl.isEmpty() && m_networkAccess) {
                QNetworkRequest request{QUrl(entry.logoUrl)};
                request.setTransferTimeout(10000);
                QNetworkReply *reply = m_networkAccess->get(request);
                connect(reply, &QNetworkReply::finished, &dialog,
                        [reply, logo = QPointer<QLabel>(logo)] {
                    reply->deleteLater();
                    QImage image;
                    if (!logo)
                        return;
                    if (reply->error() != QNetworkReply::NoError ||
                        !image.loadFromData(reply->read(1024 * 1024)) || image.isNull())
                        return;
                    const qreal dpr = logo->devicePixelRatioF();
                    QPixmap pixmap = QPixmap::fromImage(
                        image.scaled(QSize(28, 28) * dpr, Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation));
                    pixmap.setDevicePixelRatio(dpr);
                    logo->setPixmap(pixmap);
                });
            }
        }
        int rowsHeight = 2 * directoryList->frameWidth();
        for (int row = 0; row < directoryList->count(); ++row)
            rowsHeight += directoryList->item(row)->sizeHint().height();
        directoryList->setFixedHeight(qMin(170, rowsHeight));
        directoryLabel->setVisible(directoryList->count() > 0);
        directoryList->setVisible(directoryList->count() > 0);
    };
    populateDirectory(m_directoryInstances);
#ifndef FORKMESH_WINDOW_TESTS
    if (m_networkAccess) {
        QUrl url(directoryOrigin);
        url.setPath(QStringLiteral("/api/start/instances"));
        QNetworkRequest request(url);
        request.setRawHeader("accept", "application/json");
        request.setTransferTimeout(15000);
        QNetworkReply *reply = m_networkAccess->get(request);
        connect(reply, &QNetworkReply::finished, &dialog, [this, reply, populateDirectory] {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError)
                return;
            const QJsonObject document =
                QJsonDocument::fromJson(reply->read(512 * 1024)).object();
            if (!document.value(QStringLiteral("ok")).toBool())
                return;
            const QJsonArray rows = document.value(QStringLiteral("instances")).toArray();
            m_directoryInstances = QJsonArray();
            for (const QJsonValue &value : rows) {
                const QUrl site(value.toObject().value(QStringLiteral("url")).toString());
                if (site.scheme() == QLatin1String("https") && !site.host().isEmpty())
                    m_directoryInstances.append(value);
            }
            m_directoryInstancesFetchedMs = QDateTime::currentMSecsSinceEpoch();
            scheduleNodesTableRefresh();
            populateDirectory(m_directoryInstances);
        });
    }
#endif

    projects::Project created;
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (!instanceEdit->text().trimmed().isEmpty() && checked->url.isEmpty()) {
            errorLabel->setText(QStringLiteral("Check the instance first, or clear its address."));
            errorLabel->show();
            if (checkButton->isEnabled())
                checkInstance();
            return;
        }
        projects::CreateRequest request;
        request.name = nameEdit->text();
        request.logoSource = *logoPath;
        request.instance = *checked;
        request.shareIdentity = identityCheck->isChecked();
        request.removedPackages = defaultInstalled;
        request.packages = projects::starterPackages(
            projects::StarterPack(packGroup->checkedId()), offered);
        for (int i = 0; m_packageRegistry && i < request.packages.size(); ++i) {
            const forkmesh::market::PackageManifest *manifest =
                m_packageRegistry->package(request.packages.at(i));
            if (!manifest)
                continue;
            for (const QString &dependency : manifest->dependencies) {
                const forkmesh::market::PackageManifest *required =
                    m_packageRegistry->package(dependency);
                if (required && required->bundled && !required->managedExternally &&
                    !request.packages.contains(dependency))
                    request.packages.append(dependency);
            }
        }
        QString error;
        if (!projects::create(forkmesh::paths::layout(), request, &created, &error)) {
            errorLabel->setText(error);
            errorLabel->show();
            return;
        }
        dialog.accept();
    });
    nameEdit->setFocus();
    const int outcome = dialog.exec();
    ++*generation;
    *fetchInstanceLogo = nullptr;
    if (outcome != QDialog::Accepted || created.slug.isEmpty())
        return;
    openProject(created.slug, /*keepThisOneOpen=*/false);
}

void MainWindow::promptInviteToProject()
{
    namespace invite = forkmesh::projectinvite;
    const QString projectName =
        QSettings()
            .value(QStringLiteral("project/name"), forkmesh::paths::projectSlug())
            .toString();
    const bool hasInstance = !projectInstancePending() && m_activeServer >= 0 &&
                             m_activeServer < m_servers.size();
    const QString self =
        hasActiveAccountSession() ? accountOwner().trimmed().toLower() : QString();
    const invite::Invitation invitation{
        projectName, self, hasInstance ? m_servers.at(m_activeServer).url : QString()};
    const QString host = invite::instanceHost(invitation.instanceUrl);
    const QString link = invite::invitationLink(invitation);
    const bool canInviteAccount = hasInstance && !host.isEmpty() && !self.isEmpty();

    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("projectInviteDialog"));
    dialog.setWindowTitle(QStringLiteral("Invite to project"));
    dialog.setMinimumWidth(500);
    auto *column = new QVBoxLayout(&dialog);
    column->setContentsMargins(18, 16, 18, 16);
    column->setSpacing(10);

    auto *heading = new QLabel(QStringLiteral("Invite someone to %1").arg(projectName));
    heading->setStyleSheet(
        Theme::remapSheet(QStringLiteral("font-size:17px; font-weight:600;")));
    heading->setWordWrap(true);
    column->addWidget(heading);

    auto *intro = new QLabel(
        hasInstance && !host.isEmpty()
            ? QStringLiteral("%1 uses the instance %2. Invite someone who already has an "
                             "account there, or email anyone else an invitation link.")
                  .arg(projectName, host)
            : QStringLiteral("%1 is not connected to an instance yet, so for now you can "
                             "only email an invitation. Choose an instance from the "
                             "instance picker to invite people who are already on it.")
                  .arg(projectName));
    intro->setObjectName("modeHint");
    intro->setWordWrap(true);
    column->addWidget(intro);

    auto *accountLabel = new QLabel(
        host.isEmpty() ? QStringLiteral("SOMEONE ON THIS INSTANCE")
                       : QStringLiteral("SOMEONE ON %1").arg(host.toUpper()));
    accountLabel->setObjectName("sectionLabel");
    column->addWidget(accountLabel);
    auto *accountRow = new QHBoxLayout;
    auto *accountEdit = new QLineEdit;
    accountEdit->setObjectName(QStringLiteral("projectInviteAccount"));
    accountEdit->setPlaceholderText(QStringLiteral("Their ForkMesh account name"));
    accountEdit->setClearButtonEnabled(true);
    auto *completer = new QCompleter(chatPickerAccounts(), &dialog);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setCompletionMode(QCompleter::PopupCompletion);
    completer->setFilterMode(Qt::MatchContains);
    accountEdit->setCompleter(completer);
    auto *sendButton = new QPushButton(QStringLiteral("Send invite"));
    sendButton->setObjectName(QStringLiteral("primaryButton"));
    sendButton->setCursor(Qt::PointingHandCursor);
    setOcticon(sendButton, QStringLiteral("person"), 14);
    accountRow->addWidget(accountEdit, 1);
    accountRow->addWidget(sendButton);
    column->addLayout(accountRow);
    accountEdit->setEnabled(canInviteAccount);
    sendButton->setEnabled(canInviteAccount);
    if (hasInstance && self.isEmpty()) {
        auto *signInHint = new QLabel(
            QStringLiteral("Sign in to invite people on %1 directly.").arg(host));
        signInHint->setObjectName(QStringLiteral("statusLine"));
        signInHint->setWordWrap(true);
        column->addWidget(signInHint);
    }

    auto *emailLabel = new QLabel(QStringLiteral("ANYONE ELSE, BY EMAIL"));
    emailLabel->setObjectName("sectionLabel");
    column->addWidget(emailLabel);
    auto *emailRow = new QHBoxLayout;
    auto *emailEdit = new QLineEdit;
    emailEdit->setObjectName(QStringLiteral("projectInviteEmail"));
    emailEdit->setPlaceholderText(
        QStringLiteral("name@example.com (optional; separate several with commas)"));
    emailEdit->setClearButtonEnabled(true);
    auto *emailButton = new QPushButton(QStringLiteral("Open email"));
    emailButton->setObjectName(QStringLiteral("ghostButton"));
    emailButton->setCursor(Qt::PointingHandCursor);
    emailButton->setToolTip(
        QStringLiteral("Open a new email with the invitation link in your email app"));
    setOcticon(emailButton, QStringLiteral("paper-airplane"), 14);
    emailRow->addWidget(emailEdit, 1);
    emailRow->addWidget(emailButton);
    column->addLayout(emailRow);

    auto *linkRow = new QHBoxLayout;
    auto *linkEdit = new QLineEdit(link);
    linkEdit->setObjectName(QStringLiteral("projectInviteLink"));
    linkEdit->setReadOnly(true);
    linkEdit->setCursorPosition(0);
    auto *copyButton = new QPushButton(QStringLiteral("Copy link"));
    copyButton->setObjectName(QStringLiteral("ghostButton"));
    copyButton->setCursor(Qt::PointingHandCursor);
    setOcticon(copyButton, QStringLiteral("copy"), 14);
    linkRow->addWidget(linkEdit, 1);
    linkRow->addWidget(copyButton);
    column->addLayout(linkRow);
    linkEdit->setVisible(!link.isEmpty());
    copyButton->setVisible(!link.isEmpty());

    auto *status = new QLabel;
    status->setObjectName(QStringLiteral("statusLine"));
    status->setWordWrap(true);
    column->addWidget(status);
    m_projectInviteStatus = status;

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    column->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    connect(copyButton, &QPushButton::clicked, &dialog, [link, status] {
        QApplication::clipboard()->setText(link);
        status->setText(QStringLiteral("Invitation link copied."));
    });

    const auto openEmail = [emailEdit, status, invitation] {
        QString invalid;
        const QStringList recipients =
            invite::emailRecipients(emailEdit->text(), &invalid);
        if (!invalid.isEmpty()) {
            status->setText(
                QStringLiteral("“%1” is not an email address.").arg(invalid));
            emailEdit->setFocus();
            return;
        }
        if (QDesktopServices::openUrl(invite::mailtoUrl(recipients, invitation))) {
            status->setText(QStringLiteral(
                "Your email app has the invitation ready to send."));
            return;
        }
        QApplication::clipboard()->setText(invite::emailSubject(invitation) +
                                           QStringLiteral("\n\n") +
                                           invite::emailBody(invitation));
        status->setText(QStringLiteral(
            "No email app opened, so the invitation was copied. Paste it into "
            "an email."));
    };
    connect(emailButton, &QPushButton::clicked, &dialog, openEmail);
    connect(emailEdit, &QLineEdit::returnPressed, &dialog, openEmail);

    const auto sendInvite = [this, accountEdit, emailEdit, status, invitation, self,
                             host] {
        QString who = accountEdit->text().trimmed().toLower();
        if (who.startsWith(QLatin1Char('@')))
            who.remove(0, 1);
        if (who.isEmpty()) {
            status->setText(
                QStringLiteral("Enter their account name on %1.").arg(host));
            accountEdit->setFocus();
            return;
        }
        if (who.contains(QLatin1Char('@')) &&
            !invite::emailRecipients(who).isEmpty()) {
            emailEdit->setText(accountEdit->text().trimmed());
            accountEdit->clear();
            emailEdit->setFocus();
            status->setText(QStringLiteral(
                "That is an email address, so it moved to the email invitation. "
                "Press Open email to send it."));
            return;
        }
        if (who == self) {
            status->setText(QStringLiteral("You are already in this project."));
            return;
        }
        QString error;
        if (!coveInviteAccountValid(who, &error)) {
            status->setText(
                QStringLiteral("%1 Invite them by email below instead.").arg(error));
            emailEdit->setFocus();
            return;
        }
        const QString text = invite::directMessageText(invitation);
        const QString conversation = forkmesh::office::conversationForDirect(who);
        if (!m_officeChannelMirror || !m_officeChannelMirror->isActive())
            startOfficeChannelMirror();
        if (!m_officeChannelMirror || !m_officeChannelMirror->isActive() ||
            conversation.isEmpty()) {
            QApplication::clipboard()->setText(text);
            status->setText(
                QStringLiteral("Messages are not connected, so the invitation was "
                               "copied. Send it to @%1 when you are back online.")
                    .arg(who));
            return;
        }
        status->setText(QStringLiteral("Sending the invitation to @%1…").arg(who));
        m_pendingDirectInvites.insert(conversation, text);
        if (!m_officeChannelMirror->startDirectMessage(who)) {
            m_pendingDirectInvites.remove(conversation);
            status->setText(
                QStringLiteral("Could not open a conversation with @%1.").arg(who));
            return;
        }
        accountEdit->clear();
    };
    connect(sendButton, &QPushButton::clicked, &dialog, sendInvite);
    connect(accountEdit, &QLineEdit::returnPressed, &dialog, sendInvite);

    QTimer::singleShot(0, &dialog, [accountEdit, emailEdit, canInviteAccount] {
        (canInviteAccount ? accountEdit : emailEdit)->setFocus();
    });
    dialog.exec();
    m_projectInviteStatus = nullptr;
}

bool MainWindow::projectInstancePending() const
{
    return QSettings().value(forkmesh::projects::kInstancePendingSetting, false).toBool();
}

void MainWindow::promptChooseProjectInstance()
{
    if (!projectInstancePending())
        return;
    QVector<forkmesh::projects::InstanceChoice> choices =
        forkmesh::projects::decodeInstanceChoices(
            QSettings().value(forkmesh::projects::kInstanceChoicesSetting).toString());
    const QString chosen =
        QSettings().value(forkmesh::projects::kInstanceChosenSetting).toString().trimmed();
    if (!chosen.isEmpty()) {
        QSettings().remove(forkmesh::projects::kInstanceChosenSetting);
        QString chosenName;
        for (const forkmesh::projects::InstanceChoice &choice : std::as_const(choices))
            if (choice.url == chosen)
                chosenName = choice.name;
        if (otherProjectForInstance(chosen).isEmpty()) {
            connectProjectInstance(chosen, chosenName);
            return;
        }
    }
    if (choices.isEmpty())
        choices.append({serverHost(kDefaultServerUrl), kDefaultServerUrl});
    choices.erase(std::remove_if(choices.begin(), choices.end(),
                                 [this](const forkmesh::projects::InstanceChoice &choice) {
                                     return !otherProjectForInstance(choice.url).isEmpty();
                                 }),
                  choices.end());

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Choose an instance"));
    dialog.setMinimumWidth(460);
    auto *column = new QVBoxLayout(&dialog);
    column->setSpacing(10);

    auto *intro = new QLabel(QStringLiteral(
        "%1 is not connected to any instance yet. Pick the one it should use: its "
        "account, repositories and chat live there. Until you choose, the project "
        "can go online but joins no instance.")
                                 .arg(QSettings()
                                          .value(QStringLiteral("project/name"),
                                                 forkmesh::paths::projectSlug())
                                          .toString()));
    intro->setObjectName("modeHint");
    intro->setWordWrap(true);
    column->addWidget(intro);

    auto *list = new QListWidget;
    list->setMaximumHeight(200);
    for (const forkmesh::projects::InstanceChoice &choice : choices) {
        const QString host = serverHost(choice.url);
        const QString name = choice.name.isEmpty() ? host : choice.name;
        auto *item = new QListWidgetItem(
            QIcon(letterFavicon(host)),
            name == host ? host : QStringLiteral("%1  ·  %2").arg(name, host), list);
        item->setData(Qt::UserRole, choice.url);
        item->setData(Qt::UserRole + 1, name);
    }
    column->addWidget(list);

    auto *otherEdit = new QLineEdit;
    otherEdit->setPlaceholderText(QStringLiteral("Or another instance: relay.example.com"));
    column->addWidget(otherEdit);

    auto *errorLabel = new QLabel;
    errorLabel->setStyleSheet(Theme::remapSheet("color:#ff6b6b; background:transparent;"));
    errorLabel->setWordWrap(true);
    errorLabel->hide();
    column->addWidget(errorLabel);

    auto *buttons = new QDialogButtonBox;
    QPushButton *connectButton =
        buttons->addButton(QStringLiteral("Connect"), QDialogButtonBox::AcceptRole);
    connectButton->setObjectName("primaryButton");
    buttons->addButton(QStringLiteral("Not now"), QDialogButtonBox::RejectRole);
    column->addWidget(buttons);

    connect(otherEdit, &QLineEdit::textChanged, &dialog, [list](const QString &text) {
        if (!text.trimmed().isEmpty())
            list->clearSelection();
    });
    connect(list, &QListWidget::itemSelectionChanged, &dialog, [list, otherEdit] {
        if (!list->selectedItems().isEmpty())
            otherEdit->clear();
    });

    QString chosenUrl;
    QString chosenName;
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        const QString typed = otherEdit->text().trimmed();
        if (!typed.isEmpty()) {
            chosenUrl = canonicalServerUrl(typed);
            chosenName = serverHost(chosenUrl);
            if (chosenName.isEmpty()) {
                errorLabel->setText(QStringLiteral("Enter the instance's address."));
                errorLabel->show();
                return;
            }
        } else if (!list->selectedItems().isEmpty()) {
            chosenUrl = list->selectedItems().first()->data(Qt::UserRole).toString();
            chosenName = list->selectedItems().first()->data(Qt::UserRole + 1).toString();
        } else {
            errorLabel->setText(QStringLiteral("Pick an instance or enter one."));
            errorLabel->show();
            return;
        }
        const QString owner = otherProjectForInstance(chosenUrl);
        if (!owner.isEmpty()) {
            errorLabel->setText(QStringLiteral("%1 already belongs to the project %2. An "
                                               "instance can only be in one project.")
                                    .arg(serverHost(chosenUrl), owner));
            errorLabel->show();
            return;
        }
        dialog.accept();
    });
    connect(list, &QListWidget::itemDoubleClicked, &dialog,
            [&dialog, &chosenUrl, &chosenName](QListWidgetItem *item) {
                chosenUrl = item->data(Qt::UserRole).toString();
                chosenName = item->data(Qt::UserRole + 1).toString();
                dialog.accept();
            });

    if (dialog.exec() != QDialog::Accepted || chosenUrl.isEmpty()) {
        flashMessage(QStringLiteral("This project joins no instance until you choose "
                                    "one from the instance picker."));
        return;
    }
    connectProjectInstance(chosenUrl, chosenName);
}

QString MainWindow::otherProjectForInstance(const QString &url, QString *slug) const
{
    const QVector<forkmesh::projects::Project> projects = forkmesh::projects::list(
        forkmesh::paths::projectsRoot(), forkmesh::paths::homeDir());
    const forkmesh::projects::Project *owner =
        forkmesh::projects::instanceOwner(projects, url, forkmesh::paths::projectDir());
    if (!owner)
        return {};
    if (slug)
        *slug = owner->slug;
    return owner->name;
}

bool MainWindow::refuseInstanceOfAnotherProject(const QString &url)
{
    QString slug;
    const QString owner = otherProjectForInstance(url, &slug);
    if (owner.isEmpty())
        return false;
    QMessageBox box(QMessageBox::Information, QStringLiteral("Instance already in a project"),
                    QStringLiteral("%1 already belongs to the project %2. An instance can only "
                                   "be in one project.")
                        .arg(serverHost(canonicalServerUrl(url)), owner),
                    QMessageBox::Cancel, this);
    QPushButton *open =
        box.addButton(QStringLiteral("Open %1").arg(owner), QMessageBox::AcceptRole);
    box.exec();
    if (box.clickedButton() == open)
        openProject(slug, /*keepThisOneOpen=*/false);
    return true;
}

void MainWindow::recordProjectInstances()
{
    if (!forkmesh::paths::isActive() || !forkmesh::paths::layout().organized ||
        projectInstancePending())
        return;
    QStringList urls;
    for (const ServerConfig &server : std::as_const(m_servers))
        urls.append(server.url);
    QString error;
    if (!forkmesh::projects::recordInstances(forkmesh::paths::projectDir(), urls, &error))
        qWarning().noquote() << "projects:" << error;
}

void MainWindow::connectProjectInstance(const QString &url, const QString &name)
{
    if (refuseInstanceOfAnotherProject(url))
        return;
    const QString canonical = canonicalServerUrl(url);
    QSettings settings;
    settings.remove(forkmesh::projects::kInstancePendingSetting);
    settings.remove(forkmesh::projects::kInstanceChoicesSetting);

    int index = -1;
    for (int i = 0; i < m_servers.size(); ++i) {
        if (canonicalServerUrl(m_servers.at(i).url) == canonical)
            index = i;
    }
    if (index < 0) {
        ServerConfig server;
        server.url = canonical;
        server.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        server.name = name.trimmed().isEmpty() ? serverHost(canonical) : name.trimmed().left(80);
        server.room = kDefaultRoomName;
        m_servers.append(server);
        m_legacyServerUrls.append(canonical);
        m_legacyServerRooms.append(server.room);
        index = m_servers.size() - 1;
        saveServers();
    }
    if (index != m_activeServer)
        switchToServer(index);
    for (int i = m_servers.size() - 1; i >= 0; --i) {
        if (i == m_activeServer)
            continue;
        forgetServerInstance(m_servers.at(i));
        m_servers.removeAt(i);
        if (i < m_legacyServerUrls.size())
            m_legacyServerUrls.removeAt(i);
        if (i < m_legacyServerRooms.size())
            m_legacyServerRooms.removeAt(i);
        if (m_activeServer > i)
            --m_activeServer;
    }
    saveServers();
    updateBreadcrumb();
    if (!m_offlineMode) {
        const auto *networkInfo = QNetworkInformation::instance();
        if (m_backend)
            m_backend->setNetworkAvailable(
                !networkInfo || networkInfo->reachability() !=
                                    QNetworkInformation::Reachability::Disconnected);
        startNodePresence();
        probeRelayLatency();
    }
    setOfflineMode(false);
    fetchFavicon(m_activeServer);
    adoptInstanceLogo(canonical);
    updateRelaySwitcher();
}

void MainWindow::adoptInstanceLogo(const QString &serverUrl)
{
    const QString dir = forkmesh::paths::projectDir();
    QUrl api = faviconUrl(serverUrl);
    if (!m_networkAccess || dir.isEmpty() || api.isEmpty() ||
        !forkmesh::projects::logoPathIn(dir).isEmpty())
        return;
    api.setPath(QStringLiteral("/api/instance"));
    QNetworkRequest request(api);
    request.setRawHeader("Accept", "application/json");
    QNetworkReply *reply = m_networkAccess->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, api, dir] {
        reply->deleteLater();
        const QString logo =
            reply->error() == QNetworkReply::NoError
                ? QJsonDocument::fromJson(reply->readAll())
                      .object()
                      .value(QStringLiteral("logo"))
                      .toString()
                      .trimmed()
                : QString();
        QList<QUrl> candidates;
        QUrl url(api);
        if (logo.startsWith(QLatin1Char('/')) && !logo.startsWith(QStringLiteral("//"))) {
            url.setPath(logo);
            candidates.append(url);
        }
        url.setPath(faviconRootPath());
        candidates.append(url);
        url.setPath(faviconAssetPath());
        candidates.append(url);
        saveInstanceLogo(dir, candidates);
    });
}

void MainWindow::applyInstanceThemeChoice(bool enabled)
{
    forkmesh::instancetheme::setMatchEnabled(enabled);
    if (m_matchInstanceThemeCheck && m_matchInstanceThemeCheck->isChecked() != enabled) {
        const QSignalBlocker blocker(m_matchInstanceThemeCheck);
        m_matchInstanceThemeCheck->setChecked(enabled);
    }
    m_instanceThemeOrigin.clear();
    applyTheme(m_packageRegistry.get());
    if (enabled)
        refreshInstanceTheme();
}

void MainWindow::refreshInstanceTheme()
{
    if (!forkmesh::instancetheme::matchEnabled() || !m_networkAccess ||
        m_offlineMode || m_activeServer < 0 || m_activeServer >= m_servers.size())
        return;
    QUrl api = faviconUrl(m_servers.at(m_activeServer).url);
    if (api.isEmpty())
        return;
    api.setPath(QStringLiteral("/api/instance"));
    const QString origin = api.toString(QUrl::RemovePath);
    if (origin == m_instanceThemeOrigin)
        return;
    m_instanceThemeOrigin = origin;
    QNetworkRequest request(api);
    request.setRawHeader("Accept", "application/json");
    QNetworkReply *reply = m_networkAccess->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, origin] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            if (m_instanceThemeOrigin == origin)
                m_instanceThemeOrigin.clear();
            return;
        }
        const QJsonObject document =
            QJsonDocument::fromJson(reply->readAll()).object();
        if (!document.value(QStringLiteral("ok")).toBool() ||
            !forkmesh::instancetheme::matchEnabled() ||
            m_instanceThemeOrigin != origin)
            return;
        const forkmesh::instancetheme::Colors colors =
            forkmesh::instancetheme::fromInstanceDocument(document);
        if (colors == forkmesh::instancetheme::stored())
            return;
        forkmesh::instancetheme::store(colors);
        applyTheme(m_packageRegistry.get());
    });
}

void MainWindow::saveInstanceLogo(const QString &projectDir, QList<QUrl> candidates)
{
    if (candidates.isEmpty() || !m_networkAccess)
        return;
    QNetworkRequest request(candidates.takeFirst());
    forkmesh::markFaviconProbe(request);
    QNetworkReply *reply = m_networkAccess->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, projectDir, candidates] {
        reply->deleteLater();
        QImage image;
        if (reply->error() != QNetworkReply::NoError || !image.loadFromData(reply->readAll()) ||
            image.isNull()) {
            saveInstanceLogo(projectDir, candidates);
            return;
        }
        if (!forkmesh::projects::logoPathIn(projectDir).isEmpty())
            return;
        if (image.width() > 512 || image.height() > 512)
            image = image.scaled(512, 512, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        if (image.save(QDir(projectDir).filePath(QStringLiteral("logo.png")), "PNG"))
            updateProjectSwitcher();
    });
}

void MainWindow::openProject(const QString &slug, bool keepThisOneOpen)
{
    if (!forkmesh::paths::isValidSlug(slug) || slug == forkmesh::paths::projectSlug())
        return;
    if (slug != QLatin1String(forkmesh::paths::kHomeProjectSlug)) {
        const QString dir = QDir(forkmesh::paths::projectsRoot()).filePath(slug);
        if (QFileInfo::exists(QDir(dir).filePath(QStringLiteral(".git")))) {
            flashMessage(QStringLiteral("%1 is a git checkout. Opening it as a project would "
                                        "put ForkMesh’s keys and data inside the "
                                        "repository.")
                             .arg(QDir::toNativeSeparators(dir)),
                         true);
            return;
        }
    }
    // The other project has its own lock and socket: if it is already running
    // this start focuses it, otherwise it becomes that project's process.
    const bool started = forkmesh::processreaper::startReleased(
        QCoreApplication::applicationFilePath(),
        forkmesh::projects::launchArguments(QCoreApplication::arguments(), slug),
        QDir::homePath());
    if (!started) {
        flashMessage(QStringLiteral("Could not open the project."), true);
        return;
    }
    // Stepping aside must never take work down with it: a node other machines
    // depend on, or an agent in the middle of a run, keeps this process alive.
    QString staying;
    if (keepThisOneOpen)
        staying = QStringLiteral("Opened beside this project.");
    else if (runningAsService())
        staying = QStringLiteral("Opened in its own window. This project keeps running "
                                 "because it is a system service.");
    else if (anyAgentRunning())
        staying = QStringLiteral("Opened in its own window. This project keeps running "
                                 "until its agents finish.");
    if (!staying.isEmpty()) {
        flashMessage(staying);
        return;
    }
    stopLiveServicesForDataOp();
    forkmesh::releaseSingleInstance();
    QCoreApplication::exit(0);
}

void MainWindow::showProjectSettings(const QString &slug)
{
    if (!loadProjectSettingsPage(slug))
        return;
    showSection(kProjectSettingsSectionIndex);
    raise();
    activateWindow();
}

QWidget *MainWindow::buildProjectSettingsSection()
{
    m_projectSettingsScroll = new QScrollArea;
    m_projectSettingsScroll->setObjectName(QStringLiteral("projectSettingsSection"));
    m_projectSettingsScroll->setFrameShape(QFrame::NoFrame);
    m_projectSettingsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_projectSettingsScroll->setWidgetResizable(true);
    m_projectSettingsScroll->setMinimumHeight(0);
    m_projectSettingsScroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    m_projectSettingsScroll->setWidget(new QWidget);
    return m_projectSettingsScroll;
}

void MainWindow::leaveProjectSettingsPage()
{
    for (int index = qMin(m_navHistoryIndex, int(m_navHistory.size()) - 1); index >= 0;
         --index) {
        if (m_navHistory.at(index).section != kProjectSettingsSectionIndex) {
            restoreNavEntry(index);
            return;
        }
    }
    showSection(0);
}

bool MainWindow::loadProjectSettingsPage(const QString &slug)
{
    using Access = forkmesh::projects::AgentAccess;
    forkmesh::projects::Project project;
    for (const forkmesh::projects::Project &candidate : forkmesh::projects::list(
             forkmesh::paths::projectsRoot(), forkmesh::paths::homeDir()))
        if (candidate.slug == slug)
            project = candidate;
    if (project.slug.isEmpty()) {
        flashMessage(QStringLiteral("That project no longer exists."), true);
        return false;
    }
    ensureSectionBuilt(kProjectSettingsSectionIndex);
    if (!m_projectSettingsScroll)
        return false;
    m_projectSettingsSlug = slug;
    const bool isCurrent = slug == forkmesh::paths::projectSlug();
    const Access access = forkmesh::projects::loadAgentAccess(project.dir);

    auto *body = new QWidget;
    body->setObjectName(QStringLiteral("projectSettingsPage"));
    body->setProperty("projectSlug", slug);
    auto *column = new QVBoxLayout(body);
    column->setContentsMargins(20, 18, 20, 20);
    column->setSpacing(8);

    auto sectionLabel = [](const QString &text) {
        auto *label = new QLabel(text);
        label->setObjectName("sectionLabel");
        return label;
    };
    auto hint = [](const QString &text) {
        auto *label = new QLabel(text);
        label->setObjectName("modeHint");
        label->setWordWrap(true);
        return label;
    };
    auto ghostButton = [](const QString &text) {
        auto *button = new QPushButton(text);
        button->setObjectName("ghostButton");
        button->setCursor(Qt::PointingHandCursor);
        return button;
    };

    auto *heading = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("%1 settings").arg(project.name));
    title->setObjectName(QStringLiteral("pageTitle"));
    heading->addWidget(title);
    heading->addStretch();
    auto *back = ghostButton(QStringLiteral("Back"));
    back->setToolTip(QStringLiteral("Return to the previous page without saving"));
    setOcticon(back, QStringLiteral("arrow-left"), 14);
    connect(back, &QPushButton::clicked, body, [this] { leaveProjectSettingsPage(); });
    heading->addWidget(back);
    column->addLayout(heading);

    auto *header = new QHBoxLayout;
    auto *logo = new QLabel;
    logo->setPixmap(projectLogoPixmap(project, 40));
    logo->setFixedSize(40, 40);
    auto *folderLine = new QLabel(QDir::toNativeSeparators(project.dir));
    folderLine->setObjectName("statusLine");
    folderLine->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *openFolder = ghostButton(QStringLiteral("Open folder"));
    connect(openFolder, &QPushButton::clicked, body, [dir = project.dir] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });
    header->addWidget(logo);
    header->addWidget(folderLine, 1);
    header->addWidget(openFolder);
    column->addLayout(header);

    column->addWidget(sectionLabel(QStringLiteral("GENERAL")));
    auto *nameEdit = new QLineEdit(project.name);
    nameEdit->setObjectName(QStringLiteral("projectSettingsName"));
    nameEdit->setPlaceholderText(QStringLiteral("Project name"));
    nameEdit->setMaxLength(80);
    auto *descriptionEdit = new QLineEdit(project.description);
    descriptionEdit->setObjectName(QStringLiteral("projectSettingsDescription"));
    descriptionEdit->setPlaceholderText(QStringLiteral("What is it for? (optional)"));
    descriptionEdit->setMaxLength(400);
    column->addWidget(nameEdit);
    column->addWidget(descriptionEdit);

    auto *followGlobal = new QCheckBox(QStringLiteral("Follow the global agent rules"));
    followGlobal->setObjectName(QStringLiteral("projectFollowsGlobalAgentRules"));
    followGlobal->setChecked(access.followGlobal);
    followGlobal->setToolTip(QStringLiteral(
        "Take the folder boundary and external-call setting from Settings > Global agent "
        "rules. Folders and destinations added here still apply to this project."));
    auto *globalRules = ghostButton(QStringLiteral("Global agent rules…"));
    auto *followRow = new QHBoxLayout;
    followRow->addWidget(followGlobal, 1);
    followRow->addWidget(globalRules);
    column->addWidget(sectionLabel(QStringLiteral("AGENT RULES")));
    column->addLayout(followRow);
    auto *accessEditor = new AgentAccessEditor;
    const forkmesh::projects::GlobalAgentRules global = globalAgentRules();
    auto showAccess = [accessEditor, access, global](bool follow) {
        forkmesh::projects::AgentAccess shown = access;
        if (follow) {
            shown.folders = global.defaults.folders;
            shown.network = global.defaults.network;
            shown.sharedTools = global.defaults.sharedTools;
        }
        accessEditor->setAccess(shown);
        accessEditor->setChoicesEnabled(!follow);
    };
    showAccess(access.followGlobal);
    connect(followGlobal, &QCheckBox::toggled, body, [accessEditor, showAccess](bool follow) {
        forkmesh::projects::AgentAccess edited = accessEditor->access();
        showAccess(follow);
        forkmesh::projects::AgentAccess kept = accessEditor->access();
        kept.readOnlyFolders = edited.readOnlyFolders;
        kept.readWriteFolders = edited.readWriteFolders;
        kept.allowedHosts = edited.allowedHosts;
        kept.allowedDestructive = edited.allowedDestructive;
        kept.allowedDeletePaths = edited.allowedDeletePaths;
        accessEditor->setAccess(kept);
        accessEditor->setChoicesEnabled(!follow);
    });
    connect(globalRules, &QPushButton::clicked, body, [this] { showGlobalAgentRules(); });
    column->addWidget(accessEditor);

    column->addWidget(hint(
        isCurrent
            ? QStringLiteral("Applies to agents started after you save. Running agents keep "
                             "the folders they started with.")
            : QStringLiteral("Applies to agents that project starts after you save.")));
    auto *errorLabel = new QLabel;
    errorLabel->setObjectName(QStringLiteral("projectSettingsError"));
    errorLabel->setStyleSheet(Theme::remapSheet("color:#ff6b6b; background:transparent;"));
    errorLabel->setWordWrap(true);
    errorLabel->hide();
    column->addWidget(errorLabel);

    auto *buttons = new QHBoxLayout;
    auto *deleteButton = ghostButton(QStringLiteral("Remove project…"));
    if (project.isHome || isCurrent) {
        deleteButton->setEnabled(false);
        deleteButton->setToolTip(
            project.isHome ? QStringLiteral("The ForkMesh project itself cannot be removed.")
                           : QStringLiteral("This window is using the project. Switch to "
                                            "another project to remove it."));
    }
    auto *cancelButton = new QPushButton(QStringLiteral("Cancel"));
    cancelButton->setCursor(Qt::PointingHandCursor);
    auto *saveButton = new QPushButton(QStringLiteral("Save"));
    saveButton->setObjectName("primaryButton");
    saveButton->setCursor(Qt::PointingHandCursor);
    buttons->addWidget(deleteButton);
    buttons->addStretch(1);
    buttons->addWidget(cancelButton);
    buttons->addWidget(saveButton);
    column->addLayout(buttons);
    column->addStretch(1);

    connect(cancelButton, &QPushButton::clicked, body, [this] { leaveProjectSettingsPage(); });
    connect(deleteButton, &QPushButton::clicked, body,
            [this, project, errorLabel = QPointer<QLabel>(errorLabel)] {
        QMessageBox box(this);
        box.setIcon(QMessageBox::Warning);
        box.setWindowTitle(QStringLiteral("Remove project"));
        box.setTextFormat(Qt::PlainText);
        box.setText(QStringLiteral("Remove %1?").arg(project.name));
        box.setInformativeText(
            project.ownsFolder
                ? QStringLiteral("Keep folder leaves every file in\n\n%1\n\nand the project "
                                 "just stops being listed.\n\nMove folder aside also renames "
                                 "the project folder. It is kept as\n\n%1.deleted-<timestamp>"
                                 "\n\nIts identity key, settings and data are not erased. "
                                 "Delete that folder yourself if you no longer need it.")
                      .arg(QDir::toNativeSeparators(project.dir))
                : QStringLiteral("%1\n\nForkMesh did not create this folder, so it only "
                                 "stops listing it. Every file stays where it is.")
                      .arg(QDir::toNativeSeparators(project.dir)));
        if (auto *info = box.findChild<QLabel *>(QStringLiteral("qt_msgbox_informativelabel")))
            info->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                          Qt::TextSelectableByKeyboard);
        QPushButton *keep =
            box.addButton(QStringLiteral("Keep folder"), QMessageBox::AcceptRole);
        QPushButton *discard =
            project.ownsFolder
                ? box.addButton(QStringLiteral("Move folder aside"),
                                QMessageBox::DestructiveRole)
                : nullptr;
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(keep);
        box.exec();
        if (box.clickedButton() != keep && (!discard || box.clickedButton() != discard))
            return;
        const bool keptFolder = box.clickedButton() == keep;
        QString error;
        QString movedFolder;
        if (!forkmesh::projects::remove(forkmesh::paths::projectsRoot(), project.slug,
                                        keptFolder, &error, &movedFolder)) {
            if (errorLabel) {
                errorLabel->setText(error);
                errorLabel->show();
            }
            if (!QFileInfo(QDir(project.dir).filePath(
                               QLatin1String(forkmesh::paths::kProjectFileName)))
                     .isFile()) {
                leaveProjectSettingsPage();
                flashMessage(error, true);
            }
            return;
        }
        leaveProjectSettingsPage();
        flashMessage(keptFolder
                         ? QStringLiteral("Removed %1. Its folder is kept at %2.")
                               .arg(project.name, QDir::toNativeSeparators(project.dir))
                         : QStringLiteral("Removed %1. The project folder is kept as %2 — its "
                                          "identity key, settings and data are not erased. "
                                          "Delete that folder yourself if you no longer "
                                          "need it.")
                               .arg(project.name, QDir::toNativeSeparators(movedFolder)),
                     false, QString());
    });
    connect(saveButton, &QPushButton::clicked, body,
            [this, project, nameEdit, descriptionEdit, followGlobal, accessEditor, errorLabel] {
                Access saved = accessEditor->access();
                saved.followGlobal = followGlobal->isChecked();

                QString error;
                const bool detailsChanged =
                    nameEdit->text().trimmed() != project.name ||
                    descriptionEdit->text().trimmed() != project.description;
                if ((detailsChanged &&
                     !forkmesh::projects::updateDetails(project.dir, nameEdit->text(),
                                                        descriptionEdit->text(), &error)) ||
                    !forkmesh::projects::saveAgentAccess(project.dir, saved, &error)) {
                    errorLabel->setText(error);
                    errorLabel->show();
                    return;
                }
                const QString savedName = nameEdit->text().trimmed();
                if (m_agentTranscript)
                    m_agentTranscript->refreshCommandApprovals();
                updateProjectSwitcher();
                flashMessage(QStringLiteral("Saved %1’s settings.").arg(savedName));
                if (m_projectSettingsSlug == project.slug)
                    loadProjectSettingsPage(project.slug);
            });
    connect(nameEdit, &QLineEdit::returnPressed, saveButton, &QPushButton::click);
    connect(descriptionEdit, &QLineEdit::returnPressed, saveButton, &QPushButton::click);

    if (QWidget *previous = m_projectSettingsScroll->takeWidget())
        previous->deleteLater();
    m_projectSettingsScroll->setWidget(body);
    m_projectSettingsScroll->verticalScrollBar()->setValue(0);
    return true;
}

#ifdef FORKMESH_WINDOW_TESTS
bool MainWindow::testProjectSettingsShowing() const
{
    return m_sectionStack && m_sectionStack->currentIndex() == kProjectSettingsSectionIndex;
}
#endif
