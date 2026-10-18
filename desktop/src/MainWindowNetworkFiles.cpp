#include "MainWindow.h"
#include "AppPaths.h"
#include "FileTreeSupport.h"
#include "MainWindowInternal.h"
#include "NetworkFilesPanel.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTreeWidget>
#include <QtConcurrent/QtConcurrentRun>

using namespace forkmesh::ui;
using forkmesh::smb::ClipboardItem;
using forkmesh::smb::NetworkFilesPanel;

QWidget *MainWindow::buildFilesNetworkPanel()
{
    auto *panel = new NetworkFilesPanel(forkmesh::paths::configRoot() + QStringLiteral("/network-share-passwords.json"));
    panel->folderIcon = [this](bool open) { return iconForDir(open); };
    panel->fileIcon = [this](const QString &name) { return iconForFile(name); };
    panel->decorateButton = [](QPushButton *button, const QString &icon) { setOcticon(button, icon, 14); };
    panel->notify = [this](const QString &message, bool error) { flashMessage(message, error); };
    panel->openText = [this](const QString &url, const QString &title, const QString &text, bool editable) {
        openNetworkFileTab(url, title, text, editable);
    };
    panel->statusChanged = [this](const QString &text) {
        if (m_filesTreeStack && m_filesTreeStack->currentIndex() == 3 && m_fileExplorerStatus)
            m_fileExplorerStatus->setText(text);
    };
    enableHoverRowHighlight(panel->tree());
    m_networkFilesPanel = panel;
    return panel;
}

void MainWindow::showFilesNetworkTree()
{
    if (!m_filesTreeStack || m_filesTreeStack->count() < 4 || !m_networkFilesPanel)
        return;
    m_filesTreeStack->setCurrentIndex(3);
    if (m_filesRepoTreeButton)
        m_filesRepoTreeButton->setChecked(false);
    if (m_filesCoveTreeButton)
        m_filesCoveTreeButton->setChecked(false);
    if (m_filesNetworkTreeButton)
        m_filesNetworkTreeButton->setChecked(true);
    m_networkFilesPanel->activate();
    if (m_fileExplorerStatus) {
        const QString url = m_networkFilesPanel->currentUrl();
        m_fileExplorerStatus->setText(url.isEmpty() ? QStringLiteral("Folders shared on your network (SMB)") : url);
    }
}

void MainWindow::openNetworkShareLocation(const QString &url, bool guest)
{
    ensureSectionBuilt(kFilesSectionIndex);
    showSection(kFilesSectionIndex);
    if (m_filesNavButton)
        m_filesNavButton->setChecked(true);
    if (m_networkFilesPanel)
        m_networkFilesPanel->openUrl(url, guest);
    showFilesNetworkTree();
}

void MainWindow::openNetworkFileTab(const QString &url, const QString &title, const QString &text, bool editable)
{
    ensureSectionBuilt(kFilesSectionIndex);
    if (!m_repoFileTabs)
        return;
    if (QWidget *open = m_openFileTabs.value(url)) {
        m_repoFileTabs->setCurrentWidget(open);
        showSection(kFilesSectionIndex);
        return;
    }
    auto *editor = new CodePreviewEditor(url);
    editor->setProperty("networkFile", url);
    editor->setReadOnly(!editable);
    editor->setPlainText(text);
    editor->document()->setModified(false);
    new CodePreviewHighlighter(editor->document(), title);
    wireFilesEditor(editor);
    const int index = m_repoFileTabs->addTab(editor, iconForFile(title), title);
    m_repoFileTabs->setTabToolTip(index, url);
    m_repoFileTabs->setCurrentIndex(index);
    m_openFileTabs.insert(url, editor);
    updateRepoFileSaveActions();
    showSection(kFilesSectionIndex);
}

void MainWindow::copyLocalFilesToClipboard(const QStringList &paths, bool move)
{
    forkmesh::smb::FileClipboard &clipboard = forkmesh::smb::fileClipboard();
    clipboard.items.clear();
    clipboard.move = move;
    for (const QString &path : paths) {
        const QFileInfo info(path);
        ClipboardItem item;
        item.localPath = info.absoluteFilePath();
        item.name = info.fileName();
        item.folder = info.isDir();
        clipboard.items << item;
    }
    if (m_networkFilesPanel)
        m_networkFilesPanel->reload();
    flashMessage(QStringLiteral("%1 %2. Open a folder here or on a network share and paste.")
                     .arg(paths.size() == 1 ? QFileInfo(paths.first()).fileName() : QStringLiteral("%1 items").arg(paths.size()),
                          move ? QStringLiteral("ready to move") : QStringLiteral("copied")));
}

void MainWindow::pasteIntoLocalFolder(const QString &folder)
{
    if (!m_networkFilesPanel || folder.isEmpty())
        return;
    QPointer<MainWindow> self(this);
    m_networkFilesPanel->pasteIntoLocal(folder, [self](bool ok) {
        if (!self)
            return;
        self->setFileExplorerRoot(self->m_fileExplorerRoot);
        if (ok)
            self->flashMessage(QStringLiteral("Pasted."));
    });
}

void MainWindow::createLocalEntry(const QString &folder, bool folderEntry)
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, folderEntry ? QStringLiteral("New folder") : QStringLiteral("New file"),
                                               folderEntry ? QStringLiteral("Name of the new folder:") : QStringLiteral("Name of the new file:"),
                                               QLineEdit::Normal,
                                               folderEntry ? QStringLiteral("New folder") : QStringLiteral("untitled.txt"), &ok)
                             .trimmed();
    if (!ok || name.isEmpty())
        return;
    if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))) {
        flashMessage(QStringLiteral("A name cannot contain / or \\."), true);
        return;
    }
    const QString path = QDir(folder).filePath(name);
    if (QFileInfo::exists(path)) {
        flashMessage(QStringLiteral("Something with that name is already there."), true);
        return;
    }
    bool created = false;
    if (folderEntry) {
        created = QDir(folder).mkdir(name);
    } else {
        QFile file(path);
        created = file.open(QIODevice::WriteOnly | QIODevice::NewOnly);
    }
    if (!created) {
        flashMessage(QStringLiteral("Could not create %1.").arg(name), true);
        return;
    }
    setFileExplorerRoot(m_fileExplorerRoot);
    if (!folderEntry)
        openLocalFileTab(path);
}

void MainWindow::renameLocalEntry(const QString &path)
{
    const QFileInfo info(path);
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("Rename"), QStringLiteral("New name for %1:").arg(info.fileName()),
                                               QLineEdit::Normal, info.fileName(), &ok)
                             .trimmed();
    if (!ok || name.isEmpty() || name == info.fileName())
        return;
    if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))) {
        flashMessage(QStringLiteral("A name cannot contain / or \\."), true);
        return;
    }
    const QString target = info.dir().filePath(name);
    if (QFileInfo::exists(target)) {
        flashMessage(QStringLiteral("Something with that name is already there."), true);
        return;
    }
    if (!QFile::rename(path, target)) {
        flashMessage(QStringLiteral("Could not rename %1.").arg(info.fileName()), true);
        return;
    }
    QString root = m_fileExplorerRoot;
    if (QDir::cleanPath(root) == QDir::cleanPath(info.absoluteFilePath()))
        root = target;
    setFileExplorerRoot(root);
}

void MainWindow::trashLocalEntries(const QStringList &paths)
{
    if (paths.isEmpty())
        return;
    const QFileInfo first(paths.first());
    const QString title = paths.size() > 1 ? QStringLiteral("Move %1 items to the trash?").arg(paths.size())
        : first.isDir() ? QStringLiteral("Move %1 and everything in it to the trash?").arg(first.fileName())
                        : QStringLiteral("Move %1 to the trash?").arg(first.fileName());
    if (QMessageBox::question(this, title, QStringLiteral("You can restore it from the trash."), QMessageBox::Yes | QMessageBox::Cancel,
                              QMessageBox::Cancel)
        != QMessageBox::Yes)
        return;
    QStringList failed;
    for (const QString &path : paths) {
        if (!QFile::moveToTrash(path))
            failed << QFileInfo(path).fileName();
    }
    if (!failed.isEmpty())
        flashMessage(QStringLiteral("Could not move %1 to the trash.").arg(failed.join(QStringLiteral(", "))), true);
    QString root = m_fileExplorerRoot;
    while (!root.isEmpty() && !QFileInfo::exists(root))
        root = QFileInfo(root).absolutePath();
    setFileExplorerRoot(root);
}

void MainWindow::uploadIntoLocalFolder(const QString &folder)
{
    const QStringList files = QFileDialog::getOpenFileNames(this, QStringLiteral("Copy files into %1").arg(QDir::toNativeSeparators(folder)),
                                                            QStandardPaths::writableLocation(QStandardPaths::HomeLocation));
    if (files.isEmpty())
        return;
    QPointer<MainWindow> self(this);
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [self, watcher] {
        const QString error = watcher->result();
        watcher->deleteLater();
        if (!self)
            return;
        self->flashMessage(error.isEmpty() ? QStringLiteral("Copied.") : error, !error.isEmpty());
        self->setFileExplorerRoot(self->m_fileExplorerRoot);
    });
    watcher->setFuture(QtConcurrent::run([files, folder]() -> QString {
        for (const QString &file : files) {
            const QString name = forkmesh::smb::uniqueLocalName(folder, QFileInfo(file).fileName());
            if (!QFile::copy(file, QDir(folder).filePath(name)))
                return QStringLiteral("Could not copy %1.").arg(QFileInfo(file).fileName());
        }
        return {};
    }));
}

void MainWindow::showLocalEntryInfo(const QString &path)
{
    const QFileInfo info(path);
    QStringList lines = {QStringLiteral("Name: %1").arg(info.fileName()),
                         QStringLiteral("Kind: %1").arg(info.isDir() ? QStringLiteral("Folder")
                                                        : info.suffix().isEmpty() ? QStringLiteral("File")
                                                                                  : QStringLiteral("%1 file").arg(info.suffix().toUpper()))};
    if (!info.isDir())
        lines << QStringLiteral("Size: %1 (%2 bytes)").arg(forkmesh::smb::formatSize(info.size()), QLocale().toString(info.size()));
    lines << QStringLiteral("Modified: %1").arg(QLocale().toString(info.lastModified(), QLocale::LongFormat));
    lines << QStringLiteral("Location: %1").arg(QDir::toNativeSeparators(info.absoluteFilePath()));
    QMessageBox box(this);
    box.setWindowTitle(info.fileName());
    box.setText(lines.join(QLatin1Char('\n')));
    box.setTextInteractionFlags(Qt::TextSelectableByMouse);
    box.exec();
}
