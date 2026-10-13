#include "ActionStore.h"
#include "AppPaths.h"
#include "DesktopCommands.h"
#include "LocalControl.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QCheckBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QStackedWidget>
#include <QTimer>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <optional>

using namespace forkmesh::ui;

namespace {

namespace dc = forkmesh::desktopcommands;

constexpr int kMaxHostedRepos = 200;
constexpr int kMaxTrustedKeys = 32;
constexpr int kMaxQueuedPrompts = 50;
constexpr qint64 kSweepFloorMs = 10LL * 60 * 1000;
constexpr qint64 kMaxAuditBytes = 256 * 1024;

struct PendingPrompt {
    QString owner;
    QString name;
    dc::Command command;
    dc::Verdict verdict;
};

class DesktopCommandState : public QObject
{
public:
    explicit DesktopCommandState(QObject *parent) : QObject(parent)
    {
        setObjectName(QStringLiteral("forkmeshDesktopCommandState"));
        load();
    }

    QString directory() const
    {
        return forkmesh::paths::dataRoot() + QStringLiteral("/desktop_commands");
    }

    void load()
    {
        QFile file(directory() + QStringLiteral("/state.json"));
        if (!file.open(QIODevice::ReadOnly))
            return;
        const QJsonObject root = QJsonDocument::fromJson(file.read(4 * 1024 * 1024)).object();
        ledger = dc::ReplayLedger::fromJson(root.value(QStringLiteral("ledger")).toObject());
        for (const QJsonValue &value : root.value(QStringLiteral("trustedKeys")).toArray()) {
            const QJsonObject row = value.toObject();
            const QString key = row.value(QStringLiteral("key")).toString();
            if (dc::keyFingerprint(key).isEmpty() || trusted.size() >= kMaxTrustedKeys)
                continue;
            trusted.append({key, row.value(QStringLiteral("label")).toString().left(120)});
        }
    }

    void save()
    {
        ledger.prune(QDateTime::currentMSecsSinceEpoch());
        QJsonArray keys;
        for (const dc::TrustedKey &key : std::as_const(trusted))
            keys.append(QJsonObject{{QStringLiteral("key"), key.key},
                                    {QStringLiteral("label"), key.label}});
        QDir().mkpath(directory());
        QSaveFile file(directory() + QStringLiteral("/state.json"));
        if (!file.open(QIODevice::WriteOnly))
            return;
        file.write(QJsonDocument(QJsonObject{{QStringLiteral("ledger"), ledger.toJson()},
                                             {QStringLiteral("trustedKeys"), keys}})
                       .toJson(QJsonDocument::Compact));
        file.commit();
    }

    void audit(const QJsonObject &record)
    {
        QDir().mkpath(directory());
        const QString path = directory() + QStringLiteral("/audit.jsonl");
        if (QFileInfo(path).size() > kMaxAuditBytes) {
            QFile::remove(path + QStringLiteral(".1"));
            QFile::rename(path, path + QStringLiteral(".1"));
        }
        QFile file(path);
        if (!file.open(QIODevice::Append | QIODevice::WriteOnly))
            return;
        file.write(QJsonDocument(record).toJson(QJsonDocument::Compact) + '\n');
    }

    bool isQueued(const QString &sig) const
    {
        if (current && current->command.sig == sig)
            return true;
        for (const PendingPrompt &prompt : queue)
            if (prompt.command.sig == sig)
                return true;
        return false;
    }

    void trust(const QString &key, const QString &label)
    {
        for (const dc::TrustedKey &existing : std::as_const(trusted))
            if (existing.key == key)
                return;
        if (trusted.size() >= kMaxTrustedKeys)
            trusted.removeFirst();
        trusted.append({key, label});
    }

    dc::ReplayLedger ledger;
    QList<dc::TrustedKey> trusted;
    QList<PendingPrompt> queue;
    std::optional<PendingPrompt> current;
    QPointer<QDialog> dialog;
};

DesktopCommandState *commandState(QObject *window)
{
    QObject *found = window->findChild<QObject *>(
        QStringLiteral("forkmeshDesktopCommandState"), Qt::FindDirectChildrenOnly);
    if (auto *state = dynamic_cast<DesktopCommandState *>(found))
        return state;
    return new DesktopCommandState(window);
}

QString pairedDeviceLabel(const LocalControlServer::Device &device)
{
    QString label = device.name.isEmpty()
                        ? QStringLiteral("Your phone")
                        : QStringLiteral("Your phone “%1”").arg(device.name);
    if (!device.user.isEmpty())
        label += QStringLiteral(" (%1)").arg(device.user);
    return label;
}

const PullRequest *pullNumbered(const QList<PullRequest> &pulls, int number)
{
    for (const PullRequest &pull : pulls)
        if (pull.number == number)
            return &pull;
    return nullptr;
}

} // namespace

void MainWindow::onDesktopCommandsEvent(const QString &eventRepo)
{
    const QString owner = eventRepo.section(QLatin1Char('/'), 0, 0).trimmed();
    const QString name = eventRepo.section(QLatin1Char('/'), 1, 1).trimmed();
    if (owner.isEmpty() || name.isEmpty()) {
        drainAllDesktopCommands();
        return;
    }
    const QString account = accountOwner().trimmed().toLower();
    for (const RepositoryRecord &repo : std::as_const(m_repositories)) {
        if (repo.previewOnly || repo.localPath.trimmed().isEmpty())
            continue;
        const QString repoOwner = repoSegment(repo.owner, QStringLiteral("owner"));
        const QString repoName = repoSegment(repo.name, QStringLiteral("repository"));
        if (repoName.compare(name, Qt::CaseInsensitive) != 0 ||
            catalogOwner(repo).trimmed().toLower() != account)
            continue;
        if (repoOwner.compare(owner, Qt::CaseInsensitive) != 0 &&
            account != owner.toLower())
            continue;
        fetchDesktopCommands(account, repoName);
        return;
    }
}

void MainWindow::drainAllDesktopCommands()
{
    if (m_desktopCommandsUnsupported || !hasOwnerSigningCapability())
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_desktopCommandsSweptAtMs > 0 && now - m_desktopCommandsSweptAtMs < kSweepFloorMs)
        return;
    m_desktopCommandsSweptAtMs = now;
    const QString account = accountOwner().trimmed().toLower();
    if (account.isEmpty())
        return;
    QSet<QString> seen;
    for (const RepositoryRecord &repo : std::as_const(m_repositories)) {
        if (seen.size() >= kMaxHostedRepos)
            break;
        if (repo.previewOnly || repo.localPath.trimmed().isEmpty() ||
            catalogOwner(repo).trimmed().toLower() != account)
            continue;
        const QString name = repoSegment(repo.name, QStringLiteral("repository"));
        const QString key = account + QLatin1Char('/') + name.toLower();
        if (seen.contains(key))
            continue;
        seen.insert(key);
        fetchDesktopCommands(account, name);
    }
}

void MainWindow::fetchDesktopCommands(const QString &owner, const QString &name)
{
    if (m_desktopCommandsUnsupported || !m_networkAccess || !hasOwnerSigningCapability())
        return;
    const QString key = owner.toLower() + QLatin1Char('/') + name.toLower();
    if (m_desktopCommandsFetching.contains(key))
        return;
    const QUrlQuery query = signedInboxQuery(owner);
    if (query.isEmpty())
        return;
    QUrl url = catalogApiUrl();
    url.setPath(QStringLiteral("/api/repo/%1/%2/desktop-commands").arg(owner, name));
    url.setQuery(query);
    m_desktopCommandsFetching.insert(key);
    QNetworkReply *reply = m_networkAccess->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply, owner, name, key] {
        reply->deleteLater();
        m_desktopCommandsFetching.remove(key);
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 404) {
            m_desktopCommandsUnsupported = true;
            return;
        }
        if (reply->error() != QNetworkReply::NoError)
            return;
        const QJsonObject payload = QJsonDocument::fromJson(reply->read(1024 * 1024)).object();
        if (!payload.value(QStringLiteral("ok")).toBool())
            return;
        handleDesktopCommandsDrain(owner, name, payload);
    });
}

QList<forkmesh::desktopcommands::TrustedKey> MainWindow::desktopCommandTrustedKeys()
{
    QList<dc::TrustedKey> keys;
    const QString own = m_profileIdentity.publicKey();
    if (!own.isEmpty())
        keys.append({own, QStringLiteral("This desktop (%1)").arg(machineNodeName())});
    const QString account = accountOwner();
    const QString primary =
        cachedAccountRecord(account).value(QStringLiteral("pubkey")).toString();
    if (!primary.isEmpty() && primary != own)
        keys.append({primary, QStringLiteral("Your account key (%1)").arg(account)});
    if (ensureLocalControlServer() && m_localControl) {
        for (const LocalControlServer::Device &device : m_localControl->pairedDevices())
            keys.append({device.key, pairedDeviceLabel(device)});
    }
    keys.append(commandState(this)->trusted);
    return keys;
}

void MainWindow::handleDesktopCommandsDrain(const QString &owner, const QString &name,
                                            const QJsonObject &payload)
{
    DesktopCommandState *state = commandState(this);
    const QList<dc::Command> commands = dc::parseDrain(payload);
    if (commands.isEmpty())
        return;
    const QString account = accountOwner();
    const QList<dc::TrustedKey> trusted = desktopCommandTrustedKeys();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const dc::Command &command : commands) {
        if (state->isQueued(command.sig))
            continue;
        const dc::Verdict verdict =
            dc::verify(command, owner, name, account, trusted, state->ledger, now);
        if (verdict.status == dc::Status::Ok && dc::runsUnattended(command.kind) &&
            QSettings().value(kPhoneReleasesWithoutPromptSetting, true).toBool()) {
            state->ledger.record(command, dc::Outcome::Started, now);
            state->save();
            notifyIfInactive(QStringLiteral("ForkMesh — request from your phone"),
                             dc::actionLabel(command) +
                                 QStringLiteral(" in %1/%2").arg(owner, name));
            runDesktopCommand(owner, name, command, verdict.signerLabel);
            continue;
        }
        if (verdict.status == dc::Status::Ok ||
            verdict.status == dc::Status::UnrecognizedSigner) {
            if (state->queue.size() >= kMaxQueuedPrompts)
                break;
            state->queue.append({owner, name, command, verdict});
            continue;
        }
        if (verdict.status == dc::Status::Replayed) {
            bool found = false;
            const dc::Outcome previous = state->ledger.outcomeFor(command.sig, &found);
            settleDesktopCommand(owner, name, command, QString(),
                                 int(found ? previous : dc::Outcome::Rejected),
                                 dc::statusText(verdict.status));
            continue;
        }
        logSystem(QStringLiteral("Refused a desktop request for %1/%2 (%3): %4.")
                      .arg(owner, name, command.command.left(64),
                           dc::statusText(verdict.status)));
        settleDesktopCommand(owner, name, command, QString(), int(dc::Outcome::Rejected),
                             dc::statusCode(verdict.status));
    }
    showNextDesktopCommandPrompt();
}

void MainWindow::showNextDesktopCommandPrompt()
{
    DesktopCommandState *state = commandState(this);
    if (state->current || state->queue.isEmpty())
        return;
    state->current = state->queue.takeFirst();
    const PendingPrompt &prompt = *state->current;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName(QStringLiteral("desktopCommandPrompt"));
    dialog->setWindowTitle(QStringLiteral("Request from your phone"));
    auto *layout = new QVBoxLayout(dialog);
    auto *text = new QLabel(
        dc::promptText(prompt.command, prompt.owner, prompt.name, prompt.verdict, now));
    text->setTextFormat(Qt::PlainText);
    text->setWordWrap(true);
    text->setMinimumWidth(420);
    layout->addWidget(text);
    QCheckBox *trustBox = nullptr;
    if (prompt.verdict.status == dc::Status::UnrecognizedSigner) {
        trustBox = new QCheckBox(
            QStringLiteral("This is my device. Trust its key for later requests."));
        layout->addWidget(trustBox);
    }
    auto *buttons = new QDialogButtonBox;
    QPushButton *approve =
        buttons->addButton(QStringLiteral("Approve"), QDialogButtonBox::AcceptRole);
    buttons->addButton(QStringLiteral("Decline"), QDialogButtonBox::RejectRole);
    layout->addWidget(buttons);
    if (trustBox) {
        approve->setEnabled(false);
        connect(trustBox, &QCheckBox::toggled, approve, &QPushButton::setEnabled);
    }
    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(dialog, &QDialog::finished, this,
            [this, trustBox = QPointer<QCheckBox>(trustBox)](int result) {
                const bool trust = trustBox && trustBox->isChecked();
                QTimer::singleShot(0, this, [this, result, trust] {
                    finishDesktopCommandPrompt(result == QDialog::Accepted, trust);
                });
            });
    state->dialog = dialog;
    notifyIfInactive(QStringLiteral("ForkMesh — request from your phone"),
                     dc::actionLabel(prompt.command) +
                         QStringLiteral(" in %1/%2").arg(prompt.owner, prompt.name));
    dialog->open();
    dialog->raise();
    dialog->activateWindow();
}

void MainWindow::finishDesktopCommandPrompt(bool approved, bool trustSigner)
{
    DesktopCommandState *state = commandState(this);
    if (!state->current)
        return;
    const PendingPrompt prompt = *state->current;
    state->current.reset();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QString account = accountOwner();
    const dc::Verdict recheck = dc::verify(prompt.command, prompt.owner, prompt.name, account,
                                           desktopCommandTrustedKeys(), state->ledger, now);
    const bool stillValid = recheck.status == dc::Status::Ok ||
                            (recheck.status == dc::Status::UnrecognizedSigner && trustSigner);
    if (!approved) {
        settleDesktopCommand(prompt.owner, prompt.name, prompt.command,
                             prompt.verdict.signerLabel, int(dc::Outcome::Declined),
                             QStringLiteral("declined on the desktop"));
    } else if (!stillValid) {
        settleDesktopCommand(prompt.owner, prompt.name, prompt.command,
                             prompt.verdict.signerLabel, int(dc::Outcome::Rejected),
                             dc::statusCode(recheck.status));
    } else {
        QString label = recheck.signerLabel;
        if (recheck.status == dc::Status::UnrecognizedSigner) {
            label = QStringLiteral("Device %1, approved %2")
                        .arg(dc::keyFingerprint(recheck.signerKey),
                             QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd")));
            state->trust(recheck.signerKey, label);
        }
        state->ledger.record(prompt.command, dc::Outcome::Started, now);
        state->save();
        runDesktopCommand(prompt.owner, prompt.name, prompt.command, label);
    }
    QTimer::singleShot(0, this, [this] { showNextDesktopCommandPrompt(); });
}

QString MainWindow::desktopCommandRepoOwner(const QString &owner, const QString &name) const
{
    QString aliased;
    for (const RepositoryRecord &repo : std::as_const(m_repositories)) {
        if (repo.previewOnly || repo.localPath.trimmed().isEmpty() ||
            repoSegment(repo.name, QStringLiteral("repository"))
                    .compare(name, Qt::CaseInsensitive) != 0)
            continue;
        const QString repoOwner = repoSegment(repo.owner, QStringLiteral("owner"));
        if (repoOwner.compare(owner, Qt::CaseInsensitive) == 0)
            return repoOwner;
        if (aliased.isEmpty() &&
            catalogOwner(repo).trimmed().compare(owner, Qt::CaseInsensitive) == 0)
            aliased = repoOwner;
    }
    return aliased.isEmpty() ? owner : aliased;
}

void MainWindow::runDesktopCommand(const QString &owner, const QString &name,
                                   const forkmesh::desktopcommands::Command &command,
                                   const QString &signerLabel)
{
    logSystem(QStringLiteral("Running a request from %1: %2 in %3/%4.")
                  .arg(signerLabel, dc::actionLabel(command), owner, name));
    const auto settle = [&](dc::Outcome outcome, const QString &detail) {
        settleDesktopCommand(owner, name, command, signerLabel, int(outcome), detail);
    };
    const QString localOwner = desktopCommandRepoOwner(owner, name);
    if (dc::runsUnattended(command.kind)) {
        const bool create = command.kind == dc::Kind::ReleaseCreate;
        forkmesh::agentintake::ReleaseRequest request;
        request.method = create ? forkmesh::agentintake::ReleaseCreateMethod
                                : forkmesh::agentintake::ReleasePublishMethod;
        request.owner = localOwner;
        request.name = name;
        request.tag = command.tag;
        const auto finished = [this, owner, name, command, signerLabel,
                               create](const QJsonObject &reply) {
            const bool ok = reply.value(QStringLiteral("ok")).toBool();
            dc::Outcome outcome = ok ? (create ? dc::Outcome::Done : dc::Outcome::Started)
                                     : dc::Outcome::Failed;
            QString detail = reply.value(ok ? QStringLiteral("detail")
                                            : QStringLiteral("message"))
                                 .toString();
            if (!ok && reply.value(QStringLiteral("error")).toString() ==
                           QLatin1String("tag_exists")) {
                outcome = dc::Outcome::Done;
                detail = QStringLiteral("release already exists");
            }
            settleDesktopCommand(owner, name, command, signerLabel, int(outcome), detail);
        };
        if (create)
            createReleaseRemotely(request, finished);
        else
            publishReleaseRemotely(request, finished);
        return;
    }

    const int index = repoIndexFor(localOwner, name);
    if (index < 0 || !bindRepoDetailToRepo(index)) {
        settle(dc::Outcome::Failed, QStringLiteral("repository is not open on this desktop"));
        return;
    }

    if (dc::isPullKind(command.kind)) {
        const int number = command.pullNumber;
        switchToPullTab(number);
        if (!pullNumbered(m_currentPulls, number) || m_currentPullNumber != number) {
            settle(dc::Outcome::Failed, QStringLiteral("pull request not found"));
            return;
        }
        const QString before = pullNumbered(m_currentPulls, number)->status;
        switch (command.kind) {
        case dc::Kind::PullMerge:
            mergeCurrentPull();
            break;
        case dc::Kind::PullMergeDelete:
            mergeAndDeleteCurrentPull();
            break;
        case dc::Kind::PullFixAgent:
            fixCurrentPullWithAgent();
            settle(dc::Outcome::Started, QStringLiteral("agent started"));
            return;
        case dc::Kind::PullClose:
            closeCurrentPull();
            break;
        case dc::Kind::PullReopen:
            reopenCurrentPull();
            break;
        case dc::Kind::PullDelete:
            deleteCurrentPull();
            break;
        default:
            break;
        }
        if (command.kind == dc::Kind::PullDelete ||
            command.kind == dc::Kind::PullMergeDelete) {
            const bool running = m_pullDeleteInProgress;
            settle(running ? dc::Outcome::Started : dc::Outcome::Failed,
                   running ? QStringLiteral("deletion running")
                           : QStringLiteral("not deleted on the desktop"));
            return;
        }
        reloadPulls();
        const PullRequest *after = pullNumbered(m_currentPulls, number);
        const QString wanted = command.kind == dc::Kind::PullMerge   ? QStringLiteral("merged")
                               : command.kind == dc::Kind::PullClose ? QStringLiteral("closed")
                                                                     : QStringLiteral("open");
        const bool reached = after && after->status == wanted;
        settle(reached ? dc::Outcome::Done : dc::Outcome::Failed,
               reached ? QStringLiteral("%1 -> %2").arg(before, wanted)
                       : QStringLiteral("pull request is still %1")
                             .arg(after ? after->status : QStringLiteral("missing")));
        return;
    }

    if (command.kind == dc::Kind::ActionRerun) {
        bool ok = false;
        const int runId = command.runId.toInt(&ok);
        const ActionRun *run = ok ? findRun(runId) : nullptr;
        if (!run || run->owner.compare(localOwner, Qt::CaseInsensitive) != 0 ||
            run->name.compare(name, Qt::CaseInsensitive) != 0) {
            settle(dc::Outcome::Failed, QStringLiteral("run not found in this repository"));
            return;
        }
        openActionRunFromNotification(runId);
        rerunActionRun(runId);
        settle(dc::Outcome::Started, QStringLiteral("rerun handed to Actions"));
        return;
    }

    if (dc::isReleaseKind(command.kind)) {
        if (m_releasesTabIndex >= 0) {
            ensureRepoDetailTabBuilt(m_releasesTabIndex);
            if (m_repoDetailTabs && m_repoDetailTabs->button(m_releasesTabIndex))
                m_repoDetailTabs->button(m_releasesTabIndex)->setChecked(true);
            if (m_repoDetailStack)
                m_repoDetailStack->setCurrentIndex(m_releasesTabIndex);
            loadReleasesPanel();
        }
        const QString dir = repoGitDir();
        const QString tag = command.tag;
        runOffThread<bool>(
            [dir, tag] {
                return !dir.isEmpty() &&
                       runGitCapture(dir, {QStringLiteral("rev-parse"),
                                           QStringLiteral("--verify"),
                                           QStringLiteral("--quiet"),
                                           QStringLiteral("refs/tags/") + tag},
                                     nullptr, nullptr);
            },
            [this, owner, name, command, signerLabel, dir](bool tagExists) {
                const auto settleLater = [&](dc::Outcome outcome, const QString &detail) {
                    settleDesktopCommand(owner, name, command, signerLabel,
                                         int(outcome), detail);
                };
                if (repoIndexFor(owner, name) != m_repoDetailIndex ||
                    repoGitDir() != dir) {
                    settleLater(dc::Outcome::Failed,
                                QStringLiteral("repository is no longer open on "
                                               "this desktop"));
                    return;
                }
                if (tagExists) {
                    showReleaseDetail(command.tag);
                    settleLater(dc::Outcome::Done,
                                QStringLiteral("release already exists"));
                    return;
                }
                m_desktopCommandReleaseTag = command.tag;
                promptNewReleaseThen(
                    [this, owner, name, command, signerLabel](bool created) {
                        loadReleasesPanel();
                        settleDesktopCommand(
                            owner, name, command, signerLabel,
                            int(created ? dc::Outcome::Done : dc::Outcome::Failed),
                            created ? QStringLiteral("release created")
                                    : QStringLiteral(
                                          "release dialog closed without a release"));
                    });
                m_desktopCommandReleaseTag.clear();
            });
        return;
    }

    settle(dc::Outcome::Rejected, QStringLiteral("unsupported_command"));
}

void MainWindow::settleDesktopCommand(const QString &owner, const QString &name,
                                      const forkmesh::desktopcommands::Command &command,
                                      const QString &signerLabel, int outcome,
                                      const QString &detail)
{
    DesktopCommandState *state = commandState(this);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto result = static_cast<dc::Outcome>(outcome);
    if (!command.sig.isEmpty()) {
        state->ledger.record(command, result, now);
        state->save();
    }
    state->audit(dc::auditRecord(command, owner, name, signerLabel, result, detail, now));
    if (result != dc::Outcome::Rejected)
        logSystem(QStringLiteral("Phone request %1 in %2/%3: %4 (%5).")
                      .arg(dc::actionLabel(command), owner, name,
                           dc::outcomeCode(result), detail));
    if (!m_networkAccess || command.id <= 0 || !hasOwnerSigningCapability())
        return;
    const QUrlQuery query = signedInboxQuery(owner);
    if (query.isEmpty())
        return;
    QUrl url = catalogApiUrl();
    url.setPath(QStringLiteral("/api/repo/%1/%2/desktop-commands/ack").arg(owner, name));
    url.setQuery(query);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    const QJsonObject body = dc::ackBody({command.id}, {{command.id, result}});
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
}
