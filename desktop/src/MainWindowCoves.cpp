// Cove feature: encrypted, password-shared file vaults inside a repo.
// These are MainWindow member functions, defined in their own translation unit to
// keep the feature self-contained (and to stay out of the way of the very large,
// frequently-edited MainWindow.cpp). See CoveStore/CoveCrypto for the on-disk
// forkmesh/coves/<slug>.cove format and the AES-256-GCM crypto.

#include "MainWindow.h"
#include "MainWindowInternal.h"

#include "CoveCrypto.h"
#include "CoveStore.h"
#include "ForkMeshIdentity.h"

#include <QCheckBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSharedPointer>
#include <QSplitter>
#include <QSystemTrayIcon>
#include <QVBoxLayout>

using namespace forkmesh::ui;

namespace {

bool coveAlertEnabled()
{
    return QSettings().value(QStringLiteral("notifications/coveOpened"), false).toBool();
}

QString shortStamp(qint64 ms)
{
    if (ms <= 0)
        return QStringLiteral("—");
    return QDateTime::fromMSecsSinceEpoch(ms).toString(QStringLiteral("yyyy-MM-dd hh:mm"));
}

QLabel *coveSectionStatus(QWidget *section)
{
    return section ? section->findChild<QLabel *>(QStringLiteral("coveSectionStatus"))
                   : nullptr;
}

} // namespace


CoveStore MainWindow::coveStoreForRepo(int repoIndex) const
{
    if (repoIndex < 0 || repoIndex >= m_repositories.size())
        return CoveStore(QString(), QString(), &m_profileIdentity, m_userName);
    const RepositoryRecord &repo = writableRecordFor(m_repositories.at(repoIndex));
    return CoveStore(repo.localPath, repo.mirrorPath, &m_profileIdentity, m_userName);
}

QString MainWindow::coveRepoSettingsPrefix(int repoIndex) const
{
    if (repoIndex < 0 || repoIndex >= m_repositories.size())
        return QStringLiteral("coves/repo/_");
    const RepositoryRecord &repo = m_repositories.at(repoIndex);
    return QStringLiteral("coves/repo/%1/%2").arg(repo.owner, repo.name);
}

QString MainWindow::rememberedCovePassword(int repoIndex) const
{
    return QSettings()
        .value(coveRepoSettingsPrefix(repoIndex) + "/password")
        .toString();
}

bool MainWindow::coveAutoOpenEnabled(int repoIndex) const
{
    return QSettings()
        .value(coveRepoSettingsPrefix(repoIndex) + "/autoOpen", false)
        .toBool();
}

bool MainWindow::tryUnlockCove(Cove &cove, int repoIndex, QString *passwordOut) const
{
    QStringList candidates;
    if (m_coveSessionPasswords.contains(cove.id))
        candidates << m_coveSessionPasswords.value(cove.id);
    const QString repoPw =
        QSettings().value(coveRepoSettingsPrefix(repoIndex) + "/password").toString();
    if (!repoPw.isEmpty())
        candidates << repoPw;
    for (const QString &pw : std::as_const(candidates)) {
        if (pw.isEmpty())
            continue;
        const QString attempt = cove.id + QLatin1Char('\x1f') + pw;
        if (m_coveFailedUnlocks.contains(attempt))
            continue;
        if (CoveStore::unlock(cove, pw)) {
            const_cast<MainWindow *>(this)->m_coveSessionPasswords.insert(cove.id, pw);
            if (passwordOut)
                *passwordOut = pw;
            return true;
        }
        const_cast<MainWindow *>(this)->m_coveFailedUnlocks.insert(attempt);
    }
    return false;
}


void MainWindow::logCoveAccessLocal(const Cove &cove)
{
    QSettings settings;
    const QString key = QStringLiteral("coves/access/") + cove.id;
    QStringList log = settings.value(key).toStringList();
    log.prepend(QStringLiteral("%1|%2|opened")
                    .arg(QDateTime::currentMSecsSinceEpoch())
                    .arg(cove.name));
    while (log.size() > 200)
        log.removeLast();
    settings.setValue(key, log);
}

QStringList MainWindow::coveAccessLogLocal(const QString &coveId) const
{
    return QSettings().value(QStringLiteral("coves/access/") + coveId).toStringList();
}


QByteArray MainWindow::coveOpenCanonical(const QString &coveId, const QString &creatorKey,
                                         const QString &openerKey, qint64 ts)
{
    return QByteArrayLiteral("forkmesh-cove-open-v1\n") + coveId.toUtf8() + "\n" +
           creatorKey.toUtf8() + "\n" + openerKey.toUtf8() + "\n" +
           QByteArray::number(ts);
}

void MainWindow::announceCoveOpened(const Cove &cove)
{
    if (!m_backend || cove.creator.isEmpty() || !m_profileIdentity.isValid())
        return;
    const QString openerKey = m_profileIdentity.publicKey();
    const qint64 ts = QDateTime::currentMSecsSinceEpoch();
    const QString sig = m_profileIdentity.signData(
        coveOpenCanonical(cove.id, cove.creator, openerKey, ts));
    m_backend->notifyCoveOpened(cove.creator, cove.id, cove.name, openerKey,
                                m_userName, ts, sig);
}

void MainWindow::onCoveOpened(const QString &creatorKey, const QString &coveId,
                              const QString &coveName, const QString &openerKey,
                              const QString &openerName, qint64 ts,
                              const QString &signature)
{
    if (!m_profileIdentity.isValid() || creatorKey != m_profileIdentity.publicKey())
        return;
    if (openerKey == m_profileIdentity.publicKey())
        return; // our own open echoed back
    // Trust the report only if the opener's signature verifies over the canonical.
    if (!ForkMeshIdentity::verifySignature(
            openerKey, signature, coveOpenCanonical(coveId, creatorKey, openerKey, ts)))
        return;

    const QString who =
        openerName.trimmed().isEmpty() ? QStringLiteral("Someone") : openerName.trimmed();
    const QString body =
        QString::fromUtf8("%1 opened your cove \xE2\x80\x9C%2\xE2\x80\x9D.")
            .arg(who, coveName);
    logSystem(body);
    addNotification(QStringLiteral("Cove opened"), body);
    if (coveAlertEnabled())
        showPingTrayMessage(QStringLiteral("ForkMesh — cove opened"), body);
}

void MainWindow::onCoveInvited(const QString &inviteeAccount, const QString &coveId,
                               const QString &coveName, const QString &inviterName,
                               const QString &shareUrl, qint64 ts)
{
    Q_UNUSED(ts);
    const QString account = coveAccountName();
    if (account.isEmpty() ||
        inviteeAccount.compare(account, Qt::CaseInsensitive) != 0)
        return; // this invite wasn't for us

    const QString who =
        inviterName.trimmed().isEmpty() ? QStringLiteral("Someone") : inviterName.trimmed();
    const QString name =
        coveName.trimmed().isEmpty() ? QStringLiteral("a cove") : coveName.trimmed();
    const QString body =
        QString::fromUtf8("%1 invited you to the cove \xE2\x80\x9C%2\xE2\x80\x9D.")
            .arg(who, name);
    logSystem(body);
    const NotificationLink link = notificationLinkForHref(shareUrl);
    const bool linkMatchesInvite =
        link.kind == QLatin1String("cove") && link.ref == coveId;
    if (linkMatchesInvite)
        addNotification(QStringLiteral("Cove invitation"), body, false, link,
                        QStringLiteral("cove"), who);
    else
        addNotification(QStringLiteral("Cove invitation"), body);
    if (coveAlertEnabled())
        postNotification(QStringLiteral("ForkMesh — cove invitation"), body,
                         false, QStringLiteral("folder-locked"),
                         linkMatchesInvite ? shareUrl : QString());
}


void MainWindow::openCove(const QString &relPath)
{
    const int idx = m_repoDetailIndex;
    if (idx < 0 || idx >= m_repositories.size())
        return;
    CoveStore store = coveStoreForRepo(idx);
    Cove cove;
    QString err;
    if (!store.loadEnvelope(relPath, cove, &err)) {
        showLocalNotice(coveSectionStatus(m_coveSection),
                        QStringLiteral("Could not open cove: ") + err, true);
        return;
    }

    // Account-scoped Coves now live in the shared Files workspace. Opening the
    // encrypted envelope from the repository tree should land in that same
    // editor instead of reviving the legacy standalone Cove dialog.
    Cove accountCove = cove;
    const QString account = coveAccountName();
    if (!account.isEmpty() && CoveStore::unlockForAccount(accountCove, account)) {
        logCoveAccessLocal(accountCove);
        if (accountCove.notifyOnOpen && !accountCove.createdByMe(&m_profileIdentity))
            announceCoveOpened(accountCove);
        m_coveExplorerCurrentId = accountCove.id;
        const QString firstDocument =
            accountCove.documents.isEmpty()
                ? QString()
                : QDir::cleanPath(accountCove.documents.first().name);
        showRepoCoveExplorer();
        if (!firstDocument.isEmpty())
            openCoveExplorerDocument(firstDocument);
        return;
    }

    QString password;
    if (!tryUnlockCove(cove, idx, &password)) {
        bool ok = false;
        // A locked cove's name is encrypted, so it may be unknown here.
        const QString prompt =
            cove.name.isEmpty()
                ? QStringLiteral("Enter the password for this cove:")
                : QString::fromUtf8("Enter the password for \xE2\x80\x9C%1\xE2\x80\x9D:")
                      .arg(cove.name);
        const QString entered = QInputDialog::getText(
            this, QStringLiteral("Unlock cove"), prompt,
            QLineEdit::Password, QString(), &ok);
        if (!ok || entered.isEmpty())
            return;
        if (!CoveStore::unlock(cove, entered)) {
            QMessageBox::warning(this, QStringLiteral("Unlock cove"),
                                 QStringLiteral("That password didn't unlock this cove."));
            return;
        }
        m_coveSessionPasswords.insert(cove.id, entered);
        password = entered;
    }
    openCoveViewer(idx, cove, password);
}

void MainWindow::openCoveViewer(int repoIndex, Cove cove, const QString &password)
{
    logCoveAccessLocal(cove);
    if (cove.notifyOnOpen && !cove.createdByMe(&m_profileIdentity))
        announceCoveOpened(cove);

    CoveStore store = coveStoreForRepo(repoIndex);
    const bool canSave = store.canWrite();

    if (canSave) {
        CoveAccessEntry entry;
        entry.who = m_profileIdentity.publicKey();
        entry.name = m_userName;
        entry.ts = QDateTime::currentMSecsSinceEpoch();
        entry.action = QStringLiteral("open");
        CoveStore::appendAccess(cove, entry);
    }

    auto *dialog = new QDialog(this);
    dialog->setObjectName("coveViewer");
    dialog->setWindowTitle(QString::fromUtf8("Cove \xE2\x80\x94 %1").arg(cove.name));
    dialog->resize(720, 480);
    dialog->setAttribute(Qt::WA_DeleteOnClose);

    auto data = QSharedPointer<Cove>::create(cove);
    auto cur = QSharedPointer<int>::create(-1);

    auto *outer = new QVBoxLayout(dialog);
    auto *header = new QLabel(QString::fromUtf8(
        "\xF0\x9F\x94\x92 Quantum-resistant vault (AES-256). %1").arg(
        canSave ? QStringLiteral("Edit and save to commit changes.")
                : QString::fromUtf8("Read-only here \xE2\x80\x94 no local working tree.")));
    header->setObjectName("statusLine");
    header->setWordWrap(true);
    outer->addWidget(header);

    auto *splitter = new QSplitter(Qt::Horizontal, dialog);
    auto *docList = new QListWidget;
    docList->setMaximumWidth(220);
    splitter->addWidget(docList);

    auto *right = new QWidget;
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    auto *nameEdit = new QLineEdit;
    nameEdit->setPlaceholderText(QStringLiteral("Document name"));
    nameEdit->setReadOnly(!canSave);
    rightLayout->addWidget(nameEdit);
    auto *bodyEdit = new QPlainTextEdit;
    bodyEdit->setReadOnly(!canSave);
    bodyEdit->setPlaceholderText(
        QString::fromUtf8("Plans, passwords, secrets \xE2\x80\x94 encrypted at rest."));
    rightLayout->addWidget(bodyEdit, 1);
    splitter->addWidget(right);
    splitter->setStretchFactor(1, 1);
    outer->addWidget(splitter, 1);

    auto refreshList = [docList, data] {
        QSignalBlocker block(docList);
        docList->clear();
        for (const CoveDocument &d : std::as_const(data->documents))
            docList->addItem(d.name.isEmpty() ? QStringLiteral("(untitled)") : d.name);
    };
    auto syncEditorToDoc = [cur, data, nameEdit, bodyEdit] {
        if (*cur >= 0 && *cur < data->documents.size()) {
            data->documents[*cur].name = nameEdit->text();
            data->documents[*cur].body = bodyEdit->toPlainText();
            data->documents[*cur].updatedAtMs = QDateTime::currentMSecsSinceEpoch();
        }
    };
    auto showDoc = [cur, data, nameEdit, bodyEdit](int row) {
        *cur = row;
        if (row >= 0 && row < data->documents.size()) {
            nameEdit->setText(data->documents[row].name);
            bodyEdit->setPlainText(data->documents[row].body);
        } else {
            nameEdit->clear();
            bodyEdit->clear();
        }
    };

    connect(docList, &QListWidget::currentRowChanged, dialog,
            [syncEditorToDoc, showDoc, cur](int row) {
                if (*cur != row)
                    syncEditorToDoc();
                showDoc(row);
            });

    auto *buttonRow = new QHBoxLayout;
    auto *addBtn = new QPushButton(QStringLiteral("Add document"));
    auto *delBtn = new QPushButton(QStringLiteral("Delete"));
    addBtn->setEnabled(canSave);
    delBtn->setEnabled(canSave);
    buttonRow->addWidget(addBtn);
    buttonRow->addWidget(delBtn);
    buttonRow->addStretch();
    auto *accessBtn = new QPushButton(QStringLiteral("Access log"));
    buttonRow->addWidget(accessBtn);
    auto *saveBtn = new QPushButton(QStringLiteral("Save"));
    saveBtn->setEnabled(canSave);
    buttonRow->addWidget(saveBtn);
    auto *closeBtn = new QPushButton(QStringLiteral("Close"));
    buttonRow->addWidget(closeBtn);
    outer->addLayout(buttonRow);

    connect(addBtn, &QPushButton::clicked, dialog,
            [syncEditorToDoc, refreshList, data, docList] {
                syncEditorToDoc();
                CoveDocument d;
                d.id = QStringLiteral("doc-%1").arg(QDateTime::currentMSecsSinceEpoch());
                d.name = QStringLiteral("New document");
                d.updatedAtMs = QDateTime::currentMSecsSinceEpoch();
                data->documents.append(d);
                refreshList();
                docList->setCurrentRow(data->documents.size() - 1);
            });
    connect(delBtn, &QPushButton::clicked, dialog,
            [cur, data, refreshList, docList, showDoc] {
                if (*cur < 0 || *cur >= data->documents.size())
                    return;
                data->documents.removeAt(*cur);
                *cur = -1;
                refreshList();
                if (data->documents.isEmpty())
                    showDoc(-1);
                else
                    docList->setCurrentRow(0);
            });
    connect(accessBtn, &QPushButton::clicked, dialog, [this, data] {
        QStringList lines;
        for (const CoveAccessEntry &e : std::as_const(data->accessLog))
            lines << QStringLiteral("%1  %2  (%3)")
                         .arg(shortStamp(e.ts),
                              e.name.isEmpty() ? QStringLiteral("?") : e.name, e.action);
        const QStringList local = coveAccessLogLocal(data->id);
        QStringList localLines;
        for (const QString &row : local) {
            const QStringList parts = row.split('|');
            if (parts.size() >= 3)
                localLines << QStringLiteral("%1  this machine  (%2)")
                                  .arg(shortStamp(parts.at(0).toLongLong()), parts.at(2));
        }
        const QString text =
            QStringLiteral("In-cove log (syncs to the creator):\n%1\n\nThis machine:\n%2")
                .arg(lines.isEmpty() ? QStringLiteral("  (none yet)") : lines.join('\n'),
                     localLines.isEmpty() ? QStringLiteral("  (none yet)")
                                          : localLines.join('\n'));
        QMessageBox::information(this, QStringLiteral("Cove access log"), text);
    });
    connect(saveBtn, &QPushButton::clicked, dialog,
            [this, repoIndex, syncEditorToDoc, data, password, dialog] {
                syncEditorToDoc();
                CoveStore store = coveStoreForRepo(repoIndex);
                QString err;
                if (store.save(*data, password, &err)) {
                    flashMessage(QString::fromUtf8(
                        "Saved cove \xE2\x80\x9C%1\xE2\x80\x9D.").arg(data->name));
                    rebuildRepoCovesList();
                    dialog->accept();
                } else {
                    QMessageBox::warning(this, QStringLiteral("Save cove"),
                                         QStringLiteral("Could not save: ") + err);
                }
            });
    connect(closeBtn, &QPushButton::clicked, dialog, &QDialog::reject);

    refreshList();
    if (!data->documents.isEmpty())
        docList->setCurrentRow(0);
    dialog->show();
}


void MainWindow::promptCreateCove(int repoIndex)
{
    if (repoIndex < 0 || repoIndex >= m_repositories.size())
        return;
    CoveStore store = coveStoreForRepo(repoIndex);
    if (!store.canWrite()) {
        QMessageBox::information(
            this, QStringLiteral("New cove"),
            QStringLiteral("This repository has no local working tree, so a cove "
                           "can't be created here. Open it in a working copy first."));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("New cove"));
    auto *form = new QFormLayout(&dialog);
    auto *nameEdit = new QLineEdit;
    nameEdit->setPlaceholderText(QStringLiteral("e.g. Launch plans"));
    form->addRow(QStringLiteral("Name"), nameEdit);
    auto *pwEdit = new QLineEdit;
    pwEdit->setEchoMode(QLineEdit::Password);
    pwEdit->setPlaceholderText(QStringLiteral("Shared with your team out-of-band"));
    form->addRow(QStringLiteral("Password"), pwEdit);
    auto *pw2Edit = new QLineEdit;
    pw2Edit->setEchoMode(QLineEdit::Password);
    form->addRow(QStringLiteral("Confirm"), pw2Edit);
    auto *notifyCheck = new QCheckBox(QStringLiteral("Notify me when someone opens it"));
    notifyCheck->setChecked(true);
    form->addRow(QString(), notifyCheck);
    auto *rememberCheck =
        new QCheckBox(QStringLiteral("Remember this password on this machine"));
    rememberCheck->setChecked(true);
    form->addRow(QString(), rememberCheck);
    auto *hint = new QLabel(QString::fromUtf8(
        "Encrypted with AES-256-GCM. 256-bit keys are quantum-resistant; there is "
        "no recovery if the password is lost."));
    hint->setObjectName("statusLine");
    hint->setWordWrap(true);
    form->addRow(hint);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Create"));
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted)
        return;
    const QString name = nameEdit->text().trimmed();
    const QString pw = pwEdit->text();
    if (name.isEmpty() || pw.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("New cove"),
                             QStringLiteral("A name and a password are required."));
        return;
    }
    if (pw != pw2Edit->text()) {
        QMessageBox::warning(this, QStringLiteral("New cove"),
                             QStringLiteral("The passwords don't match."));
        return;
    }

    Cove created;
    QString err;
    if (!store.createCove(name, pw, notifyCheck->isChecked(), {}, &created, &err)) {
        QMessageBox::warning(this, QStringLiteral("New cove"),
                             QStringLiteral("Could not create the cove: ") + err);
        return;
    }
    m_coveSessionPasswords.insert(created.id, pw);
    if (rememberCheck->isChecked())
        QSettings().setValue(coveRepoSettingsPrefix(repoIndex) + "/password", pw);
    logSystem(QString::fromUtf8("Created cove \xE2\x80\x9C%1\xE2\x80\x9D.").arg(name));
    rebuildRepoCovesList();
    if (m_treeLoadedForIndex == m_repoDetailIndex)
        loadRepoFileTree();
    openCoveViewer(repoIndex, created, pw);
}


QWidget *MainWindow::buildCoveSection()
{
    m_coveSection = new QWidget;
    auto *layout = new QVBoxLayout(m_coveSection);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto *heading = new QLabel(QStringLiteral("Coves"));
    heading->setObjectName("sectionLabel");
    layout->addWidget(heading);

    auto *hint = new QLabel(QString::fromUtf8(
        "Encrypted vaults for plans, passwords and secrets, committed to this repo "
        "(.forkmesh/coves) and versioned by git. Share one password with your team; "
        "enter it below to unlock matching coves. AES-256, quantum-resistant."));
    hint->setObjectName("statusLine");
    hint->setWordWrap(true);
    layout->addWidget(hint);

    m_coveList = new QListWidget;
    m_coveList->setObjectName("coveList");
    m_coveList->setMaximumHeight(150);
    connect(m_coveList, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *item) {
                if (item)
                    openCove(item->data(Qt::UserRole).toString());
            });
    layout->addWidget(m_coveList);

    m_coveEmptyHint = new QLabel(QStringLiteral("No coves in this repository yet."));
    m_coveEmptyHint->setObjectName("statusLine");
    layout->addWidget(m_coveEmptyHint);

    auto *pwRow = new QHBoxLayout;
    m_covePasswordEdit = new QLineEdit;
    m_covePasswordEdit->setEchoMode(QLineEdit::Password);
    m_covePasswordEdit->setPlaceholderText(QStringLiteral("Cove password for this repo"));
    pwRow->addWidget(m_covePasswordEdit, 1);
    m_covePwRevealBtn = new QPushButton(QStringLiteral("Show"));
    m_covePwRevealBtn->setObjectName("covePwReveal");
    m_covePwRevealBtn->setProperty("buttonSize", "sm");
    m_covePwRevealBtn->setCheckable(true);
    m_covePwRevealBtn->setCursor(Qt::PointingHandCursor);
    m_covePwRevealBtn->setToolTip(
        QStringLiteral("Reveal the saved cove password on this source-of-truth node"));
    m_covePwRevealBtn->setVisible(false); // shown only on source-of-truth (rebuild)
    pwRow->addWidget(m_covePwRevealBtn);
    connect(m_covePwRevealBtn, &QPushButton::toggled, this, [this](bool on) {
        if (on && m_covePasswordEdit->text().isEmpty())
            m_covePasswordEdit->setText(rememberedCovePassword(m_repoDetailIndex));
        m_covePasswordEdit->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
        m_covePwRevealBtn->setText(on ? QStringLiteral("Hide") : QStringLiteral("Show"));
    });
    auto *unlockBtn = new QPushButton(QStringLiteral("Unlock"));
    unlockBtn->setProperty("buttonSize", "sm");
    unlockBtn->setCursor(Qt::PointingHandCursor);
    pwRow->addWidget(unlockBtn);
    layout->addLayout(pwRow);

    auto *coveStatus = new QLabel;
    coveStatus->setObjectName(QStringLiteral("coveSectionStatus"));
    coveStatus->setWordWrap(true);
    coveStatus->hide();
    layout->addWidget(coveStatus);

    auto applyPw = [this] {
        applyCovePasswordFromSettings(m_repoDetailIndex, m_covePasswordEdit->text(), true);
    };
    connect(unlockBtn, &QPushButton::clicked, this, applyPw);
    connect(m_covePasswordEdit, &QLineEdit::returnPressed, this, applyPw);

    m_coveAutoOpenCheck =
        new QCheckBox(QStringLiteral("Auto-show unlocked coves without clicking"));
    m_coveAutoOpenCheck->setCursor(Qt::PointingHandCursor);
    connect(m_coveAutoOpenCheck, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(coveRepoSettingsPrefix(m_repoDetailIndex) + "/autoOpen", on);
        rebuildRepoCovesList();
        if (m_treeLoadedForIndex == m_repoDetailIndex)
            loadRepoFileTree();
    });
    layout->addWidget(m_coveAutoOpenCheck);

    auto *actionRow = new QHBoxLayout;
    auto *newBtn = new QPushButton(QStringLiteral("New cove"));
    newBtn->setProperty("buttonSize", "sm");
    newBtn->setCursor(Qt::PointingHandCursor);
    connect(newBtn, &QPushButton::clicked, this,
            [this] { promptCreateCove(m_repoDetailIndex); });
    actionRow->addWidget(newBtn);
    auto *forgetBtn = new QPushButton(QStringLiteral("Forget password"));
    forgetBtn->setProperty("buttonSize", "sm");
    forgetBtn->setCursor(Qt::PointingHandCursor);
    connect(forgetBtn, &QPushButton::clicked, this, [this, coveStatus] {
        QSettings().remove(coveRepoSettingsPrefix(m_repoDetailIndex) + "/password");
        if (m_covePasswordEdit)
            m_covePasswordEdit->clear();
        showLocalNotice(coveStatus,
                        QStringLiteral("Forgot this repo's cove password."));
        rebuildRepoCovesList();
    });
    actionRow->addWidget(forgetBtn);
    actionRow->addStretch();
    layout->addLayout(actionRow);

    return m_coveSection;
}

void MainWindow::rebuildRepoCovesList()
{
    if (!m_coveList)
        return;
    m_coveList->clear();
    const int idx = m_repoDetailIndex;

    if (m_covePwRevealBtn) {
        if (m_covePwRevealBtn->isChecked()) {
            QSignalBlocker block(m_covePwRevealBtn);
            m_covePwRevealBtn->setChecked(false);
            m_covePwRevealBtn->setText(QStringLiteral("Show"));
            if (m_covePasswordEdit)
                m_covePasswordEdit->setEchoMode(QLineEdit::Password);
        }
        m_covePwRevealBtn->setVisible(idx >= 0 && coveStoreForRepo(idx).canWrite());
    }

    if (m_coveAutoOpenCheck) {
        QSignalBlocker block(m_coveAutoOpenCheck);
        m_coveAutoOpenCheck->setChecked(
            idx >= 0 &&
            QSettings().value(coveRepoSettingsPrefix(idx) + "/autoOpen", false).toBool());
    }

    QList<Cove> coves = coveStoreForRepo(idx).listCoves();
    if (m_coveEmptyHint)
        m_coveEmptyHint->setVisible(coves.isEmpty());
    for (Cove &cove : coves) {
        QString pw;
        const bool unlocked = tryUnlockCove(cove, idx, &pw);
        const QString mine =
            cove.createdByMe(&m_profileIdentity) ? QStringLiteral("  (yours)") : QString();
        // A locked cove keeps its name encrypted; show a neutral label until unlock.
        const QString label =
            cove.name.isEmpty() ? QStringLiteral("Locked cove") : cove.name;
        auto *item = new QListWidgetItem(
            QStringLiteral("%1%2").arg(label, mine), m_coveList);
        item->setIcon(QIcon(tintedOcticonPixmap(
            unlocked ? QStringLiteral("unlock") : QStringLiteral("lock"),
            Theme::remapColor(QColor(unlocked ? "#3fb950" : "#8b949e")), 14)));
        item->setData(Qt::UserRole, cove.relPath);
        item->setToolTip(unlocked ? QStringLiteral("Double-click to open")
                                  : QStringLiteral("Locked — enter the password to unlock"));
        if (!unlocked)
            item->setForeground(QBrush(Theme::remapColor(QColor("#8b949e"))));
    }
}

void MainWindow::applyCovePasswordFromSettings(int repoIndex, const QString &password,
                                               bool remember)
{
    if (password.isEmpty()) {
        showLocalNotice(coveSectionStatus(m_coveSection),
                        QStringLiteral("Enter a cove password first."), true);
        return;
    }
    if (remember)
        QSettings().setValue(coveRepoSettingsPrefix(repoIndex) + "/password", password);

    int unlocked = 0;
    const QList<Cove> coves = coveStoreForRepo(repoIndex).listCoves();
    for (Cove cove : coves) {
        if (CoveStore::unlock(cove, password)) {
            m_coveSessionPasswords.insert(cove.id, password);
            ++unlocked;
        }
    }
    if (unlocked > 0)
        showLocalNotice(coveSectionStatus(m_coveSection),
                        QStringLiteral("Unlocked %1 cove%2.")
                            .arg(unlocked)
                            .arg(unlocked == 1 ? QString() : QStringLiteral("s")));
    else
        showLocalNotice(coveSectionStatus(m_coveSection),
                        QStringLiteral("That password didn't match any cove here."),
                        true);
    rebuildRepoCovesList();
    if (m_treeLoadedForIndex == m_repoDetailIndex)
        loadRepoFileTree();
}
