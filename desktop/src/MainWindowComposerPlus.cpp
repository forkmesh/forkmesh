#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "ComposerConnectors.h"
#include "PromptDrop.h"
#include "SettingsSectionsView.h"

#include <QAction>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QTextCursor>

using namespace forkmesh::ui;

namespace {

bool isPromptMedia(const QString &path)
{
    static const QStringList images = {
        QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
        QStringLiteral("gif"), QStringLiteral("webp"), QStringLiteral("bmp")};
    return images.contains(QFileInfo(path).suffix().toLower()) ||
           isVideoAttachmentPath(path);
}

QByteArray readSettingsFile(const QString &path)
{
    QFile file(path);
    if (file.size() > 16 * 1024 * 1024 || !file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

}

QPlainTextEdit *MainWindow::composerTarget() const
{
    return m_composerTarget ? m_composerTarget.data() : m_issueQuickAdd;
}

void MainWindow::installComposerPlusShortcut(QPlainTextEdit *input)
{
    if (!input)
        return;
    auto *shortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_U), input);
    shortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(shortcut, &QShortcut::activated, this,
            [this, target = QPointer<QPlainTextEdit>(input)] {
                if (target && target->isEnabled())
                    addFilesToComposer(target);
            });
}

void MainWindow::openComposerPlusMenu(QWidget *anchor, QPlainTextEdit *input)
{
    if (!anchor || !input)
        return;
    m_composerTarget = input;
    m_composerMenuAnchor = anchor;
    const QColor tone = Theme::color(Theme::Role::FgMuted);
    const auto icon = [&tone](const QString &name) {
        return themedOcticon(name, tone, 16);
    };

    QMenu menu(this);
    menu.setObjectName(QStringLiteral("composerPlusMenu"));
    menu.setToolTipsVisible(true);
    QAction *files = menu.addAction(icon(QStringLiteral("paperclip")),
                                    QStringLiteral("Add files or photos"));
    files->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_U));
    files->setShortcutVisibleInContextMenu(true);
    files->setToolTip(QStringLiteral(
        "Images and videos are attached; other files are mentioned in the prompt"));
    QAction *folder = menu.addAction(icon(QStringLiteral("file-directory")),
                                     QStringLiteral("Add folder"));
    folder->setToolTip(QStringLiteral("Mention a folder in the prompt"));
    QAction *slash = menu.addAction(icon(QStringLiteral("terminal")),
                                    QStringLiteral("Slash commands"));
    slash->setToolTip(QStringLiteral(
        "Agent commands and quick actions (or type / in an empty prompt)"));
    QMenu *connectors = menu.addMenu(icon(QStringLiteral("link")),
                                     QStringLiteral("Connectors"));
    connectors->setObjectName(QStringLiteral("composerConnectorsMenu"));
    connectors->setToolTipsVisible(true);
    populateComposerConnectorsMenu(connectors);
    QAction *plugins = menu.addAction(icon(QStringLiteral("package")),
                                      QStringLiteral("Add plugins"));
    plugins->setToolTip(QStringLiteral("Install plugins from the Market"));

    const QPoint above =
        anchor->mapToGlobal(QPoint(0, -menu.sizeHint().height() - 4));
    QAction *chosen = menu.exec(above);
    if (!chosen)
        return;
    if (chosen == files) {
        addFilesToComposer(input);
    } else if (chosen == folder) {
        addFolderToComposer(input);
    } else if (chosen == slash) {
        openQuickAddSlashActions();
    } else if (chosen == plugins) {
        showSection(kMarketSectionIndex);
    }
}

QStringList MainWindow::composerMentionRoots() const
{
    QStringList roots = {m_streamWorktree.value(m_selectedAgentSessionId)};
    for (const int index : {m_repoDetailIndex, issuesRepoIndex()})
        if (index >= 0 && index < m_repositories.size())
            roots << m_repositories.at(index).localPath;
    return roots;
}

void MainWindow::addFilesToComposer(QPlainTextEdit *input)
{
    if (!input)
        return;
    const QStringList files = QFileDialog::getOpenFileNames(
        this, QStringLiteral("Add files or photos"), QString(),
        QStringLiteral("All files (*);;") + promptAttachmentFileFilter());
    addPathsToComposer(input, files);
}

void MainWindow::addFolderToComposer(QPlainTextEdit *input)
{
    if (!input)
        return;
    QString start;
    for (const QString &root : composerMentionRoots())
        if (!root.isEmpty()) {
            start = root;
            break;
        }
    const QString folder = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Add folder"), start);
    addPathsToComposer(input, folder.isEmpty() ? QStringList()
                                               : QStringList{QDir::cleanPath(folder)});
}

void MainWindow::addPathsToComposer(QPlainTextEdit *input, const QStringList &paths)
{
    if (!input)
        return;
    const forkmesh::promptdrop::Plan plan =
        forkmesh::promptdrop::plan(paths, composerMentionRoots(), isPromptMedia);
    const bool queues = input == m_issueQuickAdd ||
                        input->property("composerQueuesAttachments").toBool();
    QTextCursor cursor = input->textCursor();
    if (!plan.mentions.isEmpty())
        cursor.insertText(forkmesh::promptdrop::insertion(
            plan, input->toPlainText().left(cursor.position())));
    if (queues) {
        for (const QString &path : plan.attachments)
            queueQuickAddImage(path);
    } else if (!plan.attachments.isEmpty()) {
        cursor.movePosition(QTextCursor::End);
        for (const QString &path : plan.attachments) {
            if (!input->toPlainText().isEmpty() &&
                !input->toPlainText().endsWith(QLatin1Char('\n')))
                cursor.insertText(QStringLiteral("\n"));
            cursor.insertText(promptAttachmentLine(path));
        }
    }
    input->setTextCursor(cursor);
    input->setFocus(Qt::OtherFocusReason);
}

void MainWindow::populateComposerConnectorsMenu(QMenu *menu)
{
    namespace connectors = forkmesh::composerconnectors;
    QAction *builtIn = menu->addAction(QStringLiteral("ForkMesh"));
    builtIn->setCheckable(true);
    builtIn->setChecked(true);
    builtIn->setEnabled(false);
    builtIn->setToolTip(QStringLiteral("Built in: every agent run gets it"));

    QString project;
    const int index = issuesRepoIndex();
    if (index >= 0 && index < m_repositories.size())
        project = m_repositories.at(index).localPath;
    const QList<connectors::Connector> found = connectors::claudeConnectors(
        readSettingsFile(QDir::home().filePath(QStringLiteral(".claude.json"))),
        project,
        project.isEmpty() ? QByteArray()
                          : readSettingsFile(QDir(project).filePath(QStringLiteral(".mcp.json"))));
    const QStringList disabled =
        QSettings().value(kDisabledComposerConnectorsSetting).toStringList();
    for (const connectors::Connector &connector : found) {
        QAction *row = menu->addAction(connector.name);
        row->setCheckable(true);
        row->setChecked(!disabled.contains(connector.name));
        row->setToolTip(QStringLiteral("%1. Switching it off applies to Claude Code "
                                       "runs started or resumed afterwards.")
                            .arg(connector.source));
        connect(row, &QAction::toggled, this, [name = connector.name](bool on) {
            QStringList now =
                QSettings().value(kDisabledComposerConnectorsSetting).toStringList();
            now.removeAll(name);
            if (!on)
                now << name;
            QSettings().setValue(kDisabledComposerConnectorsSetting, now);
        });
    }
    if (found.isEmpty()) {
        QAction *none = menu->addAction(QStringLiteral("No other connectors set up"));
        none->setEnabled(false);
    }
    menu->addSeparator();
    connect(menu->addAction(QString::fromUtf8("Manage connectors\xE2\x80\xA6")),
            &QAction::triggered, this, [this] { openMcpSettings(); });
}

void MainWindow::openMcpSettings()
{
    showSection(1);
    if (!m_settingsSections)
        return;
    for (int i = 0; i < m_settingsSections->count(); ++i) {
        if (m_settingsSections->tabText(i) != QLatin1String("MCP"))
            continue;
        m_settingsSections->setExpanded(i, true);
        m_settingsSections->setCurrentIndex(i);
        return;
    }
}
