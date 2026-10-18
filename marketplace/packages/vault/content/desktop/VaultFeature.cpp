#include "VaultFeature.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <utility>

namespace forkmesh::vault {
namespace {

constexpr int kClipboardClearMs = 30000;
constexpr int kMinimumMasterPassword = 12;
constexpr char kItemIdProperty[] = "vaultItemId";

QLabel *mutedLabel(const QString &text)
{
    auto *label = new QLabel(text);
    label->setObjectName(QStringLiteral("mutedLabel"));
    label->setWordWrap(true);
    return label;
}

QLineEdit *passwordEdit(const QString &placeholder)
{
    auto *edit = new QLineEdit;
    edit->setEchoMode(QLineEdit::Password);
    edit->setPlaceholderText(placeholder);
    // A master password must not land in an input method's learned words.
    edit->setInputMethodHints(Qt::ImhHiddenText | Qt::ImhSensitiveData | Qt::ImhNoPredictiveText
                              | Qt::ImhNoAutoUppercase);
    return edit;
}

QFont monospace()
{
    return QFontDatabase::systemFont(QFontDatabase::FixedFont);
}

// Argon2id takes about a second natively: long enough to freeze a window, so
// it runs on a pool thread and answers on the GUI thread.
VaultClient::Stretcher threadedStretcher(QObject *owner)
{
    return [guard = QPointer<QObject>(owner)](const QString &password, const Kdf &kdf, VaultClient::Stretched done) {
        struct Result { bool ok = false; QByteArray key; QString code; };
        auto *watcher = new QFutureWatcher<Result>(guard);
        QObject::connect(watcher, &QFutureWatcherBase::finished, watcher, [watcher, guard, done] {
            Result result = watcher->result();
            watcher->deleteLater();
            if (guard)
                done(result.ok, result.key, result.code);
            wipe(result.key);
        });
        watcher->setFuture(QtConcurrent::run([password, kdf] {
            Result result;
            Failure failure;
            result.ok = stretch(password, kdf, &result.key, &failure);
            result.code = failure.code;
            return result;
        }));
    };
}

}  // namespace

VaultFeature::VaultFeature(const forkmesh::market::NativePackageHost &host, QWidget *parent)
    : forkmesh::market::NativePackageWidget(parent), m_searchIndexChanged(host.searchIndexChanged)
{
    setObjectName(QStringLiteral("vaultSection"));
    // Every request goes through the host, which signs it as this device's
    // account and sends it through the window's firewalled network manager.
    const QPointer<VaultFeature> guard(this);
    const forkmesh::market::NativePackageRequest hostRequest = host.request;
    m_client = std::make_unique<VaultClient>(
        [guard, hostRequest](const QByteArray &method, const QString &path, const QJsonObject &body, VaultClient::Reply reply) {
            if (!hostRequest) {
                reply(false, {}, QStringLiteral("Package requests are unavailable."));
                return;
            }
            hostRequest(method, path, body, [guard, reply = std::move(reply)](bool ok, const QJsonObject &payload, const QString &error) {
                if (guard)
                    reply(ok, payload, error);
            });
        },
        threadedStretcher(this));

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(20, 18, 20, 20);
    outer->setSpacing(10);

    auto *heading = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("Password Vault"));
    title->setObjectName(QStringLiteral("pageTitle"));
    heading->addWidget(title);
    heading->addWidget(mutedLabel(QStringLiteral("Encrypted on this device before it is uploaded. ForkMesh stores ciphertext only.")), 1);
    m_lockButton = new QPushButton(QStringLiteral("Lock"));
    m_settingsButton = new QPushButton(QStringLiteral("Vault settings…"));
    for (QPushButton *button : {m_lockButton, m_settingsButton}) {
        button->setObjectName(QStringLiteral("ghostButton"));
        heading->addWidget(button);
    }
    outer->addLayout(heading);

    m_status = mutedLabel(QStringLiteral("Loading your vault…"));
    outer->addWidget(m_status);

    m_stack = new QStackedWidget;
    auto *loading = new QWidget;
    auto *setup = new QWidget;
    auto *unlock = new QWidget;
    auto *app = new QWidget;
    buildSetupPanel(setup);
    buildUnlockPanel(unlock);
    buildAppPanel(app);
    for (QWidget *panel : {loading, setup, unlock, app})
        m_stack->addWidget(panel);
    outer->addWidget(m_stack, 1);

    m_idleTimer = new QTimer(this);
    m_idleTimer->setSingleShot(true);
    connect(m_idleTimer, &QTimer::timeout, this, [this] { lockVault(QStringLiteral("Locked after being idle.")); });
    m_clipboardTimer = new QTimer(this);
    m_clipboardTimer->setSingleShot(true);
    connect(m_clipboardTimer, &QTimer::timeout, this, [this] {
        // Only what this page put there is cleared: anything copied since is
        // somebody else's.
        QClipboard *clipboard = QGuiApplication::clipboard();
        if (clipboard && !m_clipboardValue.isEmpty() && clipboard->text() == m_clipboardValue)
            clipboard->clear();
        m_clipboardValue.clear();
    });
    connect(m_lockButton, &QPushButton::clicked, this, [this] { lockVault(); });
    connect(m_settingsButton, &QPushButton::clicked, this, [this] { openSettings(); });
    installEventFilter(this);
    showPanel(LoadingPanel);
}

VaultFeature::~VaultFeature()
{
    if (m_client)
        m_client->lock();
}

// ---- panels --------------------------------------------------------------------------

void VaultFeature::buildSetupPanel(QWidget *panel)
{
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(8, 24, 8, 8);
    auto *heading = new QLabel(QStringLiteral("Create your vault"));
    heading->setObjectName(QStringLiteral("sectionTitle"));
    layout->addWidget(heading);
    layout->addWidget(mutedLabel(QStringLiteral(
        "Choose a master password. It never leaves this device and ForkMesh cannot reset it, so it must be "
        "one you will not forget and do not use anywhere else. Four or more unrelated words work well. "
        "The same vault opens in the web dashboard, the browser extension, and the mobile app.")));
    auto *form = new QFormLayout;
    m_setupPassword = passwordEdit(QStringLiteral("At least 12 characters"));
    m_setupPassword->setObjectName(QStringLiteral("vaultSetupPassword"));
    m_setupConfirm = passwordEdit(QStringLiteral("Type it again"));
    m_setupConfirm->setObjectName(QStringLiteral("vaultSetupConfirm"));
    form->addRow(QStringLiteral("Master password"), m_setupPassword);
    form->addRow(QStringLiteral("Confirm"), m_setupConfirm);
    layout->addLayout(form);
    auto *row = new QHBoxLayout;
    m_setupButton = new QPushButton(QStringLiteral("Create vault"));
    m_setupButton->setObjectName(QStringLiteral("primaryButton"));
    row->addWidget(m_setupButton);
    row->addStretch();
    layout->addLayout(row);
    layout->addStretch();
    connect(m_setupButton, &QPushButton::clicked, this, [this] { createVault(); });
    connect(m_setupConfirm, &QLineEdit::returnPressed, this, [this] { createVault(); });
}

void VaultFeature::buildUnlockPanel(QWidget *panel)
{
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(8, 24, 8, 8);
    auto *heading = new QLabel(QStringLiteral("Unlock your vault"));
    heading->setObjectName(QStringLiteral("sectionTitle"));
    layout->addWidget(heading);
    layout->addWidget(mutedLabel(QStringLiteral(
        "Stretching the master password takes a moment on purpose: it is what makes guessing it expensive.")));
    auto *form = new QFormLayout;
    m_unlockPassword = passwordEdit(QStringLiteral("Master password"));
    m_unlockPassword->setObjectName(QStringLiteral("vaultUnlockPassword"));
    form->addRow(QStringLiteral("Master password"), m_unlockPassword);
    layout->addLayout(form);
    auto *row = new QHBoxLayout;
    m_unlockButton = new QPushButton(QStringLiteral("Unlock"));
    m_unlockButton->setObjectName(QStringLiteral("primaryButton"));
    auto *recoverButton = new QPushButton(QStringLiteral("Forgot it? Use your recovery code…"));
    recoverButton->setObjectName(QStringLiteral("ghostButton"));
    row->addWidget(m_unlockButton);
    row->addWidget(recoverButton);
    row->addStretch();
    layout->addLayout(row);
    layout->addStretch();
    connect(m_unlockButton, &QPushButton::clicked, this, [this] { unlockVault(); });
    connect(m_unlockPassword, &QLineEdit::returnPressed, this, [this] { unlockVault(); });
    connect(recoverButton, &QPushButton::clicked, this, [this] { recoverVault(); });
}

void VaultFeature::buildAppPanel(QWidget *panel)
{
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 4, 0, 0);
    auto *toolbar = new QHBoxLayout;
    m_search = new QLineEdit;
    m_search->setPlaceholderText(QStringLiteral("Search titles, usernames, and sites"));
    m_search->setClearButtonEnabled(true);
    m_search->setObjectName(QStringLiteral("vaultSearch"));
    m_filter = new QComboBox;
    m_filter->addItem(QStringLiteral("All items"), QStringLiteral("all"));
    m_filter->addItem(QStringLiteral("Mine"), QStringLiteral("mine"));
    m_filter->addItem(QStringLiteral("Shared with me"), QStringLiteral("shared"));
    auto *newButton = new QPushButton(QStringLiteral("New login"));
    newButton->setObjectName(QStringLiteral("primaryButton"));
    auto *refreshButton = new QPushButton(QStringLiteral("Refresh"));
    refreshButton->setObjectName(QStringLiteral("ghostButton"));
    m_importButton = new QPushButton(QStringLiteral("Import…"));
    m_importButton->setObjectName(QStringLiteral("ghostButton"));
    m_importButton->setToolTip(QStringLiteral("Import a CSV export from Apple Passwords or another password manager"));
    toolbar->addWidget(m_search, 1);
    toolbar->addWidget(m_filter);
    toolbar->addWidget(refreshButton);
    toolbar->addWidget(m_importButton);
    toolbar->addWidget(newButton);
    layout->addLayout(toolbar);

    m_table = new QTableWidget(0, 4);
    m_table->setObjectName(QStringLiteral("vaultItemsTable"));
    m_table->setHorizontalHeaderLabels({QStringLiteral("Title"), QStringLiteral("Username"),
                                        QStringLiteral("Site"), QStringLiteral("Sharing")});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    layout->addWidget(m_table, 1);
    m_empty = mutedLabel(QStringLiteral("Nothing here yet. Add a login, import the logins saved in Apple Passwords or another password manager, or install the ForkMesh browser extension to fill and save them as you browse."));
    m_empty->setAlignment(Qt::AlignHCenter);
    layout->addWidget(m_empty);

    auto *actions = new QHBoxLayout;
    const auto rowButton = [this, actions](const QString &text, const QString &tip, std::function<void(const QString &)> run) {
        auto *button = new QPushButton(text);
        button->setObjectName(QStringLiteral("ghostButton"));
        button->setToolTip(tip);
        actions->addWidget(button);
        m_rowButtons.append(button);
        connect(button, &QPushButton::clicked, this, [this, run = std::move(run)] {
            const QString id = selectedId();
            if (!id.isEmpty())
                run(id);
        });
        return button;
    };
    rowButton(QStringLiteral("Copy username"), QStringLiteral("Copy the selected login's username"),
              [this](const QString &id) { copyField(id, "username", QStringLiteral("Username")); });
    rowButton(QStringLiteral("Copy password"), QStringLiteral("Copy the password; the clipboard is cleared 30 seconds later"),
              [this](const QString &id) { copyField(id, "password", QStringLiteral("Password")); });
    rowButton(QStringLiteral("Open site"), QStringLiteral("Open the login's https address in your browser"),
              [this](const QString &id) { openSite(id); });
    rowButton(QStringLiteral("Open…"), QStringLiteral("View or edit the selected login"),
              [this](const QString &id) { editItem(id); });
    rowButton(QStringLiteral("Share…"), QStringLiteral("Share with a ForkMesh user or an organization"),
              [this](const QString &id) { shareItem(id); });
    actions->addStretch();
    auto *deleteButton = rowButton(QStringLiteral("Delete…"), QStringLiteral("Delete the login, or remove a shared one from your vault"),
                                   [this](const QString &id) { deleteItem(id); });
    deleteButton->setObjectName(QStringLiteral("dangerButton"));
    layout->addLayout(actions);

    connect(m_search, &QLineEdit::textChanged, this, [this] { renderItems(); });
    connect(m_filter, &QComboBox::currentIndexChanged, this, [this] { renderItems(); });
    connect(newButton, &QPushButton::clicked, this, [this] { editItem(QString()); });
    connect(refreshButton, &QPushButton::clicked, this, [this] { reload(); });
    connect(m_importButton, &QPushButton::clicked, this, [this] { importPasswords(); });
    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this] {
        const bool any = !selectedId().isEmpty();
        for (QPushButton *button : std::as_const(m_rowButtons))
            button->setEnabled(any && !m_busy);
    });
    connect(m_table, &QTableWidget::cellDoubleClicked, this, [this](int, int) {
        const QString id = selectedId();
        if (!id.isEmpty())
            editItem(id);
    });
}

void VaultFeature::showPanel(Panel panel)
{
    m_stack->setCurrentIndex(int(panel));
    m_lockButton->setVisible(panel == AppPanel);
    m_settingsButton->setVisible(panel == AppPanel);
}

void VaultFeature::setStatus(const QString &text, bool error)
{
    m_status->setObjectName(error ? QStringLiteral("errorLabel") : QStringLiteral("mutedLabel"));
    m_status->style()->unpolish(m_status);
    m_status->style()->polish(m_status);
    m_status->setText(text);
}

void VaultFeature::setBusy(bool busy)
{
    m_busy = busy;
    for (QWidget *widget : std::initializer_list<QWidget *>{m_setupButton, m_unlockButton, m_setupPassword,
                                                             m_setupConfirm, m_unlockPassword, m_lockButton, m_settingsButton,
                                                             m_importButton})
        widget->setEnabled(!busy);
    const bool any = !selectedId().isEmpty();
    for (QPushButton *button : std::as_const(m_rowButtons))
        button->setEnabled(any && !busy);
}

QString VaultFeature::explain(const QString &code) const
{
    static const QHash<QString, QString> words{
        {QStringLiteral("wrong_password"), QStringLiteral("That is not the master password.")},
        {QStringLiteral("argon2id_unavailable"), QStringLiteral("This build's OpenSSL has no Argon2id. The Password Vault needs OpenSSL 3.5 or newer; nothing was unlocked or created.")},
        {QStringLiteral("mlkem_unavailable"), QStringLiteral("This build's OpenSSL has no ML-KEM-768. The Password Vault needs OpenSSL 3.5 or newer; nothing was unlocked or created.")},
        {QStringLiteral("wrong_recovery_code"), QStringLiteral("That recovery code does not open this vault.")},
        {QStringLiteral("invalid_recovery_code"), QStringLiteral("A recovery code is 13 groups of 4 letters and digits.")},
        {QStringLiteral("unsafe_kdf_parameters"), QStringLiteral("The server returned key-stretching settings weaker than this vault allows. Nothing was unlocked.")},
        {QStringLiteral("keyset_mismatch"), QStringLiteral("The keys stored for this account do not match its published keys. Nothing was unlocked.")},
        {QStringLiteral("argon2id_unavailable"), QStringLiteral("This build's OpenSSL has no Argon2id, so the master password cannot be stretched here.")},
        {QStringLiteral("recipient_has_no_vault"), QStringLiteral("That person has not created a vault yet, so there are no keys to seal to.")},
        {QStringLiteral("recipient_not_found"), QStringLiteral("No ForkMesh user has that name.")},
        {QStringLiteral("recipient_keys_changed"), QStringLiteral("That person's keys changed since you last shared with them. Compare fingerprints before sharing.")},
        {QStringLiteral("cannot_share_with_yourself"), QStringLiteral("That is your own account.")},
        {QStringLiteral("organization_not_found"), QStringLiteral("You are not a member of that organization.")},
        {QStringLiteral("revision_conflict"), QStringLiteral("This login changed somewhere else. It was reloaded; make the change again.")},
        {QStringLiteral("read_only"), QStringLiteral("This login was shared with you to view, not to edit.")},
        {QStringLiteral("vault_locked"), QStringLiteral("The vault is locked.")},
        {QStringLiteral("vault_full"), QStringLiteral("This vault is full.")},
        {QStringLiteral("title_required"), QStringLiteral("Give the login a title.")},
        {QStringLiteral("csv_empty"), QStringLiteral("That file is empty.")},
        {QStringLiteral("csv_too_large"), QStringLiteral("That file is larger than 16 MB, which is more than any password export.")},
        {QStringLiteral("csv_unknown_columns"), QStringLiteral("That file has no Password column. Choose the CSV that Apple Passwords or another password manager exported.")},
        {QStringLiteral("csv_no_logins"), QStringLiteral("That file has no logins in it.")},
        {QStringLiteral("invalid_session"), QStringLiteral("Sign in to your ForkMesh account on this device to open your vault.")},
        {QStringLiteral("no_vault"), QStringLiteral("There is no Password Vault on this account yet. Open Vault to create one.")},
        {QStringLiteral("vault_busy"), QStringLiteral("The Password Vault is busy. Try again in a moment.")},
        {QStringLiteral("invalid_url"), QStringLiteral("That login has no web address the vault can match.")},
    };
    const QString known = words.value(code);
    if (!known.isEmpty())
        return known;
    QString plain = code;
    return plain.replace(QLatin1Char('_'), QLatin1Char(' '));
}

// ---- lifecycle -------------------------------------------------------------------------

void VaultFeature::refresh()
{
    if (m_client->unlocked()) {
        touch();
        return;
    }
    if (!m_loaded || m_stack->currentIndex() == LoadingPanel)
        load();
}

void VaultFeature::load()
{
    setStatus(QStringLiteral("Loading your vault…"));
    m_client->status([this](const QString &code) {
        m_loaded = true;
        if (!code.isEmpty()) {
            showPanel(UnlockPanel);
            setStatus(explain(code), true);
            return;
        }
        showPanel(m_client->installed() ? UnlockPanel : SetupPanel);
        setStatus(m_client->installed()
                      ? QStringLiteral("Signed in as %1. The vault is locked.").arg(m_client->account())
                      : QStringLiteral("Signed in as %1. No vault yet.").arg(m_client->account()));
        if (m_client->installed())
            m_unlockPassword->setFocus();
    });
}

bool VaultFeature::newPasswordFrom(QLineEdit *first, QLineEdit *second, QString *password)
{
    if (first->text().size() < kMinimumMasterPassword) {
        setStatus(QStringLiteral("Use at least 12 characters for the master password."), true);
        return false;
    }
    if (first->text() != second->text()) {
        setStatus(QStringLiteral("The two passwords do not match."), true);
        return false;
    }
    *password = first->text();
    return true;
}

void VaultFeature::createVault()
{
    QString password;
    if (m_busy || !newPasswordFrom(m_setupPassword, m_setupConfirm, &password))
        return;
    setBusy(true);
    setStatus(QStringLiteral("Creating your vault…"));
    m_client->setUp(password, [this](const QString &code, const QString &recoveryCode) {
        setBusy(false);
        m_setupPassword->clear();
        m_setupConfirm->clear();
        if (!code.isEmpty()) {
            setStatus(explain(code), true);
            return;
        }
        showRecoveryCode(recoveryCode);
        enter();
    });
}

void VaultFeature::unlockVault()
{
    if (m_busy || m_unlockPassword->text().isEmpty())
        return;
    setBusy(true);
    setStatus(QStringLiteral("Unlocking…"));
    const QString password = m_unlockPassword->text();
    m_unlockPassword->clear();
    m_client->unlock(password, [this](const QString &code) {
        setBusy(false);
        if (!code.isEmpty()) {
            setStatus(explain(code), true);
            m_unlockPassword->setFocus();
            return;
        }
        enter();
    });
}

void VaultFeature::recoverVault()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Recover your vault"));
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(mutedLabel(QStringLiteral(
        "The recovery code opens the vault once. You set a new master password and get a new code; the old one stops working.")));
    auto *form = new QFormLayout;
    auto *code = new QLineEdit;
    code->setFont(monospace());
    code->setPlaceholderText(QStringLiteral("XXXX-XXXX-…"));
    auto *first = passwordEdit(QStringLiteral("At least 12 characters"));
    auto *second = passwordEdit(QStringLiteral("Type it again"));
    form->addRow(QStringLiteral("Recovery code"), code);
    form->addRow(QStringLiteral("New master password"), first);
    form->addRow(QStringLiteral("Confirm"), second);
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Recover"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    QString password;
    if (!newPasswordFrom(first, second, &password))
        return;
    setBusy(true);
    setStatus(QStringLiteral("Recovering…"));
    m_client->recover(code->text(), password, [this](const QString &why, const QString &recoveryCode) {
        setBusy(false);
        if (!why.isEmpty()) {
            setStatus(explain(why), true);
            return;
        }
        showRecoveryCode(recoveryCode);
        enter();
    });
}

void VaultFeature::enter()
{
    showPanel(AppPanel);
    reload([this] {
        m_client->syncPending([this](const QString &, int sealed) {
            if (sealed > 0)
                setStatus(QStringLiteral("Sealed shared logins to %1 new organization member%2.")
                              .arg(sealed).arg(sealed == 1 ? QString() : QStringLiteral("s")));
        });
    });
    touch();
}

void VaultFeature::lockVault(const QString &note)
{
    m_idleTimer->stop();
    m_client->lock();
    // Dialogs of this page hold decrypted fields: close them with the vault.
    const QList<QDialog *> dialogs = findChildren<QDialog *>();
    for (QDialog *dialog : dialogs)
        dialog->reject();
    m_table->setRowCount(0);
    m_search->clear();
    m_unlockPassword->clear();
    // The top search box drops this vault's rows the moment it locks.
    if (m_searchIndexChanged)
        m_searchIndexChanged();
    if (m_loaded && m_stack->currentIndex() == AppPanel) {
        showPanel(UnlockPanel);
        setStatus(note.isEmpty() ? QStringLiteral("The vault is locked.") : note);
    }
}

void VaultFeature::unlockFor(const QString &purpose, QWidget *asker, const QString &error, bool checked,
                            std::function<void(const QString &)> then)
{
    if (m_client->unlocked()) {
        then(QString());
        return;
    }
    if (m_busy) {
        then(QStringLiteral("vault_busy"));
        return;
    }
    if (!checked && (!m_loaded || !m_client->installed())) {
        m_client->status([this, purpose, asker = QPointer<QWidget>(asker), then](const QString &code) {
            m_loaded = true;
            if (!code.isEmpty()) {
                showPanel(UnlockPanel);
                setStatus(explain(code), true);
                then(code);
                return;
            }
            showPanel(m_client->installed() ? UnlockPanel : SetupPanel);
            unlockFor(purpose, asker, QString(), true, then);
        });
        return;
    }
    if (!m_client->installed()) {
        then(QStringLiteral("no_vault"));
        return;
    }
    QDialog dialog(asker ? asker->window() : window());
    dialog.setWindowTitle(QStringLiteral("Unlock Password Vault"));
    dialog.setMinimumWidth(420);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(mutedLabel(purpose));
    if (!error.isEmpty()) {
        auto *problem = mutedLabel(error);
        problem->setObjectName(QStringLiteral("errorLabel"));
        layout->addWidget(problem);
    }
    auto *password = passwordEdit(QStringLiteral("Master password"));
    password->setAccessibleName(QStringLiteral("Master password"));
    layout->addWidget(password);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Unlock"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    password->setFocus();
    if (dialog.exec() != QDialog::Accepted || password->text().isEmpty()) {
        password->clear();
        then(QStringLiteral("cancelled"));
        return;
    }
    const QString typed = password->text();
    password->clear();
    setBusy(true);
    setStatus(QStringLiteral("Unlocking…"));
    m_client->unlock(typed, [this, purpose, asker = QPointer<QWidget>(asker), then](const QString &code) {
        setBusy(false);
        if (code == QLatin1String("wrong_password")) {
            setStatus(explain(code), true);
            unlockFor(purpose, asker, explain(code), true, then);
            return;
        }
        if (!code.isEmpty()) {
            setStatus(explain(code), true);
            then(code);
            return;
        }
        showPanel(AppPanel);
        touch();
        then(QString());
    });
}

void VaultFeature::saveLogin(const forkmesh::market::NativePackageLogin &login, const QString &requester, QWidget *asker,
                             forkmesh::market::NativePackageLoginHandler done)
{
    if (VaultClient::loginOrigin(login.url).isEmpty() || login.password.isEmpty()) {
        done({false, false, explain(QStringLiteral("invalid_url")), {}});
        return;
    }
    const QString purpose = QStringLiteral("%1 asks to save the password for %2 in your Password Vault. Type your master password to unlock it.")
                                .arg(requester, login.url);
    unlockFor(purpose, asker, QString(), false, [this, login, done](const QString &code) {
        if (code == QLatin1String("cancelled")) {
            done({false, true, QString(), {}});
            return;
        }
        if (!code.isEmpty()) {
            done({false, false, explain(code), {}});
            return;
        }
        const QString title = login.title.trimmed().isEmpty() ? QUrl::fromUserInput(login.url).host() : login.title;
        const QJsonObject fields{{QStringLiteral("title"), title}, {QStringLiteral("url"), login.url},
                                 {QStringLiteral("username"), login.username}, {QStringLiteral("password"), login.password},
                                 {QStringLiteral("notes"), QString()}};
        m_client->rememberLogin(fields, [this, login, done](const QString &saved, const QString &, bool changed) {
            renderItems();
            if (m_searchIndexChanged)
                m_searchIndexChanged();
            touch();
            if (!saved.isEmpty()) {
                setStatus(explain(saved), true);
                done({false, false, explain(saved), {}});
                return;
            }
            setStatus(changed ? QStringLiteral("Saved the login for %1.").arg(login.url)
                              : QStringLiteral("The login for %1 was already saved.").arg(login.url));
            done({true, false, QString(), {{login.title, login.url, login.username, QString()}}});
        });
    });
}

void VaultFeature::findLogins(const QString &url, const QString &requester, QWidget *asker,
                              forkmesh::market::NativePackageLoginHandler done)
{
    if (VaultClient::loginOrigin(url).isEmpty()) {
        done({false, false, explain(QStringLiteral("invalid_url")), {}});
        return;
    }
    const QString purpose = QStringLiteral("%1 asks for the password you saved for %2. Type your master password to unlock your Password Vault.")
                                .arg(requester, url);
    unlockFor(purpose, asker, QString(), false, [this, url, done](const QString &code) {
        if (code == QLatin1String("cancelled")) {
            done({false, true, QString(), {}});
            return;
        }
        if (!code.isEmpty()) {
            done({false, false, explain(code), {}});
            return;
        }
        m_client->load([this, url, done](const QString &loaded) {
            renderItems();
            if (m_searchIndexChanged)
                m_searchIndexChanged();
            touch();
            if (!loaded.isEmpty()) {
                setStatus(explain(loaded), true);
                done({false, false, explain(loaded), {}});
                return;
            }
            QList<forkmesh::market::NativePackageLogin> logins;
            for (const VaultItem *item : m_client->loginsFor(url))
                logins.append({item->field("title"), item->field("url"), item->field("username"), item->field("password")});
            done({true, false, QString(), logins});
        });
    });
}

void VaultFeature::touch()
{
    if (m_client->unlocked())
        m_idleTimer->start(m_autoLockMinutes * 60 * 1000);
}

bool VaultFeature::eventFilter(QObject *watched, QEvent *event)
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

// ---- list -------------------------------------------------------------------------------

void VaultFeature::reload(std::function<void()> then)
{
    if (!m_client->unlocked())
        return;
    m_client->load([this, then = std::move(then)](const QString &code) {
        if (!code.isEmpty()) {
            setStatus(explain(code), true);
            return;
        }
        renderItems();
        if (m_searchIndexChanged)
            m_searchIndexChanged();
        const int unreadable = int(std::count_if(m_client->items().cbegin(), m_client->items().cend(),
                                                  [](const VaultItem &item) { return !item.readable(); }));
        setStatus(unreadable > 0
                      ? QStringLiteral("%1 shared item%2 could not be opened. See the Sharing column.")
                            .arg(unreadable).arg(unreadable == 1 ? QString() : QStringLiteral("s"))
                      : QStringLiteral("%1 login%2. Locks after %3 idle minutes.")
                            .arg(m_client->items().size()).arg(m_client->items().size() == 1 ? QString() : QStringLiteral("s"))
                            .arg(m_autoLockMinutes),
                  unreadable > 0);
        if (then)
            then();
    });
}

void VaultFeature::renderItems()
{
    const QString query = (m_search->text().trimmed().isEmpty() ? m_hostQuery : m_search->text()).trimmed().toLower();
    const QString filter = m_filter->currentData().toString();
    const QString keep = selectedId();
    m_table->setRowCount(0);
    for (const VaultItem &item : m_client->items()) {
        if ((filter == QLatin1String("mine") && !item.owned) || (filter == QLatin1String("shared") && item.owned))
            continue;
        const QString host = loginHost(item.field("url"));
        if (!query.isEmpty() && !item.field("title").toLower().contains(query)
            && !item.field("username").toLower().contains(query) && !host.contains(query)
            && !item.owner.toLower().contains(query))
            continue;
        QString sharing;
        if (!item.readable()) {
            sharing = item.error == QLatin1String("sender_keys_changed")
                          ? QStringLiteral("%1's keys changed: compare fingerprints").arg(item.owner)
                          : QStringLiteral("cannot be opened (%1)").arg(item.error);
        } else if (!item.owned) {
            sharing = QStringLiteral("from %1%2, %3").arg(item.owner,
                item.viaKind == QLatin1String("organization") ? QStringLiteral(" via %1").arg(item.viaName) : QString(),
                item.permission == QLatin1String("write") ? QStringLiteral("can edit") : QStringLiteral("view only"));
        } else {
            QStringList names;
            for (const VaultShare &share : item.shares)
                names.append(share.kind == QLatin1String("organization") ? QStringLiteral("org %1").arg(share.name) : share.name);
            sharing = names.join(QStringLiteral(", "));
        }
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        const QStringList cells{item.readable() ? item.field("title") : QStringLiteral("Unreadable item"),
                                item.field("username"), host, sharing};
        for (int column = 0; column < cells.size(); ++column) {
            auto *cell = new QTableWidgetItem(cells.at(column));
            cell->setData(Qt::UserRole, item.id);
            cell->setToolTip(cells.at(column));
            m_table->setItem(row, column, cell);
        }
        if (item.id == keep)
            m_table->selectRow(row);
    }
    const int count = m_table->rowCount();
    const QString countText = QStringLiteral("%1 login%2")
                                  .arg(count)
                                  .arg(count == 1 ? QString() : QStringLiteral("s"));
    const QString emptyText = query.isEmpty()
        ? QStringLiteral("Nothing here yet. Add a login, import the logins saved in Apple Passwords or another password manager, or install the ForkMesh browser extension to fill and save them as you browse.")
        : QStringLiteral("No saved login matches \"%1\". Try a different search.")
              .arg(query);
    m_empty->setText(emptyText);
    m_empty->setAccessibleName(emptyText);
    m_table->setAccessibleDescription(
        query.isEmpty() ? QStringLiteral("Showing %1.").arg(countText)
                        : QStringLiteral("Showing %1 matching \"%2\".").arg(countText, query));
    m_empty->setVisible(count == 0);
    m_table->setVisible(count > 0);
}

QString VaultFeature::selectedId() const
{
    const QList<QTableWidgetItem *> selected = m_table->selectedItems();
    return selected.isEmpty() ? QString() : selected.first()->data(Qt::UserRole).toString();
}

void VaultFeature::setHostSearchQuery(const QString &query)
{
    m_hostQuery = query;
    if (m_client->unlocked())
        renderItems();
}

// Titles and usernames only, and only while unlocked: the window's search box
// never sees a password.
QList<forkmesh::market::NativePackageSearchHit> VaultFeature::searchHits(const QString &query, int limit) const
{
    QList<forkmesh::market::NativePackageSearchHit> hits;
    const QString needle = query.trimmed().toLower();
    if (needle.isEmpty() || !m_client->unlocked())
        return hits;
    for (const VaultItem &item : m_client->items()) {
        if (hits.size() >= limit)
            break;
        if (!item.readable())
            continue;
        const QString host = loginHost(item.field("url"));
        if (item.field("title").toLower().contains(needle) || item.field("username").toLower().contains(needle)
            || host.contains(needle))
            hits.append({item.id, item.field("title"),
                         QStringList{item.field("username"), host}.join(item.field("username").isEmpty() || host.isEmpty()
                                                                           ? QString() : QStringLiteral(" · "))});
    }
    return hits;
}

void VaultFeature::activateSearchHit(const QString &id)
{
    if (m_client->unlocked() && m_client->find(id))
        editItem(id);
}

// ---- row actions ---------------------------------------------------------------------------

void VaultFeature::copyField(const QString &id, const char *field, const QString &label)
{
    const VaultItem *item = m_client->find(id);
    QClipboard *clipboard = QGuiApplication::clipboard();
    if (!item || !item->readable() || !clipboard)
        return;
    m_clipboardValue = item->field(field);
    clipboard->setText(m_clipboardValue);
    m_clipboardTimer->start(kClipboardClearMs);
    setStatus(QStringLiteral("%1 copied. The clipboard is cleared in 30 seconds.").arg(label));
}

void VaultFeature::openSite(const QString &id)
{
    const VaultItem *item = m_client->find(id);
    // Only an https address (or http on localhost) is ever handed to the
    // system: never a javascript:, file:, or custom-scheme URL from a share.
    const QString href = item ? loginHref(item->field("url")) : QString();
    if (href.isEmpty())
        setStatus(QStringLiteral("This login has no https address to open."), true);
    else
        QDesktopServices::openUrl(QUrl(href));
}

void VaultFeature::editItem(const QString &id)
{
    const VaultItem *item = id.isEmpty() ? nullptr : m_client->find(id);
    if (!id.isEmpty() && (!item || !item->readable()))
        return;
    const bool readOnly = item && item->permission == QLatin1String("read");
    QDialog dialog(this);
    dialog.setWindowTitle(!item ? QStringLiteral("New login") : readOnly ? QStringLiteral("Login") : QStringLiteral("Edit login"));
    dialog.setMinimumWidth(460);
    auto *layout = new QVBoxLayout(&dialog);
    if (item && !item->owned)
        layout->addWidget(mutedLabel(QStringLiteral("Shared by %1%2.").arg(item->owner,
            !item->updatedBy.isEmpty() && item->updatedBy != item->owner ? QStringLiteral(", last changed by %1").arg(item->updatedBy) : QString())));
    auto *form = new QFormLayout;
    auto *title = new QLineEdit(item ? item->field("title") : QString());
    auto *url = new QLineEdit(item ? item->field("url") : QString());
    url->setPlaceholderText(QStringLiteral("https://"));
    auto *username = new QLineEdit(item ? item->field("username") : QString());
    auto *password = passwordEdit(QString());
    password->setText(item ? item->field("password") : QString());
    password->setFont(monospace());
    auto *notes = new QPlainTextEdit(item ? item->field("notes") : QString());
    notes->setMaximumHeight(90);
    title->setMaxLength(200);
    url->setMaxLength(2048);
    username->setMaxLength(320);
    password->setMaxLength(4096);
    auto *passwordRow = new QHBoxLayout;
    auto *reveal = new QCheckBox(QStringLiteral("Show"));
    auto *generate = new QPushButton(QStringLiteral("Generate"));
    generate->setObjectName(QStringLiteral("ghostButton"));
    passwordRow->addWidget(password, 1);
    passwordRow->addWidget(reveal);
    passwordRow->addWidget(generate);
    form->addRow(QStringLiteral("Title"), title);
    form->addRow(QStringLiteral("Website"), url);
    form->addRow(QStringLiteral("Username"), username);
    form->addRow(QStringLiteral("Password"), passwordRow);
    form->addRow(QStringLiteral("Notes"), notes);
    layout->addLayout(form);
    for (QLineEdit *edit : {title, url, username, password})
        edit->setReadOnly(readOnly);
    notes->setReadOnly(readOnly);
    generate->setVisible(!readOnly);
    auto *buttons = new QDialogButtonBox(readOnly ? QDialogButtonBox::Close : QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    connect(reveal, &QCheckBox::toggled, password, [password](bool on) {
        password->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
    });
    connect(generate, &QPushButton::clicked, &dialog, [&dialog, password, reveal] {
        if (!password->text().isEmpty()
            && QMessageBox::question(&dialog, QStringLiteral("Generate a password"),
                                     QStringLiteral("Replace the current password with a generated one?")) != QMessageBox::Yes)
            return;
        password->setText(generatePassword());
        reveal->setChecked(true);
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted || readOnly || !m_client->unlocked())
        return;
    const QJsonObject fields{{QStringLiteral("title"), title->text()}, {QStringLiteral("url"), url->text()},
                             {QStringLiteral("username"), username->text()}, {QStringLiteral("password"), password->text()},
                             {QStringLiteral("notes"), notes->toPlainText()}};
    m_client->save(fields, id, [this](const QString &code, const QString &) {
        if (!code.isEmpty())
            setStatus(explain(code), true);
        reload();
    });
}

void VaultFeature::deleteItem(const QString &id)
{
    const VaultItem *item = m_client->find(id);
    if (!item)
        return;
    if (!item->owned && item->viaKind != QLatin1String("user")) {
        setStatus(QStringLiteral("This login reaches you through %1. Only its owner, %2, can stop sharing it.").arg(item->viaName, item->owner), true);
        return;
    }
    const QString name = item->readable() ? item->field("title") : QStringLiteral("this item");
    const QString question = item->owned
        ? QStringLiteral("Delete \"%1\"? Everyone it is shared with loses it too. This cannot be undone.").arg(name)
        : QStringLiteral("Remove \"%1\" from your vault? %2 keeps it.").arg(name, item->owner);
    if (QMessageBox::warning(this, QStringLiteral("Password Vault"), question, QMessageBox::Yes | QMessageBox::Cancel,
                             QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    m_client->remove(id, [this](const QString &code) {
        if (!code.isEmpty())
            setStatus(explain(code), true);
        reload();
    });
}

void VaultFeature::shareItem(const QString &id)
{
    const VaultItem *item = m_client->find(id);
    if (!item || !item->readable())
        return;
    if (!item->owned) {
        setStatus(QStringLiteral("Only the owner, %1, can share this login.").arg(item->owner), true);
        return;
    }
    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("Share %1").arg(item->field("title")));
    dialog->setMinimumWidth(560);
    auto *layout = new QVBoxLayout(dialog);
    layout->addWidget(mutedLabel(QStringLiteral(
        "The login's key is sealed to each person's own public keys (X25519 and post-quantum ML-KEM-768). "
        "ForkMesh passes the sealed key along and cannot open it.")));
    auto *current = new QListWidget;
    current->setMaximumHeight(110);
    layout->addWidget(current);
    auto *stop = new QPushButton(QStringLiteral("Stop sharing with the selected…"));
    stop->setObjectName(QStringLiteral("dangerButton"));
    auto *stopRow = new QHBoxLayout;
    stopRow->addWidget(stop);
    stopRow->addStretch();
    layout->addLayout(stopRow);

    auto *target = new QHBoxLayout;
    auto *kind = new QComboBox;
    kind->addItem(QStringLiteral("A ForkMesh user"), QStringLiteral("user"));
    kind->addItem(QStringLiteral("An organization"), QStringLiteral("organization"));
    auto *userName = new QLineEdit;
    userName->setPlaceholderText(QStringLiteral("username"));
    auto *organization = new QComboBox;
    organization->setVisible(false);
    auto *permission = new QComboBox;
    permission->addItem(QStringLiteral("Can view and use"), QStringLiteral("read"));
    permission->addItem(QStringLiteral("Can view and edit"), QStringLiteral("write"));
    auto *checkKeys = new QPushButton(QStringLiteral("Check keys"));
    checkKeys->setObjectName(QStringLiteral("ghostButton"));
    for (QWidget *widget : std::initializer_list<QWidget *>{kind, userName, organization, permission, checkKeys})
        target->addWidget(widget, widget == userName || widget == organization ? 1 : 0);
    layout->addLayout(target);

    auto *members = new QListWidget;
    members->setFont(monospace());
    members->setVisible(false);
    layout->addWidget(members);
    auto *advice = mutedLabel(QString());
    layout->addWidget(advice);
    auto *confirmChanged = new QCheckBox(QStringLiteral("I compared the changed fingerprint with that person, outside ForkMesh."));
    confirmChanged->setVisible(false);
    layout->addWidget(confirmChanged);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    auto *shareButton = buttons->addButton(QStringLiteral("Share"), QDialogButtonBox::ActionRole);
    shareButton->setObjectName(QStringLiteral("primaryButton"));
    shareButton->setEnabled(false);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);

    const QPointer<QDialog> alive(dialog);
    auto changed = std::make_shared<QStringList>();
    const auto listShares = [this, id, current, stop] {
        current->clear();
        const VaultItem *fresh = m_client->find(id);
        for (const VaultShare &share : fresh ? fresh->shares : QList<VaultShare>()) {
            auto *row = new QListWidgetItem(QStringLiteral("%1%2: %3").arg(
                share.kind == QLatin1String("organization") ? QStringLiteral("Organization ") : QString(), share.name,
                share.permission == QLatin1String("write") ? QStringLiteral("can view and edit") : QStringLiteral("can view and use")));
            row->setData(Qt::UserRole, share.kind);
            row->setData(Qt::UserRole + 1, share.name);
            current->addItem(row);
        }
        if (current->count() == 0)
            current->addItem(QStringLiteral("Not shared with anyone."));
        stop->setEnabled(fresh && !fresh->shares.isEmpty());
    };
    listShares();
    const auto resetPreview = [members, advice, confirmChanged, shareButton, changed] {
        members->setVisible(false);
        advice->clear();
        confirmChanged->setVisible(false);
        confirmChanged->setChecked(false);
        shareButton->setEnabled(false);
        changed->clear();
    };
    const auto targetName = [kind, userName, organization] {
        return kind->currentData().toString() == QLatin1String("organization")
                   ? organization->currentText() : userName->text().trimmed().toLower();
    };
    m_client->organizations([alive, kind, organization](const QString &, const QStringList &names) {
        if (!alive)
            return;
        organization->addItems(names);
        if (names.isEmpty())
            kind->removeItem(1);
    });
    connect(kind, &QComboBox::currentIndexChanged, dialog, [kind, userName, organization, resetPreview] {
        const bool org = kind->currentData().toString() == QLatin1String("organization");
        userName->setVisible(!org);
        organization->setVisible(org);
        resetPreview();
    });
    connect(userName, &QLineEdit::textEdited, dialog, resetPreview);
    connect(organization, &QComboBox::currentIndexChanged, dialog, resetPreview);
    connect(permission, &QComboBox::currentIndexChanged, dialog, resetPreview);

    connect(checkKeys, &QPushButton::clicked, dialog, [=, this] {
        const QString name = targetName();
        if (name.isEmpty())
            return;
        resetPreview();
        m_client->previewShare(kind->currentData().toString(), name, [=, this](const QString &code, const QList<VaultRecipient> &found) {
            if (!alive)
                return;
            if (!code.isEmpty()) {
                advice->setText(explain(code));
                return;
            }
            static const QHash<QString, QString> labels{
                {QStringLiteral("new"), QStringLiteral("first share")}, {QStringLiteral("pinned"), QStringLiteral("known keys")},
                {QStringLiteral("changed"), QStringLiteral("KEYS CHANGED")}, {QStringLiteral("no_vault"), QStringLiteral("no vault yet")},
                {QStringLiteral("invalid_keys"), QStringLiteral("invalid keys")}};
            members->clear();
            int fresh = 0, without = 0;
            for (const VaultRecipient &member : found) {
                QString line = QStringLiteral("%1  [%2]").arg(member.name, labels.value(member.state, member.state));
                if (!member.fingerprint.isEmpty() && member.state != QLatin1String("pinned"))
                    line += QStringLiteral("\n    %1").arg(member.fingerprint);
                members->addItem(line);
                if (member.state == QLatin1String("changed"))
                    changed->append(member.name);
                fresh += member.state == QLatin1String("new");
                without += member.state == QLatin1String("no_vault");
            }
            if (found.isEmpty())
                members->addItem(QStringLiteral("Nobody else is in this organization yet."));
            members->setVisible(true);
            QStringList notes;
            if (!changed->isEmpty())
                notes << QStringLiteral("%1 now presents different keys than you pinned. That happens when someone deletes and recreates their vault, and it is also what an attack looks like. Compare the fingerprint with them before going on.").arg(changed->join(QStringLiteral(", ")));
            if (fresh > 0 && kind->currentData().toString() == QLatin1String("user"))
                notes << QStringLiteral("First share with this person. To be certain these keys are theirs, ask them to read their fingerprint (Vault settings) to you.");
            if (without > 0)
                notes << QStringLiteral("%1 member%2 no vault yet. They get access the next time you unlock after they create one.").arg(without).arg(without == 1 ? QStringLiteral(" has") : QStringLiteral("s have"));
            advice->setText(notes.join(QLatin1Char(' ')));
            confirmChanged->setVisible(!changed->isEmpty());
            const bool blocked = kind->currentData().toString() == QLatin1String("user") && !changed->isEmpty();
            shareButton->setEnabled(!blocked);
        });
    });
    connect(confirmChanged, &QCheckBox::toggled, dialog, [shareButton](bool on) {
        if (on)
            shareButton->setEnabled(true);
    });
    connect(shareButton, &QPushButton::clicked, dialog, [=, this] {
        const QStringList accepted = confirmChanged->isChecked() ? *changed : QStringList();
        shareButton->setEnabled(false);
        m_client->share(id, kind->currentData().toString(), targetName(), permission->currentData().toString(), accepted,
                        [=, this](const QString &code, const ShareOutcome &outcome) {
            if (!code.isEmpty()) {
                if (alive)
                    advice->setText(explain(code));
                return;
            }
            setStatus(QStringLiteral("Shared with %1 (%2 sealed%3).").arg(targetName()).arg(outcome.sealedTo.size())
                          .arg(outcome.skipped.isEmpty() ? QString() : QStringLiteral(", skipped: %1").arg(outcome.skipped.join(QStringLiteral(", ")))));
            reload([alive, listShares, resetPreview] {
                if (alive) {
                    listShares();
                    resetPreview();
                }
            });
        });
    });
    connect(stop, &QPushButton::clicked, dialog, [=, this] {
        QListWidgetItem *row = current->currentItem();
        const QString shareKind = row ? row->data(Qt::UserRole).toString() : QString();
        const QString shareName = row ? row->data(Qt::UserRole + 1).toString() : QString();
        if (shareKind.isEmpty()) {
            advice->setText(QStringLiteral("Select who to stop sharing with."));
            return;
        }
        if (QMessageBox::warning(dialog, QStringLiteral("Stop sharing"),
                                 QStringLiteral("Stop sharing with %1? The login is re-encrypted under a new key. If they already saw the password, change it on the website too.").arg(shareName),
                                 QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
            return;
        m_client->unshare(id, shareKind, shareName, [=, this](const QString &code) {
            if (!code.isEmpty() && alive)
                advice->setText(explain(code));
            reload([alive, listShares] {
                if (alive)
                    listShares();
            });
        });
    });
    dialog->open();
}

// ---- recovery code, settings ---------------------------------------------------------------

void VaultFeature::showRecoveryCode(const QString &code)
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Your recovery code"));
    // Closing the window is not the same as having saved the code.
    dialog.setWindowFlag(Qt::WindowCloseButtonHint, false);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(mutedLabel(QStringLiteral(
        "This is the only way back into the vault if the master password is forgotten. It is shown once. "
        "Print it or write it down and keep it somewhere safe, away from this device. Do not store it in the vault.")));
    auto *shown = new QLabel(code);
    shown->setFont(monospace());
    shown->setTextInteractionFlags(Qt::TextSelectableByMouse);
    shown->setWordWrap(true);
    layout->addWidget(shown);
    auto *copy = new QPushButton(QStringLiteral("Copy"));
    copy->setObjectName(QStringLiteral("ghostButton"));
    auto *saved = new QCheckBox(QStringLiteral("I have saved this code."));
    auto *done = new QPushButton(QStringLiteral("Done"));
    done->setObjectName(QStringLiteral("primaryButton"));
    done->setEnabled(false);
    auto *row = new QHBoxLayout;
    row->addWidget(copy);
    row->addWidget(saved, 1);
    row->addWidget(done);
    layout->addLayout(row);
    connect(copy, &QPushButton::clicked, &dialog, [this, code] {
        if (QClipboard *clipboard = QGuiApplication::clipboard()) {
            m_clipboardValue = code;
            clipboard->setText(code);
            m_clipboardTimer->start(kClipboardClearMs);
        }
    });
    connect(saved, &QCheckBox::toggled, done, &QPushButton::setEnabled);
    connect(done, &QPushButton::clicked, &dialog, &QDialog::accept);
    dialog.exec();
    shown->clear();
}

void VaultFeature::importPasswords()
{
    if (!m_client->unlocked() || m_busy)
        return;
    QMessageBox intro(QMessageBox::Information, QStringLiteral("Import passwords"),
                      QStringLiteral("Choose a CSV export. Each login is encrypted on this device before it is uploaded, "
                                     "and logins already in the vault are skipped."),
                      QMessageBox::Cancel, this);
    intro.setInformativeText(QStringLiteral(
        "From Apple Passwords on a Mac: open Passwords and choose File, then Export All Passwords to File.\n"
        "On an iPhone or iPad: Settings, then Apps, Safari, Export. Unzip it and choose Passwords.csv.\n\n"
        "CSV exports from DuckDuckGo, Chrome, Firefox, Bitwarden and 1Password work too. "
        "The exported file holds every password in plain text: delete it once the import is done."));
    QPushButton *choose = intro.addButton(QStringLiteral("Choose CSV file…"), QMessageBox::AcceptRole);
    intro.setDefaultButton(choose);
    intro.exec();
    if (intro.clickedButton() != choose)
        return;
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Import passwords"), QStandardPaths::writableLocation(QStandardPaths::DownloadLocation),
        QStringLiteral("CSV (*.csv);;All files (*)"));
    if (path.isEmpty() || !m_client->unlocked())
        return;
    QFile source(path);
    if (source.size() > VaultClient::kImportMaxBytes) {
        setStatus(explain(QStringLiteral("csv_too_large")), true);
        return;
    }
    if (!source.open(QIODevice::ReadOnly)) {
        setStatus(QStringLiteral("Could not open %1.").arg(QFileInfo(path).fileName()), true);
        return;
    }
    QByteArray bytes = source.readAll();
    source.close();
    ImportFile file;
    QString code;
    const bool read = VaultClient::readImportCsv(bytes, &file, &code);
    bytes.fill('\0');
    if (!read) {
        setStatus(explain(code), true);
        return;
    }
    const ImportPlan plan = VaultClient::planImport(file.logins, m_client->items());
    const QString left = file.skippedRows == 0 ? QString()
        : QStringLiteral(" %1 row%2 without a username or a password %3 left out.")
              .arg(file.skippedRows).arg(file.skippedRows == 1 ? QString() : QStringLiteral("s"),
                                          file.skippedRows == 1 ? QStringLiteral("is") : QStringLiteral("are"));
    const QString already = plan.duplicates == 0 ? QString()
        : QStringLiteral(" %1 already in the vault %2 skipped.")
              .arg(plan.duplicates).arg(plan.duplicates == 1 ? QStringLiteral("is") : QStringLiteral("are"));
    if (plan.fresh.isEmpty()) {
        setStatus(QStringLiteral("Every login in that file is already in the vault.") + left);
        return;
    }
    const QString count = QStringLiteral("%1 login%2").arg(plan.fresh.size()).arg(plan.fresh.size() == 1 ? QString() : QStringLiteral("s"));
    if (QMessageBox::question(this, QStringLiteral("Import passwords"),
                              QStringLiteral("Import %1 from %2?%3%4").arg(count, file.sourceLabel, already, left))
        != QMessageBox::Yes)
        return;
    setBusy(true);
    setStatus(QStringLiteral("Importing %1…").arg(count));
    const QPointer<VaultFeature> guard(this);
    const QString label = file.sourceLabel;
    m_client->importLogins(
        file.logins,
        [guard](int done, int total) {
            if (!guard)
                return;
            guard->touch();
            guard->setStatus(QStringLiteral("Importing… %1 of %2").arg(done).arg(total));
        },
        [guard, label, already](const ImportReport &report) {
            if (!guard)
                return;
            guard->setBusy(false);
            guard->renderItems();
            if (guard->m_searchIndexChanged)
                guard->m_searchIndexChanged();
            const QString added = QStringLiteral("Imported %1 login%2 from %3.%4")
                                      .arg(report.added).arg(report.added == 1 ? QString() : QStringLiteral("s"), label, already);
            if (!report.stoppedBy.isEmpty())
                guard->setStatus(QStringLiteral("%1 %2 could not be imported: %3")
                                     .arg(added).arg(report.remaining).arg(guard->explain(report.stoppedBy)), true);
            else
                guard->setStatus(added + QStringLiteral(" Delete the exported file now: it holds every password in plain text."));
            guard->touch();
        });
}

void VaultFeature::openSettings()
{
    if (!m_client->unlocked())
        return;
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Vault settings"));
    dialog.setMinimumWidth(520);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(QStringLiteral("Your key fingerprint")));
    auto *print = new QLabel(m_client->myFingerprint());
    print->setFont(monospace());
    print->setWordWrap(true);
    print->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(print);
    layout->addWidget(mutedLabel(QStringLiteral(
        "Someone sharing with you for the first time can read this back to you to be sure the keys they were given are yours.")));

    auto *form = new QFormLayout;
    auto *autoLock = new QComboBox;
    for (int minutes : {1, 5, 10, 30})
        autoLock->addItem(QStringLiteral("%1 minute%2 idle").arg(minutes).arg(minutes == 1 ? QString() : QStringLiteral("s")), minutes);
    autoLock->setCurrentIndex(autoLock->findData(m_autoLockMinutes));
    auto *first = passwordEdit(QStringLiteral("At least 12 characters"));
    auto *second = passwordEdit(QStringLiteral("Type it again"));
    form->addRow(QStringLiteral("Lock after"), autoLock);
    form->addRow(QStringLiteral("New master password"), first);
    form->addRow(QStringLiteral("Confirm"), second);
    layout->addLayout(form);
    auto *actions = new QHBoxLayout;
    auto *change = new QPushButton(QStringLiteral("Change master password"));
    auto *renew = new QPushButton(QStringLiteral("New recovery code…"));
    auto *exportButton = new QPushButton(QStringLiteral("Save encrypted export…"));
    auto *destroy = new QPushButton(QStringLiteral("Delete vault…"));
    destroy->setObjectName(QStringLiteral("dangerButton"));
    for (QPushButton *button : {change, renew, exportButton}) {
        button->setObjectName(QStringLiteral("ghostButton"));
        actions->addWidget(button);
    }
    actions->addStretch();
    actions->addWidget(destroy);
    layout->addLayout(actions);
    auto *note = mutedLabel(QString());
    layout->addWidget(note);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    const QPointer<QDialog> alive(&dialog);
    const auto saveExport = [this](const QJsonObject &payload) {
        const QString suggested = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QLatin1Char('/')
                                  + payload.value(QStringLiteral("filename")).toString(QStringLiteral("forkmesh-vault.json"));
        const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save encrypted vault export"), suggested,
                                                          QStringLiteral("JSON (*.json)"));
        if (path.isEmpty())
            return false;
        QSaveFile file(path);
        const QByteArray bytes = QJsonDocument(payload.value(QStringLiteral("export")).toObject()).toJson(QJsonDocument::Indented);
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
    };
    connect(autoLock, &QComboBox::currentIndexChanged, &dialog, [this, autoLock] {
        m_autoLockMinutes = autoLock->currentData().toInt();
        touch();
    });
    connect(change, &QPushButton::clicked, &dialog, [=, this] {
        if (first->text().size() < kMinimumMasterPassword) { note->setText(QStringLiteral("Use at least 12 characters for the master password.")); return; }
        if (first->text() != second->text()) { note->setText(QStringLiteral("The two passwords do not match.")); return; }
        const QString password = first->text();
        first->clear();
        second->clear();
        note->setText(QStringLiteral("Changing the master password…"));
        m_client->changePassword(password, [alive, note, this](const QString &code) {
            if (alive)
                note->setText(code.isEmpty() ? QStringLiteral("Master password changed. Other devices need the new one at their next unlock.") : explain(code));
        });
    });
    connect(renew, &QPushButton::clicked, &dialog, [=, this] {
        if (QMessageBox::question(alive, QStringLiteral("New recovery code"), QStringLiteral("Issue a new recovery code? The current one stops working.")) != QMessageBox::Yes)
            return;
        m_client->renewRecoveryCode([alive, note, this](const QString &code, const QString &recoveryCode) {
            if (!code.isEmpty()) {
                if (alive)
                    note->setText(explain(code));
                return;
            }
            showRecoveryCode(recoveryCode);
        });
    });
    connect(exportButton, &QPushButton::clicked, &dialog, [=, this] {
        m_client->exportData([alive, note, saveExport, this](const QString &code, const QJsonObject &payload) {
            if (!alive)
                return;
            note->setText(!code.isEmpty() ? explain(code) : saveExport(payload)
                              ? QStringLiteral("Saved. It opens only with your master password or recovery code.") : QStringLiteral("Nothing was saved."));
        });
    });
    connect(destroy, &QPushButton::clicked, &dialog, [=, this] {
        if (QMessageBox::warning(alive, QStringLiteral("Delete vault"),
                                 QStringLiteral("Delete your vault? Every login in it is erased here and for everyone you shared it with. An encrypted export is offered first."),
                                 QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
            return;
        m_client->exportData([=, this](const QString &code, const QJsonObject &payload) {
            // The export comes before the delete, and a declined save stops it.
            if (!code.isEmpty() || !saveExport(payload)) {
                if (alive)
                    note->setText(code.isEmpty() ? QStringLiteral("Nothing was deleted: the export was not saved.") : explain(code));
                return;
            }
            m_client->destroy([alive, this](const QString &why, const QJsonObject &) {
                if (!why.isEmpty()) {
                    setStatus(explain(why), true);
                    return;
                }
                if (alive)
                    alive->reject();
                m_table->setRowCount(0);
                showPanel(SetupPanel);
                setStatus(QStringLiteral("Vault deleted."));
            });
        });
    });
    dialog.exec();
}

}  // namespace forkmesh::vault

namespace {
// The desktop rail uses Octicons; "key" ships in desktop/resources/icons.qrc.
const forkmesh::market::NativePackageRegistration vaultRegistration({
    QStringLiteral("builtin:vault"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Vault"),
    QStringLiteral("key"),
    QStringLiteral("/api/vault"),
    QStringLiteral("forkmesh-vault-request-v1"),
    [](const forkmesh::market::NativePackageHost &host, QWidget *parent) {
        return new forkmesh::vault::VaultFeature(host, parent);
    },
});
}  // namespace
