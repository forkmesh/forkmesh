#include "MainWindow.h"

#include "ForkMeshPackageRegistry.h"
#include "MainWindowInternal.h"
#include "NativePackageExtension.h"
#include "NoteListEntry.h"

#include <QBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QMenu>
#include <QTimer>

using forkmesh::market::NativePackageContribution;
using forkmesh::market::NativePackageWidget;
using forkmesh::market::PackageManifest;

namespace {

const QString kChromeLeft = QStringLiteral("left");
const QString kChromeCenter = QStringLiteral("center");
const QString kChromeRight = QStringLiteral("right");

}

QPushButton *MainWindow::nativePackageNavigation(const QString &id) const
{
    for (const NativePackagePage &page : m_nativePackagePages) {
        if (page.id == id)
            return page.navigation;
    }
    return nullptr;
}

void MainWindow::syncNativePackageChrome()
{
    QList<QPair<QString, QString>> desired;
    if (m_packageRegistry && m_packageRegistry->isReady()) {
        for (const PackageManifest &manifest : m_packageRegistry->packages()) {
            if (!m_packageRegistry->isInstalledHere(manifest.id))
                continue;
            const NativePackageContribution *contribution =
                forkmesh::market::nativePackageContribution(
                    manifest.desktopEntrypoint, manifest.type);
            if (contribution && contribution->chromeFactory)
                desired.append({manifest.id, manifest.desktopEntrypoint});
        }
    }
    for (int index = int(m_nativePackageChrome.size()) - 1; index >= 0; --index) {
        const NativePackageChrome &chrome = m_nativePackageChrome.at(index);
        if (chrome.widget &&
            desired.contains(qMakePair(chrome.id, chrome.entrypoint)))
            continue;
        if (chrome.widget) {
            NativePackageWidget *widget = chrome.widget;
            widget->hide();
            widget->setParent(nullptr);
            widget->deleteLater();
        }
        m_nativePackageChrome.removeAt(index);
    }
    for (const auto &[id, entrypoint] : desired) {
        const bool present = std::any_of(
            m_nativePackageChrome.cbegin(), m_nativePackageChrome.cend(),
            [&id](const NativePackageChrome &chrome) { return chrome.id == id; });
        if (present)
            continue;
        const PackageManifest *manifest = m_packageRegistry->package(id);
        const NativePackageContribution *contribution =
            manifest ? forkmesh::market::nativePackageContribution(
                           entrypoint, manifest->type)
                     : nullptr;
        if (!contribution || !contribution->chromeFactory)
            continue;
        const NativePackageContribution copy = *contribution;
        NativePackageWidget *widget =
            copy.chromeFactory(nativePackageHost(*manifest, copy), nullptr);
        if (!widget)
            continue;
        widget->setProperty("forkmeshPackageId", id);
        widget->hide();
        m_nativePackageChrome.append({id, entrypoint, widget});
        placeNativePackageChrome(id);
    }
}

void MainWindow::installNativePackageChromeSlots(QWidget *chrome,
                                                 QBoxLayout *left,
                                                 QBoxLayout *center,
                                                 QBoxLayout *right,
                                                 QWidget *rightBefore)
{
    m_nativePackageChromeBar = chrome;
    m_nativePackageChromeSlots.clear();
    m_nativePackageChromeSlots.insert(kChromeLeft, {left, nullptr});
    m_nativePackageChromeSlots.insert(kChromeCenter, {center, nullptr});
    m_nativePackageChromeSlots.insert(kChromeRight, {right, rightBefore});
    for (const NativePackageChrome &entry : std::as_const(m_nativePackageChrome))
        placeNativePackageChrome(entry.id);
}

void MainWindow::placeNativePackageChrome(const QString &id)
{
    for (const NativePackageChrome &chrome : std::as_const(m_nativePackageChrome)) {
        if (chrome.id != id || !chrome.widget)
            continue;
        NativePackageWidget *widget = chrome.widget;
        QString key = widget->chromeSlot();
        if (key != kChromeCenter && key != kChromeRight)
            key = kChromeLeft;
        const NativePackageChromeSlot target = m_nativePackageChromeSlots.value(key);
        if (!target.row)
            return;
        if (target.row->indexOf(widget) < 0) {
            for (auto it = m_nativePackageChromeSlots.constBegin();
                 it != m_nativePackageChromeSlots.constEnd(); ++it) {
                if (it.value().row)
                    it.value().row->removeWidget(widget);
            }
            const int index =
                target.before ? target.row->indexOf(target.before) : -1;
            target.row->insertWidget(index, widget, 0, Qt::AlignVCenter);
        }
        widget->show();
        return;
    }
}

QString MainWindow::nativePackageChromeSlotAt(const QPoint &globalPos) const
{
    if (!m_nativePackageChromeBar)
        return kChromeLeft;
    const int width = qMax(1, m_nativePackageChromeBar->width());
    const int x = m_nativePackageChromeBar->mapFromGlobal(globalPos).x();
    return x < width / 3       ? kChromeLeft
           : x > width * 2 / 3 ? kChromeRight
                               : kChromeCenter;
}

void MainWindow::addNativePackageChromeMenuActions(QMenu *menu)
{
    if (!menu)
        return;
    for (const NativePackageChrome &chrome : std::as_const(m_nativePackageChrome)) {
        if (chrome.widget)
            chrome.widget->addChromeMenuActions(menu);
    }
}

bool MainWindow::activateNativePackageChrome(const QString &id)
{
    for (const NativePackageChrome &chrome : std::as_const(m_nativePackageChrome)) {
        if (chrome.id == id && chrome.widget) {
            chrome.widget->activate();
            return true;
        }
    }
    return false;
}

bool MainWindow::runWindowCommand(const QString &command, const QJsonObject &args)
{
    const QString title = args.value(QStringLiteral("title")).toString();
    const QString markdown = args.value(QStringLiteral("markdown")).toString();
    const QString link = args.value(QStringLiteral("link")).toString();
    if (command == QLatin1String("task.composeFromText")) {
        const QJsonObject compose{
            {QStringLiteral("heading"), args.value(QStringLiteral("heading"))},
            {QStringLiteral("title"),
             NoteListEntry::conversionTitle(title, markdown,
                                            kOrganizationTaskTitleLimit)},
            {QStringLiteral("details"),
             NoteListEntry::conversionDetails(title, markdown, link)},
            {QStringLiteral("done"), args.value(QStringLiteral("done"))},
        };
        if (!runNativePackageCommand(command, compose)) {
            flashMessage(QStringLiteral(
                             "Tasks is not installed. Install it from the Market "
                             "list to turn notes into tasks."),
                         true, QString(), forkmesh::kLocalOperationNoticeKind);
            return false;
        }
        return true;
    }
    if (runTaskWindowCommand(command, args))
        return true;
    if (command == QLatin1String("issue.composeFromText")) {
        if (m_repositories.isEmpty()) {
            flashMessage(QStringLiteral(
                "Open a repository before converting a note into an issue."), true);
            return false;
        }
        int repoIndex = m_repoDetailIndex >= 0 &&
                                m_repoDetailIndex < m_repositories.size()
                            ? m_repoDetailIndex
                            : 0;
        if (m_repositories.size() > 1) {
            QStringList labels;
            labels.reserve(m_repositories.size());
            for (const RepositoryRecord &repo : std::as_const(m_repositories))
                labels << repo.owner + QLatin1Char('/') + repo.name;
            bool ok = false;
            const QString pick = QInputDialog::getItem(
                this, args.value(QStringLiteral("heading")).toString(),
                QStringLiteral("File this note as an issue in:"), labels,
                repoIndex, false, &ok);
            if (!ok || pick.isEmpty())
                return false;
            repoIndex = labels.indexOf(pick);
            if (repoIndex < 0)
                return false;
        }
        if (repoIndex != m_repoDetailIndex)
            openRepoDetail(repoIndex);
        showSection(0);
        if (m_repoDetailTabs && m_repoDetailTabs->button(2))
            m_repoDetailTabs->button(2)->click();
        if (m_issuesRepoCombo) {
            const int combo = m_issuesRepoCombo->findData(repoIndex);
            if (combo >= 0)
                m_issuesRepoCombo->setCurrentIndex(combo);
        }
        if (issuesRepoIndex() < 0) {
            flashMessage(QStringLiteral("Could not open the issue composer for %1.")
                             .arg(m_repositories.at(repoIndex).owner +
                                  QLatin1Char('/') +
                                  m_repositories.at(repoIndex).name),
                         true);
            return false;
        }
        composeNewIssue(
            NoteListEntry::conversionTitle(title, markdown, kIssueTitleLimit),
            NoteListEntry::conversionDetails(title, markdown, link));
        return true;
    }
    return false;
}

bool MainWindow::takeQuickAddNote(const QString &text)
{
    const QString command = QStringLiteral("note.add");
    if (!hasNativePackageCommand(command)) {
        flashMessage(QStringLiteral(
                         "Notes is not installed. Install it from the Market list."),
                     true, QString(), forkmesh::kLocalOperationNoticeKind);
        return false;
    }
    QJsonArray attachments;
    for (const QString &path : std::as_const(m_quickAddImages))
        attachments.append(path);
    if (!runNativePackageCommand(
            command, QJsonObject{{QStringLiteral("text"), text},
                                 {QStringLiteral("attachments"), attachments}}))
        return false;
    if (m_issueQuickAdd)
        m_issueQuickAdd->clear();
    clearQuickAddImages();
    return true;
}

bool MainWindow::openNoteLink(const QString &href)
{
    return runNativePackageCommand(QStringLiteral("note.openLink"),
                                   QJsonObject{{QStringLiteral("href"), href}});
}

void MainWindow::openNoteFromSystemLink(const QString &target)
{
    openNoteFromSystemLink(target, 0);
}

void MainWindow::openNoteFromSystemLink(const QString &target, int attempt)
{
    if ((!m_packageRegistry || !m_packageRegistry->isReady()) && attempt < 60) {
        QTimer::singleShot(500, this, [this, target, attempt] {
            openNoteFromSystemLink(target, attempt + 1);
        });
        return;
    }
    if (!runNativePackageCommand(QStringLiteral("note.openSystemLink"),
                                 QJsonObject{{QStringLiteral("target"), target}}))
        flashMessage(QStringLiteral(
                         "Notes is not installed. Install it from the Market list "
                         "to open note links."),
                     true, QString(), forkmesh::kLocalOperationNoticeKind);
}

bool MainWindow::hasNativePackageCommand(const QString &command) const
{
    for (const NativePackageChrome &chrome : m_nativePackageChrome) {
        if (chrome.widget && chrome.widget->acceptsHostCommand(command))
            return true;
    }
    for (const NativePackagePage &page : m_nativePackagePages) {
        if (page.widget && page.widget->acceptsHostCommand(command))
            return true;
    }
    return false;
}

bool MainWindow::runNativePackageCommand(const QString &command,
                                         const QJsonObject &args)
{
    QList<QPointer<NativePackageWidget>> widgets;
    for (const NativePackageChrome &chrome : std::as_const(m_nativePackageChrome))
        widgets.append(chrome.widget);
    for (const NativePackagePage &page : std::as_const(m_nativePackagePages))
        widgets.append(page.widget);
    for (const QPointer<NativePackageWidget> &widget : std::as_const(widgets)) {
        if (widget && widget->acceptsHostCommand(command))
            return widget->hostCommand(command, args);
    }
    return false;
}

bool MainWindow::addUsageResetCountdown(const QString &label, qint64 resetAtMs)
{
    const QString command = QStringLiteral("countdown.add");
    if (!hasNativePackageCommand(command)) {
        flashMessage(QStringLiteral(
                         "Timers is not installed. Install it from the Market "
                         "list."),
                     true, QString(), forkmesh::kLocalOperationNoticeKind);
        return false;
    }
    return runNativePackageCommand(
        command, QJsonObject{{QStringLiteral("label"), label},
                             {QStringLiteral("endsAt"), double(resetAtMs)}});
}

QUrl MainWindow::nativePackageAccountUrl(
    const QString &id, const NativePackageContribution &contribution,
    const QString &path) const
{
    Q_UNUSED(id);
    const QUrl relative(path);
    if (!relative.isValid() || !relative.isRelative() || relative.hasFragment())
        return {};
    const QString cleanPath = relative.path(QUrl::FullyDecoded);
    const QStringList parts = cleanPath.split(QLatin1Char('/'));
    if (!cleanPath.startsWith(QLatin1Char('/')) ||
        cleanPath.contains(QStringLiteral("//")) ||
        parts.contains(QStringLiteral(".")) || parts.contains(QStringLiteral("..")))
        return {};
    QStringList prefixes = contribution.accountPaths;
    prefixes.prepend(contribution.apiPrefix);
    const bool inside = std::any_of(
        prefixes.cbegin(), prefixes.cend(), [&cleanPath](const QString &prefix) {
            return !prefix.isEmpty() &&
                   (cleanPath == prefix ||
                    cleanPath.startsWith(prefix + QLatin1Char('/')));
        });
    if (!inside)
        return {};
    QUrl url = catalogApiUrl();
    url.setPath(cleanPath);
    url.setQuery(relative.query());
    url.setFragment(QString());
    return url;
}

#ifdef FORKMESH_WINDOW_TESTS
NativePackageWidget *MainWindow::testNativePackagePage(const QString &id) const
{
    for (const NativePackagePage &page : m_nativePackagePages) {
        if (page.id == id)
            return page.widget;
    }
    return nullptr;
}

forkmesh::market::NativePackageHost MainWindow::testNativePackageHost(const QString &id)
{
    const PackageManifest *manifest =
        m_packageRegistry ? m_packageRegistry->package(id) : nullptr;
    const NativePackageContribution *contribution =
        manifest ? forkmesh::market::nativePackageContribution(
                       manifest->desktopEntrypoint, manifest->type)
                 : nullptr;
    if (!contribution)
        return {};
    return nativePackageHost(*manifest, *contribution);
}

NativePackageWidget *MainWindow::testNativePackageChrome(const QString &id) const
{
    for (const NativePackageChrome &chrome : m_nativePackageChrome) {
        if (chrome.id == id)
            return chrome.widget;
    }
    return nullptr;
}

QString MainWindow::testNativePackageChromeSlot(const QString &id) const
{
    NativePackageWidget *widget = testNativePackageChrome(id);
    if (!widget)
        return {};
    for (auto it = m_nativePackageChromeSlots.constBegin();
         it != m_nativePackageChromeSlots.constEnd(); ++it) {
        if (it.value().row && it.value().row->indexOf(widget) >= 0)
            return it.key();
    }
    return {};
}
#endif
