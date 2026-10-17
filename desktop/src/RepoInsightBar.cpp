#include "RepoInsightBar.h"

#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QUrl>
#include <QVBoxLayout>

#include <cmath>

namespace forkmesh::insight {

namespace {

QString cleanRoot(const QString &path)
{
    if (path.isEmpty())
        return {};
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

QString compactCount(qint64 value)
{
    const QLocale locale;
    if (value >= 1000000)
        return locale.toString(value / 1000000.0, 'f', 1) + QStringLiteral("M");
    if (value >= 10000)
        return locale.toString(value / 1000.0, 'f', 0) + QStringLiteral("k");
    return locale.toString(value);
}

}

RepoInsightBar::RepoInsightBar(const forkmesh::market::NativePackageHost &host,
                               const QString &title, const QString &subtitle, QWidget *parent)
    : QWidget(parent)
    , m_host(host)
{
    setObjectName(QStringLiteral("repoInsightBar"));
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(20, 14, 20, 6);
    outer->setSpacing(6);

    auto *row = new QHBoxLayout;
    row->setSpacing(8);
    auto *titles = new QVBoxLayout;
    titles->setSpacing(0);
    auto *heading = new QLabel(title);
    heading->setObjectName(QStringLiteral("pageTitle"));
    titles->addWidget(heading);
    if (!subtitle.isEmpty()) {
        auto *sub = new QLabel(subtitle);
        sub->setObjectName(QStringLiteral("mutedLabel"));
        titles->addWidget(sub);
    }
    row->addLayout(titles);
    row->addStretch(1);
    m_actions = new QHBoxLayout;
    m_actions->setSpacing(6);
    row->addLayout(m_actions);

    m_repositories = new QComboBox;
    m_repositories->setObjectName(QStringLiteral("repoInsightRepository"));
    m_repositories->setAccessibleName(QStringLiteral("Repository to analyze"));
    m_repositories->setMinimumContentsLength(18);
    m_repositories->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    row->addWidget(m_repositories);
    m_folderButton = new QPushButton(QStringLiteral("Folder…"));
    m_folderButton->setObjectName(QStringLiteral("ghostButton"));
    m_folderButton->setToolTip(QStringLiteral("Analyze any folder on this computer"));
    m_folderButton->setCursor(Qt::PointingHandCursor);
    row->addWidget(m_folderButton);
    m_analyzeButton = new QPushButton(QStringLiteral("Re-analyze"));
    m_analyzeButton->setObjectName(QStringLiteral("ghostButton"));
    m_analyzeButton->setToolTip(QStringLiteral("Read the files and git history again"));
    m_analyzeButton->setCursor(Qt::PointingHandCursor);
    row->addWidget(m_analyzeButton);
    outer->addLayout(row);

    auto *statusRow = new QHBoxLayout;
    statusRow->setSpacing(10);
    m_status = new QLabel;
    m_status->setObjectName(QStringLiteral("statusLine"));
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_status->setMinimumWidth(120);
    statusRow->addWidget(m_status, 1);
    m_progress = new QProgressBar;
    m_progress->setRange(0, 1000);
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(4);
    m_progress->setMaximumWidth(220);
    m_progress->hide();
    statusRow->addWidget(m_progress);
    outer->addLayout(statusRow);

    connect(m_repositories, &QComboBox::activated, this, [this](int index) {
        if (m_rebuilding)
            return;
        const QString path = m_repositories->itemData(index).toString();
        if (path.isEmpty())
            return;
        const forkmesh::market::NativePackageRepository open =
            m_host.currentRepository ? m_host.currentRepository()
                                     : forkmesh::market::NativePackageRepository();
        m_followOpen = cleanRoot(open.path) == cleanRoot(path) && path != m_pickedFolder;
        setRoot(path, m_repositories->itemText(index));
    });
    connect(m_folderButton, &QPushButton::clicked, this, [this] { chooseFolder(); });
    connect(m_analyzeButton, &QPushButton::clicked, this, [this] { reanalyze(); });
    rebuildChoices();
}

RepoInsightBar::~RepoInsightBar()
{
    if (m_watcher)
        m_watcher->disconnect(this);
}

bool RepoInsightBar::busy() const
{
    return m_watcher && m_watcher->isRunning();
}

void RepoInsightBar::setSummary(const QString &text)
{
    m_summary = text;
    if (!busy() && m_snapshot)
        m_status->setText(m_summary.isEmpty() ? describe(*m_snapshot)
                                              : describe(*m_snapshot) + QStringLiteral("  ·  ") + m_summary);
}

void RepoInsightBar::setOnSnapshot(std::function<void(SnapshotPtr)> callback)
{
    m_onSnapshot = std::move(callback);
}

void RepoInsightBar::setOnRootChanged(std::function<void(const QString &)> callback)
{
    m_onRootChanged = std::move(callback);
}

void RepoInsightBar::rebuildChoices()
{
    m_rebuilding = true;
    const QSignalBlocker blocker(m_repositories);
    m_repositories->clear();
    QList<forkmesh::market::NativePackageRepository> repositories;
    if (m_host.repositories)
        repositories = m_host.repositories();
    QStringList seen;
    for (const forkmesh::market::NativePackageRepository &repository : repositories) {
        const QString path = cleanRoot(repository.path);
        if (path.isEmpty() || seen.contains(path))
            continue;
        seen.append(path);
        m_repositories->addItem(repository.title.isEmpty() ? QFileInfo(path).fileName()
                                                           : repository.title,
                                path);
        m_repositories->setItemData(m_repositories->count() - 1, path, Qt::ToolTipRole);
    }
    if (!m_pickedFolder.isEmpty() && !seen.contains(m_pickedFolder)) {
        m_repositories->addItem(QFileInfo(m_pickedFolder).fileName() + QStringLiteral(" (folder)"),
                                m_pickedFolder);
        m_repositories->setItemData(m_repositories->count() - 1, m_pickedFolder, Qt::ToolTipRole);
        seen.append(m_pickedFolder);
    }
    if (!m_root.isEmpty() && !seen.contains(m_root)) {
        m_repositories->addItem(m_title.isEmpty() ? QFileInfo(m_root).fileName() : m_title, m_root);
        seen.append(m_root);
    }
    if (m_repositories->count() == 0)
        m_repositories->addItem(QStringLiteral("No local repositories"), QString());
    const int current = m_repositories->findData(m_root);
    m_repositories->setCurrentIndex(current < 0 ? 0 : current);
    m_repositories->setEnabled(!seen.isEmpty());
    m_analyzeButton->setEnabled(!m_root.isEmpty());
    m_rebuilding = false;
}

void RepoInsightBar::refresh()
{
    rebuildChoices();
    if (m_followOpen && m_host.currentRepository) {
        const forkmesh::market::NativePackageRepository open = m_host.currentRepository();
        const QString path = cleanRoot(open.path);
        if (!path.isEmpty() && path != m_root) {
            setRoot(path, open.title);
            return;
        }
    }
    if (m_root.isEmpty()) {
        if (m_repositories->count() > 0 && !m_repositories->itemData(0).toString().isEmpty())
            setRoot(m_repositories->itemData(0).toString(), m_repositories->itemText(0));
        else
            m_status->setText(QStringLiteral("Open or clone a repository, or pick a folder to analyze."));
        return;
    }
    if (!busy()) {
        const SnapshotPtr cached = Analysis::cached(m_root);
        if (!cached || cached != m_snapshot || !Analysis::fresh(m_root))
            start(false);
    }
}

void RepoInsightBar::repositoryChanged()
{
    if (!m_followOpen || !m_host.currentRepository)
        return;
    const forkmesh::market::NativePackageRepository open = m_host.currentRepository();
    const QString path = cleanRoot(open.path);
    if (path.isEmpty() || path == m_root)
        return;
    if (!isVisible()) {
        m_root.clear();
        m_snapshot.reset();
        return;
    }
    rebuildChoices();
    setRoot(path, open.title);
}

void RepoInsightBar::reanalyze()
{
    if (!m_root.isEmpty())
        start(true);
}

void RepoInsightBar::setRoot(const QString &root, const QString &title)
{
    const QString clean = cleanRoot(root);
    if (clean.isEmpty())
        return;
    const bool changed = clean != m_root;
    m_root = clean;
    m_title = title.isEmpty() ? QFileInfo(clean).fileName() : title;
    if (changed) {
        m_snapshot.reset();
        if (m_onRootChanged)
            m_onRootChanged(m_root);
    }
    rebuildChoices();
    start(false);
}

void RepoInsightBar::chooseFolder()
{
    const QString start = m_root.isEmpty() ? QDir::homePath() : m_root;
    const QString chosen = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Analyze a folder"), start);
    if (chosen.isEmpty())
        return;
    m_pickedFolder = cleanRoot(chosen);
    m_followOpen = false;
    setRoot(m_pickedFolder, QFileInfo(m_pickedFolder).fileName());
}

void RepoInsightBar::start(bool force)
{
    if (m_root.isEmpty())
        return;
    if (!force) {
        const SnapshotPtr cached = Analysis::cached(m_root);
        if (cached && Analysis::fresh(m_root) && !Analysis::running(m_root)) {
            if (m_watcher) {
                m_watcher->disconnect(this);
                m_watcher->deleteLater();
                m_watcher = nullptr;
            }
            m_progress->hide();
            if (cached != m_snapshot) {
                m_snapshot = cached;
                setSummary(m_summary);
                if (m_onSnapshot)
                    m_onSnapshot(m_snapshot);
            } else {
                setSummary(m_summary);
            }
            return;
        }
    }
    if (m_watcher) {
        m_watcher->disconnect(this);
        m_watcher->deleteLater();
    }
    m_watcher = new QFutureWatcher<SnapshotPtr>(this);
    connect(m_watcher, &QFutureWatcher<SnapshotPtr>::progressValueChanged, this,
            [this](int value) { showProgress(value, QString()); });
    connect(m_watcher, &QFutureWatcher<SnapshotPtr>::progressTextChanged, this,
            [this](const QString &text) { showProgress(-1, text); });
    connect(m_watcher, &QFutureWatcher<SnapshotPtr>::finished, this, [this] { finish(); });
    m_progress->setValue(0);
    m_progress->show();
    m_status->setText(QStringLiteral("Reading %1…").arg(m_title));
    m_analyzeButton->setEnabled(false);
    m_watcher->setFuture(Analysis::request(m_root, force));
}

void RepoInsightBar::showProgress(int value, const QString &text)
{
    if (value >= 0)
        m_progress->setValue(value);
    if (!text.isEmpty())
        m_status->setText(text);
}

void RepoInsightBar::finish()
{
    QFutureWatcher<SnapshotPtr> *watcher = m_watcher;
    if (!watcher)
        return;
    m_progress->hide();
    m_analyzeButton->setEnabled(!m_root.isEmpty());
    SnapshotPtr result;
    if (!watcher->isCanceled() && watcher->future().resultCount() > 0)
        result = watcher->result();
    watcher->deleteLater();
    m_watcher = nullptr;
    if (!result) {
        m_status->setText(QStringLiteral("The analysis stopped before it finished."));
        return;
    }
    if (cleanRoot(result->root) != m_root)
        return;
    if (!result->error.isEmpty()) {
        m_status->setText(result->error);
        m_snapshot = result;
        if (m_onSnapshot)
            m_onSnapshot(m_snapshot);
        return;
    }
    m_snapshot = result;
    setSummary(m_summary);
    if (m_onSnapshot)
        m_onSnapshot(m_snapshot);
}

void RepoInsightBar::openFile(const QString &relativePath, int line) const
{
    if (relativePath.isEmpty() || m_root.isEmpty())
        return;
    if (m_host.openRepositoryFile && m_host.repositories) {
        for (const forkmesh::market::NativePackageRepository &repository : m_host.repositories()) {
            if (cleanRoot(repository.path) == m_root) {
                m_host.openRepositoryFile(repository, relativePath, line);
                return;
            }
        }
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(QDir(m_root).filePath(relativePath)));
}

QString RepoInsightBar::describe(const Snapshot &snapshot)
{
    const QLocale locale;
    QStringList parts;
    parts << QStringLiteral("%1 files").arg(locale.toString(snapshot.files.size()));
    parts << QStringLiteral("%1 lines of code").arg(compactCount(snapshot.totalCodeLines()));
    if (snapshot.git && snapshot.commitsScanned > 0) {
        const int months = qMax(1, int(std::lround(snapshot.historyDays / 30.4)));
        parts << QStringLiteral("%1 commits in %2 %3")
                     .arg(locale.toString(snapshot.commitsScanned))
                     .arg(months)
                     .arg(months == 1 ? QStringLiteral("month") : QStringLiteral("months"));
    } else if (!snapshot.git) {
        parts << QStringLiteral("no git history");
    }
    if (snapshot.elapsedMs > 0)
        parts << QStringLiteral("analyzed in %1 s")
                     .arg(locale.toString(snapshot.elapsedMs / 1000.0, 'f', 1));
    QString text = parts.join(QStringLiteral("  ·  "));
    if (!snapshot.warning.isEmpty())
        text += QStringLiteral("  ·  ") + snapshot.warning;
    return text;
}

} // namespace forkmesh::insight
