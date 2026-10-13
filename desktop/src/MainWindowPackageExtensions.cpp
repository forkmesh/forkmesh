#include "MainWindow.h"

#include "ForkMeshPackageRegistry.h"
#include "MainWindowInternal.h"

#include <QHBoxLayout>
#include <QSet>
#include <QTabWidget>

using namespace forkmesh::ui;

void MainWindow::logDesktopPackageActivity(const QString &text,
                                           const QString &badge)
{
    if (badge.trimmed().isEmpty())
        return;
    logSystemFrom(text, QString(), 0, forkmesh::LogSeverity::Automatic,
                  badge.trimmed().toUpper());
}

void MainWindow::ensureDesktopPackageLogPages()
{
    if (!m_logViewTabs)
        return;
    for (auto &[id, extension] : m_desktopPackageExtensions) {
        if (m_desktopPackageLogPages.contains(id) ||
            extension->logPageTitle().isEmpty())
            continue;
        QWidget *page = extension->createLogPage(m_logViewTabs);
        if (!page)
            continue;
        const int index = m_logViewTabs->addTab(page, extension->logPageTitle());
        m_logViewTabs->setTabToolTip(index, extension->logPageToolTip());
        m_desktopPackageLogPages.insert(id, page);
    }
}

void MainWindow::refreshCurrentDesktopPackageLogPage()
{
    if (!m_logViewTabs)
        return;
    QWidget *current = m_logViewTabs->currentWidget();
    for (auto &[id, extension] : m_desktopPackageExtensions) {
        if (m_desktopPackageLogPages.value(id) == current) {
            extension->refresh();
            return;
        }
    }
}

void MainWindow::refreshDesktopPackageExtensions()
{
    for (auto &[id, extension] : m_desktopPackageExtensions) {
        Q_UNUSED(id);
        extension->refresh();
    }
}

void MainWindow::rebuildBranchPackageActions()
{
    if (!m_branchPackageActions)
        return;
    auto *row = qobject_cast<QHBoxLayout *>(m_branchPackageActions->layout());
    if (!row)
        return;
    const auto stale = m_branchPackageActions->findChildren<QPushButton *>(
        QString(), Qt::FindDirectChildrenOnly);
    for (QPushButton *button : stale) {
        row->removeWidget(button);
        button->setParent(nullptr);
        button->deleteLater();
    }
    for (auto &[id, extension] : m_desktopPackageExtensions) {
        Q_UNUSED(id);
        for (const forkmesh::market::DesktopBranchAction &action :
             extension->branchActions()) {
            if (action.label.isEmpty() || !action.trigger)
                continue;
            auto *button = new StackedIconButton(action.label, m_branchPackageActions);
            button->setObjectName("ghostButton");
            button->setProperty("buttonSize", "sm");
            button->setProperty("packageActionToolTip", action.toolTip);
            button->setCursor(Qt::PointingHandCursor);
            if (!action.icon.isEmpty())
                setOcticon(button, action.icon, 14);
            const auto trigger = action.trigger;
            connect(button, &QPushButton::clicked, this, [this, trigger] {
                const QString branch = m_branchDiffBranch;
                const QString repoPath = repoGitDir();
                if (branch.isEmpty() || repoPath.isEmpty()) {
                    setRepoDetailNotice("No local checkout to open.", true);
                    return;
                }
                QString dir = worktreePathForBranch(repoPath, branch);
                if (dir.isEmpty())
                    dir = repoPath;
                trigger({branch, dir});
            });
            row->addWidget(button);
        }
    }
    refreshBranchPackageActions(m_branchDiffBranch, repoGitDir());
}

void MainWindow::refreshBranchPackageActions(const QString &branch,
                                             const QString &dir)
{
    if (!m_branchPackageActions)
        return;
    const auto buttons = m_branchPackageActions->findChildren<QPushButton *>(
        QString(), Qt::FindDirectChildrenOnly);
    const bool canOpen = !branch.isEmpty() && !dir.isEmpty();
    for (QPushButton *button : buttons) {
        button->setEnabled(canOpen);
        button->setToolTip(
            canOpen ? button->property("packageActionToolTip").toString()
                    : QStringLiteral("No local checkout to open"));
    }
    m_branchPackageActions->setVisible(!buttons.isEmpty());
}

// code-health: allow(deep-nested-loop) runs on package install/uninstall only; the loops span the few compiled-in desktop package extensions (7 today) and their handful of log categories
void MainWindow::syncDesktopPackageExtensions()
{
    QMap<QString, QString> wanted;
    if (m_packageRegistry && m_packageRegistry->isReady()) {
        for (const forkmesh::market::PackageManifest &manifest :
             m_packageRegistry->packages()) {
            if (m_packageRegistry->isInstalledHere(manifest.id) &&
                forkmesh::market::hasDesktopPackageExtension(
                    manifest.desktopEntrypoint)) {
                wanted.insert(manifest.id, manifest.desktopEntrypoint);
            }
        }
    }

    for (auto it = m_desktopPackageExtensions.begin();
         it != m_desktopPackageExtensions.end();) {
        if (wanted.contains(it->first)) {
            ++it;
            continue;
        }
        const QString id = it->first;
        auto &extension = it->second;
        extension->deactivate();
        if (QWidget *page = m_desktopPackageLogPages.take(id)) {
            const int index = m_logViewTabs ? m_logViewTabs->indexOf(page) : -1;
            if (index >= 0) {
                if (m_logViewTabs->currentIndex() == index)
                    m_logViewTabs->setCurrentIndex(0);
                m_logViewTabs->removeTab(index);
            }
            delete page;
        }
        const auto categories = extension->logCategories();
        it = m_desktopPackageExtensions.erase(it);
        for (const auto &category : categories) {
            bool stillUsed = false;
            for (const auto &[otherId, other] : m_desktopPackageExtensions) {
                Q_UNUSED(otherId);
                for (const auto &candidate : other->logCategories()) {
                    if (candidate.badge == category.badge) {
                        stillUsed = true;
                        break;
                    }
                }
                if (stillUsed)
                    break;
            }
            if (!stillUsed)
                LogActivityLights::unregisterCategory(category.badge);
        }
    }

    for (auto it = wanted.cbegin(); it != wanted.cend(); ++it) {
        if (m_desktopPackageExtensions.find(it.key()) !=
            m_desktopPackageExtensions.end())
            continue;
        auto extension =
            forkmesh::market::createDesktopPackageExtension(it.value());
        if (!extension || extension->packageId() != it.key())
            continue;
        QSet<QString> allowedBadges;
        for (const auto &category : extension->logCategories()) {
            if (LogActivityLights::registerCategory(
                    category.badge, category.accent, category.icon))
                allowedBadges.insert(category.badge.trimmed().toUpper());
        }
        forkmesh::market::DesktopPackageContext context;
        context.window = this;
        context.network = m_networkAccess;
        context.logActivity =
            [this, allowedBadges](const QString &text, const QString &badge) {
                const QString normalized = badge.trimmed().toUpper();
                if (allowedBadges.contains(normalized))
                    logDesktopPackageActivity(text, normalized);
            };
        context.notify = [this](const QString &text, bool error) {
            flashMessage(text, error);
        };
        context.darkTheme = [] { return currentThemeIsDark(); };
        context.icon = [](const QString &name, int size) {
            return QIcon(tintedOcticonPixmap(
                name,
                Theme::nextOr(
                    Theme::Role::FgSecondary,
                    QColor(currentThemeIsDark() ? "#adbac7" : "#24292f")),
                size));
        };
        extension->activate(context);
        m_desktopPackageExtensions.emplace(it.key(), std::move(extension));
    }

    if (m_logActivityLights)
        m_logActivityLights->categoryRegistryChanged();
    if (m_logActivityHeader)
        m_logActivityHeader->categoryRegistryChanged();
    rebuildLogFilterButtons();
    ensureDesktopPackageLogPages();
    rebuildBranchPackageActions();
}

void MainWindow::clearDesktopPackageExtensions()
{
    if (m_desktopPackageExtensions.empty())
        return;
    for (auto &[id, extension] : m_desktopPackageExtensions) {
        extension->deactivate();
        if (QWidget *page = m_desktopPackageLogPages.take(id)) {
            if (m_logViewTabs) {
                const int index = m_logViewTabs->indexOf(page);
                if (index >= 0)
                    m_logViewTabs->removeTab(index);
            }
            delete page;
        }
        for (const auto &category : extension->logCategories())
            LogActivityLights::unregisterCategory(category.badge);
    }
    m_desktopPackageExtensions.clear();
}
