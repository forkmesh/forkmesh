#include "NetworkFilesPanel.h"

#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QDragEnterEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QStyle>
#include <QTreeWidgetItemIterator>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

namespace forkmesh::smb {

namespace {

constexpr int kPathRole = Qt::UserRole;
constexpr int kFolderRole = Qt::UserRole + 1;
constexpr int kLoadedRole = Qt::UserRole + 2;
constexpr int kSizeRole = Qt::UserRole + 3;
constexpr int kModifiedRole = Qt::UserRole + 4;
constexpr int kHiddenRole = Qt::UserRole + 5;
constexpr int kPlaceholderRole = Qt::UserRole + 7;

QString kindOf(const QString &name, bool folder)
{
    if (folder)
        return QStringLiteral("Folder");
    const QString suffix = QFileInfo(name).suffix().toUpper();
    return suffix.isEmpty() ? QStringLiteral("File") : QStringLiteral("%1 file").arg(suffix);
}

bool uploadPath(Client &client, const QString &localPath, const QString &remoteFolder, const Progress &progress, OpResult &result)
{
    const QFileInfo info(localPath);
    const QString target = joinPath(remoteFolder, info.fileName());
    if (info.isDir() && !info.isSymLink()) {
        if (!client.makeFolder(target) && client.status() != StatusObjectNameCollision) {
            result.ok = false;
            result.error = client.error();
            return false;
        }
        const QFileInfoList children = QDir(localPath).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        for (const QFileInfo &child : children) {
            if (!uploadPath(client, child.absoluteFilePath(), target, progress, result))
                return false;
        }
        return true;
    }
    QFile file(localPath);
    if (!file.open(QIODevice::ReadOnly)) {
        result.ok = false;
        result.error = QStringLiteral("Could not read %1: %2").arg(info.fileName(), file.errorString());
        return false;
    }
    if (!client.writeFile(target, &file, file.size(), progress)) {
        result.ok = false;
        result.cancelled = client.cancelled();
        result.error = client.error();
        return false;
    }
    return true;
}

bool downloadPath(Client &client, const QString &remotePath, bool folder, const QString &localTarget, const Progress &progress, OpResult &result)
{
    if (folder) {
        if (!QDir().mkpath(localTarget)) {
            result.ok = false;
            result.error = QStringLiteral("Could not create %1.").arg(localTarget);
            return false;
        }
        QList<Entry> children;
        if (!client.list(remotePath, &children)) {
            result.ok = false;
            result.error = client.error();
            return false;
        }
        for (const Entry &child : std::as_const(children)) {
            if (!downloadPath(client, joinPath(remotePath, child.name), child.folder, QDir(localTarget).filePath(child.name), progress, result))
                return false;
        }
        return true;
    }
    QFile file(localTarget);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        result.ok = false;
        result.error = QStringLiteral("Could not write %1: %2").arg(localTarget, file.errorString());
        return false;
    }
    if (!client.readFile(remotePath, &file, progress)) {
        file.close();
        file.remove();
        result.ok = false;
        result.cancelled = client.cancelled();
        result.error = client.error();
        return false;
    }
    return true;
}

QString uniqueRemoteName(Client &client, const QString &folder, const QString &name)
{
    Entry entry;
    if (!client.stat(joinPath(folder, name), &entry))
        return name;
    const QFileInfo info(name);
    const QString base = info.completeBaseName().isEmpty() ? name : info.completeBaseName();
    const QString suffix = info.completeBaseName().isEmpty() || info.suffix().isEmpty() ? QString() : QLatin1Char('.') + info.suffix();
    for (int i = 2; i < 1000; ++i) {
        const QString candidate = QStringLiteral("%1 (%2)%3").arg(base, QString::number(i), suffix);
        if (!client.stat(joinPath(folder, candidate), &entry))
            return candidate;
    }
    return name;
}

bool copyLocal(const QString &source, const QString &target, QString *error)
{
    const QFileInfo info(source);
    if (info.isDir() && !info.isSymLink()) {
        if (!QDir().mkpath(target)) {
            *error = QStringLiteral("Could not create %1.").arg(target);
            return false;
        }
        const QFileInfoList children = QDir(source).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        for (const QFileInfo &child : children) {
            if (!copyLocal(child.absoluteFilePath(), QDir(target).filePath(child.fileName()), error))
                return false;
        }
        return true;
    }
    if (!QFile::copy(source, target)) {
        *error = QStringLiteral("Could not copy %1.").arg(info.fileName());
        return false;
    }
    return true;
}

class DropTree : public QTreeWidget
{
public:
    std::function<void(const QStringList &, const QString &)> dropped;

protected:
    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (event->mimeData()->hasUrls())
            event->acceptProposedAction();
        else
            QTreeWidget::dragEnterEvent(event);
    }
    void dragMoveEvent(QDragMoveEvent *event) override
    {
        if (event->mimeData()->hasUrls())
            event->acceptProposedAction();
        else
            QTreeWidget::dragMoveEvent(event);
    }
    void dropEvent(QDropEvent *event) override
    {
        QStringList paths;
        for (const QUrl &url : event->mimeData()->urls()) {
            if (url.isLocalFile())
                paths << url.toLocalFile();
        }
        if (paths.isEmpty() || !dropped) {
            event->ignore();
            return;
        }
        QString folder;
        if (QTreeWidgetItem *item = itemAt(event->position().toPoint())) {
            folder = item->data(0, kFolderRole).toBool() ? item->data(0, kPathRole).toString()
                                                         : parentPath(item->data(0, kPathRole).toString());
        } else {
            folder = property("rootPath").toString();
        }
        event->acceptProposedAction();
        dropped(paths, folder);
    }
};

class ShareDialog : public QDialog
{
public:
    ShareDialog(const SavedShare &initial, const QString &password, TransportFactory factory, QWidget *parent)
        : QDialog(parent), m_factory(std::move(factory)), m_result(initial)
    {
        setWindowTitle(initial.id.isEmpty() ? QStringLiteral("Add network share") : QStringLiteral("Edit network share"));
        auto *layout = new QVBoxLayout(this);
        auto *form = new QFormLayout;
        m_name = new QLineEdit(initial.name);
        m_name->setPlaceholderText(QStringLiteral("Optional"));
        m_address = new QLineEdit(initial.host.isEmpty() ? QString() : (initial.port == kDefaultPort ? initial.host : QStringLiteral("%1:%2").arg(initial.host).arg(initial.port)));
        m_address->setPlaceholderText(QStringLiteral("192.168.1.1 or nas.local"));
        m_address->setAccessibleName(QStringLiteral("Address"));
        m_share = new QComboBox;
        m_share->setEditable(true);
        m_share->setEditText(initial.share);
        m_share->setAccessibleName(QStringLiteral("Share"));
        auto *find = new QPushButton(QStringLiteral("Find shares"));
        find->setObjectName(QStringLiteral("ghostButton"));
        auto *shareRow = new QHBoxLayout;
        shareRow->addWidget(m_share, 1);
        shareRow->addWidget(find);
        m_user = new QLineEdit(initial.user);
        m_user->setAccessibleName(QStringLiteral("User"));
        m_password = new QLineEdit(password);
        m_password->setEchoMode(QLineEdit::Password);
        m_password->setAccessibleName(QStringLiteral("Password"));
        m_guest = new QCheckBox(QStringLiteral("Sign in as guest"));
        m_guest->setChecked(initial.guest);
        m_remember = new QCheckBox(QStringLiteral("Remember password"));
        m_remember->setChecked(initial.remember);
        form->addRow(QStringLiteral("Name"), m_name);
        form->addRow(QStringLiteral("Address"), m_address);
        form->addRow(QStringLiteral("Share"), shareRow);
        form->addRow(QStringLiteral("User"), m_user);
        form->addRow(QStringLiteral("Password"), m_password);
        form->addRow(QString(), m_guest);
        form->addRow(QString(), m_remember);
        layout->addLayout(form);
        m_status = new QLabel;
        m_status->setObjectName(QStringLiteral("mutedLabel"));
        m_status->setWordWrap(true);
        layout->addWidget(m_status);
        auto *buttons = new QDialogButtonBox;
        m_connect = buttons->addButton(QStringLiteral("Connect"), QDialogButtonBox::AcceptRole);
        buttons->addButton(QDialogButtonBox::Cancel);
        layout->addWidget(buttons);
        const auto syncGuest = [this] {
            m_user->setEnabled(!m_guest->isChecked());
            m_password->setEnabled(!m_guest->isChecked());
            m_remember->setEnabled(!m_guest->isChecked());
        };
        syncGuest();
        connect(m_guest, &QCheckBox::toggled, this, syncGuest);
        connect(find, &QPushButton::clicked, this, [this] { findShares(); });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(m_connect, &QPushButton::clicked, this, [this] { tryConnect(); });
        resize(440, sizeHint().height());
    }

    SavedShare result() const { return m_result; }
    QString password() const { return m_guest->isChecked() ? QString() : m_password->text(); }

private:
    bool collect(SavedShare *share, bool needShare)
    {
        QString host;
        quint16 port = kDefaultPort;
        QString error;
        if (!parseAddress(m_address->text(), &host, &port, &error)) {
            m_status->setText(error);
            return false;
        }
        if (!isLocalName(host)) {
            m_status->setText(notLocalText());
            return false;
        }
        const QString shareName = m_share->currentText().trimmed();
        if (needShare && shareName.isEmpty()) {
            m_status->setText(QStringLiteral("Enter the share name, or press Find shares."));
            return false;
        }
        if (!m_guest->isChecked() && m_user->text().trimmed().isEmpty()) {
            m_status->setText(QStringLiteral("Enter a user name, or sign in as guest."));
            return false;
        }
        share->name = m_name->text().trimmed();
        share->host = host;
        share->port = port;
        share->share = shareName;
        share->user = m_guest->isChecked() ? QString() : m_user->text().trimmed();
        share->guest = m_guest->isChecked();
        share->remember = !m_guest->isChecked() && m_remember->isChecked();
        return true;
    }

    Credentials credentials() const
    {
        return {m_user->text().trimmed(), m_password->text(), m_guest->isChecked()};
    }

    void findShares()
    {
        SavedShare probe = m_result;
        if (!collect(&probe, false))
            return;
        probe.share.clear();
        m_status->setText(QStringLiteral("Asking %1 for its shares…").arg(probe.host));
        m_probe = std::make_shared<Connection>(probe, credentials(), m_factory);
        QPointer<ShareDialog> self(this);
        m_probe->post([self](Client &client) {
            bool ok = false;
            QList<ShareInfo> shares;
            QString error = client.error();
            if (client.isOpen()) {
                shares = client.shares(&ok);
                error = client.error();
            }
            QMetaObject::invokeMethod(qApp, [self, ok, shares, error] {
                if (!self)
                    return;
                if (!ok) {
                    self->m_status->setText(error);
                    return;
                }
                const QString typed = self->m_share->currentText();
                self->m_share->clear();
                for (const ShareInfo &share : shares)
                    self->m_share->addItem(share.name);
                self->m_share->setEditText(typed.isEmpty() && !shares.isEmpty() ? shares.first().name : typed);
                self->m_status->setText(shares.isEmpty() ? QStringLiteral("No shared folders found. Type the share name instead.")
                                                         : QStringLiteral("Found %1. Pick one and press Connect.").arg(shares.size() == 1 ? QStringLiteral("1 share") : QStringLiteral("%1 shares").arg(shares.size())));
            }, Qt::QueuedConnection);
        });
    }

    void tryConnect()
    {
        SavedShare candidate = m_result;
        if (!collect(&candidate, true))
            return;
        m_connect->setEnabled(false);
        m_status->setText(QStringLiteral("Connecting to %1…").arg(candidate.title()));
        m_probe = std::make_shared<Connection>(candidate, credentials(), m_factory);
        QPointer<ShareDialog> self(this);
        m_probe->post([self, candidate](Client &client) {
            const bool ok = client.isOpen();
            const QString error = client.error();
            QMetaObject::invokeMethod(qApp, [self, ok, error, candidate] {
                if (!self)
                    return;
                self->m_connect->setEnabled(true);
                if (!ok) {
                    self->m_status->setText(error);
                    return;
                }
                self->m_result = candidate;
                self->accept();
            }, Qt::QueuedConnection);
        });
    }

    TransportFactory m_factory;
    SavedShare m_result;
    std::shared_ptr<Connection> m_probe;
    QLineEdit *m_name = nullptr;
    QLineEdit *m_address = nullptr;
    QComboBox *m_share = nullptr;
    QLineEdit *m_user = nullptr;
    QLineEdit *m_password = nullptr;
    QCheckBox *m_guest = nullptr;
    QCheckBox *m_remember = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_connect = nullptr;
};

}

FileClipboard &fileClipboard()
{
    static FileClipboard clipboard;
    return clipboard;
}

bool looksBinary(const QByteArray &head)
{
    return head.left(8192).contains('\0');
}

bool opensExternally(const QString &name)
{
    static const QStringList suffixes = {
        QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("gif"), QStringLiteral("heic"),
        QStringLiteral("webp"), QStringLiteral("bmp"), QStringLiteral("tif"), QStringLiteral("tiff"), QStringLiteral("pdf"),
        QStringLiteral("mp3"), QStringLiteral("m4a"), QStringLiteral("flac"), QStringLiteral("wav"), QStringLiteral("ogg"),
        QStringLiteral("mp4"), QStringLiteral("mkv"), QStringLiteral("mov"), QStringLiteral("avi"), QStringLiteral("webm"),
        QStringLiteral("zip"), QStringLiteral("gz"), QStringLiteral("tar"), QStringLiteral("7z"), QStringLiteral("rar"),
        QStringLiteral("iso"), QStringLiteral("dmg"), QStringLiteral("exe"), QStringLiteral("docx"), QStringLiteral("xlsx"),
        QStringLiteral("pptx"), QStringLiteral("odt"), QStringLiteral("ods"),
    };
    return suffixes.contains(QFileInfo(name).suffix().toLower());
}

QString uniqueLocalName(const QString &folder, const QString &name)
{
    const QDir dir(folder);
    if (!QFileInfo::exists(dir.filePath(name)))
        return name;
    const QFileInfo info(name);
    const QString base = info.completeBaseName().isEmpty() ? name : info.completeBaseName();
    const QString suffix = info.completeBaseName().isEmpty() || info.suffix().isEmpty() ? QString() : QLatin1Char('.') + info.suffix();
    for (int i = 2; i < 1000; ++i) {
        const QString candidate = QStringLiteral("%1 (%2)%3").arg(base, QString::number(i), suffix);
        if (!QFileInfo::exists(dir.filePath(candidate)))
            return candidate;
    }
    return name;
}

NetworkFilesPanel::NetworkFilesPanel(const QString &secretsPath, QWidget *parent) : QWidget(parent), m_store(secretsPath)
{
    setObjectName(QStringLiteral("networkFilesPanel"));
    build();
    rebuildShareList(false);
}

NetworkFilesPanel::~NetworkFilesPanel()
{
    for (const auto &connection : std::as_const(m_connections))
        connection->cancel();
}

void NetworkFilesPanel::decorate(QPushButton *button, const QString &icon)
{
    button->setObjectName(QStringLiteral("ghostButton"));
    button->setCursor(Qt::PointingHandCursor);
    button->setProperty("networkIcon", icon);
    if (decorateButton)
        decorateButton(button, icon);
}

void NetworkFilesPanel::build()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto *top = new QHBoxLayout;
    m_shares = new QComboBox;
    m_shares->setObjectName(QStringLiteral("networkShareList"));
    m_shares->setAccessibleName(QStringLiteral("Network shares"));
    m_shares->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_shares->setMinimumContentsLength(14);
    top->addWidget(m_shares, 1);
    m_addButton = new QPushButton(QStringLiteral("Add network share"));
    m_addButton->setToolTip(QStringLiteral("Connect to a shared folder on a router, NAS or computer on your network (SMB)"));
    m_editButton = new QPushButton(QStringLiteral("Edit…"));
    m_editButton->setToolTip(QStringLiteral("Change the address, user or password of this share"));
    m_removeButton = new QPushButton(QStringLiteral("Remove"));
    m_removeButton->setToolTip(QStringLiteral("Forget this share and its saved password"));
    top->addWidget(m_addButton);
    top->addWidget(m_editButton);
    top->addWidget(m_removeButton);
    layout->addLayout(top);

    auto *nav = new QHBoxLayout;
    m_upButton = new QPushButton(QStringLiteral("Up"));
    m_upButton->setToolTip(QStringLiteral("Go to the parent folder"));
    m_crumbs = new QLabel;
    m_crumbs->setObjectName(QStringLiteral("mutedLabel"));
    m_crumbs->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_refreshButton = new QPushButton(QStringLiteral("Refresh"));
    nav->addWidget(m_upButton);
    nav->addWidget(m_crumbs, 1);
    nav->addWidget(m_refreshButton);
    layout->addLayout(nav);

    auto *actions = new QHBoxLayout;
    m_newFolderButton = new QPushButton(QStringLiteral("New folder"));
    m_newFileButton = new QPushButton(QStringLiteral("New file"));
    m_uploadButton = new QPushButton(QStringLiteral("Upload…"));
    m_uploadButton->setToolTip(QStringLiteral("Copy files from this computer into this folder. You can also drop files on the list."));
    m_pasteButton = new QPushButton(QStringLiteral("Paste"));
    m_pasteButton->setToolTip(QStringLiteral("Paste what you copied or moved, from this share, another share or this computer"));
    actions->addWidget(m_newFolderButton);
    actions->addWidget(m_newFileButton);
    actions->addWidget(m_uploadButton);
    actions->addWidget(m_pasteButton);
    actions->addStretch();
    layout->addLayout(actions);

    auto *view = new QHBoxLayout;
    m_filter = new QLineEdit;
    m_filter->setPlaceholderText(QStringLiteral("Filter this folder"));
    m_filter->setClearButtonEnabled(true);
    m_sort = new QComboBox;
    m_sort->addItems({QStringLiteral("Name"), QStringLiteral("Date"), QStringLiteral("Size")});
    m_sort->setToolTip(QStringLiteral("Sort"));
    m_sort->setAccessibleName(QStringLiteral("Sort"));
    m_hidden = new QCheckBox(QStringLiteral("Show hidden files"));
    view->addWidget(m_filter, 1);
    view->addWidget(m_sort);
    view->addWidget(m_hidden);
    layout->addLayout(view);

    auto *tree = new DropTree;
    m_tree = tree;
    m_tree->setObjectName(QStringLiteral("fileTree"));
    m_tree->setAccessibleName(QStringLiteral("Files on the network share"));
    m_tree->setColumnCount(3);
    m_tree->setHeaderLabels({QStringLiteral("Name"), QStringLiteral("Size"), QStringLiteral("Modified")});
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_tree->setIndentation(14);
    m_tree->setUniformRowHeights(true);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setAcceptDrops(true);
    m_tree->viewport()->setAcceptDrops(true);
    m_tree->setDragDropMode(QAbstractItemView::DropOnly);
    layout->addWidget(m_tree, 1);

    m_status = new QLabel;
    m_status->setObjectName(QStringLiteral("mutedLabel"));
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    m_progressRow = new QWidget;
    auto *progress = new QHBoxLayout(m_progressRow);
    progress->setContentsMargins(0, 0, 0, 0);
    m_progressLabel = new QLabel;
    m_progressLabel->setObjectName(QStringLiteral("mutedLabel"));
    m_progress = new QProgressBar;
    m_progress->setTextVisible(false);
    m_progress->setMaximumHeight(8);
    m_cancelButton = new QPushButton(QStringLiteral("Cancel"));
    progress->addWidget(m_progressLabel);
    progress->addWidget(m_progress, 1);
    progress->addWidget(m_cancelButton);
    m_progressRow->hide();
    layout->addWidget(m_progressRow);

    tree->dropped = [this](const QStringList &paths, const QString &folder) { uploadLocal(paths, folder); };
    connect(m_shares, &QComboBox::activated, this, [this](int index) { selectShare(m_shares->itemData(index).toString()); });
    connect(m_addButton, &QPushButton::clicked, this, [this] {
        ShareDialog dialog({}, {}, m_factory, this);
        if (dialog.exec() == QDialog::Accepted)
            addShare(dialog.result(), dialog.password());
    });
    connect(m_editButton, &QPushButton::clicked, this, [this] { editCurrentShare(); });
    connect(m_removeButton, &QPushButton::clicked, this, [this] { removeCurrentShare(); });
    connect(m_upButton, &QPushButton::clicked, this, [this] {
        if (!m_path.isEmpty())
            browse(parentPath(m_path));
    });
    connect(m_refreshButton, &QPushButton::clicked, this, [this] { reload(); });
    connect(m_newFolderButton, &QPushButton::clicked, this, [this] { newFolder(selectedFolder()); });
    connect(m_newFileButton, &QPushButton::clicked, this, [this] { newFile(selectedFolder()); });
    connect(m_uploadButton, &QPushButton::clicked, this, [this] {
        const QStringList paths = QFileDialog::getOpenFileNames(this, QStringLiteral("Upload to %1").arg(m_share.title()),
                                                                QStandardPaths::writableLocation(QStandardPaths::HomeLocation));
        if (!paths.isEmpty())
            uploadLocal(paths, selectedFolder());
    });
    connect(m_pasteButton, &QPushButton::clicked, this, [this] { paste(selectedFolder()); });
    connect(m_filter, &QLineEdit::textChanged, this, [this] { applyFilter(); });
    connect(m_sort, &QComboBox::currentIndexChanged, this, [this] { reload(); });
    connect(m_hidden, &QCheckBox::toggled, this, [this] { reload(); });
    connect(m_cancelButton, &QPushButton::clicked, this, [this] { cancel(); });
    connect(m_tree, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) { showMenu(pos); });
    connect(m_tree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *item) {
        if (item->data(0, kFolderRole).toBool() && !item->data(0, kLoadedRole).toBool())
            list(item->data(0, kPathRole).toString(), item);
        if (folderIcon)
            item->setIcon(0, folderIcon(true));
    });
    connect(m_tree, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem *item) {
        if (folderIcon && item->data(0, kFolderRole).toBool())
            item->setIcon(0, folderIcon(false));
    });
    connect(m_tree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item, int) {
        if (QApplication::keyboardModifiers() & (Qt::ControlModifier | Qt::ShiftModifier | Qt::MetaModifier))
            return;
        if (item && !item->data(0, kFolderRole).toBool() && !item->data(0, kPlaceholderRole).toBool())
            openEntry(item->data(0, kPathRole).toString(), false, item->data(0, kSizeRole).toLongLong());
    });
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
        if (item && item->data(0, kFolderRole).toBool())
            browse(item->data(0, kPathRole).toString());
    });
    for (const auto &[button, icon] : {std::pair{m_addButton, QStringLiteral("plus")}, std::pair{m_editButton, QStringLiteral("pencil")},
                                       std::pair{m_removeButton, QStringLiteral("trash")}, std::pair{m_upButton, QStringLiteral("chevron-up")},
                                       std::pair{m_refreshButton, QStringLiteral("sync")}, std::pair{m_newFolderButton, QStringLiteral("file-directory")},
                                       std::pair{m_newFileButton, QStringLiteral("file")}, std::pair{m_uploadButton, QStringLiteral("upload")},
                                       std::pair{m_pasteButton, QStringLiteral("copy")}, std::pair{m_cancelButton, QStringLiteral("x")}})
        decorate(button, icon);
    setBusy(false);
}

void NetworkFilesPanel::activate()
{
    for (QPushButton *button : findChildren<QPushButton *>()) {
        const QString icon = button->property("networkIcon").toString();
        if (decorateButton && !icon.isEmpty())
            decorateButton(button, icon);
    }
    if (m_share.id.isEmpty())
        rebuildShareList(true);
    else if (!m_listed)
        browse(m_path);
    if (statusChanged)
        statusChanged(status());
}

void NetworkFilesPanel::rebuildShareList(bool open)
{
    const QList<SavedShare> shares = m_store.shares();
    m_shares->clear();
    for (const SavedShare &share : shares)
        m_shares->addItem(share.title(), share.id);
    if (shares.isEmpty()) {
        m_shares->addItem(QStringLiteral("No network shares yet"), QString());
        m_share = {};
        m_tree->clear();
        m_crumbs->clear();
        setStatus(QStringLiteral("Press Add network share to browse a folder shared by your router, a NAS or another computer."));
        setBusy(false);
        return;
    }
    int index = 0;
    for (int i = 0; i < shares.size(); ++i) {
        if (shares[i].id == m_share.id)
            index = i;
    }
    m_shares->setCurrentIndex(index);
    if (m_share.id != shares[index].id) {
        if (open) {
            selectShare(shares[index].id);
        } else {
            m_share = shares[index];
            m_path.clear();
        }
    }
    setBusy(m_busy);
}

std::shared_ptr<Connection> NetworkFilesPanel::connection(const SavedShare &share, bool prompt)
{
    if (share.id.isEmpty())
        return {};
    if (auto existing = m_connections.value(share.id))
        return existing;
    Credentials credentials;
    credentials.guest = share.guest;
    credentials.user = share.user;
    if (!share.guest) {
        QString password = m_sessionPasswords.value(share.id);
        if (password.isEmpty() && share.remember)
            password = m_store.password(share.id);
        if (password.isEmpty()) {
            if (!prompt)
                return {};
            bool ok = false;
            if (m_passwordPrompt) {
                ok = m_passwordPrompt(share, &password);
            } else {
                password = QInputDialog::getText(this, QStringLiteral("Sign in to %1").arg(share.title()),
                                                 QStringLiteral("Password for %1 on %2:").arg(share.user, share.host), QLineEdit::Password,
                                                 {}, &ok);
            }
            if (!ok) {
                setStatus(QStringLiteral("Sign in to browse %1.").arg(share.title()));
                return {};
            }
            m_sessionPasswords.insert(share.id, password);
        }
        credentials.password = password;
    }
    auto created = std::make_shared<Connection>(share, credentials, m_factory);
    m_connections.insert(share.id, created);
    return created;
}

std::shared_ptr<Connection> NetworkFilesPanel::connectionFor(const QString &url, QString *path)
{
    const Location location = parseUrl(url);
    for (const SavedShare &share : m_store.shares()) {
        if (share.sameTarget(SavedShare{{}, {}, location.host, location.port, location.share})) {
            *path = location.path;
            return connection(share, true);
        }
    }
    return {};
}

void NetworkFilesPanel::post(const std::shared_ptr<Connection> &target, const QString &label, Work work, Done done)
{
    if (!target)
        return;
    const bool tracked = !label.isEmpty();
    if (tracked && m_busy) {
        if (notify)
            notify(QStringLiteral("Wait for the current transfer to finish, or cancel it."), true);
        return;
    }
    if (tracked) {
        target->resetCancel();
        m_activeConnection = target;
        setBusy(true, label);
    }
    QPointer<NetworkFilesPanel> self(this);
    const auto cancelFlag = target->cancelFlag();
    target->post([self, work, done, tracked, cancelFlag](Client &client) {
        OpResult result;
        if (!client.isOpen()) {
            result.ok = false;
            result.error = client.error();
        } else {
            auto timer = std::make_shared<QElapsedTimer>();
            timer->start();
            const Progress progress = [self, tracked, cancelFlag, timer](qint64 done, qint64 total) {
                if (*cancelFlag)
                    return false;
                if (tracked && (timer->elapsed() > 100 || done == total)) {
                    timer->restart();
                    QMetaObject::invokeMethod(qApp, [self, done, total] {
                        if (self)
                            self->setProgress(done, total);
                    }, Qt::QueuedConnection);
                }
                return true;
            };
            work(client, result, progress);
        }
        QMetaObject::invokeMethod(qApp, [self, done, result, tracked] {
            if (!self)
                return;
            if (tracked) {
                self->m_activeConnection.reset();
                self->setBusy(false);
            }
            done(result);
        }, Qt::QueuedConnection);
    });
}

void NetworkFilesPanel::selectShare(const QString &id)
{
    const SavedShare share = m_store.share(id);
    if (share.id.isEmpty())
        return;
    m_share = share;
    for (int i = 0; i < m_shares->count(); ++i) {
        if (m_shares->itemData(i).toString() == id)
            m_shares->setCurrentIndex(i);
    }
    browse({});
}

void NetworkFilesPanel::browse(const QString &path)
{
    m_path = path;
    m_listed = true;
    m_filter->clear();
    QStringList crumbs = {m_share.share};
    if (!path.isEmpty())
        crumbs += path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    m_crumbs->setText(m_share.id.isEmpty() ? QString() : crumbs.join(QString::fromUtf8(" \xE2\x80\xBA ")));
    m_crumbs->setToolTip(currentUrl());
    m_tree->setProperty("rootPath", path);
    m_tree->clear();
    setBusy(m_busy);
    list(path, nullptr);
}

void NetworkFilesPanel::reload()
{
    if (m_share.id.isEmpty()) {
        rebuildShareList(true);
        return;
    }
    browse(m_path);
}

void NetworkFilesPanel::list(const QString &path, QTreeWidgetItem *parent)
{
    const auto target = connection(m_share, true);
    if (!target)
        return;
    const int generation = parent ? m_listGeneration : ++m_listGeneration;
    if (!parent)
        setStatus(QStringLiteral("Loading %1…").arg(m_share.url(path)));
    QPointer<QTreeWidget> tree(m_tree);
    const QString shareId = m_share.id;
    post(target, {}, [path](Client &client, OpResult &result, const Progress &) {
        if (!client.list(path, &result.entries)) {
            result.ok = false;
            result.error = client.error();
        }
    }, [this, parent, generation, shareId, path, tree](const OpResult &result) {
        if (generation != m_listGeneration || shareId != m_share.id || !tree)
            return;
        if (!result.ok) {
            if (!parent)
                m_connections.remove(shareId);
            if (result.error == statusText(StatusLogonFailure))
                m_sessionPasswords.remove(shareId);
            setStatus(result.error, true);
            return;
        }
        if (parent) {
            bool alive = false;
            for (QTreeWidgetItemIterator it(m_tree); *it; ++it) {
                if (*it == parent)
                    alive = true;
            }
            if (!alive)
                return;
        }
        fill(parent, result.entries);
        if (!parent)
            setStatus(result.entries.isEmpty() ? QStringLiteral("This folder is empty.") : QString());
    });
}

QIcon NetworkFilesPanel::iconFor(const Entry &entry) const
{
    if (entry.folder)
        return folderIcon ? folderIcon(false) : QFileIconProvider().icon(QFileIconProvider::Folder);
    return fileIcon ? fileIcon(entry.name) : QFileIconProvider().icon(QFileIconProvider::File);
}

void NetworkFilesPanel::fill(QTreeWidgetItem *parent, const QList<Entry> &entries)
{
    QList<Entry> sorted;
    for (const Entry &entry : entries) {
        if (!entry.hidden || m_hidden->isChecked())
            sorted << entry;
    }
    const int sort = m_sort->currentIndex();
    std::sort(sorted.begin(), sorted.end(), [sort](const Entry &a, const Entry &b) {
        if (a.folder != b.folder)
            return a.folder;
        if (sort == 1 && a.modified != b.modified)
            return a.modified > b.modified;
        if (sort == 2 && a.size != b.size)
            return a.size > b.size;
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    if (parent) {
        qDeleteAll(parent->takeChildren());
        parent->setData(0, kLoadedRole, true);
    }
    const QString base = parent ? parent->data(0, kPathRole).toString() : m_path;
    const QLocale locale;
    for (const Entry &entry : std::as_const(sorted)) {
        auto *item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_tree);
        item->setText(0, entry.name);
        item->setIcon(0, iconFor(entry));
        item->setData(0, kPathRole, joinPath(base, entry.name));
        item->setData(0, kFolderRole, entry.folder);
        item->setData(0, kSizeRole, entry.size);
        item->setData(0, kModifiedRole, entry.modified);
        item->setData(0, kHiddenRole, entry.hidden);
        item->setToolTip(0, m_share.url(joinPath(base, entry.name)));
        if (!entry.folder) {
            item->setText(1, formatSize(entry.size));
            item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        }
        if (entry.modified.isValid())
            item->setText(2, locale.toString(entry.modified.toLocalTime(), QLocale::ShortFormat));
        if (entry.folder) {
            auto *placeholder = new QTreeWidgetItem(item);
            placeholder->setText(0, QStringLiteral("Loading…"));
            placeholder->setData(0, kPlaceholderRole, true);
            placeholder->setFlags(Qt::NoItemFlags);
        }
    }
    if (parent && sorted.isEmpty()) {
        auto *empty = new QTreeWidgetItem(parent);
        empty->setText(0, QStringLiteral("This folder is empty."));
        empty->setData(0, kPlaceholderRole, true);
        empty->setFlags(Qt::NoItemFlags);
    }
    applyFilter();
}

void NetworkFilesPanel::applyFilter()
{
    const QString needle = m_filter->text().trimmed();
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = m_tree->topLevelItem(i);
        item->setHidden(!needle.isEmpty() && !item->text(0).contains(needle, Qt::CaseInsensitive));
    }
}

QStringList NetworkFilesPanel::visibleNames() const
{
    QStringList out;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        if (!m_tree->topLevelItem(i)->isHidden())
            out << m_tree->topLevelItem(i)->text(0);
    }
    return out;
}

QString NetworkFilesPanel::currentUrl() const
{
    return m_share.id.isEmpty() ? QString() : m_share.url(m_path);
}

QString NetworkFilesPanel::status() const
{
    return m_status->text();
}

void NetworkFilesPanel::setStatus(const QString &text, bool error)
{
    m_status->setText(text);
    m_status->setObjectName(error ? QStringLiteral("errorLabel") : QStringLiteral("mutedLabel"));
    m_status->style()->unpolish(m_status);
    m_status->style()->polish(m_status);
    m_status->setVisible(!text.isEmpty());
    if (statusChanged)
        statusChanged(text.isEmpty() ? currentUrl() : text);
}

void NetworkFilesPanel::setBusy(bool busy, const QString &label)
{
    m_busy = busy;
    const bool ready = !m_share.id.isEmpty() && !busy;
    m_editButton->setEnabled(!m_share.id.isEmpty() && !busy);
    m_removeButton->setEnabled(!m_share.id.isEmpty() && !busy);
    m_upButton->setEnabled(ready && !m_path.isEmpty());
    m_refreshButton->setEnabled(!busy);
    m_newFolderButton->setEnabled(ready);
    m_newFileButton->setEnabled(ready);
    m_uploadButton->setEnabled(ready);
    m_pasteButton->setEnabled(ready && !fileClipboard().items.isEmpty());
    m_progressRow->setVisible(busy);
    if (busy) {
        m_progressLabel->setText(label);
        m_progress->setRange(0, 0);
    }
}

void NetworkFilesPanel::setProgress(qint64 done, qint64 total)
{
    if (total <= 0) {
        m_progress->setRange(0, 0);
        return;
    }
    m_progress->setRange(0, 1000);
    m_progress->setValue(int(done * 1000 / total));
    m_progress->setToolTip(QStringLiteral("%1 of %2").arg(formatSize(done), formatSize(total)));
}

void NetworkFilesPanel::cancel()
{
    if (m_activeConnection)
        m_activeConnection->cancel();
}

void NetworkFilesPanel::finish(const OpResult &result, const QString &success)
{
    if (!result.ok) {
        const QString text = result.cancelled ? QStringLiteral("Cancelled.") : result.error;
        setStatus(text, !result.cancelled);
        if (notify)
            notify(text, !result.cancelled);
        return;
    }
    if (!success.isEmpty() && notify)
        notify(success, false);
    reload();
}

bool NetworkFilesPanel::ask(const QString &title, const QString &text)
{
    if (m_confirm)
        return m_confirm(title, text);
    return QMessageBox::question(this, title, text, QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes;
}

bool NetworkFilesPanel::askText(const QString &title, const QString &label, QString *value)
{
    if (m_textPrompt)
        return m_textPrompt(title, label, value);
    bool ok = false;
    *value = QInputDialog::getText(this, title, label, QLineEdit::Normal, *value, &ok).trimmed();
    return ok && !value->isEmpty();
}

QString NetworkFilesPanel::selectedFolder() const
{
    const QList<QTreeWidgetItem *> selected = m_tree->selectedItems();
    if (selected.size() == 1 && selected.first()->data(0, kFolderRole).toBool() && selected.first()->isExpanded())
        return selected.first()->data(0, kPathRole).toString();
    return m_path;
}

void NetworkFilesPanel::addShare(const SavedShare &share, const QString &password)
{
    SavedShare saved = m_store.save(share);
    m_connections.remove(saved.id);
    if (!saved.guest) {
        m_sessionPasswords.insert(saved.id, password);
        m_store.setPassword(saved.id, saved.remember ? password : QString());
    } else {
        m_sessionPasswords.remove(saved.id);
        m_store.setPassword(saved.id, {});
    }
    m_share = saved;
    rebuildShareList(false);
    selectShare(saved.id);
}

void NetworkFilesPanel::editCurrentShare()
{
    if (m_share.id.isEmpty())
        return;
    QString password = m_sessionPasswords.value(m_share.id);
    if (password.isEmpty() && m_share.remember)
        password = m_store.password(m_share.id);
    ShareDialog dialog(m_share, password, m_factory, this);
    if (dialog.exec() == QDialog::Accepted)
        addShare(dialog.result(), dialog.password());
}

void NetworkFilesPanel::removeCurrentShare()
{
    if (m_share.id.isEmpty())
        return;
    if (!ask(QStringLiteral("Remove %1?").arg(m_share.title()), QStringLiteral("ForkMesh forgets this share and its saved password. The files on it stay where they are.")))
        return;
    m_connections.remove(m_share.id);
    m_sessionPasswords.remove(m_share.id);
    m_store.remove(m_share.id);
    m_share = {};
    m_path.clear();
    rebuildShareList(true);
}

void NetworkFilesPanel::openUrl(const QString &url, bool guest)
{
    const Location location = parseUrl(url);
    if (!location.valid())
        return;
    for (const SavedShare &share : m_store.shares()) {
        if (share.sameTarget(SavedShare{{}, {}, location.host, location.port, location.share})) {
            m_share = share;
            rebuildShareList(false);
            browse(location.path);
            return;
        }
    }
    SavedShare share;
    share.host = location.host;
    share.port = location.port;
    share.share = location.share;
    share.user = location.user;
    share.guest = guest;
    if (guest) {
        addShare(share, {});
        if (!location.path.isEmpty())
            browse(location.path);
        return;
    }
    ShareDialog dialog(share, {}, m_factory, this);
    if (dialog.exec() == QDialog::Accepted)
        addShare(dialog.result(), dialog.password());
}

void NetworkFilesPanel::saveText(const QString &url, const QString &text, std::function<void(bool, const QString &)> done)
{
    QString path;
    const auto target = connectionFor(url, &path);
    if (!target) {
        done(false, QStringLiteral("Add the share again to save this file."));
        return;
    }
    const QByteArray data = text.toUtf8();
    post(target, {}, [path, data](Client &client, OpResult &result, const Progress &progress) {
        QByteArray copy = data;
        QBuffer buffer(&copy);
        buffer.open(QIODevice::ReadOnly);
        if (!client.writeFile(path, &buffer, copy.size(), progress)) {
            result.ok = false;
            result.cancelled = client.cancelled();
            result.error = client.error();
        }
    }, [done](const OpResult &result) { done(result.ok, result.error); });
}

void NetworkFilesPanel::openEntry(const QString &path, bool folder, qint64 size)
{
    if (folder) {
        browse(path);
        return;
    }
    const auto target = connection(m_share, true);
    if (!target)
        return;
    const QString name = leafName(path);
    const QString url = m_share.url(path);
    const QString localFolder = m_temp.path() + QLatin1Char('/') + QString::number(qHash(url), 16);
    const bool external = opensExternally(name) || size > kTextOpenLimit;
    post(target, QStringLiteral("Opening %1…").arg(name), [path, external, localFolder, name](Client &client, OpResult &result, const Progress &progress) {
        QBuffer buffer;
        buffer.open(QIODevice::WriteOnly);
        if (!external && client.readFile(path, &buffer, progress, kTextOpenLimit) && !looksBinary(buffer.data())) {
            result.data = buffer.data();
            return;
        }
        if (client.cancelled()) {
            result.ok = false;
            result.cancelled = true;
            return;
        }
        if (!client.error().isEmpty() && !client.tooLarge() && !external && buffer.data().isEmpty()) {
            result.ok = false;
            result.error = client.error();
            return;
        }
        QDir().mkpath(localFolder);
        result.localPath = QDir(localFolder).filePath(name);
        if (!buffer.data().isEmpty() && !client.tooLarge()) {
            QFile file(result.localPath);
            if (file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(buffer.data()) == buffer.data().size())
                return;
        }
        downloadPath(client, path, false, result.localPath, progress, result);
    }, [this, url, name](const OpResult &result) {
        if (!result.ok) {
            setStatus(result.cancelled ? QStringLiteral("Cancelled.") : result.error, !result.cancelled);
            return;
        }
        if (!result.localPath.isEmpty()) {
            if (m_openExternal)
                m_openExternal(result.localPath);
            else
                QDesktopServices::openUrl(QUrl::fromLocalFile(result.localPath));
            return;
        }
        if (openText)
            openText(url, name, QString::fromUtf8(result.data), true);
    });
}

void NetworkFilesPanel::downloadEntry(const QString &path, bool folder, const QString &localTarget)
{
    const auto target = connection(m_share, true);
    if (!target || localTarget.isEmpty())
        return;
    const QString name = leafName(path);
    post(target, QStringLiteral("Downloading %1…").arg(name), [path, folder, localTarget](Client &client, OpResult &result, const Progress &progress) {
        downloadPath(client, path, folder, localTarget, progress, result);
    }, [this, localTarget](const OpResult &result) {
        finish(result, QStringLiteral("Downloaded to %1.").arg(QDir::toNativeSeparators(localTarget)));
    });
}

void NetworkFilesPanel::uploadLocal(const QStringList &paths, const QString &folder)
{
    const auto target = connection(m_share, true);
    if (!target || paths.isEmpty())
        return;
    const QString label = paths.size() == 1 ? QStringLiteral("Uploading %1…").arg(QFileInfo(paths.first()).fileName())
                                            : QStringLiteral("Uploading %1 items…").arg(paths.size());
    post(target, label, [paths, folder](Client &client, OpResult &result, const Progress &progress) {
        for (const QString &path : paths) {
            if (!uploadPath(client, path, folder, progress, result))
                return;
        }
    }, [this, paths](const OpResult &result) {
        finish(result, paths.size() == 1 ? QStringLiteral("Uploaded %1.").arg(QFileInfo(paths.first()).fileName())
                                         : QStringLiteral("Uploaded %1 items.").arg(paths.size()));
    });
}

void NetworkFilesPanel::newFolder(const QString &folder)
{
    const auto target = connection(m_share, true);
    QString name = QStringLiteral("New folder");
    if (!target || !askText(QStringLiteral("New folder"), QStringLiteral("Name of the new folder:"), &name))
        return;
    post(target, QStringLiteral("Creating %1…").arg(name), [folder, name](Client &client, OpResult &result, const Progress &) {
        if (!client.makeFolder(joinPath(folder, name))) {
            result.ok = false;
            result.error = client.error();
        }
    }, [this](const OpResult &result) { finish(result, {}); });
}

void NetworkFilesPanel::newFile(const QString &folder)
{
    const auto target = connection(m_share, true);
    QString name = QStringLiteral("untitled.txt");
    if (!target || !askText(QStringLiteral("New file"), QStringLiteral("Name of the new file:"), &name))
        return;
    const QString path = joinPath(folder, name);
    post(target, QStringLiteral("Creating %1…").arg(name), [path](Client &client, OpResult &result, const Progress &) {
        if (!client.createFile(path)) {
            result.ok = false;
            result.error = client.error();
        }
    }, [this, path](const OpResult &result) {
        finish(result, {});
        if (result.ok && openText)
            openText(m_share.url(path), leafName(path), QString(), true);
    });
}

void NetworkFilesPanel::renameEntry(const QString &path)
{
    const auto target = connection(m_share, true);
    QString name = leafName(path);
    if (!target || !askText(QStringLiteral("Rename"), QStringLiteral("New name for %1:").arg(name), &name) || name == leafName(path))
        return;
    if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))) {
        setStatus(QStringLiteral("A name cannot contain / or \\."), true);
        return;
    }
    const QString to = joinPath(parentPath(path), name);
    post(target, QStringLiteral("Renaming %1…").arg(leafName(path)), [path, to](Client &client, OpResult &result, const Progress &) {
        if (!client.rename(path, to)) {
            result.ok = false;
            result.error = client.error();
        }
    }, [this](const OpResult &result) { finish(result, {}); });
}

void NetworkFilesPanel::deleteEntries(const QStringList &paths, const QList<bool> &folders)
{
    const auto target = connection(m_share, true);
    if (!target || paths.isEmpty())
        return;
    const QString name = leafName(paths.first());
    const QString question = paths.size() > 1 ? QStringLiteral("Delete %1 items?").arg(paths.size())
        : folders.value(0) ? QStringLiteral("Delete %1 and everything in it?").arg(name)
                           : QStringLiteral("Delete %1?").arg(name);
    if (!ask(question, QStringLiteral("This deletes it from %1. Network shares have no trash, so it cannot be undone.").arg(m_share.title())))
        return;
    post(target, QStringLiteral("Deleting…"), [paths](Client &client, OpResult &result, const Progress &) {
        for (const QString &path : paths) {
            if (!client.removeEntry(path)) {
                result.ok = false;
                result.error = client.error();
                return;
            }
        }
    }, [this](const OpResult &result) { finish(result, {}); });
}

void NetworkFilesPanel::copyEntries(const QStringList &paths, const QList<bool> &folders, bool move)
{
    FileClipboard &clipboard = fileClipboard();
    clipboard.items.clear();
    clipboard.move = move;
    for (int i = 0; i < paths.size(); ++i) {
        ClipboardItem item;
        item.network = true;
        item.shareId = m_share.id;
        item.path = paths[i];
        item.name = leafName(paths[i]);
        item.folder = folders.value(i);
        clipboard.items << item;
    }
    setBusy(m_busy);
    if (clipboardChanged)
        clipboardChanged();
    if (notify)
        notify(QStringLiteral("%1 %2. Open a folder and press Paste.").arg(paths.size() == 1 ? leafName(paths.first()) : QStringLiteral("%1 items").arg(paths.size()),
                                                                        move ? QStringLiteral("ready to move") : QStringLiteral("copied")),
               false);
}

void NetworkFilesPanel::paste(const QString &folder)
{
    const FileClipboard clipboard = fileClipboard();
    const auto target = connection(m_share, true);
    if (!target || clipboard.items.isEmpty())
        return;
    const QString destination = m_share.id;
    const QString stage = m_temp.path() + QStringLiteral("/stage-") + QString::number(QDateTime::currentMSecsSinceEpoch());
    QList<ClipboardItem> local;
    QList<ClipboardItem> same;
    QHash<QString, QList<ClipboardItem>> other;
    for (const ClipboardItem &item : clipboard.items) {
        if (!item.network)
            local << item;
        else if (item.shareId == destination)
            same << item;
        else
            other[item.shareId] << item;
    }
    auto staged = std::make_shared<QStringList>();
    auto steps = std::make_shared<QList<std::function<void(std::function<void(bool)>)>>>();
    for (auto it = other.begin(); it != other.end(); ++it) {
        const SavedShare source = m_store.share(it.key());
        const auto sourceConnection = source.id.isEmpty() ? nullptr : connection(source, true);
        const QList<ClipboardItem> items = it.value();
        steps->append([this, sourceConnection, items, stage, staged](std::function<void(bool)> next) {
            if (!sourceConnection) {
                setStatus(QStringLiteral("The share these files came from was removed."), true);
                next(false);
                return;
            }
            post(sourceConnection, QStringLiteral("Copying from the other share…"), [items, stage](Client &client, OpResult &result, const Progress &progress) {
                QDir().mkpath(stage);
                for (const ClipboardItem &item : items) {
                    const QString local = QDir(stage).filePath(item.name);
                    if (!downloadPath(client, item.path, item.folder, local, progress, result))
                        return;
                    result.entries << Entry{local, item.folder};
                }
            }, [this, staged, next](const OpResult &result) {
                if (!result.ok) {
                    finish(result, {});
                    next(false);
                    return;
                }
                for (const Entry &entry : result.entries)
                    *staged << entry.name;
                next(true);
            });
        });
    }
    const bool move = clipboard.move;
    steps->append([this, target, local, same, staged, folder, move, stage](std::function<void(bool)> next) {
        QStringList uploads;
        for (const ClipboardItem &item : local)
            uploads << item.localPath;
        uploads += *staged;
        post(target, QStringLiteral("Pasting…"), [uploads, same, folder, move, stage](Client &client, OpResult &result, const Progress &progress) {
            for (const QString &path : uploads) {
                if (!uploadPath(client, path, folder, progress, result))
                    return;
            }
            for (const ClipboardItem &item : same) {
                if (move && parentPath(item.path) == folder)
                    continue;
                const QString name = uniqueRemoteName(client, folder, item.name);
                if (move) {
                    if (!client.rename(item.path, joinPath(folder, name))) {
                        result.ok = false;
                        result.error = client.error();
                        return;
                    }
                    continue;
                }
                const QString copyStage = stage + QStringLiteral("-same");
                QDir().mkpath(copyStage);
                const QString local = QDir(copyStage).filePath(name);
                if (!downloadPath(client, item.path, item.folder, local, progress, result) || !uploadPath(client, local, folder, progress, result))
                    return;
            }
        }, [this, next](const OpResult &result) {
            if (!result.ok) {
                finish(result, {});
                next(false);
                return;
            }
            next(true);
        });
    });
    if (move) {
        for (auto it = other.begin(); it != other.end(); ++it) {
            const SavedShare source = m_store.share(it.key());
            const auto sourceConnection = source.id.isEmpty() ? nullptr : connection(source, false);
            const QList<ClipboardItem> items = it.value();
            steps->append([this, sourceConnection, items](std::function<void(bool)> next) {
                if (!sourceConnection) {
                    next(true);
                    return;
                }
                post(sourceConnection, QStringLiteral("Removing the moved files…"), [items](Client &client, OpResult &result, const Progress &) {
                    for (const ClipboardItem &item : items) {
                        if (!client.removeEntry(item.path)) {
                            result.ok = false;
                            result.error = client.error();
                            return;
                        }
                    }
                }, [this, next](const OpResult &result) {
                    if (!result.ok)
                        finish(result, {});
                    next(result.ok);
                });
            });
        }
        steps->append([local](std::function<void(bool)> next) {
            for (const ClipboardItem &item : local)
                QFile::moveToTrash(item.localPath);
            next(true);
        });
    }
    auto runner = std::make_shared<std::function<void(int)>>();
    QPointer<NetworkFilesPanel> self(this);
    *runner = [self, steps, runner, move](int index) {
        if (!self)
            return;
        if (index >= steps->size()) {
            if (move)
                fileClipboard().items.clear();
            self->finish({}, QStringLiteral("Pasted."));
            if (self->clipboardChanged)
                self->clipboardChanged();
            return;
        }
        steps->at(index)([runner, index](bool ok) {
            if (ok)
                (*runner)(index + 1);
        });
    };
    (*runner)(0);
}

void NetworkFilesPanel::pasteIntoLocal(const QString &folder, std::function<void(bool)> done)
{
    const FileClipboard clipboard = fileClipboard();
    if (clipboard.items.isEmpty()) {
        done(false);
        return;
    }
    QList<ClipboardItem> local;
    QHash<QString, QList<ClipboardItem>> network;
    for (const ClipboardItem &item : clipboard.items) {
        if (item.network)
            network[item.shareId] << item;
        else
            local << item;
    }
    const bool move = clipboard.move;
    auto remaining = std::make_shared<int>(network.size() + 1);
    auto success = std::make_shared<bool>(true);
    QPointer<NetworkFilesPanel> self(this);
    const auto step = [self, remaining, success, done, move](bool ok) {
        *success = *success && ok;
        if (--*remaining > 0)
            return;
        if (move && *success)
            fileClipboard().items.clear();
        if (self && self->clipboardChanged)
            self->clipboardChanged();
        done(*success);
    };
    for (auto it = network.begin(); it != network.end(); ++it) {
        const SavedShare source = m_store.share(it.key());
        const auto sourceConnection = source.id.isEmpty() ? nullptr : connection(source, true);
        if (!sourceConnection) {
            step(false);
            continue;
        }
        const QList<ClipboardItem> items = it.value();
        post(sourceConnection, network.size() == 1 ? QStringLiteral("Downloading to this computer…") : QString(), [items, folder, move](Client &client, OpResult &result, const Progress &progress) {
            for (const ClipboardItem &item : items) {
                const QString target = QDir(folder).filePath(uniqueLocalName(folder, item.name));
                if (!downloadPath(client, item.path, item.folder, target, progress, result))
                    return;
                if (move && !client.removeEntry(item.path)) {
                    result.ok = false;
                    result.error = client.error();
                    return;
                }
            }
        }, [self, step](const OpResult &result) {
            if (!result.ok && self) {
                self->setStatus(result.cancelled ? QStringLiteral("Cancelled.") : result.error, !result.cancelled);
                if (self->notify)
                    self->notify(result.cancelled ? QStringLiteral("Cancelled.") : result.error, !result.cancelled);
            }
            step(result.ok);
        });
    }
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [self, watcher, step] {
        const QString error = watcher->result();
        watcher->deleteLater();
        if (!error.isEmpty() && self && self->notify)
            self->notify(error, true);
        step(error.isEmpty());
    });
    watcher->setFuture(QtConcurrent::run([local, folder, move]() -> QString {
        for (const ClipboardItem &item : local) {
            const QFileInfo source(item.localPath);
            if (move && source.absolutePath() == QDir(folder).absolutePath())
                continue;
            const QString target = QDir(folder).filePath(uniqueLocalName(folder, source.fileName()));
            if (move && QFile::rename(item.localPath, target))
                continue;
            QString error;
            if (!copyLocal(item.localPath, target, &error))
                return error;
            if (move)
                QFile::moveToTrash(item.localPath);
        }
        return {};
    }));
}

void NetworkFilesPanel::showInfo(QTreeWidgetItem *item)
{
    if (!item)
        return;
    const bool folder = item->data(0, kFolderRole).toBool();
    const QString path = item->data(0, kPathRole).toString();
    const QDateTime modified = item->data(0, kModifiedRole).toDateTime();
    QStringList lines = {QStringLiteral("Name: %1").arg(item->text(0)), QStringLiteral("Kind: %1").arg(kindOf(item->text(0), folder))};
    if (!folder)
        lines << QStringLiteral("Size: %1 (%2 bytes)").arg(formatSize(item->data(0, kSizeRole).toLongLong()), QLocale().toString(item->data(0, kSizeRole).toLongLong()));
    if (modified.isValid())
        lines << QStringLiteral("Modified: %1").arg(QLocale().toString(modified.toLocalTime(), QLocale::LongFormat));
    lines << QStringLiteral("Location: %1").arg(m_share.url(path));
    QMessageBox box(this);
    box.setWindowTitle(item->text(0));
    box.setText(lines.join(QLatin1Char('\n')));
    box.setTextInteractionFlags(Qt::TextSelectableByMouse);
    box.exec();
}

void NetworkFilesPanel::showMenu(const QPoint &pos)
{
    if (m_share.id.isEmpty())
        return;
    QTreeWidgetItem *item = m_tree->itemAt(pos);
    if (item && item->data(0, kPlaceholderRole).toBool())
        item = nullptr;
    QList<QTreeWidgetItem *> selected = m_tree->selectedItems();
    if (item && !selected.contains(item))
        selected = {item};
    if (!item)
        selected.clear();
    QStringList paths;
    QList<bool> folders;
    for (QTreeWidgetItem *entry : std::as_const(selected)) {
        if (entry->data(0, kPlaceholderRole).toBool())
            continue;
        paths << entry->data(0, kPathRole).toString();
        folders << entry->data(0, kFolderRole).toBool();
    }
    const bool folder = item && item->data(0, kFolderRole).toBool();
    const QString path = item ? item->data(0, kPathRole).toString() : m_path;
    const QString into = item ? (folder ? path : parentPath(path)) : m_path;

    QMenu menu(this);
    QAction *open = item ? menu.addAction(folder ? QStringLiteral("Open folder") : QStringLiteral("Open")) : nullptr;
    QAction *download = item ? menu.addAction(QStringLiteral("Download…")) : nullptr;
    if (item)
        menu.addSeparator();
    QAction *copy = item ? menu.addAction(QStringLiteral("Copy")) : nullptr;
    QAction *move = item ? menu.addAction(QStringLiteral("Move")) : nullptr;
    QAction *pasteAction = menu.addAction(folder ? QStringLiteral("Paste into %1").arg(item->text(0)) : QStringLiteral("Paste"));
    pasteAction->setEnabled(!fileClipboard().items.isEmpty());
    menu.addSeparator();
    QAction *rename = item && paths.size() == 1 ? menu.addAction(QStringLiteral("Rename…")) : nullptr;
    QAction *remove = item ? menu.addAction(QStringLiteral("Delete…")) : nullptr;
    menu.addSeparator();
    QAction *makeFolder = menu.addAction(QStringLiteral("New folder"));
    QAction *makeFile = menu.addAction(QStringLiteral("New file"));
    QAction *upload = menu.addAction(QStringLiteral("Upload…"));
    menu.addSeparator();
    QAction *link = item ? menu.addAction(QStringLiteral("Copy link")) : nullptr;
    QAction *info = item && paths.size() == 1 ? menu.addAction(QStringLiteral("Info")) : nullptr;
    QAction *refresh = menu.addAction(QStringLiteral("Refresh"));

    QAction *chosen = menu.exec(m_tree->viewport()->mapToGlobal(pos));
    if (!chosen)
        return;
    if (chosen == open) {
        openEntry(path, folder, item->data(0, kSizeRole).toLongLong());
    } else if (chosen == download) {
        if (folder) {
            const QString target = QFileDialog::getExistingDirectory(this, QStringLiteral("Download %1 to").arg(item->text(0)),
                                                                     QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
            if (!target.isEmpty())
                downloadEntry(path, true, QDir(target).filePath(uniqueLocalName(target, item->text(0))));
        } else {
            const QString target = QFileDialog::getSaveFileName(this, QStringLiteral("Download %1").arg(item->text(0)),
                                                                QDir(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)).filePath(item->text(0)));
            if (!target.isEmpty())
                downloadEntry(path, false, target);
        }
    } else if (chosen == copy || chosen == move) {
        copyEntries(paths, folders, chosen == move);
    } else if (chosen == pasteAction) {
        paste(into);
    } else if (chosen == rename) {
        renameEntry(path);
    } else if (chosen == remove) {
        deleteEntries(paths, folders);
    } else if (chosen == makeFolder) {
        newFolder(into);
    } else if (chosen == makeFile) {
        newFile(into);
    } else if (chosen == upload) {
        const QStringList files = QFileDialog::getOpenFileNames(this, QStringLiteral("Upload to %1").arg(m_share.title()),
                                                                QStandardPaths::writableLocation(QStandardPaths::HomeLocation));
        if (!files.isEmpty())
            uploadLocal(files, into);
    } else if (chosen == link) {
        QGuiApplication::clipboard()->setText(m_share.url(path));
        if (notify)
            notify(QStringLiteral("Link copied."), false);
    } else if (chosen == info) {
        showInfo(item);
    } else if (chosen == refresh) {
        reload();
    }
}

}
