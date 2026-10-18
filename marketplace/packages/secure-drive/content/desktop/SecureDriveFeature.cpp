#include "SecureDriveFeature.h"

#include "AppPaths.h"
#include "SecureDriveEditors.h"

#include <QBuffer>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QSysInfo>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

namespace forkmesh::securedrive {
namespace {

constexpr int kIdleMinutes = 15;
constexpr int kPollMs = 10000;
constexpr qint64 kViewerLimit = qint64(256) << 20;
const char kLastSlotKey[] = "secureDrive/lastSlot";

QLabel *mutedLabel(const QString &text)
{
    auto *label = new QLabel(text);
    label->setObjectName(QStringLiteral("mutedLabel"));
    label->setWordWrap(true);
    return label;
}

QLineEdit *passphraseEdit(const QString &placeholder)
{
    auto *edit = new QLineEdit;
    edit->setEchoMode(QLineEdit::Password);
    edit->setPlaceholderText(placeholder);
    edit->setInputMethodHints(Qt::ImhHiddenText | Qt::ImhSensitiveData | Qt::ImhNoPredictiveText
                              | Qt::ImhNoAutoUppercase);
    return edit;
}

QPushButton *button(const QString &text, const char *style = "ghostButton")
{
    auto *b = new QPushButton(text);
    b->setObjectName(QLatin1String(style));
    return b;
}

void restyle(QWidget *widget, const QString &objectName)
{
    widget->setObjectName(objectName);
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

QString when(qint64 ms)
{
    if (ms <= 0)
        return QStringLiteral("—");
    return QLocale().toString(QDateTime::fromMSecsSinceEpoch(ms), QLocale::ShortFormat);
}

QString kindLabel(const FileEntry &entry)
{
    if (entry.kind == QLatin1String("document"))
        return QStringLiteral("Document");
    if (entry.kind == QLatin1String("spreadsheet"))
        return QStringLiteral("Spreadsheet");
    if (entry.kind == QLatin1String("drawing"))
        return QStringLiteral("Drawing");
    if (entry.kind == QLatin1String("text"))
        return QStringLiteral("Text");
    if (entry.kind == QLatin1String("image"))
        return QStringLiteral("Picture");
    const QString suffix = QFileInfo(entry.name).suffix().toUpper();
    return suffix.isEmpty() ? QStringLiteral("File") : suffix + QStringLiteral(" file");
}

QString safeFileName(QString name)
{
    static const QString forbidden = QStringLiteral("/\\:*?\"<>|");
    for (QChar &c : name)
        if (forbidden.contains(c) || c.unicode() < 32)
            c = QLatin1Char('_');
    name = name.trimmed();
    if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String(".."))
        name = QStringLiteral("file");
    return name;
}

QByteArray fileDigest(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(&file);
    return hash.result();
}

void overwriteAndRemove(const QString &path)
{
    QFile file(path);
    if (file.open(QIODevice::ReadWrite)) {
        const QByteArray zeros(1 << 16, '\0');
        qint64 left = file.size();
        while (left > 0) {
            const qint64 step = std::min<qint64>(left, zeros.size());
            if (file.write(zeros.constData(), step) != step)
                break;
            left -= step;
        }
        file.flush();
        file.close();
    }
    QFile::remove(path);
}

QString withSuffix(QString name, const QString &suffix)
{
    name = name.trimmed();
    if (!name.toLower().endsWith(QLatin1Char('.') + suffix))
        name += QLatin1Char('.') + suffix;
    return name;
}

}  // namespace

SecureDriveFeature::SecureDriveFeature(const forkmesh::market::NativePackageHost &host, QWidget *parent)
    : forkmesh::market::NativePackageWidget(parent), m_searchIndexChanged(host.searchIndexChanged)
{
    setObjectName(QStringLiteral("secureDriveSection"));
    setAcceptDrops(true);
    m_root = QDir(forkmesh::paths::localDataRoot()).filePath(QStringLiteral("secure-drive"));
    m_state = std::make_unique<LocalState>(m_root);
    wipeExternalFolder();

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(20, 18, 20, 20);
    outer->setSpacing(10);
    auto *heading = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("Secure Drive"));
    title->setObjectName(QStringLiteral("pageTitle"));
    heading->addWidget(title);
    heading->addWidget(mutedLabel(QStringLiteral(
                           "One encrypted file on your router's USB drive holds every document, version and log entry. The keys live only on the devices you approve.")),
                       1);
    m_lockButton = button(QStringLiteral("Lock"));
    heading->addWidget(m_lockButton);
    outer->addLayout(heading);
    m_status = mutedLabel(QString());
    outer->addWidget(m_status);

    m_stack = new QStackedWidget;
    m_stack->addWidget(buildSetupPanel());
    m_stack->addWidget(buildUnlockPanel());
    m_stack->addWidget(buildMainPanel());
    outer->addWidget(m_stack, 1);

    m_idleTimer = new QTimer(this);
    m_idleTimer->setSingleShot(true);
    connect(m_idleTimer, &QTimer::timeout, this, [this] {
        for (const QPointer<EditorWindow> &editor : std::as_const(m_editors))
            if (editor && editor->isDirty()) {
                touch();
                return;
            }
        if (!m_external.isEmpty()) {
            touch();
            return;
        }
        lock(QStringLiteral("Locked after %1 idle minutes. The keys were cleared from memory.").arg(kIdleMinutes));
    });
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(kPollMs);
    connect(m_pollTimer, &QTimer::timeout, this, [this] { poll(); });
    connect(m_lockButton, &QPushButton::clicked, this, [this] { lock(); });
    installEventFilter(this);
    showPanel(m_state->exists() ? UnlockPanel : SetupPanel);
}

SecureDriveFeature::~SecureDriveFeature()
{
    if (m_watcher)
        m_watcher->waitForFinished();
    for (const QPointer<EditorWindow> &editor : std::as_const(m_editors))
        if (editor)
            editor->discardAndClose();
    closeExternalCopies();
    wipeExternalFolder();
    m_drive.reset();
    m_state->lock();
}

QWidget *SecureDriveFeature::buildSetupPanel()
{
    auto *panel = new QWidget;
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 12, 0, 0);
    auto *heading = new QLabel(QStringLiteral("Set up Secure Drive on this computer"));
    heading->setObjectName(QStringLiteral("sectionTitle"));
    layout->addWidget(heading);
    layout->addWidget(mutedLabel(QStringLiteral(
        "This computer gets its own keys: X25519 and post-quantum ML-KEM-768 to receive drive keys, and Ed25519 to sign everything it writes. "
        "They never leave this computer, and on its disk they are locked by a passphrase you choose (Argon2id, 64 MiB). "
        "ForkMesh cannot reset the passphrase; if you forget it, another approved computer can invite this one again.")));
    auto *form = new QFormLayout;
    m_setupName = new QLineEdit(QSysInfo::machineHostName());
    m_setupName->setMaxLength(64);
    m_setupPassphrase = passphraseEdit(QStringLiteral("At least 10 characters; a few unrelated words work well"));
    m_setupConfirm = passphraseEdit(QStringLiteral("Type it again"));
    form->addRow(QStringLiteral("Computer name"), m_setupName);
    form->addRow(QStringLiteral("Passphrase"), m_setupPassphrase);
    form->addRow(QStringLiteral("Confirm"), m_setupConfirm);
    layout->addLayout(form);
    auto *create = button(QStringLiteral("Create this computer's keys"), "primaryButton");
    auto *row = new QHBoxLayout;
    row->addWidget(create);
    row->addStretch(1);
    layout->addLayout(row);
    layout->addStretch(1);
    connect(create, &QPushButton::clicked, this, [this] { createDevice(); });
    connect(m_setupConfirm, &QLineEdit::returnPressed, this, [this] { createDevice(); });
    return panel;
}

QWidget *SecureDriveFeature::buildUnlockPanel()
{
    auto *panel = new QWidget;
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 12, 0, 0);
    auto *heading = new QLabel(QStringLiteral("Unlock Secure Drive"));
    heading->setObjectName(QStringLiteral("sectionTitle"));
    layout->addWidget(heading);
    layout->addWidget(mutedLabel(QStringLiteral("Your passphrase opens this computer's keys. It is never stored or sent anywhere.")));
    m_unlockPassphrase = passphraseEdit(QStringLiteral("Passphrase"));
    auto *row = new QHBoxLayout;
    row->addWidget(m_unlockPassphrase, 1);
    auto *unlockButton = button(QStringLiteral("Unlock"), "primaryButton");
    row->addWidget(unlockButton);
    layout->addLayout(row);
    layout->addStretch(1);
    connect(unlockButton, &QPushButton::clicked, this, [this] { unlock(); });
    connect(m_unlockPassphrase, &QLineEdit::returnPressed, this, [this] { unlock(); });
    return panel;
}

QWidget *SecureDriveFeature::buildMainPanel()
{
    auto *panel = new QWidget;
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto *bar = new QHBoxLayout;
    m_drivePicker = new QComboBox;
    m_drivePicker->setMinimumWidth(260);
    m_drivePicker->setAccessibleName(QStringLiteral("Drive"));
    bar->addWidget(m_drivePicker);
    auto *create = button(QStringLiteral("New drive…"));
    auto *join = button(QStringLiteral("Join a drive…"));
    auto *more = button(QStringLiteral("⋯"));
    more->setAccessibleName(QStringLiteral("More drive actions"));
    auto *menu = new QMenu(more);
    menu->addAction(QStringLiteral("Show the drive file"), this, [this] {
        if (m_drive && m_drive->membership())
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(m_drive->membership()->container).absolutePath()));
    });
    menu->addAction(QStringLiteral("Check the drive now"), this, [this] { scanCurrent(false); });
    menu->addSeparator();
    menu->addAction(QStringLiteral("Change passphrase…"), this, [this] { changePassphrase(); });
    menu->addAction(QStringLiteral("Forget this drive on this computer…"), this, [this] { forgetDrive(); });
    more->setMenu(menu);
    bar->addWidget(create);
    bar->addWidget(join);
    bar->addStretch(1);
    bar->addWidget(more);
    layout->addLayout(bar);
    connect(create, &QPushButton::clicked, this, [this] { createDrive(); });
    connect(join, &QPushButton::clicked, this, [this] { joinDrive(); });
    connect(m_drivePicker, &QComboBox::activated, this, [this](int index) {
        selectDrive(m_drivePicker->itemData(index).toString());
    });

    m_banner = mutedLabel(QString());
    m_banner->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_banner);

    m_driveStack = new QStackedWidget;
    auto *empty = new QWidget;
    auto *emptyLayout = new QVBoxLayout(empty);
    auto *emptyTitle = new QLabel(QStringLiteral("No drive yet"));
    emptyTitle->setObjectName(QStringLiteral("sectionTitle"));
    emptyLayout->addWidget(emptyTitle);
    emptyLayout->addWidget(mutedLabel(QStringLiteral(
        "1. Plug a USB drive into your router and turn on its file sharing (often called USB storage, SMB or Samba).\n"
        "2. Open the shared folder once in your file manager so this computer can reach it, for example \\\\router\\usb on Windows or smb://router/usb on macOS and Linux.\n"
        "3. Choose New drive and save the drive file there. Other computers and phones open the same file and join with an invitation code that you approve here.\n\n"
        "The share holds one file of random-looking bytes. File names, contents, how many files there are, every version and who opened what are all inside it, encrypted. The router sees only the file's size and when it changes.")));
    emptyLayout->addStretch(1);
    m_driveStack->addWidget(empty);

    auto *pending = new QWidget;
    auto *pendingLayout = new QVBoxLayout(pending);
    auto *pendingTitle = new QLabel(QStringLiteral("Waiting for approval"));
    pendingTitle->setObjectName(QStringLiteral("sectionTitle"));
    pendingLayout->addWidget(pendingTitle);
    m_pendingText = mutedLabel(QString());
    pendingLayout->addWidget(m_pendingText);
    m_pendingSafety = new QLabel;
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSizeF(mono.pointSizeF() * 1.8);
    m_pendingSafety->setFont(mono);
    m_pendingSafety->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pendingLayout->addWidget(m_pendingSafety);
    auto *pendingRow = new QHBoxLayout;
    auto *check = button(QStringLiteral("Check now"), "primaryButton");
    auto *cancel = button(QStringLiteral("Cancel request"), "dangerButton");
    pendingRow->addWidget(check);
    pendingRow->addWidget(cancel);
    pendingRow->addStretch(1);
    pendingLayout->addLayout(pendingRow);
    pendingLayout->addStretch(1);
    connect(check, &QPushButton::clicked, this, [this] { scanCurrent(false); });
    connect(cancel, &QPushButton::clicked, this, [this] { forgetDrive(); });
    m_driveStack->addWidget(pending);

    m_tabs = new QTabWidget;
    m_tabs->addTab(buildFilesTab(), QStringLiteral("Files"));
    m_tabs->addTab(buildActivityTab(), QStringLiteral("Activity"));
    m_tabs->addTab(buildDevicesTab(), QStringLiteral("Computers"));
    m_driveStack->addWidget(m_tabs);
    layout->addWidget(m_driveStack, 1);
    return panel;
}

QWidget *SecureDriveFeature::buildFilesTab()
{
    auto *tab = new QWidget;
    auto *layout = new QVBoxLayout(tab);
    auto *bar = new QHBoxLayout;
    m_newButton = button(QStringLiteral("New"), "primaryButton");
    auto *menu = new QMenu(m_newButton);
    menu->addAction(QStringLiteral("Document"), this, [this] { newFile(QStringLiteral("document")); });
    menu->addAction(QStringLiteral("Spreadsheet"), this, [this] { newFile(QStringLiteral("spreadsheet")); });
    menu->addAction(QStringLiteral("Drawing"), this, [this] { newFile(QStringLiteral("drawing")); });
    m_newButton->setMenu(menu);
    m_addButton = button(QStringLiteral("Add files…"));
    m_fileFilter = new QLineEdit;
    m_fileFilter->setPlaceholderText(QStringLiteral("Filter files"));
    m_fileFilter->setClearButtonEnabled(true);
    bar->addWidget(m_newButton);
    bar->addWidget(m_addButton);
    bar->addWidget(m_fileFilter, 1);
    layout->addLayout(bar);

    m_files = new QTreeWidget;
    m_files->setRootIsDecorated(false);
    m_files->setUniformRowHeights(true);
    m_files->setSortingEnabled(true);
    m_files->setHeaderLabels({QStringLiteral("Name"), QStringLiteral("Type"), QStringLiteral("Size"),
                              QStringLiteral("Modified"), QStringLiteral("By"), QStringLiteral("Last opened")});
    m_files->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_files->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_files->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_files, 1);

    auto *actions = new QHBoxLayout;
    const QList<QPair<QString, std::function<void(const QString &)>>> specs{
        {QStringLiteral("Open"), [this](const QString &id) { openFile(id); }},
        {QStringLiteral("Open in another app"), [this](const QString &id) { openExternally(id); }},
        {QStringLiteral("Save a decrypted copy…"), [this](const QString &id) { saveCopy(id); }},
        {QStringLiteral("Versions…"), [this](const QString &id) { showVersions(id); }},
        {QStringLiteral("Rename…"), [this](const QString &id) { renameFile(id); }},
        {QStringLiteral("Delete…"), [this](const QString &id) { deleteFile(id); }}};
    for (const auto &spec : specs) {
        auto *b = button(spec.first, spec.first.startsWith(QLatin1String("Delete")) ? "dangerButton" : "ghostButton");
        const auto action = spec.second;
        connect(b, &QPushButton::clicked, this, [this, action] {
            const QString id = selectedFileId();
            if (!id.isEmpty())
                action(id);
        });
        actions->addWidget(b);
        m_fileButtons.append(b);
    }
    actions->addStretch(1);
    layout->addLayout(actions);

    m_externalBar = new QWidget;
    auto *externalLayout = new QHBoxLayout(m_externalBar);
    externalLayout->setContentsMargins(0, 0, 0, 0);
    m_externalText = mutedLabel(QString());
    externalLayout->addWidget(m_externalText, 1);
    auto *saveBack = button(QStringLiteral("Save changes to the drive"), "primaryButton");
    auto *closeCopies = button(QStringLiteral("Delete decrypted copies"));
    externalLayout->addWidget(saveBack);
    externalLayout->addWidget(closeCopies);
    m_externalBar->hide();
    layout->addWidget(m_externalBar);

    connect(m_addButton, &QPushButton::clicked, this, [this] {
        const QStringList paths = QFileDialog::getOpenFileNames(this, QStringLiteral("Add files to the drive"));
        if (!paths.isEmpty())
            addFiles(paths);
    });
    connect(m_fileFilter, &QLineEdit::textChanged, this, [this] { renderFiles(); });
    connect(m_files, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
        openFile(item->data(0, Qt::UserRole).toString());
    });
    connect(m_files, &QTreeWidget::itemSelectionChanged, this, [this] {
        const bool any = !selectedFileId().isEmpty();
        for (QPushButton *b : std::as_const(m_fileButtons))
            b->setEnabled(any);
    });
    connect(m_files, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &point) {
        QTreeWidgetItem *item = m_files->itemAt(point);
        if (!item)
            return;
        m_files->setCurrentItem(item);
        QMenu contextMenu(this);
        for (QPushButton *b : std::as_const(m_fileButtons))
            contextMenu.addAction(b->text(), b, &QPushButton::click);
        contextMenu.exec(m_files->viewport()->mapToGlobal(point));
    });
    connect(saveBack, &QPushButton::clicked, this, [this] { saveExternalCopies(); });
    connect(closeCopies, &QPushButton::clicked, this, [this] {
        bool changed = false;
        for (const ExternalCopy &copy : std::as_const(m_external))
            changed = changed || fileDigest(copy.path) != copy.digest;
        if (changed
            && QMessageBox::question(this, QStringLiteral("Delete decrypted copies?"),
                                     QStringLiteral("Some copies were changed and those changes are not on the drive. Delete them anyway?"),
                                     QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel)
                   != QMessageBox::Yes)
            return;
        closeExternalCopies();
    });
    return tab;
}

QWidget *SecureDriveFeature::buildActivityTab()
{
    auto *tab = new QWidget;
    auto *layout = new QVBoxLayout(tab);
    layout->addWidget(mutedLabel(QStringLiteral(
        "Every open, save, export, approval and removal, as each computer signed it. Records are chained per computer and cross-checked, "
        "so a deleted, edited, replayed or rolled-back record shows up as a problem above.")));
    auto *bar = new QHBoxLayout;
    m_activityFilter = new QLineEdit;
    m_activityFilter->setPlaceholderText(QStringLiteral("Filter activity"));
    m_activityFilter->setClearButtonEnabled(true);
    auto *exportButton = button(QStringLiteral("Export as CSV…"));
    bar->addWidget(m_activityFilter, 1);
    bar->addWidget(exportButton);
    layout->addLayout(bar);
    m_activity = new QTreeWidget;
    m_activity->setRootIsDecorated(false);
    m_activity->setUniformRowHeights(true);
    m_activity->setHeaderLabels({QStringLiteral("Time"), QStringLiteral("Computer"), QStringLiteral("Activity")});
    m_activity->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_activity->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    layout->addWidget(m_activity, 1);
    connect(m_activityFilter, &QLineEdit::textChanged, this, [this] { renderActivity(); });
    connect(exportButton, &QPushButton::clicked, this, [this] { exportActivity(); });
    return tab;
}

QWidget *SecureDriveFeature::buildDevicesTab()
{
    auto *tab = new QWidget;
    auto *layout = new QVBoxLayout(tab);
    m_requestsBox = new QWidget;
    auto *requestsLayout = new QVBoxLayout(m_requestsBox);
    requestsLayout->setContentsMargins(0, 0, 0, 0);
    auto *requestsTitle = new QLabel(QStringLiteral("Asking to join"));
    requestsTitle->setObjectName(QStringLiteral("sectionTitle"));
    requestsLayout->addWidget(requestsTitle);
    requestsLayout->addWidget(mutedLabel(QStringLiteral(
        "Approve a computer only if its screen shows the same safety number. A different number means someone else used the code.")));
    m_requests = new QTreeWidget;
    m_requests->setRootIsDecorated(false);
    m_requests->setHeaderLabels({QStringLiteral("Computer"), QStringLiteral("Safety number"), QStringLiteral("Asked")});
    m_requests->setMaximumHeight(140);
    m_requests->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_requests->header()->setStretchLastSection(true);
    requestsLayout->addWidget(m_requests);
    auto *requestRow = new QHBoxLayout;
    auto *approve = button(QStringLiteral("Approve…"), "primaryButton");
    auto *deny = button(QStringLiteral("Turn down"), "dangerButton");
    requestRow->addWidget(approve);
    requestRow->addWidget(deny);
    requestRow->addStretch(1);
    requestsLayout->addLayout(requestRow);
    layout->addWidget(m_requestsBox);

    auto *title = new QLabel(QStringLiteral("Approved computers"));
    title->setObjectName(QStringLiteral("sectionTitle"));
    layout->addWidget(title);
    m_devices = new QTreeWidget;
    m_devices->setRootIsDecorated(false);
    m_devices->setHeaderLabels({QStringLiteral("Computer"), QStringLiteral("Safety number"),
                                QStringLiteral("Added"), QStringLiteral("Status")});
    m_devices->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_devices->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    layout->addWidget(m_devices, 1);
    auto *row = new QHBoxLayout;
    m_inviteButton = button(QStringLiteral("Invite a computer…"), "primaryButton");
    m_removeDeviceButton = button(QStringLiteral("Remove…"), "dangerButton");
    row->addWidget(m_inviteButton);
    row->addWidget(m_removeDeviceButton);
    row->addStretch(1);
    layout->addLayout(row);
    auto *inviteRow = new QHBoxLayout;
    m_inviteNote = mutedLabel(QString());
    m_cancelInvitesButton = button(QStringLiteral("Cancel open invitations"));
    inviteRow->addWidget(m_inviteNote, 1);
    inviteRow->addWidget(m_cancelInvitesButton);
    layout->addLayout(inviteRow);
    m_ownSafety = mutedLabel(QString());
    m_ownSafety->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_ownSafety);

    const auto selectedRequest = [this] {
        QTreeWidgetItem *item = m_requests->currentItem();
        return item ? item->data(0, Qt::UserRole).toInt() : -1;
    };
    connect(approve, &QPushButton::clicked, this, [this, selectedRequest] {
        const int mailbox = selectedRequest();
        if (mailbox >= 0)
            approveRequest(mailbox);
    });
    connect(deny, &QPushButton::clicked, this, [this, selectedRequest] {
        const int mailbox = selectedRequest();
        if (mailbox >= 0)
            denyRequest(mailbox);
    });
    connect(m_inviteButton, &QPushButton::clicked, this, [this] { invite(); });
    connect(m_removeDeviceButton, &QPushButton::clicked, this, [this] {
        QTreeWidgetItem *item = m_devices->currentItem();
        if (item)
            removeDevice(item->data(0, Qt::UserRole).toString());
    });
    connect(m_devices, &QTreeWidget::itemSelectionChanged, this, [this] {
        QTreeWidgetItem *item = m_devices->currentItem();
        m_removeDeviceButton->setEnabled(item && item->data(0, Qt::UserRole + 1).toBool());
    });
    connect(m_cancelInvitesButton, &QPushButton::clicked, this, [this] { cancelInvites(); });
    return tab;
}

void SecureDriveFeature::showPanel(Panel panel)
{
    m_stack->setCurrentIndex(int(panel));
    m_lockButton->setVisible(panel == MainPanel);
    if (panel == UnlockPanel)
        m_unlockPassphrase->setFocus();
    else if (panel == SetupPanel)
        m_setupPassphrase->setFocus();
}

void SecureDriveFeature::setStatus(const QString &text, bool error)
{
    restyle(m_status, error ? QStringLiteral("errorLabel") : QStringLiteral("mutedLabel"));
    m_status->setText(text);
    m_status->setVisible(!text.isEmpty());
}

QString SecureDriveFeature::explain(const QString &code) const
{
    static const QHash<QString, QString> texts{
        {QStringLiteral("locked"), QStringLiteral("Unlock Secure Drive first.")},
        {QStringLiteral("wrong_passphrase"), QStringLiteral("That passphrase is not right.")},
        {QStringLiteral("passphrase_too_short"), QStringLiteral("Use at least 10 characters; a few unrelated words work well.")},
        {QStringLiteral("device_unreadable"), QStringLiteral("This computer's Secure Drive key file cannot be read.")},
        {QStringLiteral("device_write_failed"), QStringLiteral("Could not save this computer's key file.")},
        {QStringLiteral("folder_unavailable"), QStringLiteral("The folder cannot be reached or is read-only. Check that the router's drive is plugged in and shared, and that this computer can write to it.")},
        {QStringLiteral("container_exists"), QStringLiteral("A file with that name already exists. Pick a new name, or use Join a drive to open an existing drive.")},
        {QStringLiteral("container_unavailable"), QStringLiteral("The drive file cannot be reached or is read-only. Check that the router's drive is plugged in and shared, and that this device can write to it.")},
        {QStringLiteral("not_a_drive"), QStringLiteral("That file is not a Secure Drive.")},
        {QStringLiteral("drive_unreadable"), QStringLiteral("This device cannot read the drive's index with its keys. It may have been removed from the drive, or the file was damaged.")},
        {QStringLiteral("drive_busy"), QStringLiteral("Another device has been writing to the drive for a long time. Try again in a minute.")},
        {QStringLiteral("drive_changed"), QStringLiteral("Another device changed the drive at the same moment. Try again.")},
        {QStringLiteral("drive_full"), QStringLiteral("The drive has no room left. Free space on the router's USB drive, or delete files.")},
        {QStringLiteral("drive_index_full"), QStringLiteral("The drive's index is full. Delete old files or versions.")},
        {QStringLiteral("folder_already_connected"), QStringLiteral("This device is already connected to that drive.")},
        {QStringLiteral("invalid_invite_code"), QStringLiteral("That invitation code is not valid. Check it for typos.")},
        {QStringLiteral("invite_expired"), QStringLiteral("That invitation expired or was cancelled. Create a new one.")},
        {QStringLiteral("unknown_request"), QStringLiteral("That request is no longer on the drive.")},
        {QStringLiteral("content_damaged"), QStringLiteral("The file's encrypted content was changed or damaged, so it was not opened. This is recorded in the activity log.")},
        {QStringLiteral("content_missing"), QStringLiteral("The file's encrypted content is missing from the drive.")},
        {QStringLiteral("drive_write_failed"), QStringLiteral("Could not write to the drive. Check that it is still connected and has free space.")},
        {QStringLiteral("device_removed"), QStringLiteral("This computer was removed from the drive and can no longer change it.")},
        {QStringLiteral("cannot_remove_self"), QStringLiteral("To take this computer off the drive, use Forget this drive.")},
        {QStringLiteral("invalid_name"), QStringLiteral("Use a name without slashes, up to 255 characters.")},
        {QStringLiteral("argon2id_unavailable"), QStringLiteral("This computer's OpenSSL is older than 3.5, which Secure Drive needs for Argon2id and ML-KEM.")},
        {QStringLiteral("mlkem_unavailable"), QStringLiteral("This computer's OpenSSL is older than 3.5, which Secure Drive needs for Argon2id and ML-KEM.")},
        {QStringLiteral("read_failed"), QStringLiteral("Could not read that file on this computer.")},
        {QStringLiteral("write_failed"), QStringLiteral("Could not write the decrypted copy.")},
    };
    return texts.value(code, code.isEmpty() ? QStringLiteral("Something went wrong.") : code);
}

void SecureDriveFeature::run(const QString &busyText, std::function<Outcome()> work,
                             std::function<void(const Outcome &)> done)
{
    if (m_busy) {
        setStatus(QStringLiteral("Secure Drive is still busy with the last task. Try again in a moment."), true);
        return;
    }
    m_busy = true;
    if (!busyText.isEmpty())
        setStatus(busyText);
    auto *watcher = new QFutureWatcher<Outcome>(this);
    m_watcher = watcher;
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, done = std::move(done)] {
        Outcome outcome = watcher->result();
        watcher->deleteLater();
        m_watcher = nullptr;
        m_busy = false;
        if (done)
            done(outcome);
        outcome.bytes.fill('\0');
        if (m_lockPending) {
            m_lockPending = false;
            lock(m_lockNote);
        }
    });
    watcher->setFuture(QtConcurrent::run(std::move(work)));
}

void SecureDriveFeature::touch()
{
    if (m_state && m_state->isUnlocked())
        m_idleTimer->start(kIdleMinutes * 60 * 1000);
}

bool SecureDriveFeature::eventFilter(QObject *watched, QEvent *event)
{
    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::KeyPress:
    case QEvent::Wheel:
        touch();
        break;
    default:
        break;
    }
    return forkmesh::market::NativePackageWidget::eventFilter(watched, event);
}

void SecureDriveFeature::dragEnterEvent(QDragEnterEvent *event)
{
    if (m_state->isUnlocked() && m_drive && m_drive->membership() && m_drive->membership()->isMember()
        && event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void SecureDriveFeature::dropEvent(QDropEvent *event)
{
    QStringList paths;
    for (const QUrl &url : event->mimeData()->urls())
        if (url.isLocalFile() && QFileInfo(url.toLocalFile()).isFile())
            paths << url.toLocalFile();
    if (!paths.isEmpty()) {
        event->acceptProposedAction();
        addFiles(paths);
    }
}

void SecureDriveFeature::refresh()
{
    if (m_busy)
        return;
    if (!m_state->isUnlocked()) {
        showPanel(m_state->exists() ? UnlockPanel : SetupPanel);
        return;
    }
    scanCurrent(false);
}

void SecureDriveFeature::createDevice()
{
    const QString passphrase = m_setupPassphrase->text();
    if (passphrase != m_setupConfirm->text()) {
        setStatus(QStringLiteral("The two passphrases do not match."), true);
        return;
    }
    if (passphrase.size() < 10) {
        setStatus(explain(QStringLiteral("passphrase_too_short")), true);
        return;
    }
    const QString name = m_setupName->text().trimmed();
    m_setupPassphrase->clear();
    m_setupConfirm->clear();
    LocalState *state = m_state.get();
    run(QStringLiteral("Creating this computer's keys…"),
        [state, passphrase, name] {
            Failure failure;
            Outcome outcome;
            outcome.ok = state->create(passphrase, name, &failure);
            outcome.code = failure.code;
            return outcome;
        },
        [this](const Outcome &outcome) {
            if (!outcome.ok) {
                setStatus(explain(outcome.code), true);
                return;
            }
            setStatus(QStringLiteral("This computer's keys are ready. Create a drive, or join one with an invitation code."));
            showPanel(MainPanel);
            touch();
            m_pollTimer->start();
            renderDrives();
            selectDrive(QString());
        });
}

void SecureDriveFeature::unlock()
{
    const QString passphrase = m_unlockPassphrase->text();
    m_unlockPassphrase->clear();
    if (passphrase.isEmpty())
        return;
    LocalState *state = m_state.get();
    run(QStringLiteral("Unlocking…"),
        [state, passphrase] {
            Failure failure;
            Outcome outcome;
            outcome.ok = state->unlock(passphrase, &failure);
            outcome.code = failure.code;
            return outcome;
        },
        [this](const Outcome &outcome) {
            if (!outcome.ok) {
                setStatus(explain(outcome.code), true);
                return;
            }
            setStatus(QString());
            showPanel(MainPanel);
            touch();
            m_pollTimer->start();
            renderDrives();
            selectDrive(QSettings().value(QLatin1String(kLastSlotKey)).toString());
        });
}

void SecureDriveFeature::lock(const QString &note)
{
    if (m_busy) {
        m_lockPending = true;
        m_lockNote = note;
        return;
    }
    bool dirty = false;
    for (const QPointer<EditorWindow> &editor : std::as_const(m_editors))
        dirty = dirty || (editor && editor->isDirty());
    bool changedCopies = false;
    for (const ExternalCopy &copy : std::as_const(m_external))
        changedCopies = changedCopies || fileDigest(copy.path) != copy.digest;
    if (note.isEmpty() && (dirty || changedCopies)
        && QMessageBox::question(this, QStringLiteral("Lock and discard changes?"),
                                 QStringLiteral("Some open files have changes that are not on the drive yet. Lock anyway and discard them?"),
                                 QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel)
               != QMessageBox::Yes)
        return;
    closeEditors(true);
    closeExternalCopies();
    m_drive.reset();
    m_state->lock();
    m_idleTimer->stop();
    m_pollTimer->stop();
    m_files->clear();
    m_activity->clear();
    m_devices->clear();
    m_requests->clear();
    m_drivePicker->clear();
    showPanel(m_state->exists() ? UnlockPanel : SetupPanel);
    setStatus(note.isEmpty() ? QStringLiteral("Locked. The keys were cleared from memory.") : note);
    if (m_searchIndexChanged)
        m_searchIndexChanged();
}

void SecureDriveFeature::renderDrives()
{
    m_drivePicker->clear();
    for (const Membership &m : std::as_const(m_state->drives)) {
        const QString label = m.isPending() ? QStringLiteral("%1 (waiting for approval)").arg(m.label) : m.label;
        m_drivePicker->addItem(label, m.id);
        m_drivePicker->setItemData(m_drivePicker->count() - 1, m.container, Qt::ToolTipRole);
    }
    if (m_drive)
        m_drivePicker->setCurrentIndex(m_drivePicker->findData(m_drive->id()));
    m_drivePicker->setVisible(m_drivePicker->count() > 0);
}

void SecureDriveFeature::selectDrive(const QString &requested)
{
    if (m_busy)
        return;
    QString slot = requested;
    if (!m_state->membership(slot))
        slot = m_state->drives.isEmpty() ? QString() : m_state->drives.first().id;
    if (m_drive && m_drive->id() == slot && m_drive->scanned()) {
        renderDrives();
        return;
    }
    closeEditors(false);
    closeExternalCopies();
    m_drive.reset();
    if (!slot.isEmpty()) {
        m_drive = std::make_unique<Drive>(m_state.get(), slot);
        QSettings().setValue(QLatin1String(kLastSlotKey), slot);
    }
    renderDrives();
    renderAll();
    if (m_drive)
        scanCurrent(false);
}

void SecureDriveFeature::scanCurrent(bool quiet)
{
    if (!m_drive || m_busy)
        return;
    Drive *drive = m_drive.get();
    const bool wasPending = drive->membership() && drive->membership()->isPending();
    run(quiet ? QString() : QStringLiteral("Reading and verifying the drive…"),
        [drive] {
            Failure failure;
            Outcome outcome;
            outcome.ok = drive->scan(&failure);
            outcome.code = failure.code;
            return outcome;
        },
        [this, quiet, wasPending](const Outcome &outcome) {
            if (!outcome.ok) {
                if (!quiet)
                    setStatus(explain(outcome.code), true);
                renderBanner();
                return;
            }
            if (!quiet)
                setStatus(QString());
            const bool nowMember = m_drive && m_drive->membership() && m_drive->membership()->isMember();
            if (wasPending && nowMember)
                setStatus(QStringLiteral("This computer was approved. The drive's files are below."));
            renderDrives();
            renderAll();
            if (m_searchIndexChanged)
                m_searchIndexChanged();
        });
}

void SecureDriveFeature::poll()
{
    if (!isVisible() || m_busy || !m_drive || !m_state->isUnlocked())
        return;
    Drive *drive = m_drive.get();
    run(QString(),
        [drive] {
            Outcome outcome;
            if (drive->scanned() && !drive->changedSinceScan()) {
                outcome.ok = true;
                outcome.code = QStringLiteral("unchanged");
                return outcome;
            }
            Failure failure;
            outcome.ok = drive->scan(&failure);
            outcome.code = failure.code;
            return outcome;
        },
        [this](const Outcome &outcome) {
            if (outcome.code == QLatin1String("unchanged"))
                return;
            renderDrives();
            renderAll();
            if (m_searchIndexChanged)
                m_searchIndexChanged();
        });
}

void SecureDriveFeature::renderAll()
{
    if (m_busy)
        return;
    const Membership *m = m_drive ? m_drive->membership() : nullptr;
    if (!m) {
        m_driveStack->setCurrentIndex(0);
        m_banner->hide();
        return;
    }
    if (m->isPending()) {
        m_driveStack->setCurrentIndex(1);
        m_pendingText->setText(
            m_drive->joinRequestMissing()
                ? QStringLiteral("The request is no longer on the drive: it was turned down, or someone deleted it. Cancel it and ask for a new invitation code.")
                : QStringLiteral("This device asked to join the drive %1. On a computer that is already approved, open Secure Drive, then Computers, "
                                 "and approve this one only if it shows this safety number:")
                      .arg(QDir::toNativeSeparators(m->container)));
        m_pendingSafety->setText(m_drive->joinSafetyNumber());
        m_pendingSafety->setVisible(!m_drive->joinRequestMissing());
        m_banner->hide();
        return;
    }
    m_driveStack->setCurrentIndex(2);
    renderBanner();
    renderFiles();
    renderActivity();
    renderDevices();
}

void SecureDriveFeature::renderBanner()
{
    if (m_busy)
        return;
    if (!m_drive || !m_drive->scanned() || !m_drive->membership() || !m_drive->membership()->isMember()) {
        m_banner->hide();
        return;
    }
    const auto name = [this](const QString &id) { return m_drive->deviceName(id); };
    const QList<Issue> issues = m_drive->issues();
    QStringList lines;
    if (m_drive->removedFromDrive())
        lines << QStringLiteral("This computer was removed from the drive. It can still read what it opened before, but nothing new.");
    for (const Issue &issue : issues)
        lines << QStringLiteral("• ") + issueText(issue, name);
    if (!lines.isEmpty()) {
        restyle(m_banner, QStringLiteral("errorLabel"));
        m_banner->setText(QStringLiteral("Problems found on the drive:\n") + lines.join(QLatin1Char('\n')));
    } else {
        int members = 0;
        for (const DeviceInfo &device : m_drive->devices())
            members += device.removed ? 0 : 1;
        QString text = QStringLiteral("Verified: %1 signed activity records from %2 approved device%3, none missing or altered. The drive file is %4, %5 in use.")
                           .arg(m_drive->records().size())
                           .arg(members)
                           .arg(members == 1 ? QString() : QStringLiteral("s"))
                           .arg(QLocale().formattedDataSize(m_drive->containerBytes()),
                                QLocale().formattedDataSize(m_drive->usedBytes()));
        restyle(m_banner, QStringLiteral("mutedLabel"));
        m_banner->setText(text);
    }
    m_banner->show();
}

void SecureDriveFeature::renderFiles()
{
    if (m_busy)
        return;
    const QString selected = selectedFileId();
    m_files->setSortingEnabled(false);
    m_files->clear();
    if (!m_drive || !m_drive->scanned())
        return;
    const QString filter = (m_fileFilter->text().isEmpty() ? m_hostQuery : m_fileFilter->text()).trimmed();
    const bool canWrite = !m_drive->removedFromDrive();
    m_newButton->setEnabled(canWrite);
    m_addButton->setEnabled(canWrite);
    const QList<FileEntry> files = m_drive->files();
    for (const FileEntry &entry : files) {
        if (!filter.isEmpty() && !entry.name.contains(filter, Qt::CaseInsensitive))
            continue;
        auto *item = new QTreeWidgetItem(m_files);
        const FileVersion *version = entry.current();
        item->setText(0, entry.name);
        item->setText(1, kindLabel(entry));
        item->setText(2, QLocale().formattedDataSize(version ? version->size : 0));
        item->setData(2, Qt::UserRole, double(version ? version->size : 0));
        item->setText(3, when(entry.modified));
        item->setText(4, m_drive->deviceName(entry.modifiedBy));
        item->setText(5, entry.lastOpened ? QStringLiteral("%1 · %2").arg(when(entry.lastOpened), m_drive->deviceName(entry.lastOpenedBy))
                                          : QStringLiteral("—"));
        item->setData(0, Qt::UserRole, entry.id);
        QString tip = QStringLiteral("%1 version%2, opened %3 time%4")
                          .arg(entry.versions.size())
                          .arg(entry.versions.size() == 1 ? QString() : QStringLiteral("s"))
                          .arg(entry.opens)
                          .arg(entry.opens == 1 ? QString() : QStringLiteral("s"));
        if (entry.conflicts > 0)
            tip += QStringLiteral(". Saved on two computers at once %1 time%2; every version is kept under Versions.")
                       .arg(entry.conflicts)
                       .arg(entry.conflicts == 1 ? QString() : QStringLiteral("s"));
        item->setToolTip(0, tip);
        if (entry.id == selected)
            m_files->setCurrentItem(item);
    }
    m_files->setSortingEnabled(true);
    const bool any = !selectedFileId().isEmpty();
    for (QPushButton *b : std::as_const(m_fileButtons))
        b->setEnabled(any);
    m_externalBar->setVisible(!m_external.isEmpty());
    QStringList names;
    for (const ExternalCopy &copy : std::as_const(m_external))
        names << copy.name;
    m_externalText->setText(QStringLiteral("Decrypted copies open in other apps: %1. They are deleted when you lock.").arg(names.join(QStringLiteral(", "))));
}

void SecureDriveFeature::renderActivity()
{
    if (m_busy)
        return;
    m_activity->clear();
    if (!m_drive || !m_drive->scanned())
        return;
    const auto name = [this](const QString &id) { return m_drive->deviceName(id); };
    QList<Record> records = m_drive->records();
    std::sort(records.begin(), records.end(), [](const Record &a, const Record &b) {
        return a.time != b.time ? a.time > b.time : a.lamport > b.lamport;
    });
    const QString filter = m_activityFilter->text().trimmed();
    int shown = 0;
    for (const Record &record : std::as_const(records)) {
        const QString device = m_drive->deviceName(record.device);
        const QString text = describeRecord(record, name);
        if (!filter.isEmpty() && !text.contains(filter, Qt::CaseInsensitive)
            && !device.contains(filter, Qt::CaseInsensitive))
            continue;
        auto *item = new QTreeWidgetItem(m_activity, {when(record.time), device, text});
        item->setToolTip(2, QStringLiteral("Record %1 from %2, hash %3").arg(record.seq + 1).arg(device, record.hash));
        if (++shown >= 5000)
            break;
    }
}

void SecureDriveFeature::renderDevices()
{
    if (m_busy)
        return;
    m_requests->clear();
    m_devices->clear();
    if (!m_drive || !m_drive->scanned())
        return;
    for (const JoinRequest &request : m_drive->requests()) {
        auto *item = new QTreeWidgetItem(m_requests, {request.name, request.safety, when(request.time)});
        item->setData(0, Qt::UserRole, request.mailbox);
        item->setFont(1, QFontDatabase::systemFont(QFontDatabase::FixedFont));
    }
    m_requestsBox->setVisible(m_requests->topLevelItemCount() > 0);
    if (m_requests->topLevelItemCount() > 0)
        m_requests->setCurrentItem(m_requests->topLevelItem(0));
    const QString self = m_state->deviceId;
    for (const DeviceInfo &device : m_drive->devices()) {
        const QString status = device.id == self ? QStringLiteral("This computer")
                               : device.removed ? QStringLiteral("Removed %1").arg(when(device.removedAt))
                                                : QStringLiteral("Approved");
        auto *item = new QTreeWidgetItem(
            m_devices, {m_drive->deviceName(device.id), m_drive->deviceSafetyNumber(device.id),
                        device.root ? QStringLiteral("%1, created the drive").arg(when(device.addedAt))
                                    : QStringLiteral("%1 by %2").arg(when(device.addedAt), m_drive->deviceName(device.addedBy)),
                        status});
        item->setData(0, Qt::UserRole, device.id);
        item->setData(0, Qt::UserRole + 1, device.id != self && !device.removed);
        item->setFont(1, QFontDatabase::systemFont(QFontDatabase::FixedFont));
    }
    m_removeDeviceButton->setEnabled(false);
    const bool canWrite = !m_drive->removedFromDrive();
    m_inviteButton->setEnabled(canWrite);
    const int open = int(m_drive->openInvites().size());
    m_inviteNote->setText(open == 0 ? QString()
                                    : QStringLiteral("%1 invitation code%2 still open. Codes work once and expire 24 hours after they are made.")
                                          .arg(open)
                                          .arg(open == 1 ? QStringLiteral(" is") : QStringLiteral("s are")));
    m_cancelInvitesButton->setVisible(open > 0 && canWrite);
    m_ownSafety->setText(QStringLiteral("This computer's safety number: %1").arg(m_drive->deviceSafetyNumber(self)));
}

QString SecureDriveFeature::selectedFileId() const
{
    QTreeWidgetItem *item = m_files ? m_files->currentItem() : nullptr;
    return item && item->isSelected() ? item->data(0, Qt::UserRole).toString() : QString();
}

void SecureDriveFeature::newFile(const QString &kind)
{
    if (!m_drive || m_busy)
        return;
    const QString suffix = kind == QLatin1String("document") ? QStringLiteral("html")
                           : kind == QLatin1String("spreadsheet") ? QStringLiteral("csv")
                                                                   : QStringLiteral("svg");
    const QString initial = kind == QLatin1String("document") ? QStringLiteral("Untitled document")
                            : kind == QLatin1String("spreadsheet") ? QStringLiteral("Untitled spreadsheet")
                                                                    : QStringLiteral("Untitled drawing");
    bool ok = false;
    const QString typed = QInputDialog::getText(this, QStringLiteral("New %1").arg(kind),
                                                QStringLiteral("Name"), QLineEdit::Normal, initial, &ok);
    if (!ok || typed.trimmed().isEmpty())
        return;
    const QString name = withSuffix(typed, suffix);
    const QByteArray content = kind == QLatin1String("document") ? EditorWindow::newDocument()
                               : kind == QLatin1String("spreadsheet") ? EditorWindow::newSpreadsheet()
                                                                       : EditorWindow::newDrawing();
    Drive *drive = m_drive.get();
    run(QStringLiteral("Encrypting %1…").arg(name),
        [drive, name, content, kind] {
            QByteArray copy = content;
            QBuffer buffer(&copy);
            buffer.open(QIODevice::ReadOnly);
            Failure failure;
            Outcome outcome;
            outcome.ok = drive->addFile(name, &buffer, &outcome.text, &failure, kind);
            outcome.code = failure.code;
            return outcome;
        },
        [this](const Outcome &outcome) {
            if (!outcome.ok) {
                setStatus(explain(outcome.code), true);
                return;
            }
            setStatus(QString());
            renderAll();
            openFile(outcome.text);
        });
}

void SecureDriveFeature::addFiles(const QStringList &paths)
{
    if (!m_drive || m_busy || !m_drive->membership() || !m_drive->membership()->isMember())
        return;
    Drive *drive = m_drive.get();
    run(QStringLiteral("Encrypting %1 file%2 onto the drive…").arg(paths.size()).arg(paths.size() == 1 ? QString() : QStringLiteral("s")),
        [drive, paths] {
            Outcome outcome;
            QStringList failed;
            for (const QString &path : paths) {
                QFile file(path);
                Failure failure;
                if (!file.open(QIODevice::ReadOnly)
                    || !drive->addFile(QFileInfo(path).fileName(), &file, nullptr, &failure))
                    failed << QFileInfo(path).fileName() + QStringLiteral(" (") + (failure.code.isEmpty() ? QStringLiteral("unreadable") : failure.code) + QLatin1Char(')');
            }
            outcome.ok = failed.isEmpty();
            outcome.text = failed.join(QStringLiteral(", "));
            return outcome;
        },
        [this, count = int(paths.size())](const Outcome &outcome) {
            if (!outcome.ok)
                setStatus(QStringLiteral("Some files were not added: %1").arg(outcome.text), true);
            else
                setStatus(QStringLiteral("Encrypted %1 file%2 onto the drive. The originals on this computer were not changed.")
                              .arg(count)
                              .arg(count == 1 ? QString() : QStringLiteral("s")));
            renderAll();
        });
}

void SecureDriveFeature::openFile(const QString &fileId, int version)
{
    if (!m_drive || m_busy)
        return;
    const FileEntry *entry = m_drive->file(fileId);
    if (!entry)
        return;
    for (const QPointer<EditorWindow> &editor : std::as_const(m_editors))
        if (editor && editor->fileId == fileId && version < 0 && !editor->isReadOnly()) {
            editor->raise();
            editor->activateWindow();
            return;
        }
    const QString kind = entry->kind;
    const QString name = version >= 0 ? QStringLiteral("%1 (version %2)").arg(entry->name).arg(version + 1) : entry->name;
    if (!editableKind(kind) && kind != QLatin1String("image")) {
        const auto answer = QMessageBox::question(
            this, QStringLiteral("Open %1").arg(entry->name),
            QStringLiteral("There is no built-in viewer for this kind of file. Open it in another app? Secure Drive writes a decrypted copy to this computer and deletes it when you lock."),
            QMessageBox::Open | QMessageBox::Cancel, QMessageBox::Open);
        if (answer == QMessageBox::Open)
            openExternally(fileId);
        return;
    }
    const FileVersion *chosen = version >= 0 && version < entry->versions.size() ? &entry->versions.at(version) : entry->current();
    if (chosen && chosen->size > kViewerLimit) {
        if (QMessageBox::question(this, QStringLiteral("Open %1").arg(entry->name),
                                  QStringLiteral("This file is too large to open in memory here. Open it in another app instead?"),
                                  QMessageBox::Open | QMessageBox::Cancel, QMessageBox::Open)
            == QMessageBox::Open)
            openExternally(fileId);
        return;
    }
    const bool readOnly = version >= 0 || m_drive->removedFromDrive();
    const QString purpose = editableKind(kind) && !readOnly ? QStringLiteral("edit") : QStringLiteral("view");
    Drive *drive = m_drive.get();
    const QString slot = drive->id();
    const auto startedFrom = std::make_shared<QString>(
        version < 0 && entry->current() ? entry->current()->blob : QString());
    run(QStringLiteral("Decrypting %1…").arg(entry->name),
        [drive, fileId, version, purpose] {
            Outcome outcome;
            QBuffer buffer(&outcome.bytes);
            buffer.open(QIODevice::WriteOnly);
            Failure failure;
            outcome.ok = drive->readFile(fileId, version, &buffer, purpose, &failure);
            outcome.code = failure.code;
            if (!outcome.ok)
                outcome.bytes.fill('\0');
            return outcome;
        },
        [this, fileId, kind, name, readOnly, slot, startedFrom](const Outcome &outcome) {
            if (!outcome.ok) {
                setStatus(explain(outcome.code), true);
                renderAll();
                return;
            }
            setStatus(QString());
            const QPointer<SecureDriveFeature> guard(this);
            SaveHandler save = [guard, slot, fileId, startedFrom](const QByteArray &bytes, QString *error) {
                if (!guard)
                    return false;
                SecureDriveFeature *self = guard.data();
                if (self->m_busy) {
                    *error = QStringLiteral("The drive is busy with another task. Try again in a moment.");
                    return false;
                }
                if (!self->m_drive || self->m_drive->id() != slot) {
                    *error = QStringLiteral("Switch back to the drive this file came from to save it.");
                    return false;
                }
                QByteArray copy = bytes;
                QBuffer buffer(&copy);
                buffer.open(QIODevice::ReadOnly);
                Failure failure;
                const bool ok = self->m_drive->saveFile(fileId, &buffer, &failure, *startedFrom);
                copy.fill('\0');
                if (!ok) {
                    *error = self->explain(failure.code);
                    return false;
                }
                if (const FileEntry *saved = self->m_drive->file(fileId); saved && saved->current())
                    *startedFrom = saved->current()->blob;
                self->renderAll();
                return true;
            };
            QString error;
            EditorWindow *editor = openEditor(kind, name, outcome.bytes, readOnly, save, this, &error);
            if (!editor) {
                setStatus(error, true);
                renderAll();
                return;
            }
            editor->fileId = fileId;
            editor->installEventFilter(this);
            m_editors.removeAll(QPointer<EditorWindow>());
            m_editors.append(editor);
            editor->show();
            renderAll();
        });
}

QString SecureDriveFeature::externalFolder() const
{
    return QDir(m_root).filePath(QStringLiteral("open"));
}

void SecureDriveFeature::openExternally(const QString &fileId)
{
    if (!m_drive || m_busy)
        return;
    const FileEntry *entry = m_drive->file(fileId);
    if (!entry)
        return;
    const QString name = entry->name;
    const QString folder = QDir(externalFolder()).filePath(randomId());
    Drive *drive = m_drive.get();
    const QString slot = drive->id();
    run(QStringLiteral("Decrypting %1 for another app…").arg(name),
        [drive, fileId, name, folder] {
            Outcome outcome;
            QDir().mkpath(folder);
            QFile::setPermissions(QFileInfo(folder).absolutePath(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
            QFile::setPermissions(folder, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
            const QString path = QDir(folder).filePath(safeFileName(name));
            QSaveFile out(path);
            Failure failure;
            if (!out.open(QIODevice::WriteOnly)) {
                outcome.code = QStringLiteral("write_failed");
                return outcome;
            }
            out.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
            if (!drive->readFile(fileId, -1, &out, QStringLiteral("external"), &failure)) {
                out.cancelWriting();
                QDir().rmdir(folder);
                outcome.code = failure.code;
                return outcome;
            }
            outcome.ok = out.commit();
            outcome.code = outcome.ok ? QString() : QStringLiteral("write_failed");
            outcome.text = path;
            outcome.bytes = fileDigest(path);
            return outcome;
        },
        [this, fileId, name, slot](const Outcome &outcome) {
            if (!outcome.ok) {
                setStatus(explain(outcome.code), true);
                renderAll();
                return;
            }
            m_external.append({slot, fileId, name, outcome.text, outcome.bytes});
            QDesktopServices::openUrl(QUrl::fromLocalFile(outcome.text));
            setStatus(QStringLiteral("Opened a decrypted copy of %1. When you are done, save changes back to the drive; the copy is deleted when you lock.").arg(name));
            renderAll();
        });
}

void SecureDriveFeature::saveExternalCopies()
{
    if (!m_drive || m_busy || m_external.isEmpty())
        return;
    Drive *drive = m_drive.get();
    auto copies = std::make_shared<QList<ExternalCopy>>(m_external);
    const QString slot = drive->id();
    run(QStringLiteral("Encrypting changed copies back onto the drive…"),
        [drive, copies, slot] {
            Outcome outcome;
            int saved = 0;
            QStringList failed;
            for (ExternalCopy &copy : *copies) {
                if (copy.slot != slot)
                    continue;
                const QByteArray digest = fileDigest(copy.path);
                if (digest.isEmpty() || digest == copy.digest)
                    continue;
                QFile file(copy.path);
                Failure failure;
                if (file.open(QIODevice::ReadOnly) && drive->saveFile(copy.fileId, &file, &failure)) {
                    copy.digest = digest;
                    ++saved;
                } else {
                    failed << copy.name;
                }
            }
            outcome.ok = failed.isEmpty();
            outcome.text = outcome.ok ? QString::number(saved) : failed.join(QStringLiteral(", "));
            return outcome;
        },
        [this, copies](const Outcome &outcome) {
            m_external = *copies;
            if (!outcome.ok)
                setStatus(QStringLiteral("Could not save: %1").arg(outcome.text), true);
            else if (outcome.text == QLatin1String("0"))
                setStatus(QStringLiteral("No copy has changed since it was opened."));
            else
                setStatus(QStringLiteral("Saved %1 changed cop%2 as new encrypted versions.")
                              .arg(outcome.text, outcome.text == QLatin1String("1") ? QStringLiteral("y") : QStringLiteral("ies")));
            renderAll();
        });
}

void SecureDriveFeature::closeExternalCopies()
{
    for (const ExternalCopy &copy : std::as_const(m_external)) {
        overwriteAndRemove(copy.path);
        QDir().rmdir(QFileInfo(copy.path).absolutePath());
    }
    m_external.clear();
    if (m_externalBar)
        m_externalBar->hide();
}

void SecureDriveFeature::wipeExternalFolder()
{
    const QString folder = externalFolder();
    if (!QFileInfo(folder).isDir())
        return;
    QDirIterator files(folder, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (files.hasNext())
        overwriteAndRemove(files.next());
    const QStringList dirs = QDir(folder).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
    for (const QString &dir : dirs)
        QDir(folder).rmdir(dir);
}

void SecureDriveFeature::saveCopy(const QString &fileId)
{
    if (!m_drive || m_busy)
        return;
    const FileEntry *entry = m_drive->file(fileId);
    if (!entry)
        return;
    const QString name = entry->name;
    if (QMessageBox::question(this, QStringLiteral("Save a decrypted copy?"),
                              QStringLiteral("The copy is not encrypted and is not part of the drive: anyone who can read where you save it can read it. "
                                             "The drive's activity log records that this computer saved a copy."),
                              QMessageBox::Save | QMessageBox::Cancel, QMessageBox::Cancel)
        != QMessageBox::Save)
        return;
    const QString start = QDir(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)).filePath(safeFileName(name));
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save a decrypted copy"), start);
    if (path.isEmpty())
        return;
    Drive *drive = m_drive.get();
    run(QStringLiteral("Decrypting %1…").arg(name),
        [drive, fileId, path] {
            Outcome outcome;
            QSaveFile out(path);
            Failure failure;
            if (!out.open(QIODevice::WriteOnly)) {
                outcome.code = QStringLiteral("write_failed");
                return outcome;
            }
            if (!drive->readFile(fileId, -1, &out, QStringLiteral("export"), &failure)) {
                out.cancelWriting();
                outcome.code = failure.code;
                return outcome;
            }
            outcome.ok = out.commit();
            outcome.code = outcome.ok ? QString() : QStringLiteral("write_failed");
            return outcome;
        },
        [this, path](const Outcome &outcome) {
            setStatus(outcome.ok ? QStringLiteral("Saved a decrypted copy to %1.").arg(QDir::toNativeSeparators(path))
                                 : explain(outcome.code),
                      !outcome.ok);
            renderAll();
        });
}

void SecureDriveFeature::showVersions(const QString &fileId)
{
    if (!m_drive || m_busy)
        return;
    const FileEntry *entry = m_drive->file(fileId);
    if (!entry)
        return;
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Versions of %1").arg(entry->name));
    dialog.resize(640, 380);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(mutedLabel(QStringLiteral("Every save is kept as its own encrypted version until the file is deleted.")));
    auto *list = new QTreeWidget;
    list->setRootIsDecorated(false);
    list->setHeaderLabels({QStringLiteral("Version"), QStringLiteral("Saved"), QStringLiteral("By"), QStringLiteral("Size")});
    for (int i = int(entry->versions.size()) - 1; i >= 0; --i) {
        const FileVersion &version = entry->versions.at(i);
        auto *item = new QTreeWidgetItem(list, {QString::number(i + 1) + (i == entry->versions.size() - 1 ? QStringLiteral(" (current)") : QString()),
                                                when(version.time), m_drive->deviceName(version.device),
                                                QLocale().formattedDataSize(version.size)});
        item->setData(0, Qt::UserRole, i);
    }
    list->setCurrentItem(list->topLevelItem(0));
    layout->addWidget(list, 1);
    auto *buttons = new QDialogButtonBox;
    auto *open = buttons->addButton(QStringLiteral("Open read-only"), QDialogButtonBox::ActionRole);
    auto *restore = buttons->addButton(QStringLiteral("Make this the current version"), QDialogButtonBox::ActionRole);
    buttons->addButton(QDialogButtonBox::Close);
    layout->addWidget(buttons);
    const bool canWrite = !m_drive->removedFromDrive();
    restore->setEnabled(canWrite);
    int chosen = -1;
    bool restoreChosen = false;
    connect(open, &QPushButton::clicked, &dialog, [&] {
        if (list->currentItem()) {
            chosen = list->currentItem()->data(0, Qt::UserRole).toInt();
            dialog.accept();
        }
    });
    connect(restore, &QPushButton::clicked, &dialog, [&] {
        if (list->currentItem()) {
            chosen = list->currentItem()->data(0, Qt::UserRole).toInt();
            restoreChosen = true;
            dialog.accept();
        }
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted || chosen < 0)
        return;
    if (!restoreChosen) {
        openFile(fileId, chosen);
        return;
    }
    Drive *drive = m_drive.get();
    run(QStringLiteral("Restoring version %1…").arg(chosen + 1),
        [drive, fileId, chosen] {
            Failure failure;
            Outcome outcome;
            outcome.ok = drive->restoreVersion(fileId, chosen, &failure);
            outcome.code = failure.code;
            return outcome;
        },
        [this](const Outcome &outcome) {
            setStatus(outcome.ok ? QStringLiteral("Restored. The newer versions are still kept.") : explain(outcome.code), !outcome.ok);
            renderAll();
        });
}

void SecureDriveFeature::renameFile(const QString &fileId)
{
    if (!m_drive || m_busy)
        return;
    const FileEntry *entry = m_drive->file(fileId);
    if (!entry)
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("Rename"), QStringLiteral("New name"),
                                               QLineEdit::Normal, entry->name, &ok);
    if (!ok || name.trimmed().isEmpty() || name.trimmed() == entry->name)
        return;
    Drive *drive = m_drive.get();
    run(QStringLiteral("Renaming…"),
        [drive, fileId, name] {
            Failure failure;
            Outcome outcome;
            outcome.ok = drive->renameFile(fileId, name, &failure);
            outcome.code = failure.code;
            return outcome;
        },
        [this](const Outcome &outcome) {
            setStatus(outcome.ok ? QString() : explain(outcome.code), !outcome.ok);
            renderAll();
        });
}

void SecureDriveFeature::deleteFile(const QString &fileId)
{
    if (!m_drive || m_busy)
        return;
    const FileEntry *entry = m_drive->file(fileId);
    if (!entry)
        return;
    if (QMessageBox::warning(this, QStringLiteral("Delete %1?").arg(entry->name),
                             QStringLiteral("Every version of %1 is removed from the drive for every computer. The activity log keeps a record that it was deleted, and by whom.")
                                 .arg(entry->name),
                             QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel)
        != QMessageBox::Yes)
        return;
    for (const QPointer<EditorWindow> &editor : std::as_const(m_editors))
        if (editor && editor->fileId == fileId)
            editor->discardAndClose();
    Drive *drive = m_drive.get();
    run(QStringLiteral("Deleting…"),
        [drive, fileId] {
            Failure failure;
            Outcome outcome;
            outcome.ok = drive->deleteFile(fileId, &failure);
            outcome.code = failure.code;
            return outcome;
        },
        [this](const Outcome &outcome) {
            setStatus(outcome.ok ? QString() : explain(outcome.code), !outcome.ok);
            renderAll();
        });
}

void SecureDriveFeature::exportActivity()
{
    if (!m_drive || m_busy)
        return;
    const QString start = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
                              .filePath(safeFileName(m_drive->membership()->label) + QStringLiteral(" activity.csv"));
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export activity"), start,
                                                      QStringLiteral("CSV files (*.csv)"));
    if (path.isEmpty())
        return;
    Drive *drive = m_drive.get();
    run(QStringLiteral("Exporting activity…"),
        [drive, path] {
            Outcome outcome;
            Failure failure;
            if (!drive->note(QStringLiteral("log_exported"), {}, &failure)) {
                outcome.code = failure.code;
                return outcome;
            }
            QSaveFile out(path);
            const QByteArray csv = drive->activityCsv().toUtf8();
            outcome.ok = out.open(QIODevice::WriteOnly) && out.write(csv) == csv.size() && out.commit();
            outcome.code = outcome.ok ? QString() : QStringLiteral("write_failed");
            return outcome;
        },
        [this, path](const Outcome &outcome) {
            setStatus(outcome.ok ? QStringLiteral("Exported the activity log to %1. It is not encrypted.").arg(QDir::toNativeSeparators(path))
                                 : explain(outcome.code),
                      !outcome.ok);
            renderAll();
        });
}

void SecureDriveFeature::invite()
{
    if (!m_drive || m_busy)
        return;
    Drive *drive = m_drive.get();
    run(QStringLiteral("Creating an invitation…"),
        [drive] {
            Failure failure;
            Outcome outcome;
            outcome.ok = drive->createInvite(&outcome.text, nullptr, &failure);
            outcome.code = failure.code;
            return outcome;
        },
        [this](const Outcome &outcome) {
            if (!outcome.ok) {
                setStatus(explain(outcome.code), true);
                return;
            }
            setStatus(QString());
            renderAll();
            QDialog dialog(this);
            dialog.setWindowTitle(QStringLiteral("Invite a computer"));
            auto *layout = new QVBoxLayout(&dialog);
            layout->addWidget(mutedLabel(QStringLiteral(
                "On the other computer or phone, open Secure Drive, choose Join a drive, pick the same drive file on the router and type this code. "
                "Then approve it here under Computers, after checking that both screens show the same safety number.\n\n"
                "Tell the code in person or over a call rather than by email. It works once and expires in 24 hours. "
                "On its own it opens nothing: the drive key is only handed over when you approve.")));
            auto *code = new QLabel(outcome.text);
            QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
            mono.setPointSizeF(mono.pointSizeF() * 1.6);
            code->setFont(mono);
            code->setTextInteractionFlags(Qt::TextSelectableByMouse);
            layout->addWidget(code);
            auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
            auto *copy = buttons->addButton(QStringLiteral("Copy code"), QDialogButtonBox::ActionRole);
            const QString text = outcome.text;
            connect(copy, &QPushButton::clicked, &dialog, [text] { QGuiApplication::clipboard()->setText(text); });
            connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
            layout->addWidget(buttons);
            dialog.exec();
            QClipboard *clipboard = QGuiApplication::clipboard();
            if (clipboard && clipboard->text() == text)
                clipboard->clear();
            m_tabs->setCurrentIndex(2);
        });
}

void SecureDriveFeature::approveRequest(int mailbox)
{
    if (!m_drive || m_busy)
        return;
    JoinRequest request;
    for (const JoinRequest &candidate : m_drive->requests())
        if (candidate.mailbox == mailbox)
            request = candidate;
    if (request.mailbox < 0)
        return;
    if (QMessageBox::question(this, QStringLiteral("Approve %1?").arg(request.name),
                              QStringLiteral("Approve only if %1 shows this safety number:\n\n%2\n\n"
                                             "Once approved it can open, change and delete every file on the drive.")
                                  .arg(request.name, request.safety),
                              QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel)
        != QMessageBox::Yes)
        return;
    Drive *drive = m_drive.get();
    run(QStringLiteral("Sealing the drive key to %1…").arg(request.name),
        [drive, mailbox] {
            Failure failure;
            Outcome outcome;
            outcome.ok = drive->approve(mailbox, &failure);
            outcome.code = failure.code;
            return outcome;
        },
        [this, name = request.name](const Outcome &outcome) {
            setStatus(outcome.ok ? QStringLiteral("Approved %1. It picks up the drive key the next time it checks the drive.").arg(name)
                                 : explain(outcome.code),
                      !outcome.ok);
            renderAll();
        });
}

void SecureDriveFeature::denyRequest(int mailbox)
{
    if (!m_drive || m_busy)
        return;
    Drive *drive = m_drive.get();
    run(QStringLiteral("Turning the request down…"),
        [drive, mailbox] {
            Failure failure;
            Outcome outcome;
            outcome.ok = drive->deny(mailbox, &failure);
            outcome.code = failure.code;
            return outcome;
        },
        [this](const Outcome &outcome) {
            setStatus(outcome.ok ? QStringLiteral("Turned down. Its invitation code no longer works.") : explain(outcome.code),
                      !outcome.ok);
            renderAll();
        });
}

void SecureDriveFeature::removeDevice(const QString &deviceId)
{
    if (!m_drive || m_busy)
        return;
    const QString name = m_drive->deviceName(deviceId);
    if (QMessageBox::warning(this, QStringLiteral("Remove %1?").arg(name),
                             QStringLiteral("%1 loses access to everything on the drive from now on. Secure Drive makes a new drive key, seals it to the other approved computers, "
                                            "and re-encrypts every file under it, which can take a while for large drives.\n\n"
                                            "Copies %1 already opened or saved stay with it. Remove it before handing the computer on or when it is lost.")
                                 .arg(name),
                             QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel)
        != QMessageBox::Yes)
        return;
    Drive *drive = m_drive.get();
    const QPointer<SecureDriveFeature> guard(this);
    run(QStringLiteral("Removing %1 and replacing the drive key…").arg(name),
        [drive, deviceId, guard] {
            Failure failure;
            Outcome outcome;
            outcome.ok = drive->removeDevice(deviceId, [guard](int done, int total) {
                QMetaObject::invokeMethod(guard.data(), [guard, done, total] {
                    if (guard)
                        guard->setStatus(QStringLiteral("Re-encrypting under the new key: %1 of %2…").arg(done).arg(total));
                }, Qt::QueuedConnection);
            }, &failure);
            outcome.code = failure.code;
            return outcome;
        },
        [this, name](const Outcome &outcome) {
            setStatus(outcome.ok ? QStringLiteral("Removed %1. Everything on the drive is now under a key it never had.").arg(name)
                                 : explain(outcome.code),
                      !outcome.ok);
            renderAll();
        });
}

void SecureDriveFeature::cancelInvites()
{
    if (!m_drive || m_busy)
        return;
    QStringList ids;
    for (const Invite &invite : m_drive->openInvites())
        ids << invite.id;
    Drive *drive = m_drive.get();
    run(QStringLiteral("Cancelling invitations…"),
        [drive, ids] {
            Outcome outcome;
            outcome.ok = true;
            for (const QString &id : ids) {
                Failure failure;
                if (!drive->revokeInvite(id, &failure)) {
                    outcome.ok = false;
                    outcome.code = failure.code;
                }
            }
            return outcome;
        },
        [this](const Outcome &outcome) {
            setStatus(outcome.ok ? QStringLiteral("Cancelled. Those codes no longer work.") : explain(outcome.code), !outcome.ok);
            renderAll();
        });
}

void SecureDriveFeature::createDrive()
{
    if (m_busy || !m_state->isUnlocked())
        return;
    const QString folder = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save the drive file on the router's USB drive"),
        QStringLiteral("SecureDrive.fmsd"), QStringLiteral("Secure Drive (*.fmsd);;All files (*)"));
    if (folder.isEmpty())
        return;
    bool ok = false;
    const QString label = QInputDialog::getText(this, QStringLiteral("Name the drive"),
                                                QStringLiteral("Shown only on approved devices; the drive file itself never holds it in the clear."),
                                                QLineEdit::Normal, QFileInfo(folder).completeBaseName(), &ok);
    if (!ok)
        return;
    LocalState *state = m_state.get();
    run(QStringLiteral("Creating the drive…"),
        [state, folder, label] {
            Failure failure;
            Outcome outcome;
            outcome.ok = Drive::create(state, folder, label, &outcome.text, &failure);
            outcome.code = failure.code;
            return outcome;
        },
        [this](const Outcome &outcome) {
            if (!outcome.ok) {
                setStatus(explain(outcome.code), true);
                return;
            }
            setStatus(QStringLiteral("The drive is ready. Add files, or invite another computer under Computers."));
            selectDrive(outcome.text);
        });
}

void SecureDriveFeature::joinDrive()
{
    if (m_busy || !m_state->isUnlocked())
        return;
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Join a drive"));
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(mutedLabel(QStringLiteral("Pick the drive file on the router and type the invitation code from a device that is already approved.")));
    auto *form = new QFormLayout;
    auto *folderRow = new QHBoxLayout;
    auto *folder = new QLineEdit;
    folder->setReadOnly(true);
    auto *choose = button(QStringLiteral("Choose…"));
    folderRow->addWidget(folder, 1);
    folderRow->addWidget(choose);
    auto *code = new QLineEdit;
    code->setPlaceholderText(QStringLiteral("ABC-DEF-…"));
    code->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    form->addRow(QStringLiteral("Drive file"), folderRow);
    form->addRow(QStringLiteral("Invitation code"), code);
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Ask to join"));
    layout->addWidget(buttons);
    connect(choose, &QPushButton::clicked, &dialog, [&dialog, folder] {
        const QString picked = QFileDialog::getOpenFileName(&dialog, QStringLiteral("Choose the drive file"), QString(),
                                                            QStringLiteral("Secure Drive (*.fmsd);;All files (*)"));
        if (!picked.isEmpty())
            folder->setText(picked);
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.resize(560, dialog.sizeHint().height());
    if (dialog.exec() != QDialog::Accepted || folder->text().isEmpty() || code->text().trimmed().isEmpty())
        return;
    LocalState *state = m_state.get();
    const QString path = folder->text();
    const QString typed = code->text();
    run(QStringLiteral("Asking to join…"),
        [state, path, typed] {
            Failure failure;
            Outcome outcome;
            outcome.ok = Drive::requestJoin(state, path, typed, &outcome.text, nullptr, &failure);
            outcome.code = failure.code;
            return outcome;
        },
        [this](const Outcome &outcome) {
            if (!outcome.ok) {
                setStatus(explain(outcome.code), true);
                return;
            }
            setStatus(QString());
            selectDrive(outcome.text);
        });
}

void SecureDriveFeature::forgetDrive()
{
    if (!m_drive || m_busy)
        return;
    const Membership *m = m_drive->membership();
    if (!m)
        return;
    const bool pending = m->isPending();
    const QString label = m->label;
    if (QMessageBox::warning(this, pending ? QStringLiteral("Cancel the request?") : QStringLiteral("Forget %1?").arg(label),
                             pending ? QStringLiteral("The request is removed from the drive. You can ask again with a new code.")
                                     : QStringLiteral("This computer deletes its keys to %1. The files on the drive stay where they are for the other computers. "
                                                      "To come back, this computer needs a new invitation. To also lock it out for good, remove it under Computers from another computer.")
                                           .arg(label),
                             QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel)
        != QMessageBox::Yes)
        return;
    closeEditors(false);
    closeExternalCopies();
    Failure failure;
    bool ok = false;
    if (pending) {
        ok = m_drive->cancelJoin(&failure);
    } else {
        m_state->forget(m_drive->id());
        ok = m_state->save(&failure);
    }
    m_drive.reset();
    setStatus(ok ? QString() : explain(failure.code), !ok);
    selectDrive(QString());
}

void SecureDriveFeature::changePassphrase()
{
    if (m_busy || !m_state->isUnlocked())
        return;
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Change passphrase"));
    auto *layout = new QFormLayout(&dialog);
    auto *current = passphraseEdit(QString());
    auto *next = passphraseEdit(QStringLiteral("At least 10 characters"));
    auto *confirm = passphraseEdit(QString());
    layout->addRow(QStringLiteral("Current"), current);
    layout->addRow(QStringLiteral("New"), next);
    layout->addRow(QStringLiteral("Confirm"), confirm);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    if (next->text() != confirm->text()) {
        setStatus(QStringLiteral("The two new passphrases do not match."), true);
        return;
    }
    LocalState *state = m_state.get();
    const QString was = current->text(), now = next->text();
    run(QStringLiteral("Changing the passphrase…"),
        [state, was, now] {
            Failure failure;
            Outcome outcome;
            outcome.ok = state->changePassphrase(was, now, &failure);
            outcome.code = failure.code;
            return outcome;
        },
        [this](const Outcome &outcome) {
            setStatus(outcome.ok ? QStringLiteral("Passphrase changed.") : explain(outcome.code), !outcome.ok);
        });
}

void SecureDriveFeature::closeEditors(bool discard)
{
    for (const QPointer<EditorWindow> &editor : std::as_const(m_editors))
        if (editor) {
            if (discard)
                editor->discardAndClose();
            else
                editor->close();
        }
    m_editors.removeAll(QPointer<EditorWindow>());
}

void SecureDriveFeature::setHostSearchQuery(const QString &query)
{
    m_hostQuery = query;
    if (m_fileFilter && m_fileFilter->text().isEmpty() && !m_busy)
        renderFiles();
}

QList<forkmesh::market::NativePackageSearchHit> SecureDriveFeature::searchHits(const QString &query, int limit) const
{
    QList<forkmesh::market::NativePackageSearchHit> hits;
    if (m_busy || !m_drive || !m_drive->scanned() || !m_state->isUnlocked() || query.trimmed().isEmpty())
        return hits;
    const QString label = m_drive->membership() ? m_drive->membership()->label : QString();
    for (const FileEntry &entry : m_drive->files()) {
        if (!entry.name.contains(query.trimmed(), Qt::CaseInsensitive))
            continue;
        hits.append({entry.id, entry.name, QStringLiteral("Secure Drive · %1").arg(label)});
        if (hits.size() >= limit)
            break;
    }
    return hits;
}

void SecureDriveFeature::activateSearchHit(const QString &id)
{
    m_tabs->setCurrentIndex(0);
    for (int i = 0; i < m_files->topLevelItemCount(); ++i)
        if (m_files->topLevelItem(i)->data(0, Qt::UserRole).toString() == id)
            m_files->setCurrentItem(m_files->topLevelItem(i));
    openFile(id);
}

}  // namespace forkmesh::securedrive

namespace {
const forkmesh::market::NativePackageRegistration secureDriveRegistration({
    QStringLiteral("builtin:secure-drive"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Secure Drive"),
    QStringLiteral("lock"),
    QString(),
    QString(),
    [](const forkmesh::market::NativePackageHost &host, QWidget *parent) {
        return new forkmesh::securedrive::SecureDriveFeature(host, parent);
    },
});
}  // namespace
