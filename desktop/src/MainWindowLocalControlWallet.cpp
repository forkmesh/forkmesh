// The "wallet." local control family (WS19): tip and bounty signing requests the owner approves on the phone.
// Registered by registerLocalControlFamilies() (MainWindowLocalControlFamilies.cpp),
// which has already checked that the device belongs to this desktop's owner.

#include "LocalControl.h"
#include "LocalControlHosts.h"
#include "LocalControlWallet.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "TipSigner.h"

#include <QApplication>
#include <QDateTime>
#include <QDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QLineEdit>
#include <QPointer>
#include <QRandomGenerator>
#include <QTimer>

namespace walletctl = forkmesh::localcontrol::wallet;
namespace hostctl = forkmesh::localcontrol::hosts;

namespace {

class WalletControlState : public QObject
{
public:
    explicit WalletControlState(QObject *parent) : QObject(parent)
    {
        setObjectName(QStringLiteral("forkmeshWalletControlState"));
    }

    hostctl::ConfirmationStore confirmations;
    walletctl::PhoneRequest request;
};

WalletControlState *walletControlState(QObject *window)
{
    QObject *found = window->findChild<QObject *>(
        QStringLiteral("forkmeshWalletControlState"), Qt::FindDirectChildrenOnly);
    if (auto *state = dynamic_cast<WalletControlState *>(found))
        return state;
    return new WalletControlState(window);
}

QString walletConfirmationError(hostctl::ConfirmationStore::Result result)
{
    switch (result) {
    case hostctl::ConfirmationStore::Result::Ok:
        return {};
    case hostctl::ConfirmationStore::Result::Expired:
        return QStringLiteral("confirmation_expired");
    case hostctl::ConfirmationStore::Result::Mismatch:
        return QStringLiteral("confirmation_mismatch");
    case hostctl::ConfirmationStore::Result::Missing:
        break;
    }
    return QStringLiteral("confirmation_required");
}

QByteArray walletRandomBytes()
{
    QByteArray bytes(16, '\0');
    for (char &byte : bytes)
        byte = char(QRandomGenerator::system()->bounded(256));
    return bytes;
}

}

void MainWindow::handleLocalControlWalletRequest(int connectionId, int requestId,
                                                 const QString &method,
                                                 const QJsonObject &params)
{
    LocalControlServer *server = m_localControl;
    if (!server)
        return;
    WalletControlState *state = walletControlState(this);
    const LocalControlServer::Device device = server->deviceFor(connectionId);
    const QString holder = walletctl::confirmationHolder(device.key, connectionId);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto fail = [server, connectionId, requestId](const QString &code) {
        server->respondError(connectionId, requestId, code);
    };
    const QString vaultPath = forkmesh::tips::defaultTipVaultPath();
    const QString linkedAddress = forkmesh::tips::linkedTipAddress(vaultPath);
    const quint64 minimum = forkmesh::tips::minimumTipLamports();
    const quint64 maximum = forkmesh::tips::maximumTipLamports();

    const auto payableRewards = [this]() {
        QList<walletctl::RewardFacts> facts;
        for (const auto &record : std::as_const(m_ledgerPayable)) {
            if (!record.payable)
                continue;
            walletctl::RewardFacts one;
            one.rewardId = record.rewardId;
            one.recipient = record.counterpartyName;
            one.address = record.recipientAddress;
            one.lamports = record.amountLamports;
            one.repository = record.repository;
            one.kind = record.sourceKind;
            one.number = record.subjectNumber;
            one.title = record.subjectTitle.isEmpty() ? record.title : record.subjectTitle;
            if (walletctl::rewardPayable(one) &&
                forkmesh::tips::isValidTipAddress(one.address))
                facts.append(one);
        }
        return facts;
    };
    const auto findReward = [&payableRewards](const QString &rewardId,
                                              walletctl::RewardFacts *out) {
        for (const walletctl::RewardFacts &one : payableRewards()) {
            if (one.rewardId == rewardId) {
                *out = one;
                return true;
            }
        }
        return false;
    };

    if (method == QLatin1String("wallet.status")) {
        walletctl::WalletFacts facts;
        facts.linked = !linkedAddress.isEmpty();
        facts.address = linkedAddress;
        facts.busy = m_tipInFlight;
        facts.queued = int(m_tipTransferQueue.size());
        facts.minLamports = minimum;
        facts.maxLamports = maximum;
        server->respond(connectionId, requestId,
                        walletctl::statusPayload(facts, state->request, now));
        return;
    }

    if (method == QLatin1String("wallet.rewards")) {
        server->respond(connectionId, requestId,
                        {{QStringLiteral("rewards"), walletctl::rewardRows(payableRewards())}});
        return;
    }

    const QString action = params.value(QStringLiteral("action")).toString();
    QString target;
    QString summary;
    QString recipient;
    QString address;
    quint64 lamports = 0;
    QString rewardId;
    if (method == QLatin1String("wallet.prepare") ||
        method == QLatin1String("wallet.request")) {
        if (!walletctl::isKnownAction(action)) {
            fail(QStringLiteral("unknown_action"));
            return;
        }
        if (linkedAddress.isEmpty()) {
            fail(QStringLiteral("no_wallet"));
            return;
        }
        if (action == QLatin1String("tip")) {
            recipient = params.value(QStringLiteral("recipient")).toString().trimmed().toLower();
            address = params.value(QStringLiteral("address")).toString().trimmed();
            if (!walletctl::validRecipient(recipient)) {
                fail(QStringLiteral("bad_request"));
                return;
            }
            if (!walletctl::plausibleAddress(address) ||
                !forkmesh::tips::isValidTipAddress(address)) {
                fail(QStringLiteral("bad_address"));
                return;
            }
            if (!walletctl::parseLamports(params.value(QStringLiteral("lamports")), &lamports) ||
                !walletctl::lamportsInRange(lamports, minimum, maximum)) {
                fail(QStringLiteral("bad_amount"));
                return;
            }
            target = walletctl::tipTarget(recipient, address, lamports);
            summary = walletctl::tipSummary(recipient, address, lamports);
        } else {
            rewardId = params.value(QStringLiteral("rewardId")).toString();
            walletctl::RewardFacts reward;
            if (!walletctl::validRewardId(rewardId) || !findReward(rewardId, &reward)) {
                fail(QStringLiteral("unknown_reward"));
                return;
            }
            recipient = reward.recipient;
            address = reward.address;
            lamports = reward.lamports;
            target = walletctl::rewardTarget(rewardId);
            summary = walletctl::rewardSummary(recipient, lamports,
                                               walletctl::rewardSubject(reward));
        }
        if (walletctl::requestBlocks(state->request, now)) {
            fail(QStringLiteral("wallet_busy"));
            return;
        }
    }

    if (method == QLatin1String("wallet.prepare")) {
        const QString token = state->confirmations.issue(holder, action, target, now);
        server->respond(connectionId, requestId,
                        {{QStringLiteral("token"), token},
                         {QStringLiteral("action"), action},
                         {QStringLiteral("summary"), summary},
                         {QStringLiteral("expiresInMs"),
                          int(hostctl::ConfirmationStore::kLifetimeMs)}});
        return;
    }

    if (method == QLatin1String("wallet.request")) {
        const auto result = state->confirmations.consume(
            params.value(QStringLiteral("token")).toString(), holder, action, target, now);
        if (result != hostctl::ConfirmationStore::Result::Ok) {
            fail(walletConfirmationError(result));
            return;
        }
        walletctl::PhoneRequest request;
        request.id = walletctl::newRequestId(walletRandomBytes());
        request.action = action;
        request.summary = summary;
        request.status = QStringLiteral("awaiting_owner");
        request.requestedAtMs = now;
        state->request = request;
        logSystem(QStringLiteral("A %1 payment of %2 to @%3 was requested by %4. "
                                 "Review it on this desktop to sign it.")
                      .arg(action, walletctl::solText(lamports), recipient,
                           localControlDeviceLabel(connectionId)));
        server->respond(connectionId, requestId, walletctl::requestRow(request));
        server->broadcast(QStringLiteral("wallet.changed"),
                          {{QStringLiteral("what"), QStringLiteral("request")},
                           {QStringLiteral("status"), request.status}});
        const QString requestKey = request.id;
        QPointer<WalletControlState> guard(state);
        QTimer::singleShot(0, this, [this, guard, requestKey, action, recipient, address,
                                     lamports, rewardId] {
            if (!guard || guard->request.id != requestKey)
                return;
            showNormal();
            raise();
            activateWindow();
            const int queuedBefore = int(m_tipTransferQueue.size());
            const bool inFlightBefore = m_tipInFlight;
            if (action == QLatin1String("tip")) {
                QPointer<MainWindow> self(this);
                const QString amount = walletctl::solText(lamports).remove(QStringLiteral(" SOL"));
                QTimer::singleShot(0, this, [self, amount] {
                    if (!self)
                        return;
                    for (QDialog *dialog : self->findChildren<QDialog *>(QStringLiteral("sendTipDialog"))) {
                        if (auto *edit = dialog->findChild<QLineEdit *>(QStringLiteral("tipAmountInput")))
                            edit->setText(amount);
                    }
                });
                sendTipToMember(recipient, address);
            } else {
                sendTipToMember(recipient, address, lamports, rewardId, false);
            }
            if (!guard || guard->request.id != requestKey)
                return;
            const bool started = (!inFlightBefore && m_tipInFlight) ||
                                 int(m_tipTransferQueue.size()) > queuedBefore;
            guard->request.status = started ? QStringLiteral("approved")
                                            : QStringLiteral("declined");
            if (m_localControl)
                m_localControl->broadcast(QStringLiteral("wallet.changed"),
                                          {{QStringLiteral("what"), QStringLiteral("request")},
                                           {QStringLiteral("status"), guard->request.status}});
        });
        return;
    }

    fail(QStringLiteral("unknown_method"));
}
