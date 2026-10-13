// Member-to-member tips.
//
// ForkMesh is not in the middle of this transfer and cannot be: the sender's
// Solana key is imported once into a passphrase-encrypted, owner-only vault on
// this machine, this desktop builds and signs exactly one System Program
// transfer, and it broadcasts that transfer straight to a public JSON-RPC
// endpoint. Only afterwards does it tell the relay the resulting *public* chain
// signature, so the relay can email the recipient that a tip is on the way.
// The passphrase lives in memory for one signature and the vault never leaves
// this disk.

#include "MainWindow.h"

#include "ContributorPayoutReceipt.h"
#include "MainWindowInternal.h"
#include "RewardPoolSigner.h"
#include "TipSigner.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSettings>
#include <QTableWidget>
#include <QTimer>
#include <QUrlQuery>

#include <algorithm>

using namespace forkmesh::ui;

namespace {

// The same keyless public endpoints the profile balance chip already uses.
const char *kTipRpcEndpoints[] = {
    "https://api.mainnet-beta.solana.com",
    "https://solana-rpc.publicnode.com",
};
constexpr int kTipRpcEndpointCount =
    int(sizeof(kTipRpcEndpoints) / sizeof(kTipRpcEndpoints[0]));
constexpr int kMaximumTipResponseBytes = 256 * 1024;
constexpr int kTipKeyMaterialMaxBytes = 4096;
constexpr int kContributorReceiptAttemptLimit = 36;
constexpr int kPendingReceiptStartupAuthPollMs = 250;
const QString kPendingContributorRecipientSetting =
    ContributorPayoutReceipt::recipientKey();
const QString kPendingContributorLamportsSetting =
    ContributorPayoutReceipt::lamportsKey();
const QString kPendingContributorSignatureSetting =
    ContributorPayoutReceipt::signatureKey();
const QString kPendingContributorSourceSetting =
    ContributorPayoutReceipt::sourceAddressKey();
const QString kPendingContributorRewardSetting =
    ContributorPayoutReceipt::rewardIdKey();
const QString kPendingContributorEndpointSetting =
    ContributorPayoutReceipt::endpointKey();
constexpr char kSolUsdPriceEndpoint[] =
    "https://api.coingecko.com/api/v3/simple/price"
    "?ids=solana&vs_currencies=usd";

struct TipDialogLayout {
    QVBoxLayout *outer = nullptr;
    QVBoxLayout *body = nullptr;
    QScrollArea *scroll = nullptr;
};

// Tip dialogs contain deliberately detailed safety copy and can gain another
// paragraph when validation runs or a wallet is generated. Keep that content
// scrollable and the final action pinned in place: on a short screen Qt would
// otherwise compress styled inputs and buttons until their painted bounds
// overlap, making the later states of the flow unusable.
TipDialogLayout makeTipDialogLayout(QDialog *dialog,
                                    const QSize &preferredSize)
{
    dialog->resize(preferredSize);
    dialog->setSizeGripEnabled(true);

    auto *outer = new QVBoxLayout(dialog);
    outer->setContentsMargins(20, 20, 20, 16);
    outer->setSpacing(14);

    auto *scroll = new QScrollArea(dialog);
    scroll->setObjectName(QStringLiteral("tipDialogScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto *bodyWidget = new QWidget(scroll);
    bodyWidget->setObjectName(QStringLiteral("tipDialogBody"));
    auto *body = new QVBoxLayout(bodyWidget);
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(12);
    scroll->setWidget(bodyWidget);
    outer->addWidget(scroll, 1);
    return {outer, body, scroll};
}

QLabel *tipDialogSectionTitle(const QString &text)
{
    auto *label = new QLabel(text);
    QFont font = label->font();
    font.setBold(true);
    label->setFont(font);
    return label;
}

void polishTipForm(QFormLayout *form)
{
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setHorizontalSpacing(16);
    form->setVerticalSpacing(12);
}

void polishTipDialogButtons(QDialogButtonBox *buttons,
                            QDialogButtonBox::StandardButton primary =
                                QDialogButtonBox::NoButton)
{
    buttons->setObjectName(QStringLiteral("tipDialogButtons"));
    for (QAbstractButton *button : buttons->buttons())
        button->setIcon(QIcon());
    if (QPushButton *button = buttons->button(primary))
        button->setObjectName(QStringLiteral("primaryButton"));
}

void revealTipStatus(QLabel *status, QScrollArea *scroll)
{
    status->setVisible(true);
    // Visibility changes update the scroll range on the next layout pass. Move
    // only after that pass so newly generated guidance and errors are actually
    // brought into view on compact screens.
    QTimer::singleShot(0, status, [status, scroll] {
        scroll->ensureWidgetVisible(status, 0, 12);
    });
}

void hardenTipRequest(QNetworkRequest &request)
{
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("Cache-Control", "no-store");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::SameOriginRedirectPolicy);
    request.setTransferTimeout(15000);
}

QByteArray boundedTipBody(QNetworkReply *reply)
{
    if (!reply)
        return {};
    const QByteArray body = reply->read(kMaximumTipResponseBytes + 1);
    return body.size() <= kMaximumTipResponseBytes ? body : QByteArray();
}

double solUsdSpotPrice(const QByteArray &body)
{
    const double price =
        QJsonDocument::fromJson(body).object()
            .value(QStringLiteral("solana")).toObject()
            .value(QStringLiteral("usd")).toDouble();
    return qIsFinite(price) && price > 0.0 ? price : 0.0;
}

QString tipUsdEstimate(const QString &amount, double solUsdPrice)
{
    quint64 lamports = 0;
    if (solUsdPrice <= 0.0 ||
        !forkmesh::tips::parseSolAmount(amount, &lamports)) {
        return {};
    }
    const double usd = (double(lamports) / 1000000000.0) * solUsdPrice;
    return QString::fromUtf8("\xE2\x89\x88 $%1 USD")
        .arg(QLocale::c().toString(usd, 'f', 2));
}

// A wallet key is the one thing that must never reach a log line, a status
// label, or an error dialog. Everything user-visible in this file goes through
// the reward signer's redactor first.
QString safeTipText(const QString &text)
{
    return forkmesh::rewards::redactRewardText(text);
}

// The one shape a contributor payout may have, checked identically wherever
// one is saved, resumed, settled or re-announced. Each of those either
// suppresses a future payment or authorises a public POST, so nothing
// shapeless may reach any of them.
bool isWellFormedContributorPayout(const QString &recipientName,
                                   quint64 lamports,
                                   const QString &chainSignature,
                                   const QString &sourceAddress,
                                   const QString &rewardId)
{
    static const QRegularExpression accountPattern(
        QStringLiteral("^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$"));
    static const QRegularExpression rewardPattern(
        QStringLiteral("^reward-[a-z0-9-]{8,88}$"));
    return accountPattern.match(recipientName).hasMatch() &&
           rewardPattern.match(rewardId).hasMatch() &&
           forkmesh::tips::isValidTipAddress(sourceAddress) &&
           lamports >= forkmesh::tips::minimumContributorPayoutLamports() &&
           lamports <= forkmesh::tips::maximumContributorPayoutLamports() &&
           forkmesh::rewards::decodeBase58(chainSignature, 64).size() == 64;
}

bool isContributorReceiptEndpoint(const QUrl &endpoint)
{
    return endpoint.isValid() &&
           endpoint.scheme() == QLatin1String("https") &&
           !endpoint.host().isEmpty() && endpoint.userInfo().isEmpty() &&
           endpoint.path() == QLatin1String("/api/accounts/tip-notice") &&
           !endpoint.hasQuery() && endpoint.fragment().isEmpty();
}

// Why a report that is still owed did not land, in the terms a reader can act
// on. Every one of these leaves the entry queued and retried, and none of them
// puts the money at risk: the transfer is final on Solana whatever the relay
// says, and the queued entry is what keeps that reward from being signed again.
QString settledNoticeFailureText(
    const ContributorPayoutReceipt::Settled::Notice &notice, bool networkOk,
    int status)
{
    const QString subject =
        QStringLiteral("@%1 (%2)")
            .arg(notice.recipientName,
                 forkmesh::tips::formatLamports(notice.lamports));
    QString wall;
    if (!networkOk && status == 0) {
        wall = QStringLiteral("ForkMesh could not be reached");
    } else if (status == 401 || status == 403) {
        wall = QStringLiteral(
            "ForkMesh did not accept this desktop's credentials (%1)")
                   .arg(status);
    } else if (status == 202) {
        wall = QStringLiteral(
            "ForkMesh has the report and is still confirming the transfer on "
            "Solana");
    } else if (status >= 400) {
        wall = QStringLiteral("ForkMesh rejected the report (%1)").arg(status);
    } else {
        wall = QStringLiteral(
            "ForkMesh answered without recording the reward as paid");
    }
    return QStringLiteral(
               "%1, so %2 has not been told about the payment yet. The "
               "transfer %3 is already final on Solana and will never be "
               "signed again; the report stays queued and is retried on the "
               "next sign-in, reward-pool run, or press of this button.")
        .arg(wall, subject,
             ContributorPayoutReceipt::shorten(notice.chainSignature));
}

void scrubTipEdit(QLineEdit *edit)
{
    if (!edit)
        return;
    QString overwrite(edit->text().size(), QChar(u'\0'));
    edit->setText(overwrite);
    edit->clear();
    forkmesh::rewards::secureErase(overwrite);
}

// Write an exported keypair the way solana-keygen does: a plain JSON byte
// array, owner-only, created fresh rather than appended to. The permissions are
// set before the bytes land and re-checked afterwards, so a key never sits in a
// world-readable file even briefly.
bool writeTipKeypairFile(const QString &path, const QByteArray &keypairJson,
                         QString *error)
{
    const QFileInfo info(path);
    if (info.isSymLink() || info.isDir()) {
        if (error)
            *error = QStringLiteral("Refusing to write a secret key to that path.");
        return false;
    }
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    const QFileDevice::Permissions ownerOnly =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(ownerOnly) ||
        file.write(keypairJson) != keypairJson.size() || !file.commit()) {
        file.cancelWriting();
        if (error)
            *error = QStringLiteral("The keypair file could not be written.");
        return false;
    }
    const QFileDevice::Permissions forbidden =
        QFileDevice::ReadGroup | QFileDevice::WriteGroup |
        QFileDevice::ExeGroup | QFileDevice::ReadOther |
        QFileDevice::WriteOther | QFileDevice::ExeOther;
    if (!QFile::setPermissions(path, ownerOnly) ||
        (QFileInfo(path).permissions() & forbidden) != 0) {
        QFile::remove(path);
        if (error)
            *error = QStringLiteral(
                "That location cannot hold an owner-only file, so nothing was "
                "written. Save the key somewhere only you can read.");
        return false;
    }
    return true;
}

} // namespace

void MainWindow::tipStatus(const QString &message)
{
    const QString text = safeTipText(message);
    logSystem(text);
    // The whole tip flow — refusals, progress, failures, receipts — speaks
    // only through here, and here is the system log. Someone who pressed Send
    // payment on the Ledger is looking at the Ledger, so a payment started
    // there says the same things on that page. Keeping the last line lets the
    // redraw that re-arms the row put it back afterwards.
    if (m_ledgerPaymentReporting) {
        m_ledgerPaymentLine = text;
        setLedgerStatus(text);
    }
}

bool MainWindow::savePendingContributorReceipt(
    const QString &recipientName, quint64 lamports,
    const QString &chainSignature, const QString &sourceAddress,
    const QString &rewardId, bool automated)
{
    if (!isWellFormedContributorPayout(recipientName, lamports, chainSignature,
                                       sourceAddress, rewardId)) {
        return false;
    }
    // A reward this desktop has already settled on-chain must never be signed
    // again, however the relay still lists it. Refusing the receipt refuses
    // the broadcast that was about to follow it.
    if (ContributorPayoutReceipt::Settled::contains(rewardId))
        return false;
    QSettings settings;
    const QString existingReward =
        settings.value(kPendingContributorRewardSetting).toString();
    const QString existingSignature =
        settings.value(kPendingContributorSignatureSetting).toString();
    if ((!existingReward.isEmpty() || !existingSignature.isEmpty()) &&
        (existingReward != rewardId || existingSignature != chainSignature)) {
        return false;
    }
    const QUrl endpoint =
        automated ? m_contributorPayoutReceiptEndpoint
                  : accountsApiUrl(QStringLiteral("tip-notice"));
    if (!isContributorReceiptEndpoint(endpoint))
        return false;
    settings.setValue(kPendingContributorRecipientSetting, recipientName);
    settings.setValue(kPendingContributorLamportsSetting, qulonglong(lamports));
    settings.setValue(kPendingContributorSignatureSetting, chainSignature);
    settings.setValue(kPendingContributorSourceSetting, sourceAddress);
    settings.setValue(kPendingContributorRewardSetting, rewardId);
    m_contributorPayoutReceiptEndpoint = endpoint;
    settings.setValue(kPendingContributorEndpointSetting,
                      m_contributorPayoutReceiptEndpoint.toString(
                          QUrl::FullyEncoded));
    settings.sync();
    return settings.status() == QSettings::NoError;
}

void MainWindow::clearPendingContributorReceipt(
    const QString &rewardId, const QString &chainSignature)
{
    QSettings settings;
    if (settings.value(kPendingContributorRewardSetting).toString() != rewardId ||
        settings.value(kPendingContributorSignatureSetting).toString() !=
            chainSignature) {
        return;
    }
    settings.remove(ContributorPayoutReceipt::group());
    settings.sync();
    m_contributorPayoutReceiptEndpoint = QUrl();
}

bool MainWindow::prepareContributorNoticeRequest(
    const QUrl &endpoint, const QString &recipientName, quint64 lamports,
    const QString &chainSignature, const QString &rewardId,
    const QString &sourceAddress, QNetworkRequest *request)
{
    if (!request || !isContributorReceiptEndpoint(endpoint))
        return false;
    const QString sessionToken = accountSessionTokenForUrl(endpoint);
    QUrl url = endpoint;
    // Every credential this desktop actually holds, on the one request that
    // decides whether a contributor is ever told they were paid. A desktop that
    // authenticated with this account's key rather than a password holds no
    // session token at all — the ordinary state of a restarted app, and why one
    // plainly showing itself signed in still reported "not signed in" here. A
    // stored token can equally be expired, revoked, or older than a relay that
    // has since learned to read the signed proof. Sending whichever single one
    // this code guessed at makes the notice turn on that guess; sending both
    // costs one query string and lets the relay pick. It reads the session
    // first and falls back to the proof, so nothing here weakens that order.
    //
    // The canonical names this one payout — recipient, amount, chain signature
    // and reward — so the proof authorizes this one notice and no other.
    //
    // The query names the signer as what it is: the user whose account key
    // signs. This machine is one of that user's nodes and has no part in the
    // proof, so the line in the network log reads user=<account>, never
    // node=<account>; the relay reads the triple under that name alone.
    const QString signer = accountOwner().trimmed().toLower();
    if (!signer.isEmpty() && hasOwnerSigningCapability(signer) &&
        (m_profileIdentity.isValid() || m_profileIdentity.load())) {
        const QString ts =
            QString::number(QDateTime::currentMSecsSinceEpoch());
        const bool workPayment = !m_tipTaskDescription.isEmpty();
        const QString canonicalText =
            (workPayment
                 ? QStringLiteral("forkmesh-account-tip-work-payment-v2\n")
                 : QStringLiteral("forkmesh-account-tip-notice-v1\n")) +
            signer + QLatin1Char('\n') + ts + QLatin1Char('\n') +
            recipientName.toLower() + QLatin1Char('\n') +
            QString::number(lamports) + QLatin1Char('\n') + chainSignature +
            QLatin1Char('\n') + rewardId +
            (workPayment
                 ? QLatin1Char('\n') + m_tipTaskDescription +
                       QLatin1Char('\n') +
                       m_tipOutputUrl.toString(QUrl::FullyEncoded) +
                       QLatin1Char('\n') + m_tipEnteredCurrency +
                       QLatin1Char('\n') +
                       QString::number(m_tipEnteredUsdMicros) +
                       QLatin1Char('\n') +
                       QString::number(m_tipSolUsdPriceMicros) +
                       QLatin1Char('\n') +
                       QString::number(m_tipPriceObservedAt) +
                       QLatin1Char('\n') + sourceAddress
                 : QString());
        const QByteArray canonical = canonicalText.toUtf8();
        const QString proof = m_profileIdentity.signData(canonical);
        if (!proof.isEmpty()) {
            QUrlQuery query;
            query.addQueryItem(QStringLiteral("user"), signer);
            query.addQueryItem(QStringLiteral("ts"), ts);
            query.addQueryItem(QStringLiteral("sig"), proof);
            url.setQuery(query);
        }
    }
    if (sessionToken.isEmpty() && !url.hasQuery())
        return false; // no credential of either kind
    *request = QNetworkRequest(url);
    hardenTipRequest(*request);
    if (!sessionToken.isEmpty()) {
        request->setRawHeader(
            "Authorization",
            (QStringLiteral("Bearer ") + sessionToken).toUtf8());
    }
    return true;
}

#ifdef FORKMESH_WINDOW_TESTS
void MainWindow::testWithOwnerSigningAccount(
    const QString &accountName, const std::function<void()> &body)
{
    // Everything hasOwnerSigningCapability() reads, saved and put back, so a
    // check can stand the window up as a signed-in, desktop-capable account
    // and leave nothing behind for the checks that follow it.
    const bool authenticated = m_accountAuthenticated;
    const QString name = m_accountName;
    const QString tier = m_accountTier;
    const bool capable = m_accountDesktopCapable;
    QSettings settings;
    const QVariant markerAccount =
        settings.value(kDesktopCapableAccountSetting);
    const QVariant markerKey = settings.value(kDesktopCapablePublicKeySetting);
    m_accountAuthenticated = true;
    m_accountName = accountName;
    m_accountTier = QStringLiteral("active");
    if (!m_profileIdentity.isValid())
        m_profileIdentity.load();
    setDesktopCapability(accountName, true);
    body();
    m_accountAuthenticated = authenticated;
    m_accountName = name;
    m_accountTier = tier;
    m_accountDesktopCapable = capable;
    if (markerAccount.isValid())
        settings.setValue(kDesktopCapableAccountSetting, markerAccount);
    else
        settings.remove(kDesktopCapableAccountSetting);
    if (markerKey.isValid())
        settings.setValue(kDesktopCapablePublicKeySetting, markerKey);
    else
        settings.remove(kDesktopCapablePublicKeySetting);
}

QUrl MainWindow::testContributorNoticeRequestUrl(
    const QString &recipientName, quint64 lamports,
    const QString &chainSignature, const QString &rewardId)
{
    QNetworkRequest request;
    if (!prepareContributorNoticeRequest(
            accountsApiUrl(QStringLiteral("tip-notice")), recipientName,
            lamports, chainSignature, rewardId, QString(), &request)) {
        return {};
    }
    return request.url();
}
#endif

bool MainWindow::recordSettledContributorPayout(
    const QString &recipientName, quint64 lamports,
    const QString &chainSignature, const QString &sourceAddress,
    const QString &rewardId, const QUrl &endpoint)
{
    if (!isWellFormedContributorPayout(recipientName, lamports, chainSignature,
                                       sourceAddress, rewardId) ||
        !isContributorReceiptEndpoint(endpoint)) {
        return false;
    }
    QSettings settings;
    const QStringList queued =
        ContributorPayoutReceipt::Settled::rewardIds(settings);
    // Settling is what releases the hold, so an unbounded queue of undelivered
    // notices would quietly retire the guard altogether. Past the limit this
    // fails, which leaves the receipt held — the honest answer for a machine
    // that has paid this many rewards without once reaching the relay.
    if (!queued.contains(rewardId) &&
        queued.size() >= ContributorPayoutReceipt::Settled::noticeLimit()) {
        return false;
    }
    using ContributorPayoutReceipt::Settled::key;
    settings.setValue(key(rewardId, QStringLiteral("recipient")), recipientName);
    settings.setValue(key(rewardId, QStringLiteral("lamports")),
                      qulonglong(lamports));
    settings.setValue(key(rewardId, QStringLiteral("signature")), chainSignature);
    settings.setValue(key(rewardId, QStringLiteral("sourceAddress")),
                      sourceAddress);
    settings.setValue(key(rewardId, QStringLiteral("endpoint")),
                      endpoint.toString(QUrl::FullyEncoded));
    settings.sync();
    return settings.status() == QSettings::NoError;
}

void MainWindow::settlePendingContributorPayoutOnChain(
    const QString &recipientName, quint64 lamports,
    const QString &chainSignature, const QString &sourceAddress,
    const QString &rewardId, bool automated, int endpointIndex)
{
    // Everything the relay would have settled, settled without it. The money
    // has already left this wallet; the only open question is whether it
    // arrived, and Solana answers that to anybody. Nothing here signs.
    const auto keepHolding = [this, rewardId, lamports,
                              automated](const QString &detail) {
        tipStatus(detail);
        if (automated) {
            finishContributorPayoutTransfer(false, rewardId, lamports, detail);
        } else {
            m_tipInFlight = false;
            refreshTipWalletControls();
        }
    };
    if (rewardId.isEmpty() ||
        forkmesh::rewards::decodeBase58(chainSignature, 64).size() != 64) {
        keepHolding(QStringLiteral(
            "There is no exact signature to check on Solana, so this payout "
            "stays held. No replacement will be signed."));
        return;
    }
    if (endpointIndex >= kTipRpcEndpointCount || !m_networkAccess) {
        keepHolding(
            QStringLiteral(
                "No public Solana endpoint could confirm %1 as finalized, so "
                "this payout stays held until one does. Nothing will be signed "
                "again for it.")
                .arg(chainSignature));
        return;
    }

    QNetworkRequest request{
        QUrl(QString::fromLatin1(kTipRpcEndpoints[endpointIndex]))};
    hardenTipRequest(request);
    const QJsonObject payload{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), QStringLiteral("forkmesh-payout-status")},
        {QStringLiteral("method"), QStringLiteral("getSignatureStatuses")},
        {QStringLiteral("params"),
         QJsonArray{
             QJsonArray{chainSignature},
             QJsonObject{{QStringLiteral("searchTransactionHistory"), true}}}}};
    tipStatus(QStringLiteral(
                  "ForkMesh cannot be told about %1 right now; reading that "
                  "transfer's status straight from Solana instead")
                  .arg(chainSignature) +
              QString::fromUtf8("\xE2\x80\xA6"));
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, recipientName, lamports, chainSignature,
             sourceAddress, rewardId, automated, endpointIndex, keepHolding] {
        const QByteArray body = boundedTipBody(reply);
        const bool networkOk = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();
        const QJsonObject response = QJsonDocument::fromJson(body).object();
        const QJsonArray values = response.value(QStringLiteral("result"))
                                      .toObject()
                                      .value(QStringLiteral("value"))
                                      .toArray();
        const QJsonValue statusValue =
            values.isEmpty() ? QJsonValue() : values.at(0);
        const QJsonObject status = statusValue.toObject();
        const bool answered =
            networkOk && response.value(QStringLiteral("error")).isUndefined() &&
            statusValue.isObject();
        // Only finality settles anything. A confirmed-but-not-finalized
        // transfer can still be dropped, and that is the ambiguity the hold
        // is for.
        const bool finalized =
            answered && status.value(QStringLiteral("confirmationStatus"))
                                .toString() == QLatin1String("finalized");
        const QJsonValue err = status.value(QStringLiteral("err"));
        const bool failedOnChain =
            finalized && !err.isNull() && !err.isUndefined();

        if (finalized && !failedOnChain) {
            // Same endpoint the receipt was saved against; a settled entry
            // authorises a later POST, so it may not name anywhere else.
            const QUrl endpoint =
                m_contributorPayoutReceiptEndpoint.isValid()
                    ? m_contributorPayoutReceiptEndpoint
                    : accountsApiUrl(QStringLiteral("tip-notice"));
            if (!recordSettledContributorPayout(recipientName, lamports,
                                                chainSignature, sourceAddress,
                                                rewardId, endpoint)) {
                keepHolding(
                    QStringLiteral(
                        "%1 is finalized on Solana, but this desktop could not "
                        "record it as settled, so payouts stay held rather "
                        "than risk paying the same reward twice.")
                        .arg(chainSignature));
                return;
            }
            // The question the hold exists for is answered, so the hold goes.
            // Everything the relay would have done is done where this machine
            // can do it, and the notice it still owes becomes an errand rather
            // than a blockade across every other reward.
            clearPendingContributorReceipt(rewardId, chainSignature);
            recordRewardReceiptLocally(rewardId, chainSignature);
            const QString detail =
                QStringLiteral(
                    "Paid %1 to %2 — %3 is finalized on Solana. ForkMesh has "
                    "not been told yet, so %2 gets no email until this app is "
                    "signed in; the notice is queued and payouts continue.")
                    .arg(forkmesh::tips::formatLamports(lamports),
                         recipientName, chainSignature);
            tipStatus(detail);
            if (automated) {
                finishContributorPayoutTransfer(true, rewardId, lamports,
                                                detail);
            } else {
                m_tipInFlight = false;
                refreshTipWalletControls();
                fetchContributorRewards();
                refreshLedgerPage(true);
            }
            return;
        }

        if (failedOnChain) {
            // Finalized with an error moved no lamports. That is as provable
            // as the paid case and the opposite conclusion: nothing was paid,
            // so the reward goes back in the queue rather than needing the
            // owner to acknowledge a double-payment risk that does not exist.
            clearPendingContributorReceipt(rewardId, chainSignature);
            const QString detail =
                QStringLiteral(
                    "Transfer %1 finalized as failed on Solana, so nothing was "
                    "paid and nothing is held. The reward is payable again.")
                    .arg(chainSignature);
            tipStatus(detail);
            if (automated) {
                finishContributorPayoutTransfer(false, rewardId, lamports,
                                                detail);
            } else {
                m_tipInFlight = false;
                refreshTipWalletControls();
                fetchContributorRewards();
                refreshLedgerPage(true);
            }
            return;
        }

        // Unanswered, or answered with a transfer that is not final yet. The
        // next endpoint may know it; if none of them do, the hold stands.
        settlePendingContributorPayoutOnChain(recipientName, lamports,
                                              chainSignature, sourceAddress,
                                              rewardId, automated,
                                              endpointIndex + 1);
    });
}

void MainWindow::flushSettledContributorNotices(bool announce)
{
    // Report to ForkMesh used to be the only control on this page that could
    // be pressed and answer with nothing at all: every way this errand fails
    // leaves the entry queued and says so only in the network log, so the
    // panel sat there unchanged and the button looked broken. Nothing here is
    // at risk when it fails — the transfers are final on Solana and the queue
    // is what stops them being signed twice — but a reader who asked is owed
    // which of the walls it hit. The background passes (startup, sign-in,
    // every reward-pool run) stay silent, as they always have.
    const auto report = [this, announce](const QString &text) {
        if (!announce)
            return;
        m_ledgerPaymentLine = text;
        setLedgerStatus(text);
    };
    if (!m_networkAccess) {
        report(QStringLiteral(
            "This app has no network access right now, so the payments "
            "already made cannot be reported to ForkMesh yet."));
        return;
    }
    if (m_settledNoticeInFlight) {
        report(QStringLiteral(
            "A report is already on its way to ForkMesh; its answer will "
            "appear here."));
        return;
    }
    const QList<ContributorPayoutReceipt::Settled::Notice> queued =
        ContributorPayoutReceipt::Settled::readAll();
    bool credentialMissing = false;
    for (const ContributorPayoutReceipt::Settled::Notice &notice : queued) {
        const QUrl endpoint(notice.endpoint);
        // An entry that fails this is never sent and never removed. That is
        // deliberate: dropping it would let its reward be paid a second time,
        // and the chain already proved it was paid once.
        if (!isWellFormedContributorPayout(
                notice.recipientName, notice.lamports, notice.chainSignature,
                notice.sourceAddress, notice.rewardId) ||
            !isContributorReceiptEndpoint(endpoint)) {
            continue;
        }
        QNetworkRequest request;
        if (!prepareContributorNoticeRequest(endpoint, notice.recipientName,
                                             notice.lamports,
                                             notice.chainSignature,
                                             notice.rewardId,
                                             notice.sourceAddress, &request)) {
            // No credential of either kind; the errand keeps waiting.
            credentialMissing = true;
            continue;
        }
        const QJsonObject payload{
            {QStringLiteral("toNodeName"), notice.recipientName.toLower()},
            {QStringLiteral("lamports"), qint64(notice.lamports)},
            {QStringLiteral("signature"), notice.chainSignature},
            {QStringLiteral("network"), QStringLiteral("mainnet")},
            {QStringLiteral("rewardId"), notice.rewardId},
            {QStringLiteral("fromAddress"), notice.sourceAddress}};
        m_settledNoticeInFlight = true;
        QNetworkReply *reply = m_networkAccess->post(
            request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this,
                [this, reply, notice, announce, report] {
            const QByteArray body = boundedTipBody(reply);
            const bool networkOk = reply->error() == QNetworkReply::NoError;
            const int status =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                    .toInt();
            reply->deleteLater();
            m_settledNoticeInFlight = false;
            const QJsonObject response = QJsonDocument::fromJson(body).object();
            // Only the relay agreeing that this reward is paid retires the
            // entry. Anything else — a 202 while its indexer catches up, a
            // rejected session — leaves it queued for the next flush.
            if (!networkOk || !response.value(QStringLiteral("ok")).toBool() ||
                response.value(QStringLiteral("rewardStatus")).toString() !=
                    QLatin1String("paid")) {
                report(settledNoticeFailureText(notice, networkOk, status));
                return;
            }
            {
                QSettings settings;
                settings.remove(ContributorPayoutReceipt::Settled::group() +
                                QLatin1Char('/') + notice.rewardId);
                settings.sync();
            }
            recordRewardReceiptLocally(notice.rewardId, notice.chainSignature);
            const bool emailed =
                response.value(QStringLiteral("emailed")).toBool();
            // Deliberately not tipStatus(): a background pass runs behind
            // whatever the reader is doing and must not take over the Ledger's
            // status line from the payment that put it there. A reader who
            // pressed Report to ForkMesh is waiting on exactly this line, and
            // gets it without the log's own "Ledger:" prefix.
            const QString outcome =
                emailed ? QStringLiteral(
                              "ForkMesh recorded the earlier payout of reward "
                              "%1 and emailed %2.")
                              .arg(notice.rewardId.left(32),
                                   notice.recipientName)
                        : QString::fromUtf8(
                              "ForkMesh recorded the earlier payout of reward "
                              "%1. No email was sent \xE2\x80\x94 %2 has no "
                              "verified address, or has turned these emails "
                              "off.")
                              .arg(notice.rewardId.left(32),
                                   notice.recipientName);
            logSystem(QStringLiteral("Ledger: ") + outcome);
            report(outcome);
            rebuildLedgerTable();
            // Whatever else is still owed, and only then: re-entering with an
            // empty queue would answer "everything is already reported" over
            // the line that just said which payout landed.
            if (!ContributorPayoutReceipt::Settled::rewardIds().isEmpty())
                flushSettledContributorNotices(announce);
        });
        return; // one at a time; the reply asks for the next
    }
    report(queued.isEmpty()
               ? QStringLiteral("Every payment made from this desktop has "
                                "already been reported to ForkMesh.")
           : credentialMissing
               ? QStringLiteral(
                     "This machine holds no ForkMesh credential to report "
                     "these payments with. Sign in as the repository owner "
                     "and press Report to ForkMesh again; the transfers "
                     "themselves are already final on Solana.")
               : QStringLiteral(
                     "The saved receipts are not in a shape that can be "
                     "reported. The transfers are already final on Solana — "
                     "check them there before changing anything here."));
}

bool MainWindow::resumePendingContributorReceipt(bool automated,
                                                 bool reportToLedger)
{
    const ContributorPayoutReceipt::Pending pending =
        ContributorPayoutReceipt::read();
    const QString recipient = pending.recipientName;
    const quint64 lamports = pending.lamports;
    const QString signature = pending.chainSignature;
    const QString source = pending.sourceAddress;
    const QString rewardId = pending.rewardId;
    const QUrl endpoint(pending.endpoint);
    if (!pending.blocking())
        return false;

    // A retry explicitly started on the Ledger owns that page's status line.
    // The scheduled CFO path remains background housekeeping and writes only
    // to the system log, as it did before the Ledger offered recovery tools.
    m_ledgerPaymentReporting = reportToLedger;

    const bool valid =
        isWellFormedContributorPayout(recipient, lamports, signature, source,
                                      rewardId) &&
        isContributorReceiptEndpoint(endpoint);
    if (!valid) {
        const QString detail = QStringLiteral(
            "Stopped: the saved contributor receipt failed local validation. Payouts remain blocked to prevent a replacement transfer.");
        tipStatus(detail);
        if (automated) {
            finishContributorPayoutTransfer(false, QString(), 0, detail);
        } else {
            m_tipInFlight = false;
            refreshTipWalletControls();
        }
        return true;
    }

    if (automated) {
        m_contributorPayoutCurrentRewardId = rewardId;
        m_contributorPayoutCurrentLamports = lamports;
    }
    m_contributorPayoutReceiptEndpoint = endpoint;
    m_tipInFlight = true;
    m_tipInFlightRewardId = rewardId;
    refreshTipWalletControls();
    announceTip(recipient, lamports, signature, source, rewardId, automated, 0);
    return true;
}

void MainWindow::refreshTipWalletControls()
{
    // A manual payment's passphrase is meaningful only while its transfer
    // owns the tip flow. Every normal completion and failure comes through
    // here after clearing m_tipInFlight, so this makes an abandoned request
    // fail closed rather than leaving its password in the window object.
    if (!m_tipInFlight && !m_tipOneShotPassphrase.isEmpty()) {
        forkmesh::rewards::secureErase(m_tipOneShotPassphrase);
        m_tipOneShotPassphrase.clear();
    }
    // A payment approved behind the transfer that just ended takes the
    // wallet next. Deferred a turn: this runs inside that transfer's reply
    // handler, and the controls below still describe its end.
    if (!m_tipInFlight && !m_tipTransferQueue.isEmpty())
        QTimer::singleShot(0, this, &MainWindow::drainTipTransferQueue);
    // A ledger row's Pay button is armed by the same in-flight state as the
    // rest of the tip controls; keeping it here means a transfer that ends in
    // an error re-arms the row too.
    updateLedgerPayButtons();
    // The Users directory carries its own send button, and it is gated on the
    // same in-flight flag, so it refreshes even when the profile panel has not
    // been built yet.
    refreshUsersTipButton();
    const QString vaultPath = forkmesh::tips::defaultTipVaultPath();
    const QString linked = forkmesh::tips::linkedTipAddress(vaultPath);
    const bool hasWallet = !linked.isEmpty();
    if (!m_contributorAutoPayoutPassphrase.isEmpty() &&
        (m_contributorAutoPayoutWallet.isEmpty() ||
         linked != m_contributorAutoPayoutWallet)) {
        forkmesh::rewards::secureErase(m_contributorAutoPayoutPassphrase);
        m_contributorAutoPayoutPassphrase.clear();
        m_contributorAutoPayoutWallet.clear();
        m_contributorAutoPayoutScope.clear();
        m_contributorPayoutRunActive = false;
        if (m_contributorAutoPayoutStatus) {
            m_contributorAutoPayoutStatus->setText(
                QStringLiteral(
                    "Disarmed because the desktop-linked wallet changed."));
        }
    }
    if (!m_profileTipWalletSection) {
        refreshRewardPoolControls();
        return;
    }

    // Linking is a per-machine action, so the section only appears on your own
    // profile — the place where every other "this machine" control lives.
    m_profileTipWalletSection->setVisible(m_profileIsSelf);
    if (m_profileTipWalletLabel) {
        m_profileTipWalletLabel->setText(
            hasWallet
                ? QString::fromUtf8(
                      "Linked wallet <b>%1</b> \xE2\x80\x94 encrypted on this "
                      "machine with your vault passphrase. Tips are signed "
                      "here; the key is never uploaded.")
                      .arg(linked.toHtmlEscaped())
                : QStringLiteral(
                      "No tip wallet linked. Generate one here, or import an "
                      "existing Solana key, to send tips to other members. It "
                      "is encrypted with a passphrase and stored only on this "
                      "machine."));
    }
    if (m_profileTipLinkButton) {
        m_profileTipLinkButton->setText(
            hasWallet ? QStringLiteral("Replace wallet…")
                      : QStringLiteral("Set up a wallet…"));
        m_profileTipLinkButton->setEnabled(!m_tipInFlight);
    }
    if (m_profileTipExportButton) {
        m_profileTipExportButton->setVisible(hasWallet);
        m_profileTipExportButton->setEnabled(!m_tipInFlight);
    }
    if (m_profileTipUnlinkButton) {
        m_profileTipUnlinkButton->setVisible(hasWallet);
        m_profileTipUnlinkButton->setEnabled(!m_tipInFlight);
    }
    if (m_profileTipButton) {
        // A profile publishes a payout address that can receive a tip,
        // including the signed-in account's own separately managed address.
        const bool canTip = forkmesh::tips::isValidTipAddress(
            m_profileSolanaValue.trimmed());
        m_profileTipButton->setVisible(canTip);
        // A tip sent while another transfer runs queues behind it.
        m_profileTipButton->setEnabled(canTip);
        m_profileTipButton->setToolTip(
            hasWallet
                ? QStringLiteral(
                      "Sign a SOL transfer to this member's public payout "
                      "address with your linked wallet and broadcast it. "
                      "ForkMesh emails them that a tip is on the way.")
                : QStringLiteral(
                      "Link a Solana wallet on your own profile first; tips "
                      "are signed on this machine."));
    }
    refreshRewardPoolControls();
}

void MainWindow::linkTipWallet()
{
    const QString vaultPath = forkmesh::tips::defaultTipVaultPath();
    if (vaultPath.trimmed().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Link a tip wallet"),
                             QStringLiteral(
                                 "This machine has no private application data "
                                 "directory, so a wallet cannot be stored "
                                 "securely."));
        return;
    }
    const QString existing = forkmesh::tips::linkedTipAddress(vaultPath);
    if (!existing.isEmpty() &&
        QMessageBox::question(
            this, QStringLiteral("Replace the linked tip wallet?"),
            QStringLiteral(
                "This machine already holds an encrypted wallet for %1. "
                "Replacing it discards that key — make sure you still have it "
                "backed up elsewhere.\n\nReplace it?")
                .arg(existing),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    const int minimumPassphrase = forkmesh::tips::minimumVaultPassphraseLength();

    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("tipWalletSetupDialog"));
    dialog.setWindowTitle(QStringLiteral("Set up a tip wallet"));
    const TipDialogLayout dialogLayout =
        makeTipDialogLayout(&dialog, QSize(640, 500));
    QVBoxLayout *layout = dialogLayout.body;
    auto *intro = new QLabel(
        QStringLiteral(
            "Generate a new Solana wallet, paste an existing secret key, or "
            "load the keypair file <code>solana-keygen</code> wrote. ForkMesh "
            "encrypts it with the passphrase below and stores it only on this "
            "machine — it is never sent to the relay, and it is used only to "
            "sign transfers you confirm.\n\n"
            "Use a wallet you fund for tipping, not your main wallet."));
    intro->setWordWrap(true);
    layout->addWidget(intro);
    layout->addSpacing(4);
    layout->addWidget(
        tipDialogSectionTitle(QStringLiteral("1. Choose a wallet")));

    auto *keyEdit = new QLineEdit;
    keyEdit->setObjectName(QStringLiteral("tipWalletKeyInput"));
    keyEdit->setEchoMode(QLineEdit::Password);
    keyEdit->setMaxLength(kTipKeyMaterialMaxBytes);
    keyEdit->setMinimumWidth(0);
    keyEdit->setPlaceholderText(
        QStringLiteral("[12,34,…] byte array, or a base58 secret key"));
    auto *generateButton = new QPushButton(QStringLiteral("Generate a new wallet"));
    generateButton->setObjectName(QStringLiteral("tipWalletGenerateButton"));
    auto *loadButton = new QPushButton(QStringLiteral("Load keypair file…"));
    loadButton->setObjectName(QStringLiteral("tipWalletLoadButton"));
    auto *passEdit = new QLineEdit;
    passEdit->setObjectName(QStringLiteral("tipWalletPassphraseInput"));
    passEdit->setEchoMode(QLineEdit::Password);
    passEdit->setMaxLength(1024);
    passEdit->setMinimumWidth(0);
    passEdit->setPlaceholderText(
        QStringLiteral("at least %1 characters").arg(minimumPassphrase));
    auto *confirmEdit = new QLineEdit;
    confirmEdit->setObjectName(QStringLiteral("tipWalletConfirmInput"));
    confirmEdit->setEchoMode(QLineEdit::Password);
    confirmEdit->setMaxLength(1024);
    confirmEdit->setMinimumWidth(0);

    auto *keyForm = new QFormLayout;
    polishTipForm(keyForm);
    auto *keyLabel = new QLabel(QStringLiteral("Secret key"));
    keyLabel->setBuddy(keyEdit);
    keyForm->addRow(keyLabel, keyEdit);
    layout->addLayout(keyForm);

    auto *sourceButtons = new QHBoxLayout;
    sourceButtons->setContentsMargins(0, 0, 0, 0);
    sourceButtons->setSpacing(8);
    sourceButtons->addWidget(generateButton);
    sourceButtons->addWidget(loadButton);
    sourceButtons->addStretch(1);
    layout->addLayout(sourceButtons);

    auto *form = new QFormLayout;
    polishTipForm(form);
    auto *passLabel = new QLabel(QStringLiteral("Vault passphrase"));
    passLabel->setBuddy(passEdit);
    auto *confirmLabel = new QLabel(QStringLiteral("Confirm passphrase"));
    confirmLabel->setBuddy(confirmEdit);
    layout->addSpacing(4);
    layout->addWidget(
        tipDialogSectionTitle(QStringLiteral("2. Protect the local vault")));
    form->addRow(passLabel, passEdit);
    form->addRow(confirmLabel, confirmEdit);
    layout->addLayout(form);

    // A generated wallet is empty and unbacked-up until the owner acts on it,
    // so the address goes on screen the moment it exists.
    auto *generated = new QLabel;
    generated->setObjectName(QStringLiteral("statusLine"));
    generated->setProperty("tipStatusKind", QStringLiteral("generated"));
    generated->setWordWrap(true);
    generated->setTextInteractionFlags(Qt::TextSelectableByMouse);
    generated->setVisible(false);
    layout->addWidget(generated);

    auto *problem = new QLabel;
    problem->setObjectName(QStringLiteral("statusLine"));
    problem->setProperty("tipStatusKind", QStringLiteral("problem"));
    problem->setWordWrap(true);
    problem->setVisible(false);
    layout->addWidget(problem);
    layout->addStretch(1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
                                         QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Link wallet"));
    polishTipDialogButtons(buttons, QDialogButtonBox::Ok);
    dialogLayout.outer->addWidget(buttons);

    connect(generateButton, &QPushButton::clicked, &dialog,
            [keyEdit, generated, problem, scroll = dialogLayout.scroll] {
        QByteArray keypairJson;
        QString address;
        QString error;
        if (!forkmesh::tips::generateTipKeypair(&keypairJson, &address,
                                                &error)) {
            generated->setVisible(false);
            problem->setText(safeTipText(error));
            revealTipStatus(problem, scroll);
            return;
        }
        keyEdit->setText(QString::fromLatin1(keypairJson));
        forkmesh::rewards::secureErase(keypairJson);
        problem->setVisible(false);
        generated->setText(
            QStringLiteral(
                "New wallet <b>%1</b> — nothing is stored until you choose a "
                "passphrase and press Link wallet. It starts empty: fund it "
                "from an exchange or another wallet, and export a backup of "
                "the key afterwards.")
                .arg(address.toHtmlEscaped()));
        revealTipStatus(generated, scroll);
    });
    connect(loadButton, &QPushButton::clicked, &dialog,
            [&dialog, keyEdit, generated] {
        const QString path = QFileDialog::getOpenFileName(
            &dialog, QStringLiteral("Select a Solana keypair file"), QString(),
            QStringLiteral("Keypair files (*.json);;All files (*)"));
        if (path.isEmpty())
            return;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return;
        // Bounded read: a keypair file is a few hundred bytes, and this string
        // is about to hold private material.
        keyEdit->setText(
            QString::fromLatin1(file.read(kTipKeyMaterialMaxBytes)).trimmed());
        // Whatever was loaded now owns the field; a stale "new wallet" line
        // would name an address that is no longer the one being linked.
        generated->setVisible(false);
    });
    connect(keyEdit, &QLineEdit::textEdited, &dialog,
            [generated] { generated->setVisible(false); });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog,
            [&dialog, keyEdit, passEdit, confirmEdit, problem,
             minimumPassphrase, scroll = dialogLayout.scroll] {
        if (passEdit->text() != confirmEdit->text()) {
            problem->setText(QStringLiteral("The two passphrases do not match."));
            revealTipStatus(problem, scroll);
            return;
        }
        QString error;
        QByteArray material = keyEdit->text().trimmed().toUtf8();
        const QString address =
            forkmesh::rewards::poolAddressForKeyMaterial(material, &error);
        forkmesh::rewards::secureErase(material);
        if (address.isEmpty()) {
            problem->setText(error.isEmpty()
                                 ? QStringLiteral("That is not a usable Solana "
                                                  "secret key.")
                                 : safeTipText(error));
            revealTipStatus(problem, scroll);
            return;
        }
        if (passEdit->text().size() < minimumPassphrase) {
            problem->setText(
                QStringLiteral(
                    "Choose a vault passphrase of at least %1 characters. It "
                    "is the only thing protecting this key on disk.")
                    .arg(minimumPassphrase));
            revealTipStatus(problem, scroll);
            return;
        }
        dialog.accept();
    });

    const int outcome = dialog.exec();
    QByteArray material = keyEdit->text().trimmed().toUtf8();
    QString passphrase = passEdit->text();
    scrubTipEdit(keyEdit);
    scrubTipEdit(passEdit);
    scrubTipEdit(confirmEdit);
    if (outcome != QDialog::Accepted) {
        forkmesh::rewards::secureErase(material);
        forkmesh::rewards::secureErase(passphrase);
        return;
    }

    QString address;
    QString error;
    const bool linked = forkmesh::tips::linkTipWallet(material, passphrase,
                                                      vaultPath, &address,
                                                      &error);
    forkmesh::rewards::secureErase(material);
    forkmesh::rewards::secureErase(passphrase);
    if (!linked) {
        QMessageBox::warning(this, QStringLiteral("Link a tip wallet"),
                             safeTipText(error.isEmpty()
                                             ? QStringLiteral(
                                                   "The wallet could not be "
                                                   "stored securely.")
                                             : error));
        refreshTipWalletControls();
        return;
    }
    tipStatus(QStringLiteral("Linked tip wallet %1; the key is encrypted on "
                             "this machine only.")
                  .arg(address));
    refreshTipWalletControls();
    QMessageBox::information(
        this, QStringLiteral("Tip wallet linked"),
        QStringLiteral(
            "%1 is ready to send tips.\n\nThe key is encrypted with your "
            "passphrase and stored only on this machine. ForkMesh cannot read "
            "it, and cannot recover it if you forget the passphrase — use "
            "\"Export secret key\" to keep your own backup.")
            .arg(address));
}

// Handing a private key back to the person it belongs to. A wallet ForkMesh
// generated would otherwise be trapped in this vault: no backup, no way to move
// it into Phantom or the Solana CLI, and nothing recoverable if this machine
// dies. The key only ever leaves the vault through this function, only behind
// the vault passphrase, and only into the owner's clipboard or a file they pick.
void MainWindow::exportTipWalletSecret()
{
    const QString vaultPath = forkmesh::tips::defaultTipVaultPath();
    const QString linked = forkmesh::tips::linkedTipAddress(vaultPath);
    if (linked.isEmpty())
        return;
    if (QMessageBox::warning(
            this, QStringLiteral("Export the tip wallet's secret key?"),
            QStringLiteral(
                "This shows the secret key for %1 in plain text.\n\nAnyone who "
                "gets a copy can spend everything in that wallet, and no "
                "transfer can be reversed. Only do this to make a backup you "
                "keep offline, or to load the wallet into another wallet app "
                "you control — never paste it into a website, a chat, or a "
                "support request.\n\nShow the secret key?")
                .arg(linked),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    QInputDialog unlock(this);
    unlock.setWindowTitle(QStringLiteral("Unlock your tip wallet"));
    unlock.resize(560, 220);
    unlock.setSizeGripEnabled(true);
    unlock.setLabelText(
        QStringLiteral("Enter your vault passphrase to decrypt the key for "
                       "%1.")
            .arg(linked));
    unlock.setTextEchoMode(QLineEdit::Password);
    unlock.setInputMode(QInputDialog::TextInput);
    const bool unlocked = unlock.exec() == QDialog::Accepted;
    QString passphrase = unlock.textValue();
    if (!unlocked || passphrase.isEmpty()) {
        forkmesh::rewards::secureErase(passphrase);
        return;
    }

    QByteArray keypairJson;
    QString base58Secret;
    QString address;
    QString error;
    const bool exported = forkmesh::tips::exportTipWalletKey(
        passphrase, vaultPath, &keypairJson, &base58Secret, &address, &error);
    forkmesh::rewards::secureErase(passphrase);
    if (!exported) {
        QMessageBox::warning(this, QStringLiteral("Export secret key"),
                             safeTipText(error.isEmpty()
                                             ? QStringLiteral(
                                                   "The key could not be "
                                                   "decrypted.")
                                             : error));
        return;
    }

    showTipSecretKey(address, keypairJson, base58Secret);
    forkmesh::rewards::secureErase(keypairJson);
    forkmesh::rewards::secureErase(base58Secret);
}

// The one place a decrypted key is on screen. It stays hidden behind a reveal
// checkbox, both copy buttons take the clipboard back afterwards, and the
// status line never carries the key itself.
void MainWindow::showTipSecretKey(const QString &address,
                                  const QByteArray &keypairJson,
                                  const QString &base58Secret)
{
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("tipWalletSecretDialog"));
    dialog.setWindowTitle(QStringLiteral("Tip wallet secret key"));
    const TipDialogLayout dialogLayout =
        makeTipDialogLayout(&dialog, QSize(680, 480));
    QVBoxLayout *layout = dialogLayout.body;
    auto *intro = new QLabel(
        QStringLiteral(
            "Secret key for <b>%1</b>.<br><br>The JSON byte array is what "
            "<code>solana-keygen</code> writes and what the Solana CLI reads; "
            "the base58 form is what Phantom and similar wallets import. Both "
            "are the same key — whoever holds either one owns the wallet.")
            .arg(address.toHtmlEscaped()));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *jsonEdit = new QLineEdit(QString::fromLatin1(keypairJson));
    jsonEdit->setReadOnly(true);
    jsonEdit->setEchoMode(QLineEdit::Password);
    auto *base58Edit = new QLineEdit(base58Secret);
    base58Edit->setReadOnly(true);
    base58Edit->setEchoMode(QLineEdit::Password);
    auto *secretForm = new QFormLayout;
    polishTipForm(secretForm);
    auto *jsonLabel = new QLabel(QStringLiteral("Keypair JSON"));
    jsonLabel->setBuddy(jsonEdit);
    auto *base58Label = new QLabel(QStringLiteral("Base58 key"));
    base58Label->setBuddy(base58Edit);
    secretForm->addRow(jsonLabel, jsonEdit);
    secretForm->addRow(base58Label, base58Edit);
    layout->addLayout(secretForm);

    // Masked until asked for: an export is often done in a room with other
    // people or a screen share running, and the copy buttons work either way.
    auto *reveal = new QCheckBox(QStringLiteral("Show the key on screen"));
    layout->addWidget(reveal);
    connect(reveal, &QCheckBox::toggled, &dialog,
            [jsonEdit, base58Edit](bool on) {
        const QLineEdit::EchoMode mode =
            on ? QLineEdit::Normal : QLineEdit::Password;
        jsonEdit->setEchoMode(mode);
        base58Edit->setEchoMode(mode);
    });

    auto *status = new QLabel;
    status->setObjectName(QStringLiteral("statusLine"));
    status->setWordWrap(true);
    status->setVisible(false);

    auto *copyJson = new QPushButton(QStringLiteral("Copy keypair JSON"));
    auto *copyBase58 = new QPushButton(QStringLiteral("Copy base58 key"));
    auto *saveFile = new QPushButton(QStringLiteral("Save keypair file…"));
    auto *actions = new QGridLayout;
    actions->setContentsMargins(0, 0, 0, 0);
    actions->setHorizontalSpacing(8);
    actions->setVerticalSpacing(8);
    actions->addWidget(copyJson, 0, 0);
    actions->addWidget(copyBase58, 0, 1);
    actions->addWidget(saveFile, 1, 0, 1, 2);
    actions->setColumnStretch(0, 1);
    actions->setColumnStretch(1, 1);
    layout->addLayout(actions);
    layout->addWidget(status);
    layout->addStretch(1);

    // A clipboard holding a spendable key is a liability, so every copy is on a
    // timer and is taken back only if the user has not copied something else.
    auto copySecret = [status, scroll = dialogLayout.scroll](
                          const QString &secret, const QString &what) {
        QApplication::clipboard()->setText(secret);
        status->setText(
            QStringLiteral("%1 copied. The clipboard is cleared in 90 seconds "
                           "unless you copy something else — paste it "
                           "somewhere safe now.")
                .arg(what));
        revealTipStatus(status, scroll);
        QTimer::singleShot(90000, QApplication::clipboard(), [secret] {
            if (QApplication::clipboard()->text() == secret)
                QApplication::clipboard()->clear();
        });
    };
    connect(copyJson, &QPushButton::clicked, &dialog,
            [copySecret, keypairJson] {
        copySecret(QString::fromLatin1(keypairJson),
                   QStringLiteral("The keypair JSON"));
    });
    connect(copyBase58, &QPushButton::clicked, &dialog,
            [copySecret, base58Secret] {
        copySecret(base58Secret, QStringLiteral("The base58 secret key"));
    });
    connect(saveFile, &QPushButton::clicked, &dialog,
            [&dialog, status, keypairJson, address,
             scroll = dialogLayout.scroll] {
        const QString path = QFileDialog::getSaveFileName(
            &dialog, QStringLiteral("Save the keypair file"),
            QStringLiteral("%1.json").arg(address),
            QStringLiteral("Keypair files (*.json);;All files (*)"));
        if (path.isEmpty())
            return;
        QString error;
        if (!writeTipKeypairFile(path, keypairJson, &error)) {
            status->setText(safeTipText(error));
            revealTipStatus(status, scroll);
            return;
        }
        status->setText(
            QStringLiteral("Saved to %1, readable only by your user account. "
                           "Move it somewhere you control — a backup on this "
                           "disk dies with this disk.")
                .arg(QDir::toNativeSeparators(path)));
        revealTipStatus(status, scroll);
    });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    polishTipDialogButtons(buttons);
    dialogLayout.outer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
    scrubTipEdit(jsonEdit);
    scrubTipEdit(base58Edit);
}

void MainWindow::unlinkTipWallet()
{
    const QString vaultPath = forkmesh::tips::defaultTipVaultPath();
    const QString linked = forkmesh::tips::linkedTipAddress(vaultPath);
    if (linked.isEmpty())
        return;
    if (QMessageBox::question(
            this, QStringLiteral("Remove the linked tip wallet?"),
            QStringLiteral(
                "This deletes the encrypted copy of %1 from this machine. The "
                "wallet and its funds are unaffected, but you will need the "
                "original key to link it again.\n\nRemove it?")
                .arg(linked),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    QString error;
    if (!forkmesh::tips::unlinkTipWallet(vaultPath, &error)) {
        QMessageBox::warning(this, QStringLiteral("Remove tip wallet"),
                             safeTipText(error));
        return;
    }
    tipStatus(QStringLiteral("Removed the local tip wallet vault."));
    refreshTipWalletControls();
}

void MainWindow::sendTipToProfileMember()
{
    if (m_profileIsSelf)
        return;
    sendTipToMember(m_profileNodeName.trimmed(), m_profileSolanaValue.trimmed());
}

// The refusal above, said out loud. Deliberately modeless: this is raised from
// a button press that can arrive while a headless or automated node is running
// the same flow, and a nested exec() there parks the app behind a dialog
// nobody is present to close. show() states the case and lets the window carry
// on.
void MainWindow::showHeldContributorPayoutAlert(
    const ContributorPayoutReceipt::Pending &pending,
    const QString &attemptedRecipient)
{
    // An 88-character base58 signature is one unbreakable word, and Qt's rich
    // text will run it straight off the edge of the dialog rather than wrap
    // it. Zero-width spaces give the layout somewhere to break without putting
    // a character into the value anyone might copy back out by hand.
    const auto breakable = [](const QString &value) {
        constexpr int kRun = 28;
        if (value.size() <= kRun)
            return value.toHtmlEscaped();
        QString wrapped;
        for (int at = 0; at < value.size(); at += kRun) {
            if (at > 0)
                wrapped += QString::fromUtf8("\xE2\x80\x8B");
            wrapped += value.mid(at, kRun).toHtmlEscaped();
        }
        return wrapped;
    };

    QStringList rows;
    for (const QString &line :
         ContributorPayoutReceipt::detailLines(pending, /*shortened=*/false)) {
        const qsizetype split = line.indexOf(QLatin1String(": "));
        if (split < 0) {
            rows.append(QStringLiteral("<tr><td colspan='2'>%1</td></tr>")
                            .arg(line.toHtmlEscaped()));
            continue;
        }
        rows.append(
            QStringLiteral(
                "<tr><td style='padding-right:12px;white-space:nowrap'>%1</td>"
                "<td><code>%2</code></td></tr>")
                .arg(line.left(split).toHtmlEscaped(),
                     breakable(line.mid(split + 2))));
    }

    auto *box = new QMessageBox(this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowTitle(QStringLiteral("Payout held for reconciliation"));
    box->setIcon(QMessageBox::Warning);
    box->setTextFormat(Qt::RichText);
    box->setText(
        QStringLiteral("<b>%1</b><br><br>Nothing was signed%2.")
            .arg(ContributorPayoutReceipt::refusalSummary().toHtmlEscaped(),
                 attemptedRecipient.isEmpty()
                     ? QString()
                     : QStringLiteral(" for @%1")
                           .arg(attemptedRecipient.toHtmlEscaped())));
    box->setInformativeText(
        QStringLiteral("<p>The payout being held:</p><table>%1</table>"
                       "<p>%2</p>")
            .arg(rows.join(QString()),
                 ContributorPayoutReceipt::reconciliationAdvice(pending)
                     .toHtmlEscaped()));
    box->setStandardButtons(QMessageBox::Close);
    if (!pending.chainSignature.isEmpty()) {
        // The signature is public and is the only thing that answers "did it
        // actually land?", so hand it over rather than making it be retyped
        // out of a dialog by eye.
        QPushButton *copy = box->addButton(QStringLiteral("Copy signature"),
                                           QMessageBox::ActionRole);
        const QString signature = pending.chainSignature;
        connect(copy, &QPushButton::clicked, this, [this, signature] {
            QApplication::clipboard()->setText(signature);
            tipStatus(QStringLiteral(
                          "Copied the held payout signature %1 to the "
                          "clipboard.")
                          .arg(signature));
        });
    }
    box->setDefaultButton(QMessageBox::Close);
    box->setModal(false);
    box->show();
    box->raise();
    box->activateWindow();
}

// The one tip flow. Every entry point — a member's profile, the Users
// directory — funnels through here with a name and a public payout address, so
// the wallet checks, transfer review, and one-shot passphrase form are written
// once.
void MainWindow::sendTipToMember(const QString &recipientName,
                                 const QString &recipientAddress,
                                 quint64 fixedLamports,
                                 const QString &rewardId, bool reportToLedger)
{
    // Whoever starts a transfer owns where it reports, so this is set on every
    // entry rather than cleared on every exit: a tip sent from a profile stops
    // narrating itself onto the Ledger simply by not asking for it. A transfer
    // already in flight keeps its reporting; this payment can still be
    // reviewed now and queued behind it.
    if (!m_tipInFlight)
        m_ledgerPaymentReporting = reportToLedger;
    if (!rewardId.isEmpty() && m_tipInFlight &&
        rewardId == m_tipInFlightRewardId) {
        tipStatus(QStringLiteral("Reward %1 is already being paid from this "
                                 "machine. Nothing was signed.")
                      .arg(rewardId));
        return;
    }
    if (recipientName.isEmpty() ||
        !forkmesh::tips::isValidTipAddress(recipientAddress)) {
        tipStatus(QStringLiteral(
            "That recipient has no valid public payout address, so nothing "
            "was signed."));
        return;
    }
    const ContributorPayoutReceipt::Pending heldPayout =
        ContributorPayoutReceipt::read();
    if (!rewardId.isEmpty() && heldPayout.blocking() &&
        !heldPayoutIsInFlight(heldPayout)) {
        tipStatus(ContributorPayoutReceipt::refusalSummary());
        // A refusal that only writes a status line is how this reads as a dead
        // button: the reader pressed Pay on money they expect to move, and the
        // one surface that answered is a line they were not looking at. Say it
        // to their face, with the figures of the payout that is actually being
        // held, since the next question is always "held by what?".
        showHeldContributorPayoutAlert(heldPayout, recipientName);
        return;
    }
    // Nothing holds the queue, but this one reward has already been paid from
    // here and proven on-chain. The relay goes on publishing it as ready until
    // its notice gets through, so its own Pay button is the last place a
    // double payment could still be signed.
    if (!rewardId.isEmpty() &&
        ContributorPayoutReceipt::Settled::contains(rewardId)) {
        const ContributorPayoutReceipt::Settled::Notice notice =
            ContributorPayoutReceipt::Settled::read(QSettings(), rewardId);
        tipStatus(QStringLiteral(
                      "Reward %1 was already paid from this machine: %2. "
                      "Nothing was signed.")
                      .arg(rewardId, notice.chainSignature));
        // Modeless, like every other refusal in this flow: a headless node
        // reaches this path from a scheduled run, and a modal box there parks
        // the whole app on a dialog nobody is present to close.
        auto *box = new QMessageBox(this);
        box->setObjectName(QStringLiteral("tipAlreadyPaidDialog"));
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->setWindowTitle(QStringLiteral("Already paid"));
        box->setIcon(QMessageBox::Information);
        box->setText(
            QStringLiteral("%1 has already been paid %2 for this reward.")
                .arg(recipientName,
                     forkmesh::tips::formatLamports(notice.lamports)));
        box->setInformativeText(
            ContributorPayoutReceipt::Settled::summary(notice) +
            QStringLiteral("\n\nSignature: %1").arg(notice.chainSignature));
        box->setStandardButtons(QMessageBox::Close);
        box->setModal(false);
        box->show();
        box->raise();
        box->activateWindow();
        // A settled notice is exactly what this press was waiting on, so use
        // the press to try sending it again.
        flushSettledContributorNotices();
        return;
    }

    const QString vaultPath = forkmesh::tips::defaultTipVaultPath();
    const QString sender = forkmesh::tips::linkedTipAddress(vaultPath);
    if (sender.isEmpty()) {
        if (QMessageBox::question(
                this, QStringLiteral("Link a tip wallet first"),
                QStringLiteral(
                    "Tips are signed on this machine with a Solana key you "
                    "link yourself — ForkMesh never holds one.\n\nLink a "
                    "wallet now?"),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::Yes) == QMessageBox::Yes) {
            linkTipWallet();
        }
        return;
    }
    const bool rewardPayment = !rewardId.isEmpty() && fixedLamports > 0;
    QDialog dialog(this);
    dialog.setObjectName(rewardPayment
                             ? QStringLiteral("sendPaymentDialog")
                             : QStringLiteral("sendTipDialog"));
    dialog.setWindowTitle(
        rewardPayment
            ? QStringLiteral("Pay reward to %1").arg(recipientName)
            : QStringLiteral("Send a tip to %1").arg(recipientName));
    const TipDialogLayout dialogLayout =
        makeTipDialogLayout(&dialog, QSize(640, 500));
    QVBoxLayout *layout = dialogLayout.body;
    auto *intro = new QLabel(
        rewardPayment
            ? QStringLiteral(
                  "Review this exact reward payment, enter your vault "
                  "passphrase, then send it.")
            : QStringLiteral(
                  "Review this transfer, enter your vault passphrase, then "
                  "send it."));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *form = new QFormLayout;
    polishTipForm(form);
    auto *senderValue = new QLabel(
        QStringLiteral("<code>%1</code>").arg(sender.toHtmlEscaped()));
    senderValue->setTextFormat(Qt::RichText);
    senderValue->setTextInteractionFlags(Qt::TextSelectableByMouse);
    senderValue->setWordWrap(true);
    form->addRow(QStringLiteral("From wallet"), senderValue);
    auto *recipientValue = new QLabel(
        QStringLiteral("<b>%1</b>").arg(recipientName.toHtmlEscaped()));
    recipientValue->setTextFormat(Qt::RichText);
    recipientValue->setTextInteractionFlags(Qt::TextSelectableByMouse);
    recipientValue->setWordWrap(true);
    form->addRow(QStringLiteral("Recipient"), recipientValue);
    auto *addressValue = new QLabel(
        QStringLiteral("<code>%1</code>").arg(recipientAddress.toHtmlEscaped()));
    addressValue->setTextFormat(Qt::RichText);
    addressValue->setTextInteractionFlags(Qt::TextSelectableByMouse);
    addressValue->setWordWrap(true);
    form->addRow(QStringLiteral("Recipient address"), addressValue);

    auto *amountEdit = new QLineEdit(
        rewardPayment ? forkmesh::tips::formatLamports(fixedLamports)
                            .remove(QStringLiteral(" SOL"))
                      : QStringLiteral("0.01"));
    amountEdit->setObjectName(QStringLiteral("tipAmountInput"));
    amountEdit->setMinimumWidth(0);
    amountEdit->setPlaceholderText(QStringLiteral("0.01"));
    amountEdit->setReadOnly(rewardPayment);
    auto *amountLabel = new QLabel(QStringLiteral("Amount (SOL)"));
    amountLabel->setBuddy(amountEdit);
    form->addRow(amountLabel, amountEdit);
    auto *usdEstimate = new QLabel(QString::fromUtf8("Loading USD estimate\xE2\x80\xA6"));
    usdEstimate->setObjectName(QStringLiteral("tipUsdEstimate"));
    usdEstimate->setToolTip(
        QStringLiteral("Approximate value using the current CoinGecko SOL-USD "
                       "spot price. The final value may change."));
    form->addRow(new QLabel(QStringLiteral("Value (USD)")), usdEstimate);
    auto *passphraseEdit = new QLineEdit;
    passphraseEdit->setObjectName(QStringLiteral("tipPaymentPassphraseInput"));
    passphraseEdit->setEchoMode(QLineEdit::Password);
    passphraseEdit->setMaxLength(1024);
    passphraseEdit->setMinimumWidth(0);
    passphraseEdit->setPlaceholderText(
        QStringLiteral("Enter your vault passphrase"));
    auto *passphraseLabel = new QLabel(QStringLiteral("Vault passphrase"));
    passphraseLabel->setBuddy(passphraseEdit);
    form->addRow(passphraseLabel, passphraseEdit);
    layout->addLayout(form);

    double solUsdPrice = 0.0;
    const auto refreshUsdEstimate = [amountEdit, usdEstimate, &solUsdPrice] {
        if (solUsdPrice <= 0.0)
            return;
        const QString estimate =
            tipUsdEstimate(amountEdit->text(), solUsdPrice);
        usdEstimate->setText(
            estimate.isEmpty() ? QStringLiteral("Enter a valid SOL amount")
                               : estimate);
    };
    connect(amountEdit, &QLineEdit::textChanged, &dialog,
            [refreshUsdEstimate] { refreshUsdEstimate(); });

    if (m_networkAccess) {
        QNetworkRequest priceRequest{
            QUrl(QString::fromLatin1(kSolUsdPriceEndpoint))};
        hardenTipRequest(priceRequest);
        QNetworkReply *priceReply = m_networkAccess->get(priceRequest);
        connect(priceReply, &QNetworkReply::finished, &dialog,
                [priceReply, usdEstimate, refreshUsdEstimate,
                 &solUsdPrice] {
            const int status = priceReply
                                   ->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                                   .toInt();
            const QByteArray body = boundedTipBody(priceReply);
            const bool networkOk =
                priceReply->error() == QNetworkReply::NoError && status == 200;
            solUsdPrice = networkOk ? solUsdSpotPrice(body) : 0.0;
            priceReply->deleteLater();
            if (solUsdPrice > 0.0)
                refreshUsdEstimate();
            else
                usdEstimate->setText(QStringLiteral("USD estimate unavailable"));
        });
        connect(&dialog, &QDialog::finished, priceReply, [priceReply] {
            if (priceReply->isRunning())
                priceReply->abort();
        });
    } else {
        usdEstimate->setText(QStringLiteral("USD estimate unavailable"));
    }

    auto *bounds = new QLabel(
        QStringLiteral("Between %1 and %2. Network fees are paid by your "
                       "wallet and are not included.")
            .arg(forkmesh::tips::formatLamports(
                     forkmesh::tips::minimumTipLamports()),
                 forkmesh::tips::formatLamports(
                     forkmesh::tips::maximumTipLamports())));
    bounds->setObjectName(QStringLiteral("statusLine"));
    bounds->setWordWrap(true);
    layout->addWidget(bounds);

    auto *warning = new QLabel(
        QStringLiteral(
            "On-chain transfers cannot be reversed. Your passphrase is used "
            "only in memory for this transfer; it is never saved or sent to "
            "ForkMesh."));
    warning->setObjectName(QStringLiteral("tipPaymentWarning"));
    warning->setWordWrap(true);
    layout->addWidget(warning);

    auto *problem = new QLabel;
    problem->setObjectName(QStringLiteral("statusLine"));
    problem->setWordWrap(true);
    problem->setVisible(false);
    layout->addWidget(problem);
    layout->addStretch(1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
                                         QDialogButtonBox::Cancel);
    QPushButton *send = buttons->button(QDialogButtonBox::Ok);
    send->setText(rewardPayment ? QStringLiteral("Send payment")
                                 : QStringLiteral("Send tip"));
    send->setEnabled(false);
    send->setDefault(true);
    polishTipDialogButtons(buttons, QDialogButtonBox::Ok);
    dialogLayout.outer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(passphraseEdit, &QLineEdit::textChanged, &dialog,
            [passphraseEdit, send] {
                send->setEnabled(!passphraseEdit->text().isEmpty());
            });
    quint64 lamports = 0;
    connect(buttons, &QDialogButtonBox::accepted, &dialog,
            [&dialog, amountEdit, passphraseEdit, problem,
             scroll = dialogLayout.scroll, &lamports] {
        QString error;
        if (!forkmesh::tips::parseSolAmount(amountEdit->text(), &lamports,
                                            &error)) {
            problem->setText(error);
            revealTipStatus(problem, scroll);
            return;
        }
        if (passphraseEdit->text().isEmpty()) {
            problem->setText(
                QStringLiteral("Enter your vault passphrase before sending."));
            revealTipStatus(problem, scroll);
            return;
        }
        dialog.accept();
    });
    if (dialog.exec() != QDialog::Accepted) {
        scrubTipEdit(passphraseEdit);
        return;
    }
    QString passphrase = passphraseEdit->text();
    scrubTipEdit(passphraseEdit);
    if (passphrase.isEmpty())
        return;
    // Keep a deep copy just long enough to reach the local signer. The form
    // and the short-lived local copy are wiped immediately. A modal dialog
    // pumps the event loop, so another transfer may own the wallet by now;
    // then this payment waits its turn rather than being dropped.
    TipTransferRequest request;
    request.recipientName = recipientName;
    request.recipientAddress = recipientAddress;
    request.lamports = lamports;
    request.rewardId = rewardId;
    request.reportToLedger = reportToLedger;
    if (reportToLedger)
        request.ledgerRecordId = m_ledgerPreparingRecordId;
    request.passphrase = QString(passphrase.constData(), passphrase.size());
    forkmesh::rewards::secureErase(passphrase);
    beginOrQueueTipTransfer(std::move(request));
}

void MainWindow::beginOrQueueTipTransfer(TipTransferRequest &&request)
{
    if (!m_tipInFlight) {
        startTipTransferRequest(std::move(request));
        return;
    }
    // Said to the log, and to the Ledger when the payment came from there,
    // without taking over the reporting of the transfer still in flight.
    const auto say = [this, &request](const QString &message) {
        const QString text = safeTipText(message);
        logSystem(text);
        if (request.reportToLedger) {
            m_ledgerPaymentLine = text;
            setLedgerStatus(text);
        }
    };
    const bool rewardAlreadyGoing =
        !request.rewardId.isEmpty() &&
        (request.rewardId == m_tipInFlightRewardId ||
         std::any_of(m_tipTransferQueue.cbegin(), m_tipTransferQueue.cend(),
                     [&request](const TipTransferRequest &queued) {
                         return queued.rewardId == request.rewardId;
                     }));
    if (rewardAlreadyGoing) {
        forkmesh::rewards::secureErase(request.passphrase);
        say(QStringLiteral("Reward %1 is already being paid from this "
                           "machine; nothing was queued.")
                .arg(request.rewardId));
        return;
    }
    const QString queuedLine =
        QStringLiteral("Queued %1 to %2. It is signed and sent as soon as the "
                       "payment in progress finishes.")
            .arg(forkmesh::tips::formatLamports(request.lamports),
                 request.recipientName);
    const bool reportToLedger = request.reportToLedger;
    m_tipTransferQueue.append(std::move(request));
    logSystem(queuedLine);
    if (reportToLedger) {
        m_ledgerPaymentLine = queuedLine;
        setLedgerStatus(queuedLine);
    }
    updateLedgerPayButtons();
}

void MainWindow::startTipTransferRequest(TipTransferRequest &&request)
{
    m_tipTaskDescription = request.taskDescription;
    m_tipOutputUrl = request.outputUrl;
    m_tipEnteredCurrency = request.enteredCurrency;
    m_tipEnteredUsdMicros = request.enteredUsdMicros;
    m_tipSolUsdPriceMicros = request.solUsdPriceMicros;
    m_tipPriceObservedAt = request.priceObservedAt;
    m_ledgerPaymentReporting = request.reportToLedger;
    m_ledgerPayingRecordId = request.ledgerRecordId;
    // A queued reward was checked when it was approved, against a receipt
    // the transfer before it still owned. That transfer has ended now, so
    // anything it left held, or a settlement it proved, refuses this one.
    if (!request.rewardId.isEmpty()) {
        const ContributorPayoutReceipt::Pending held =
            ContributorPayoutReceipt::read();
        if (held.blocking() || m_tipPaidRewardIds.contains(request.rewardId) ||
            ContributorPayoutReceipt::Settled::contains(request.rewardId)) {
            forkmesh::rewards::secureErase(request.passphrase);
            tipStatus(held.blocking()
                          ? ContributorPayoutReceipt::refusalSummary()
                          : QStringLiteral("Reward %1 was already paid from "
                                           "this machine. Nothing was signed.")
                                .arg(request.rewardId));
            return;
        }
    }
    // refreshTipWalletControls() wipes this copy on every exit.
    forkmesh::rewards::secureErase(m_tipOneShotPassphrase);
    m_tipOneShotPassphrase = QString(request.passphrase.constData(),
                                     request.passphrase.size());
    forkmesh::rewards::secureErase(request.passphrase);
    startTipTransfer(request.recipientName, request.recipientAddress,
                     request.lamports, request.rewardId);
}

void MainWindow::drainTipTransferQueue()
{
    while (!m_tipInFlight && !m_tipTransferQueue.isEmpty()) {
        startTipTransferRequest(m_tipTransferQueue.takeFirst());
        if (m_tipInFlight)
            break;
        // Refused, or failed before reaching the network: release the row it
        // would have owned, as payLedgerRecord does after a refusal, and let
        // the next approved payment have the wallet.
        m_ledgerPaymentStarted = false;
        m_ledgerPaymentProcessing = false;
        m_ledgerPayingRecordId.clear();
        m_ledgerPayingWalletAddress.clear();
        m_ledgerPaymentReporting = false;
        rebuildLedgerTable();
    }
    updateLedgerPayButtons();
}

bool MainWindow::heldPayoutIsInFlight(
    const ContributorPayoutReceipt::Pending &held) const
{
    return m_tipInFlight && !held.rewardId.isEmpty() &&
           held.rewardId == m_tipInFlightRewardId;
}

bool MainWindow::promptTipPassphrase(const QString &recipientName,
                                     quint64 lamports, QString *passphrase)
{
    QInputDialog unlock(this);
    unlock.setWindowTitle(QStringLiteral("Unlock your tip wallet"));
    unlock.resize(560, 240);
    unlock.setSizeGripEnabled(true);
    unlock.setLabelText(
        QStringLiteral(
            "Enter your vault passphrase to sign %1 to %2.\n\nIt is used "
            "in memory for this one transfer and is never saved or "
            "transmitted.")
            .arg(forkmesh::tips::formatLamports(lamports), recipientName));
    unlock.setTextEchoMode(QLineEdit::Password);
    unlock.setInputMode(QInputDialog::TextInput);
    // exec() pumps the event loop, so nothing captured by a caller may be
    // used by reference afterwards; every value handed in here is a copy.
    const bool unlocked = unlock.exec() == QDialog::Accepted;
    *passphrase = unlock.textValue();
    unlock.setTextValue(QString(passphrase->size(), QChar(u'\0')));
    unlock.setTextValue(QString());
    if (!unlocked)
        forkmesh::rewards::secureErase(*passphrase);
    return unlocked;
}

void MainWindow::startTipTransfer(const QString &recipientName,
                                  const QString &recipientAddress,
                                  quint64 lamports,
                                  const QString &rewardId, bool automated)
{
    if (m_tipInFlight)
        return;
    // A scheduled CFO run reaches here without going through sendTipToMember,
    // so it has to disown any reporting an earlier manual payment left set.
    if (automated) {
        m_ledgerPaymentReporting = false;
        m_tipTaskDescription.clear();
        m_tipOutputUrl.clear();
        m_tipEnteredCurrency = QStringLiteral("SOL");
        m_tipEnteredUsdMicros = 0;
        m_tipSolUsdPriceMicros = 0;
        m_tipPriceObservedAt = 0;
    }
    if (!m_networkAccess) {
        // The reader has already confirmed an irreversible transfer by this
        // point. Returning quietly here is what makes the button look dead.
        tipStatus(QStringLiteral(
            "This app has no network access, so nothing was signed and no "
            "payment was sent."));
        if (!automated) {
            forkmesh::rewards::secureErase(m_tipOneShotPassphrase);
            m_tipOneShotPassphrase.clear();
        }
        return;
    }
    m_tipInFlight = true;
    m_tipInFlightRewardId = rewardId;
    if (m_ledgerPaymentReporting && !m_ledgerPayingRecordId.isEmpty()) {
        m_ledgerPaymentStarted = true;
        m_ledgerPayingWalletAddress = forkmesh::tips::linkedTipAddress(
            forkmesh::tips::defaultTipVaultPath());
    }
    refreshTipWalletControls();
    tipStatus(QStringLiteral("Fetching a recent Solana blockhash before "
                             "signing the payment…"));
    signAndBroadcastTip(recipientName, recipientAddress, QString(), 0, lamports,
                        0, rewardId, automated);
}

void MainWindow::signAndBroadcastTip(const QString &recipientName,
                                     const QString &recipientAddress,
                                     const QString &blockhash,
                                     quint64 lastValidBlockHeight,
                                     quint64 lamports, int endpointIndex,
                                     const QString &rewardId, bool automated)
{
    // Phase one: walk the public endpoints until one hands back a blockhash.
    if (blockhash.isEmpty()) {
        if (endpointIndex >= kTipRpcEndpointCount || !m_networkAccess) {
            const QString detail = QStringLiteral(
                "No Solana RPC endpoint answered with a recent blockhash; "
                "nothing was signed and no payout was sent.");
            tipStatus(detail);
            if (automated)
                finishContributorPayoutTransfer(false, rewardId, lamports,
                                                detail);
            else {
                m_tipInFlight = false;
                refreshTipWalletControls();
            }
            return;
        }
        QNetworkRequest request{
            QUrl(QString::fromLatin1(kTipRpcEndpoints[endpointIndex]))};
        hardenTipRequest(request);
        const QJsonObject payload{
            {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), QStringLiteral("forkmesh-tip-blockhash")},
            {QStringLiteral("method"), QStringLiteral("getLatestBlockhash")},
            {QStringLiteral("params"),
             QJsonArray{QJsonObject{{QStringLiteral("commitment"),
                                     QStringLiteral("finalized")}}}}};
        QNetworkReply *reply = m_networkAccess->post(
            request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this,
                [this, reply, recipientName, recipientAddress, lamports,
                 endpointIndex, rewardId, automated] {
            const QByteArray body = boundedTipBody(reply);
            const bool networkOk = reply->error() == QNetworkReply::NoError;
            reply->deleteLater();
            const QJsonObject value =
                QJsonDocument::fromJson(body)
                    .object()
                    .value(QStringLiteral("result"))
                    .toObject()
                    .value(QStringLiteral("value"))
                    .toObject();
            const QString hash =
                value.value(QStringLiteral("blockhash")).toString().trimmed();
            const quint64 height = quint64(
                value.value(QStringLiteral("lastValidBlockHeight"))
                    .toVariant()
                    .toULongLong());
            if (!networkOk || hash.isEmpty() || height == 0) {
                const auto retry =
                    [this, recipientName, recipientAddress, lamports,
                     endpointIndex, rewardId, automated] {
                        signAndBroadcastTip(
                            recipientName, recipientAddress, QString(), 0,
                            lamports, endpointIndex + 1, rewardId, automated);
                    };
                if (automated) {
                    QTimer::singleShot(
                        forkmesh::tips::contributorPayoutRetryDelayMs(), this,
                        retry);
                } else {
                    retry();
                }
                return;
            }
            signAndBroadcastTip(recipientName, recipientAddress, hash, height,
                                lamports, endpointIndex, rewardId, automated);
        });
        return;
    }

    // Phase two: build, unlock, sign. Every rejection here leaves the wallet
    // untouched and nothing on the network.
    if (endpointIndex < 0 || endpointIndex >= kTipRpcEndpointCount) {
        const QString detail = QStringLiteral(
            "Lost track of the Solana endpoint that issued the blockhash; "
            "nothing was signed.");
        tipStatus(detail);
        if (automated)
            finishContributorPayoutTransfer(false, rewardId, lamports, detail);
        else {
            m_tipInFlight = false;
            refreshTipWalletControls();
        }
        return;
    }
    const QString vaultPath = forkmesh::tips::defaultTipVaultPath();
    const QString sender = forkmesh::tips::linkedTipAddress(vaultPath);
    forkmesh::tips::TipTransfer transfer;
    QString error;
    if (sender.isEmpty() ||
        !forkmesh::tips::buildTipTransfer(sender, recipientAddress, lamports,
                                          blockhash, lastValidBlockHeight,
                                          &transfer, &error)) {
        const QString detail =
            error.isEmpty()
                ? QStringLiteral(
                      "The payout transfer could not be built; nothing was signed.")
                : safeTipText(error);
        tipStatus(detail);
        if (automated)
            finishContributorPayoutTransfer(false, rewardId, lamports, detail);
        else {
            m_tipInFlight = false;
            refreshTipWalletControls();
        }
        return;
    }

    QString passphrase;
    if (automated) {
        const bool authorized =
            !m_contributorAutoPayoutPassphrase.isEmpty() &&
            !m_contributorAutoPayoutWallet.isEmpty() &&
            sender == m_contributorAutoPayoutWallet &&
            m_contributorAutoPayoutScope ==
                m_contributorPayoutTeammatesDirectory &&
            rewardId == m_contributorPayoutCurrentRewardId &&
            lamports == m_contributorPayoutCurrentLamports;
        if (!authorized) {
            const QString detail = QStringLiteral(
                "CFO payout authorization changed; nothing was signed.");
            tipStatus(detail);
            finishContributorPayoutTransfer(false, rewardId, lamports, detail);
            return;
        }
        passphrase = m_contributorAutoPayoutPassphrase;
    } else {
        if (!m_tipOneShotPassphrase.isEmpty()) {
            // The review dialog keeps a deep copy only while the public
            // blockhash is fetched. Move a separate copy into the signer and
            // wipe the stored one before any local key work begins.
            passphrase = QString(m_tipOneShotPassphrase.constData(),
                                 m_tipOneShotPassphrase.size());
            forkmesh::rewards::secureErase(m_tipOneShotPassphrase);
            m_tipOneShotPassphrase.clear();
        } else {
            // This is a compatibility path for programmatic callers that
            // start a manual transfer without going through the combined
            // review form. All user-facing Send payment and Send tip actions
            // provide their passphrase in that form, so they open no second
            // unlock window.
            if (!promptTipPassphrase(recipientName, lamports, &passphrase)) {
                // Said before the controls are re-armed, as on every other
                // exit from this flow: refreshTipWalletControls() is where a
                // payment hands its reporting back, and this line is still
                // the payment's.
                tipStatus(QStringLiteral("Tip cancelled; nothing was signed."));
                m_tipInFlight = false;
                refreshTipWalletControls();
                return;
            }
        }
        if (passphrase.isEmpty()) {
            forkmesh::rewards::secureErase(passphrase);
            tipStatus(QStringLiteral("Tip cancelled; nothing was signed."));
            m_tipInFlight = false;
            refreshTipWalletControls();
            return;
        }
        // The reader has already reviewed the transfer and entered its
        // passphrase in the same dialog. Show that the request now owns the
        // originating Ledger row before the synchronous local signature.
        tipStatus(QString::fromUtf8("Signing the payment locally\xE2\x80\xA6"));
        markLedgerPaymentProcessing(sender);
    }

    forkmesh::tips::SignedTip signedTip;
    const bool signedOk = forkmesh::tips::signTipTransfer(
        transfer, passphrase, vaultPath, &signedTip, &error);
    forkmesh::rewards::secureErase(passphrase);
    if (!signedOk) {
        const QString detail = safeTipText(
            error.isEmpty()
                ? QStringLiteral(
                      "The payout could not be signed; nothing was sent.")
                : error);
        tipStatus(detail);
        if (automated)
            finishContributorPayoutTransfer(false, rewardId, lamports, detail);
        else {
            m_tipInFlight = false;
            refreshTipWalletControls();
        }
        return;
    }

    // Phase three: broadcast. Only ever to the endpoint that produced the
    // blockhash this transaction is bound to.
    QNetworkRequest request{
        QUrl(QString::fromLatin1(kTipRpcEndpoints[endpointIndex]))};
    hardenTipRequest(request);
    const QJsonObject payload{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), QStringLiteral("forkmesh-tip-send")},
        {QStringLiteral("method"), QStringLiteral("sendTransaction")},
        {QStringLiteral("params"),
         QJsonArray{signedTip.signedTransactionBase64,
                    QJsonObject{
                        {QStringLiteral("encoding"), QStringLiteral("base64")},
                        {QStringLiteral("skipPreflight"), false},
                        {QStringLiteral("preflightCommitment"),
                         QStringLiteral("finalized")},
                        {QStringLiteral("maxRetries"), 3}}}}};
    tipStatus(QStringLiteral("Signed locally; broadcasting %1 to %2…")
                  .arg(forkmesh::tips::formatLamports(lamports), recipientName));
    const QString expectedSignature = signedTip.chainSignature;
    // Persist only public receipt fields before the irreversible network
    // attempt. If the RPC response is lost, a later CFO run reconciles this
    // exact signature instead of ever signing a replacement payment.
    if (!rewardId.isEmpty() &&
        !savePendingContributorReceipt(recipientName, lamports,
                                       expectedSignature, sender, rewardId,
                                       automated)) {
        const QString detail = QStringLiteral(
            "The signed payout was not broadcast because its public reconciliation receipt could not be saved safely.");
        tipStatus(detail);
        if (automated)
            finishContributorPayoutTransfer(false, rewardId, lamports, detail);
        else {
            m_tipInFlight = false;
            refreshTipWalletControls();
        }
        return;
    }
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, recipientName, lamports, expectedSignature, sender,
             rewardId, automated] {
        const QByteArray body = boundedTipBody(reply);
        const bool networkOk = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();
        const QJsonObject response = QJsonDocument::fromJson(body).object();
        const QString returned =
            response.value(QStringLiteral("result")).toString();
        if (!networkOk || returned != expectedSignature) {
            const QString code =
                response.value(QStringLiteral("error"))
                    .toObject()
                    .value(QStringLiteral("code"))
                    .toVariant()
                    .toString();
            const QString detail =
                code.isEmpty()
                    ? QStringLiteral(
                          "The RPC did not confirm the signed payout. Its public signature was saved for reconciliation; no replacement will be signed.")
                    : QStringLiteral(
                          "The RPC did not confirm the signed payout (error %1). Its public signature was saved for reconciliation; no replacement will be signed.")
                          .arg(code);
            tipStatus(detail);
            if (automated)
                finishContributorPayoutTransfer(false, rewardId, lamports,
                                                detail);
            else {
                m_tipInFlight = false;
                refreshTipWalletControls();
            }
            return;
        }
        tipStatus(QStringLiteral("Submitted %1 to %2 (%3). Confirming finality…")
                      .arg(forkmesh::tips::formatLamports(lamports),
                           recipientName, expectedSignature));
        announceTip(recipientName, lamports, expectedSignature, sender,
                    rewardId, automated);
    });
}

void MainWindow::announceTip(const QString &recipientName, quint64 lamports,
                             const QString &chainSignature,
                             const QString &sourceAddress,
                             const QString &rewardId, bool automated,
                             int attempt)
{
    // The transfer is already signed at this point. For an ordinary tip the
    // notice is a courtesy; for a reviewed reward this is also the required
    // exact-signature reconciliation that moves its transaction record to
    // paid. The public receipt remains saved until that paid response arrives.
    // A scheduled CFO run can resume while deferred startup is still opening
    // the saved repository. Silent authentication runs immediately after that
    // restore. Wait for its authoritative result instead of treating the
    // temporary empty token as a signed-out machine and failing an already
    // broadcast transfer.
    if (automated && !m_startupAuthResolved) {
        if (!m_contributorPayoutWaitingForStartupAuth) {
            m_contributorPayoutWaitingForStartupAuth = true;
            tipStatus(QStringLiteral(
                "The payout is on-chain; waiting for startup sign-in before "
                "reconciling its saved public signature."));
        }
        QTimer::singleShot(
            kPendingReceiptStartupAuthPollMs, this,
            [this, recipientName, lamports, chainSignature, sourceAddress,
             rewardId, automated, attempt] {
                announceTip(recipientName, lamports, chainSignature,
                            sourceAddress, rewardId, automated, attempt);
            });
        return;
    }
    m_contributorPayoutWaitingForStartupAuth = false;
    if (!m_networkAccess) {
        const QString detail = QStringLiteral(
            "The payout is signed, but its ForkMesh transaction record cannot be reached. The public receipt is saved for reconciliation.");
        tipStatus(detail);
        if (automated)
            finishContributorPayoutTransfer(false, rewardId, lamports, detail);
        else {
            m_tipInFlight = false;
            refreshTipWalletControls();
        }
        return;
    }
    const QUrl endpoint =
        !rewardId.isEmpty() && m_contributorPayoutReceiptEndpoint.isValid()
            ? m_contributorPayoutReceiptEndpoint
            : accountsApiUrl(QStringLiteral("tip-notice"));
    QNetworkRequest request;
    if (!prepareContributorNoticeRequest(endpoint, recipientName, lamports,
                                         chainSignature, rewardId,
                                         sourceAddress, &request)) {
        // Neither a session token nor an account key this app may sign with.
        // A reward payout is signed with a wallet on this disk, so having no
        // ForkMesh credential says nothing about whether its transfer landed —
        // and that, not the credential, is what the hold is waiting on. Ask
        // the chain: a finalized signature settles the receipt here and keeps
        // the queue moving, with the undelivered notice queued behind it. Only
        // an ordinary tip, which holds nothing, ends at this line.
        if (!rewardId.isEmpty()) {
            settlePendingContributorPayoutOnChain(recipientName, lamports,
                                                  chainSignature, sourceAddress,
                                                  rewardId, automated);
            return;
        }
        const QString detail = QStringLiteral(
            "The tip is on-chain, but this machine holds no ForkMesh "
            "credential to report it with, so %1 could not be emailed about "
            "it. The transaction signature is %2.")
                                   .arg(recipientName, chainSignature);
        tipStatus(detail);
        if (automated)
            finishContributorPayoutTransfer(false, rewardId, lamports, detail);
        else {
            m_tipInFlight = false;
            refreshTipWalletControls();
        }
        return;
    }
    QJsonObject payload{
        {QStringLiteral("toNodeName"), recipientName.toLower()},
        // Capped well below 2^53 by maximumTipLamports(), so this is exact.
        {QStringLiteral("lamports"), qint64(lamports)},
        {QStringLiteral("signature"), chainSignature},
        {QStringLiteral("network"), QStringLiteral("mainnet")}};
    if (!m_tipTaskDescription.isEmpty()) {
        payload.insert(QStringLiteral("taskDescription"),
                       m_tipTaskDescription);
        payload.insert(QStringLiteral("outputUrl"),
                       m_tipOutputUrl.toString(QUrl::FullyEncoded));
        payload.insert(QStringLiteral("enteredCurrency"),
                       m_tipEnteredCurrency);
        payload.insert(QStringLiteral("enteredUsdMicros"),
                       m_tipEnteredUsdMicros);
        payload.insert(QStringLiteral("solUsdPriceMicros"),
                       m_tipSolUsdPriceMicros);
        payload.insert(QStringLiteral("priceObservedAt"),
                       m_tipPriceObservedAt);
        payload.insert(QStringLiteral("fromAddress"), sourceAddress);
    }
    if (!rewardId.isEmpty()) {
        payload.insert(QStringLiteral("rewardId"), rewardId);
        payload.insert(QStringLiteral("fromAddress"), sourceAddress);
    }
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, recipientName, lamports, chainSignature,
             sourceAddress, rewardId, automated, attempt] {
        const QByteArray body = boundedTipBody(reply);
        const bool networkOk = reply->error() == QNetworkReply::NoError;
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        bool retryAfterOk = false;
        const int retryAfterSeconds =
            QString::fromLatin1(reply->rawHeader("Retry-After"))
                .trimmed()
                .toInt(&retryAfterOk);
        reply->deleteLater();
        const QJsonObject response = QJsonDocument::fromJson(body).object();
        if (networkOk && status == 202 &&
            (!rewardId.isEmpty() || !m_tipTaskDescription.isEmpty())
            && attempt < kContributorReceiptAttemptLimit) {
            tipStatus(QStringLiteral(
                "Transfer %1 is on-chain and awaiting finality; "
                "ForkMesh will verify it again shortly.")
                          .arg(chainSignature));
            const int delay =
                automated
                    ? forkmesh::tips::contributorPayoutFinalityPollMs()
                    : 5000;
            QTimer::singleShot(delay, this,
                               [this, recipientName, lamports, chainSignature,
                                sourceAddress, rewardId, automated, attempt] {
                announceTip(recipientName, lamports, chainSignature,
                            sourceAddress, rewardId, automated, attempt + 1);
            });
            return;
        }
        const bool rewardPaid =
            rewardId.isEmpty() ||
            response.value(QStringLiteral("rewardStatus")).toString() ==
                QLatin1String("paid");
        // A 202 body deliberately carries {ok:true}: it means the relay has
        // accepted the signature for another finality check, not that Solana
        // finalized it. Requiring a completed 2xx response here prevents an
        // exhausted poll loop from announcing a dropped transaction as paid.
        const bool responseOk = forkmesh::tips::noticeResponseConfirmsFinality(
            networkOk, status,
            response.value(QStringLiteral("ok")).toBool(), rewardPaid);
        const bool transient = status == 0 || status == 429 || status >= 500 ||
                               (status == 202 &&
                                (!rewardId.isEmpty() ||
                                 !m_tipTaskDescription.isEmpty()));
        if (!responseOk && (automated || !m_tipTaskDescription.isEmpty()) &&
            transient &&
            attempt < kContributorReceiptAttemptLimit) {
            const int delay = qMax(
                forkmesh::tips::contributorPayoutRetryDelayMs(),
                retryAfterOk ? qBound(1, retryAfterSeconds, 15 * 60) * 1000
                             : 0);
            tipStatus(QStringLiteral(
                "The payout receipt is temporarily unavailable; retrying the same public signature in %1 seconds.")
                          .arg(delay / 1000));
            QTimer::singleShot(
                delay, this,
                [this, recipientName, lamports, chainSignature, sourceAddress,
                 rewardId, automated, attempt] {
                    announceTip(recipientName, lamports, chainSignature,
                                sourceAddress, rewardId, automated,
                                attempt + 1);
                });
            return;
        }
        if (!responseOk) {
            // Out of retries, or a relay that will not agree this reward is
            // paid. Either way the transfer itself is not in doubt here — ask
            // the chain rather than holding every other payout on the relay's
            // account of one that may well have landed.
            if (!rewardId.isEmpty()) {
                settlePendingContributorPayoutOnChain(
                    recipientName, lamports, chainSignature, sourceAddress,
                    rewardId, automated);
                return;
            }
            const bool finalityPending = networkOk && status == 202;
            const QString detail = finalityPending
                ? QStringLiteral(
                      "Solana did not confirm %1 before the finality checks expired. The payment was not recorded in the Ledger; verify the signature and wallet balance before deciding whether to send again.")
                      .arg(chainSignature)
                : (m_tipTaskDescription.isEmpty()
                       ? QStringLiteral(
                             "The tip was submitted as %1, but ForkMesh could not confirm or notify %2. Verify the signature before sending again.")
                             .arg(chainSignature, recipientName)
                       : QStringLiteral(
                             "The payment was submitted as %1, but ForkMesh could not confirm it or add a work receipt to the Ledger.")
                             .arg(chainSignature));
            tipStatus(detail);
            if (automated)
                finishContributorPayoutTransfer(false, rewardId, lamports,
                                                detail);
            else {
                m_tipInFlight = false;
                refreshTipWalletControls();
            }
            return;
        }

        if (!rewardId.isEmpty()) {
            const QJsonValue balance =
                response.value(QStringLiteral("sourceBalanceLamports"));
            if (balance.isDouble()) {
                applyLedgerWalletBalance(
                    sourceAddress, qint64(balance.toDouble()),
                    qint64(response
                               .value(QStringLiteral(
                                   "sourceBalanceCheckedAt"))
                               .toDouble()));
            }
            clearPendingContributorReceipt(rewardId, chainSignature);
            m_tipPaidRewardIds.insert(rewardId);
            // The relay has verified this exact finalized transfer, so the
            // issue or pull it paid for can carry the same signature. Without
            // this the repository record still reads "ready to pay" while the
            // public ledger says paid.
            recordRewardReceiptLocally(rewardId, chainSignature);
        }
        // Said before the controls are re-armed, as on every failing exit from
        // this flow: refreshTipWalletControls() is where a payment hands its
        // reporting back, and the one line the reader pressed the button for
        // is this one. Saying it afterwards would leave the Ledger showing
        // "waiting for the network" until the refresh below wiped that too.
        const bool workPayment = !m_tipTaskDescription.isEmpty();
        tipStatus(response.value(QStringLiteral("emailed")).toBool()
                      ? (workPayment
                             ? QStringLiteral(
                                   "Paid %1 for completed work; the finalized receipt is now in the Ledger and they were emailed.")
                                   .arg(recipientName)
                             : QStringLiteral("%1 has been emailed that a tip is on the way.")
                                   .arg(recipientName))
                      : QStringLiteral(
                            "%1 has been notified in ForkMesh%2. No email was "
                            "sent — they have no verified address, or have "
                            "turned these emails off.")
                            .arg(recipientName,
                                 workPayment
                                     ? QStringLiteral(
                                           "; the finalized work receipt is now in the Ledger")
                                     : QString())
                      );
        m_tipInFlight = false;
        refreshTipWalletControls();
        if (!rewardId.isEmpty() && automated) {
            finishContributorPayoutTransfer(
                true, rewardId, lamports,
                QStringLiteral("Paid and reconciled %1 · %2 · %3")
                    .arg(rewardId.left(32),
                         forkmesh::tips::formatLamports(lamports),
                         chainSignature.left(20)));
        } else if (!rewardId.isEmpty()) {
            fetchContributorRewards();
            refreshLedgerPage(true);
        } else if (workPayment) {
            refreshLedgerPage(true);
        }
    });
}

// --- Tipping straight from the Users directory ------------------------------
//
// Most members are discovered in the Users table rather than by opening a
// profile, so the same "Send tip" action lives in that header and acts on the
// selected row. Columns are matched by header text: the table is user-sortable
// and its column order is not fixed at build time.

namespace {

QString usersRowCell(const QTableWidget *table, int row, const QString &header)
{
    if (!table || row < 0 || row >= table->rowCount())
        return {};
    for (int column = 0; column < table->columnCount(); ++column) {
        const QTableWidgetItem *label = table->horizontalHeaderItem(column);
        if (!label || label->text() != header)
            continue;
        const QTableWidgetItem *cell = table->item(row, column);
        return cell ? cell->text().trimmed() : QString();
    }
    return {};
}

} // namespace

bool MainWindow::selectedUsersTipTarget(QString *name, QString *address) const
{
    if (name)
        name->clear();
    if (address)
        address->clear();
    if (!m_usersTable || !m_usersTable->selectionModel() ||
        m_usersTable->selectionModel()->selectedRows().size() != 1)
        return false;
    const int row = m_usersTable->currentRow();
    const QString rowName = usersRowCell(m_usersTable, row, QStringLiteral("User"));
    const QString rowAddress =
        usersRowCell(m_usersTable, row, QStringLiteral("Solana"));
    if (rowName.isEmpty() || !forkmesh::tips::isValidTipAddress(rowAddress))
        return false;
    if (name)
        *name = rowName;
    if (address)
        *address = rowAddress;
    return true;
}

void MainWindow::refreshUsersTipButton()
{
    if (!m_usersTipButton)
        return;
    QString name;
    QString address;
    const bool tippable = selectedUsersTipTarget(&name, &address);
    const int selectedCount =
        m_usersTable && m_usersTable->selectionModel()
            ? m_usersTable->selectionModel()->selectedRows().size()
            : 0;
    m_usersTipButton->setEnabled(
        tippable &&
        !m_usersQuarantineInFlight && !m_usersDeleteInFlight);
    if (selectedCount == 0) {
        m_usersTipButton->setToolTip(
            QStringLiteral("Select a user to send them a SOL tip."));
        return;
    }
    if (selectedCount > 1) {
        m_usersTipButton->setToolTip(QStringLiteral(
            "Select exactly one user to send a SOL tip. Quarantine and Delete "
            "can act on multiple selected users."));
        return;
    }
    if (!tippable) {
        m_usersTipButton->setToolTip(QStringLiteral(
            "This account has not published a Solana payout address, so there "
            "is nowhere to send a tip."));
        return;
    }
    m_usersTipButton->setToolTip(
        QStringLiteral("Sign a SOL transfer to %1's public payout address with "
                       "your linked wallet and broadcast it. ForkMesh emails "
                       "them that a tip is on the way.")
            .arg(name));
}

void MainWindow::sendTipToSelectedUser()
{
    QString name;
    QString address;
    if (!selectedUsersTipTarget(&name, &address)) {
        QMessageBox::information(
            this, QStringLiteral("Send a tip"),
            QStringLiteral(
                "Select a user whose Solana column shows a payout address. "
                "Members who have not published one cannot be tipped."));
        return;
    }
    sendTipToMember(name, address);
}
