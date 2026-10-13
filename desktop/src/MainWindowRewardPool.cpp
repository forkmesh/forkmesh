// First-instance-owner community reward-pool controls. The Worker creates
// auditable public transfer intents; this desktop verifies and signs one exact
// System Program transfer after explicit owner confirmation, broadcasts it
// directly to the locally configured Solana RPC, then reports finalized public
// chain state back to the Worker. Private key bytes never cross the encrypted
// vault/signer boundary.

#include "MainWindow.h"

#include "ContributorPayoutReceipt.h"
#include "MainWindowInternal.h"
#include "RewardPoolSigner.h"
#include "TipSigner.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonDocument>
#include <QMessageBox>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QTimer>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>
#include <utility>

using namespace forkmesh::ui;

namespace {

const QString kRewardRpcSetting =
    QStringLiteral("control/rewardPool/publicRpc");
const QString kRewardNetworkSetting =
    QStringLiteral("control/rewardPool/network");
const QString kPendingIntentSetting =
    QStringLiteral("control/rewardPool/pendingIntent");
const QString kPendingSignatureSetting =
    QStringLiteral("control/rewardPool/pendingSignature");
const QString kPendingPoolSetting =
    QStringLiteral("control/rewardPool/pendingPool");
const QString kPendingRpcSetting =
    QStringLiteral("control/rewardPool/pendingRpc");
const QString kPendingNetworkSetting =
    QStringLiteral("control/rewardPool/pendingNetwork");
const QString kPendingSubmittedAtSetting =
    QStringLiteral("control/rewardPool/pendingSubmittedAt");
const QString kContributorPayoutDaySetting =
    QStringLiteral("control/contributorPayout/day");
const QString kContributorPayoutPaidSetting =
    QStringLiteral("control/contributorPayout/paidLamports");

constexpr int kMaximumWorkerResponse = 1024 * 1024;
constexpr int kFinalizationPollLimit = 36;
constexpr int kFinalizationPollMs = 5000;

// Microjob payouts are fixed, tiny amounts for work the relay already
// verified, and a contributor should not have to wait for the owner to be at
// their desk. The owner may therefore arm them once per session. Two things
// keep that from becoming a blank cheque: the session passphrase is held only
// in memory and only after an explicit arm, and no more than this much may be
// auto-signed in one local day. Past the cap the plans simply wait for the
// ordinary reviewed path.
constexpr quint64 kMicrojobAutoPayoutDailyCapLamports = 200000000ull; // 0.2 SOL
const QString kMicrojobPayoutKind = QStringLiteral("microjob_payout");

QFrame *rewardCard()
{
    auto *card = new QFrame;
    card->setObjectName(QStringLiteral("controlTabPane"));
    card->setFrameShape(QFrame::NoFrame);
    return card;
}

QLabel *rewardHint(const QString &text)
{
    auto *label = new QLabel(text);
    label->setObjectName(QStringLiteral("mutedLabel"));
    label->setWordWrap(true);
    label->setProperty("fmControlHelp", true);
    return label;
}

QString formatLamports(quint64 lamports)
{
    const quint64 whole = lamports / 1000000000ull;
    QString fraction =
        QString::number(lamports % 1000000000ull).rightJustified(
            9, QLatin1Char('0'));
    while (fraction.endsWith(QLatin1Char('0')) && fraction.size() > 1)
        fraction.chop(1);
    return QStringLiteral("%1.%2 SOL").arg(whole).arg(fraction);
}

void scrubEdit(QLineEdit *edit)
{
    if (!edit)
        return;
    QString overwrite(edit->text().size(), QChar(u'\0'));
    edit->setText(overwrite);
    edit->clear();
    forkmesh::rewards::secureErase(overwrite);
}

void hardenJsonRequest(QNetworkRequest &request)
{
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("Cache-Control", "no-store");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::SameOriginRedirectPolicy);
    request.setTransferTimeout(15000);
}

QByteArray boundedReplyBody(QNetworkReply *reply)
{
    if (!reply)
        return {};
    const QByteArray body = reply->read(kMaximumWorkerResponse + 1);
    return body.size() <= kMaximumWorkerResponse ? body : QByteArray();
}

QString publicNetworkFailure(QNetworkReply *reply, const QString &operation)
{
    if (!reply)
        return operation + QStringLiteral(" failed.");
    const int status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 404 || status == 405)
        return operation +
               QStringLiteral(
                   " is unavailable on this Worker; install the reward-intent API contract.");
    if (status == 401 || status == 403)
        return operation +
               QStringLiteral(
                   " was denied: this node identity is not authorized as the first-instance owner.");
    if (status > 0)
        return QStringLiteral("%1 failed (HTTP %2).").arg(operation).arg(status);
    return operation + QStringLiteral(" failed before an HTTP response.");
}

QString selectedIntentId(QTableWidget *table)
{
    if (!table)
        return {};
    const int row = table->currentRow();
    if (row < 0 || !table->item(row, 0))
        return {};
    return table->item(row, 0)->data(Qt::UserRole).toString();
}

QString jsonSlotString(const QJsonValue &value)
{
    if (value.isString())
        return value.toString();
    if (value.isDouble()) {
        const double slot = value.toDouble(-1);
        if (slot >= 0 && slot <= 9007199254740991.0 &&
            std::floor(slot) == slot)
            return QString::number(quint64(slot));
    }
    return {};
}

QString contributorRecipientDiscordIdentity(const QJsonObject &reward)
{
    QString username = reward.value(
        QStringLiteral("recipientDiscordUsername")).toString().trimmed();
    while (username.startsWith(QLatin1Char('@')))
        username.remove(0, 1);
    const QString displayName = reward.value(
        QStringLiteral("recipientDiscordDisplayName")).toString().trimmed();
    if (!username.isEmpty() && !displayName.isEmpty()) {
        return QStringLiteral("Discord @%1 (%2)").arg(username, displayName);
    }
    if (!username.isEmpty())
        return QStringLiteral("Discord @%1").arg(username);
    return displayName.isEmpty()
               ? QString()
               : QStringLiteral("Discord %1").arg(displayName);
}

} // namespace

QWidget *MainWindow::buildRewardPoolControlCard()
{
    QFrame *card = rewardCard();
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    layout->addWidget(rewardHint(
        QStringLiteral(
            "First-instance owner only. Import an existing Solana pool key into "
            "this desktop's passphrase-encrypted, owner-permission-only vault. "
            "ForkMesh never generates the key, uploads it to a Worker or D1, "
            "writes it to logs, or holds community members' funds. Only the "
            "public pool address and public on-chain transaction state leave "
            "this device. Rewards are community incentives, not investments, "
            "and visual fountain effects are not transactions.")));

    layout->addWidget(rewardHint(
        QStringLiteral(
            "Issue and pull-request payments are owner-authored estimates, "
            "not contributor claims. Pending and paid records stay together "
            "here; a paid row is retained with its transaction receipt after "
            "the relay verifies the exact finalized transfer.")));
    m_contributorRewardsTable = new QTableWidget(0, 6);
    m_contributorRewardsTable->setObjectName(
        QStringLiteral("contributorRewardsTable"));
    m_contributorRewardsTable->setHorizontalHeaderLabels(
        {QStringLiteral("Issue / PR"), QStringLiteral("Recipient"),
         QStringLiteral("Estimate"), QStringLiteral("Condition"),
         QStringLiteral("Status"), QStringLiteral("Transaction")});
    m_contributorRewardsTable->verticalHeader()->setVisible(false);
    m_contributorRewardsTable->setSelectionBehavior(
        QAbstractItemView::SelectRows);
    m_contributorRewardsTable->setSelectionMode(
        QAbstractItemView::SingleSelection);
    m_contributorRewardsTable->setEditTriggers(
        QAbstractItemView::NoEditTriggers);
    m_contributorRewardsTable->setShowGrid(false);
    m_contributorRewardsTable->setWordWrap(false);
    m_contributorRewardsTable->setTextElideMode(Qt::ElideMiddle);
    m_contributorRewardsTable->horizontalHeader()->setSectionResizeMode(
        0, QHeaderView::Stretch);
    for (int column = 1; column < 6; ++column) {
        m_contributorRewardsTable->horizontalHeader()->setSectionResizeMode(
            column, QHeaderView::ResizeToContents);
    }
    connect(m_contributorRewardsTable,
            &QTableWidget::itemSelectionChanged, this,
            &MainWindow::refreshRewardPoolControls);
    layout->addWidget(m_contributorRewardsTable);
    auto *contributorActions = new QHBoxLayout;
    m_contributorRewardsFetchButton =
        new QPushButton(QStringLiteral("Refresh payments"));
    m_contributorRewardsFetchButton->setObjectName(
        QStringLiteral("contributorRewardsFetchButton"));
    setOcticon(m_contributorRewardsFetchButton, QStringLiteral("sync"), 14);
    connect(m_contributorRewardsFetchButton, &QPushButton::clicked, this,
            &MainWindow::fetchContributorRewards);
    contributorActions->addWidget(m_contributorRewardsFetchButton);
    m_contributorRewardsPayButton =
        new QPushButton(QStringLiteral("Pay selected from app wallet"));
    m_contributorRewardsPayButton->setObjectName(
        QStringLiteral("contributorRewardsPayButton"));
    m_contributorRewardsPayButton->setProperty("buttonSize", "primary");
    setOcticon(m_contributorRewardsPayButton,
               QStringLiteral("credit-card"), 14);
    connect(m_contributorRewardsPayButton, &QPushButton::clicked, this,
            &MainWindow::paySelectedContributorReward);
    contributorActions->addWidget(m_contributorRewardsPayButton);
    contributorActions->addStretch();
    layout->addLayout(contributorActions);

    auto *cfoActions = new QHBoxLayout;
    m_contributorAutoPayoutButton =
        new QPushButton(QStringLiteral("Arm CFO daily payouts"));
    m_contributorAutoPayoutButton->setObjectName(
        QStringLiteral("contributorAutoPayoutButton"));
    m_contributorAutoPayoutButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_contributorAutoPayoutButton, QStringLiteral("shield-check"),
               14);
    connect(m_contributorAutoPayoutButton, &QPushButton::clicked, this,
            &MainWindow::toggleContributorAutoPayouts);
    cfoActions->addWidget(m_contributorAutoPayoutButton);
    m_contributorAutoPayoutStatus = new QLabel;
    m_contributorAutoPayoutStatus->setObjectName(QStringLiteral("mutedLabel"));
    m_contributorAutoPayoutStatus->setWordWrap(true);
    cfoActions->addWidget(m_contributorAutoPayoutStatus, 1);
    layout->addLayout(cfoActions);
    layout->addWidget(rewardHint(
        QStringLiteral(
            "The CFO special agent uses only the Solana wallet linked in this app. "
            "Arming lasts for this app session, sends one reviewed reward at "
            "a time, waits for the matching finalized transaction record, "
            "and pauses at least one minute before the next payment. The "
            "passphrase is never saved.")));
    m_contributorRewardsStatus = new QLabel(
        QStringLiteral(
            "Refresh to load owner-reviewed issue and PR payment history."));
    m_contributorRewardsStatus->setObjectName(QStringLiteral("mutedLabel"));
    m_contributorRewardsStatus->setWordWrap(true);
    layout->addWidget(m_contributorRewardsStatus);

    m_rewardPoolVaultStatus = new QLabel;
    m_rewardPoolVaultStatus->setObjectName(
        QStringLiteral("rewardPoolVaultStatus"));
    m_rewardPoolVaultStatus->setWordWrap(true);
    layout->addWidget(m_rewardPoolVaultStatus);

    auto *addressRow = new QHBoxLayout;
    auto *addressTitle = new QLabel(QStringLiteral("Public pool address"));
    addressTitle->setObjectName(QStringLiteral("mutedLabel"));
    addressRow->addWidget(addressTitle);
    m_rewardPoolAddress = new QLabel(QString::fromUtf8("\xE2\x80\x94"));
    m_rewardPoolAddress->setObjectName(
        QStringLiteral("rewardPoolPublicAddress"));
    m_rewardPoolAddress->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_rewardPoolAddress->setWordWrap(true);
    addressRow->addWidget(m_rewardPoolAddress, 1);
    auto *importButton =
        new QPushButton(QStringLiteral("Import existing pool key"));
    importButton->setObjectName(QStringLiteral("rewardPoolImportButton"));
    importButton->setCursor(Qt::PointingHandCursor);
    setOcticon(importButton, QStringLiteral("key"), 14);
    connect(importButton, &QPushButton::clicked, this,
            &MainWindow::importRewardPoolKey);
    addressRow->addWidget(importButton);
    layout->addLayout(addressRow);

    auto *configuration = new QFormLayout;
    configuration->setLabelAlignment(Qt::AlignRight);
    const QSettings settings;
    m_rewardPoolNetworkCombo = new QComboBox;
    m_rewardPoolNetworkCombo->setObjectName(
        QStringLiteral("rewardPoolNetwork"));
    m_rewardPoolNetworkCombo->addItem(QStringLiteral("Mainnet beta"),
                                      QStringLiteral("mainnet-beta"));
    m_rewardPoolNetworkCombo->addItem(QStringLiteral("Devnet (development only)"),
                                      QStringLiteral("devnet"));
    m_rewardPoolNetworkCombo->addItem(QStringLiteral("Testnet (development only)"),
                                      QStringLiteral("testnet"));
    const QString savedNetwork = settings.value(kRewardNetworkSetting).toString();
    const int savedIndex =
        m_rewardPoolNetworkCombo->findData(savedNetwork);
    m_rewardPoolNetworkCombo->setCurrentIndex(savedIndex >= 0 ? savedIndex : 0);
    configuration->addRow(QStringLiteral("Expected network"),
                          m_rewardPoolNetworkCombo);

    m_rewardPoolRpcEdit = new QLineEdit(
        settings.value(kRewardRpcSetting).toString());
    m_rewardPoolRpcEdit->setObjectName(QStringLiteral("rewardPoolRpc"));
    m_rewardPoolRpcEdit->setPlaceholderText(
        QStringLiteral("https://api.mainnet-beta.solana.com (no API key)"));
    m_rewardPoolRpcEdit->setClearButtonEnabled(true);
    configuration->addRow(QStringLiteral("Public Solana RPC"),
                          m_rewardPoolRpcEdit);
    layout->addLayout(configuration);

    auto *configurationRow = new QHBoxLayout;
    auto *saveConfiguration =
        new QPushButton(QStringLiteral("Save public RPC configuration"));
    saveConfiguration->setObjectName(
        QStringLiteral("rewardPoolSaveRpcButton"));
    saveConfiguration->setCursor(Qt::PointingHandCursor);
    setOcticon(saveConfiguration, QStringLiteral("shield-check"), 14);
    connect(saveConfiguration, &QPushButton::clicked, this,
            &MainWindow::saveRewardPoolRpcConfiguration);
    configurationRow->addWidget(saveConfiguration);
    configurationRow->addWidget(rewardHint(
        QStringLiteral(
            "Configuration is deliberately blank on first use. Credential- or "
            "API-key-bearing RPC URLs are rejected instead of stored.")));
    layout->addLayout(configurationRow);

    m_rewardPoolIntentsTable = new QTableWidget(0, 6);
    m_rewardPoolIntentsTable->setObjectName(
        QStringLiteral("rewardPoolIntentsTable"));
    m_rewardPoolIntentsTable->setHorizontalHeaderLabels(
        {QStringLiteral("Intent"), QStringLiteral("Destination"),
         QStringLiteral("Amount"), QStringLiteral("Network"),
         QStringLiteral("Expires"), QStringLiteral("Selected node")});
    m_rewardPoolIntentsTable->verticalHeader()->setVisible(false);
    m_rewardPoolIntentsTable->setSelectionBehavior(
        QAbstractItemView::SelectRows);
    m_rewardPoolIntentsTable->setSelectionMode(
        QAbstractItemView::SingleSelection);
    m_rewardPoolIntentsTable->setEditTriggers(
        QAbstractItemView::NoEditTriggers);
    m_rewardPoolIntentsTable->setShowGrid(false);
    m_rewardPoolIntentsTable->horizontalHeader()->setSectionResizeMode(
        0, QHeaderView::ResizeToContents);
    m_rewardPoolIntentsTable->horizontalHeader()->setSectionResizeMode(
        1, QHeaderView::Stretch);
    for (int column = 2; column < 6; ++column) {
        m_rewardPoolIntentsTable->horizontalHeader()->setSectionResizeMode(
            column, QHeaderView::ResizeToContents);
    }
    connect(m_rewardPoolIntentsTable, &QTableWidget::itemSelectionChanged,
            this, &MainWindow::refreshRewardPoolControls);
    layout->addWidget(m_rewardPoolIntentsTable);

    auto *actions = new QHBoxLayout;
    m_rewardPoolFetchButton =
        new QPushButton(QStringLiteral("Fetch pending chain intents"));
    m_rewardPoolFetchButton->setObjectName(
        QStringLiteral("rewardPoolFetchButton"));
    m_rewardPoolFetchButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_rewardPoolFetchButton, QStringLiteral("sync"), 14);
    connect(m_rewardPoolFetchButton, &QPushButton::clicked, this,
            &MainWindow::fetchRewardPoolIntents);
    actions->addWidget(m_rewardPoolFetchButton);
    m_rewardPoolSignButton =
        new QPushButton(QStringLiteral("Review, sign and submit selected"));
    m_rewardPoolSignButton->setObjectName(
        QStringLiteral("rewardPoolSignButton"));
    m_rewardPoolSignButton->setProperty("buttonSize", "primary");
    m_rewardPoolSignButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_rewardPoolSignButton, QStringLiteral("credit-card"), 14);
    connect(m_rewardPoolSignButton, &QPushButton::clicked, this,
            &MainWindow::reviewAndSignRewardIntent);
    actions->addWidget(m_rewardPoolSignButton);
    m_rewardPoolReconcileButton =
        new QPushButton(QStringLiteral("Reconcile last submission"));
    m_rewardPoolReconcileButton->setObjectName(
        QStringLiteral("rewardPoolReconcileButton"));
    m_rewardPoolReconcileButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_rewardPoolReconcileButton,
               QStringLiteral("check-circle"), 14);
    connect(m_rewardPoolReconcileButton, &QPushButton::clicked, this,
            [this] {
                if (m_rewardPoolFinalizeTimer)
                    m_rewardPoolFinalizeTimer->stop();
                pollRewardPoolFinalization(0);
            });
    actions->addWidget(m_rewardPoolReconcileButton);
    actions->addStretch();
    layout->addLayout(actions);

    auto *autoPayoutRow = new QHBoxLayout;
    m_microjobAutoPayoutButton =
        new QPushButton(QStringLiteral("Arm microjob auto-payouts"));
    m_microjobAutoPayoutButton->setObjectName(
        QStringLiteral("microjobAutoPayoutButton"));
    m_microjobAutoPayoutButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_microjobAutoPayoutButton, QStringLiteral("sparkle"), 14);
    connect(m_microjobAutoPayoutButton, &QPushButton::clicked, this,
            &MainWindow::toggleMicrojobAutoPayouts);
    autoPayoutRow->addWidget(m_microjobAutoPayoutButton);
    m_microjobAutoPayoutStatus = new QLabel;
    m_microjobAutoPayoutStatus->setObjectName(QStringLiteral("mutedLabel"));
    m_microjobAutoPayoutStatus->setWordWrap(true);
    autoPayoutRow->addWidget(m_microjobAutoPayoutStatus, 1);
    layout->addLayout(autoPayoutRow);
    layout->addWidget(rewardHint(
        QStringLiteral(
            "Microjob payouts are fixed, tiny amounts for work the relay has "
            "already verified. Arming signs those plans — and only those — "
            "without a prompt for the rest of this session, up to %1 per day. "
            "The passphrase stays in memory, is never saved, and is wiped when "
            "you disarm or quit. Every other reward plan still requires "
            "explicit per-transfer approval.")
            .arg(formatLamports(kMicrojobAutoPayoutDailyCapLamports))));

    m_rewardPoolStatus = new QLabel(
        QStringLiteral(
            "No chain operation has run. Import a key and explicitly save an "
            "RPC/network configuration before fetching intents."));
    m_rewardPoolStatus->setObjectName(QStringLiteral("rewardPoolStatus"));
    m_rewardPoolStatus->setWordWrap(true);
    layout->addWidget(m_rewardPoolStatus);

    m_rewardPoolFinalizeTimer = new QTimer(card);
    m_rewardPoolFinalizeTimer->setSingleShot(true);
    connect(m_rewardPoolFinalizeTimer, &QTimer::timeout, this, [this] {
        pollRewardPoolFinalization(
            m_rewardPoolFinalizeTimer->property("attempt").toInt());
    });
    refreshRewardPoolControls();
    return card;
}

void MainWindow::refreshRewardPoolControls()
{
    QString vaultError;
    const QString vaultPath = forkmesh::rewards::defaultPoolVaultPath();
    const QString poolAddress =
        vaultPath.isEmpty()
            ? QString()
            : forkmesh::rewards::poolVaultPublicAddress(vaultPath, &vaultError);
    const bool vaultReady = !poolAddress.isEmpty();
    if (m_rewardPoolAddress) {
        m_rewardPoolAddress->setText(
            vaultReady ? poolAddress : QString::fromUtf8("\xE2\x80\x94"));
    }
    if (m_rewardPoolVaultStatus) {
        m_rewardPoolVaultStatus->setText(
            vaultReady
                ? QStringLiteral(
                      "Encrypted local vault ready. The clear value below is "
                      "only the public pool address; unlocking and signing "
                      "require the owner's passphrase every time.")
                : QStringLiteral(
                      "No secure local pool-key vault is configured. %1")
                      .arg(vaultError.isEmpty()
                               ? QStringLiteral(
                                     "ForkMesh will not generate a custody key.")
                               : vaultError));
    }

    const QSettings settings;
    const QString savedRpc = settings.value(kRewardRpcSetting).toString();
    const QString savedNetwork =
        settings.value(kRewardNetworkSetting).toString();
    QUrl rpc;
    QString rpcError;
    const bool rpcReady =
        !savedNetwork.isEmpty() &&
        forkmesh::rewards::validatePublicRpcUrl(savedRpc, &rpc, &rpcError);
    const bool widgetsMatchSaved =
        m_rewardPoolRpcEdit && m_rewardPoolNetworkCombo &&
        m_rewardPoolRpcEdit->text().trimmed() == savedRpc &&
        m_rewardPoolNetworkCombo->currentData().toString() == savedNetwork;
    const bool identityReady =
        m_profileIdentity.isValid();
    const bool pending =
        !settings.value(kPendingSignatureSetting).toString().isEmpty();
    const QString selected = selectedIntentId(m_rewardPoolIntentsTable);
    const bool selectedUnsigned =
        !selected.isEmpty() &&
        m_rewardPoolIntents.value(selected)
                .value(QStringLiteral("status"))
                .toString() == QLatin1String("pending_signature");
    if (m_rewardPoolFetchButton) {
        m_rewardPoolFetchButton->setEnabled(
            !m_rewardPoolBusy && vaultReady && rpcReady &&
            widgetsMatchSaved && identityReady);
    }
    if (m_rewardPoolSignButton) {
        m_rewardPoolSignButton->setEnabled(
            !m_rewardPoolBusy && vaultReady && rpcReady &&
            widgetsMatchSaved && identityReady && !pending &&
            selectedUnsigned);
    }
    if (m_rewardPoolReconcileButton)
        m_rewardPoolReconcileButton->setEnabled(!m_rewardPoolBusy && pending);
    const QString selectedReward = selectedIntentId(m_contributorRewardsTable);
    const QJsonObject reward = m_contributorRewards.value(selectedReward);
    const bool rewardReady =
        reward.value(QStringLiteral("status")).toString()
            == QLatin1String("ready")
        && reward.value(QStringLiteral("amountLamports")).toDouble() > 0
        && !reward.value(QStringLiteral("recipientAddress"))
                .toString().isEmpty();
    const ContributorPayoutReceipt::Pending heldPayout =
        ContributorPayoutReceipt::read(settings);
    const bool contributorReceiptPending = heldPayout.blocking();
    if (m_contributorRewardsFetchButton)
        m_contributorRewardsFetchButton->setEnabled(!m_tipInFlight);
    // Already paid from here and proven finalized, only unreported. The relay
    // goes on calling it ready, so the button has to know better.
    const bool rewardAlreadySettled =
        ContributorPayoutReceipt::Settled::contains(settings, selectedReward);
    if (m_contributorRewardsPayButton) {
        // A transfer in flight only queues this payment behind it, and the
        // receipt that transfer saved for itself holds nothing yet.
        m_contributorRewardsPayButton->setEnabled(
            (!contributorReceiptPending || heldPayoutIsInFlight(heldPayout)) &&
            !rewardAlreadySettled && rewardReady);
        // A button that greys out without saying why is the same complaint the
        // Ledger rows answer. Tooltips still show on a disabled button, so the
        // held receipt is readable from the page that stopped offering to pay.
        if (rewardAlreadySettled) {
            m_contributorRewardsPayButton->setToolTip(
                ContributorPayoutReceipt::Settled::summary(
                    ContributorPayoutReceipt::Settled::read(settings,
                                                            selectedReward)));
        } else if (contributorReceiptPending &&
                   !heldPayoutIsInFlight(heldPayout)) {
            QStringList held;
            held.append(ContributorPayoutReceipt::refusalSummary());
            held.append(QString());
            held.append(ContributorPayoutReceipt::detailLines(heldPayout));
            held.append(QString());
            held.append(
                ContributorPayoutReceipt::reconciliationAdvice(heldPayout));
            m_contributorRewardsPayButton->setToolTip(
                held.join(QLatin1Char('\n')));
        } else {
            m_contributorRewardsPayButton->setToolTip(QString());
        }
    }
    const QString contributorWallet = forkmesh::tips::linkedTipAddress(
        forkmesh::tips::defaultTipVaultPath());
    const bool contributorArmed =
        !m_contributorAutoPayoutPassphrase.isEmpty();
    if (m_contributorAutoPayoutButton) {
        m_contributorAutoPayoutButton->setText(
            contributorArmed ? QStringLiteral("Disarm CFO daily payouts")
                             : QStringLiteral("Arm CFO daily payouts"));
        // An armed session can always be disarmed. Arming is blocked while a
        // transfer is in flight and until a desktop wallet exists.
        m_contributorAutoPayoutButton->setEnabled(
            contributorArmed || (!m_tipInFlight && !contributorWallet.isEmpty()));
    }
    if (m_contributorAutoPayoutStatus &&
        m_contributorAutoPayoutStatus->text().isEmpty()) {
        m_contributorAutoPayoutStatus->setText(
            contributorWallet.isEmpty()
                ? QStringLiteral(
                      "Link a Solana wallet on your profile before arming the CFO.")
                : QStringLiteral(
                      "Not armed. Scheduled payouts wait for local owner authorization."));
    }
    const bool armed = !m_microjobAutoPayoutPassphrase.isEmpty();
    if (m_microjobAutoPayoutButton) {
        m_microjobAutoPayoutButton->setText(
            armed ? QStringLiteral("Disarm microjob auto-payouts")
                  : QStringLiteral("Arm microjob auto-payouts"));
        // Disarming must always be reachable, even mid-transfer.
        m_microjobAutoPayoutButton->setEnabled(
            armed || (!m_rewardPoolBusy && vaultReady && rpcReady &&
                      widgetsMatchSaved && identityReady));
    }
    if (m_microjobAutoPayoutStatus &&
        m_microjobAutoPayoutStatus->text().isEmpty()) {
        m_microjobAutoPayoutStatus->setText(
            QStringLiteral(
                "Not armed. Microjob payout plans wait for explicit "
                "per-transfer approval like every other reward plan."));
    }
}

void MainWindow::importRewardPoolKey()
{
    const QString vaultPath = forkmesh::rewards::defaultPoolVaultPath();
    if (vaultPath.isEmpty()) {
        QMessageBox::critical(
            this, QStringLiteral("Community reward-pool key"),
            QStringLiteral(
                "A secure application-data location is unavailable. No key was imported."));
        return;
    }
    if (QFileInfo::exists(vaultPath) &&
        QMessageBox::warning(
            this, QStringLiteral("Replace encrypted pool-key vault?"),
            QStringLiteral(
                "A community reward-pool key is already configured on this "
                "desktop. Replacing it can make the existing pool impossible "
                "to sign from here. Continue only if you have independently "
                "backed up the old key."),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Import existing Solana pool key"));
    dialog.resize(620, 280);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(rewardHint(
        QStringLiteral(
            "Paste an existing Solana 64-byte keypair (CLI JSON array or "
            "base58), or load a local keypair file. ForkMesh does not generate "
            "a key, transmit it, or delete the source file. The imported seed "
            "is immediately encrypted with scrypt + AES-256-GCM.")));
    auto *form = new QFormLayout;
    auto *keyEdit = new QLineEdit;
    keyEdit->setObjectName(QStringLiteral("rewardPoolPrivateKeyInput"));
    keyEdit->setEchoMode(QLineEdit::Password);
    keyEdit->setMaxLength(4096);
    keyEdit->setPlaceholderText(
        QStringLiteral("64-byte keypair; never logged or uploaded"));
    form->addRow(QStringLiteral("Existing private key"), keyEdit);
    auto *passphraseEdit = new QLineEdit;
    passphraseEdit->setEchoMode(QLineEdit::Password);
    passphraseEdit->setMaxLength(1024);
    passphraseEdit->setPlaceholderText(
        QStringLiteral("at least 12 characters; not stored"));
    form->addRow(QStringLiteral("Vault passphrase"), passphraseEdit);
    auto *repeatEdit = new QLineEdit;
    repeatEdit->setEchoMode(QLineEdit::Password);
    repeatEdit->setMaxLength(1024);
    form->addRow(QStringLiteral("Repeat passphrase"), repeatEdit);
    layout->addLayout(form);
    auto *loadFile =
        new QPushButton(QStringLiteral("Load Solana keypair file…"));
    loadFile->setCursor(Qt::PointingHandCursor);
    connect(loadFile, &QPushButton::clicked, &dialog, [this, keyEdit] {
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Select an existing Solana keypair"),
            QString(), QStringLiteral("JSON keypair (*.json);;All files (*)"));
        if (path.isEmpty())
            return;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 ||
            file.size() > 4096) {
            QMessageBox::warning(
                this, QStringLiteral("Solana keypair"),
                QStringLiteral("The selected keypair file is unreadable or too large."));
            return;
        }
        QByteArray bytes = file.read(4097);
        keyEdit->setText(QString::fromUtf8(bytes).trimmed());
        forkmesh::rewards::secureErase(bytes);
    });
    layout->addWidget(loadFile, 0, Qt::AlignLeft);
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Cancel | QDialogButtonBox::Ok);
    buttons->button(QDialogButtonBox::Ok)->setText(
        QStringLiteral("Inspect existing key"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) {
        scrubEdit(keyEdit);
        scrubEdit(passphraseEdit);
        scrubEdit(repeatEdit);
        return;
    }

    QString keyText = keyEdit->text();
    QByteArray keyMaterial = keyText.toUtf8();
    QString passphrase = passphraseEdit->text();
    QString repeated = repeatEdit->text();
    scrubEdit(keyEdit);
    scrubEdit(passphraseEdit);
    scrubEdit(repeatEdit);
    forkmesh::rewards::secureErase(keyText);
    if (passphrase != repeated || passphrase.size() < 12) {
        forkmesh::rewards::secureErase(keyMaterial);
        forkmesh::rewards::secureErase(passphrase);
        forkmesh::rewards::secureErase(repeated);
        QMessageBox::warning(
            this, QStringLiteral("Community reward-pool key"),
            QStringLiteral(
                "The passphrases did not match, or were shorter than 12 characters."));
        return;
    }
    forkmesh::rewards::secureErase(repeated);

    QString inspectError;
    const QString address =
        forkmesh::rewards::poolAddressForKeyMaterial(keyMaterial,
                                                     &inspectError);
    if (address.isEmpty()) {
        forkmesh::rewards::secureErase(keyMaterial);
        forkmesh::rewards::secureErase(passphrase);
        QMessageBox::warning(this,
                             QStringLiteral("Community reward-pool key"),
                             inspectError);
        return;
    }
    if (QMessageBox::question(
            this, QStringLiteral("Confirm public pool address"),
            QStringLiteral(
                "The imported key controls this public Solana address:\n\n%1\n\n"
                "Confirm that this is the intended community reward pool. "
                "ForkMesh will store only its encrypted private seed and this "
                "public address on the local device.")
                .arg(address),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes) {
        forkmesh::rewards::secureErase(keyMaterial);
        forkmesh::rewards::secureErase(passphrase);
        return;
    }

    QString imported;
    QString importError;
    const bool ok = forkmesh::rewards::importPoolKey(
        keyMaterial, passphrase, vaultPath, &imported, &importError);
    forkmesh::rewards::secureErase(keyMaterial);
    forkmesh::rewards::secureErase(passphrase);
    if (!ok) {
        QMessageBox::critical(this,
                              QStringLiteral("Community reward-pool key"),
                              importError);
        return;
    }
    if (m_rewardPoolStatus) {
        m_rewardPoolStatus->setText(
            QStringLiteral(
                "Existing pool key imported into the encrypted local vault. "
                "Only public address %1 is displayed or shared.")
                .arg(imported));
    }
    logSystem(
        QStringLiteral(
            "Reward pool: encrypted local signer vault configured for public address %1.")
            .arg(imported));
    refreshRewardPoolControls();
}

void MainWindow::saveRewardPoolRpcConfiguration()
{
    const QString rpcText =
        m_rewardPoolRpcEdit ? m_rewardPoolRpcEdit->text().trimmed()
                            : QString();
    const QString network =
        m_rewardPoolNetworkCombo
            ? m_rewardPoolNetworkCombo->currentData().toString()
            : QString();
    QUrl normalized;
    QString error;
    if ((network != QLatin1String("devnet") &&
         network != QLatin1String("testnet") &&
         network != QLatin1String("mainnet-beta")) ||
        !forkmesh::rewards::validatePublicRpcUrl(rpcText, &normalized,
                                                 &error)) {
        if (m_rewardPoolStatus)
            m_rewardPoolStatus->setText(error);
        refreshRewardPoolControls();
        return;
    }
    QSettings settings;
    settings.setValue(kRewardRpcSetting,
                      normalized.toString(QUrl::StripTrailingSlash));
    settings.setValue(kRewardNetworkSetting, network);
    if (m_rewardPoolRpcEdit)
        m_rewardPoolRpcEdit->setText(
            normalized.toString(QUrl::StripTrailingSlash));
    if (m_rewardPoolStatus) {
        m_rewardPoolStatus->setText(
            QStringLiteral(
                "%1 public RPC configuration saved locally. No RPC API key or "
                "credential was accepted.")
                .arg(network));
    }
    logSystem(
        QStringLiteral("Reward pool: public %1 RPC configuration updated.")
            .arg(network));
    refreshRewardPoolControls();
}

QUrl MainWindow::rewardPoolWorkerEndpoint(const QString &path) const
{
    if ((path != QLatin1String("/api/rewards/signing-jobs") &&
         path != QLatin1String("/api/rewards/pool") &&
         path != QLatin1String("/api/rewards/estimates") &&
         path != QLatin1String("/api/rewards/wallet-reminder")) ||
        m_servers.isEmpty() || m_activeServer < 0 ||
        m_activeServer >= m_servers.size())
        return {};
    QUrl url(canonicalServerUrl(m_servers.at(m_activeServer).url));
    if (!url.isValid() || url.host().isEmpty())
        return {};
    if (url.scheme() == QLatin1String("wss"))
        url.setScheme(QStringLiteral("https"));
    else if (url.scheme() == QLatin1String("ws"))
        url.setScheme(QStringLiteral("http"));
    if (url.scheme() != QLatin1String("https"))
        return {};
    url.setUserInfo(QString());
    url.setPath(path);
    url.setQuery(QString());
    url.setFragment(QString());
    return url;
}

void MainWindow::fetchContributorRewards()
{
    fetchContributorRewardsRequest(false);
}

void MainWindow::fetchContributorRewardsForAutomation()
{
    if (m_contributorPayoutRunActive)
        fetchContributorRewardsRequest(true);
}

void MainWindow::renderContributorRewards(const QJsonArray &rewards)
{
    m_contributorRewards.clear();
    if (m_contributorRewardsTable)
        m_contributorRewardsTable->setRowCount(0);

    int outstanding = 0;
    int paid = 0;
    int firstReadyRow = -1;
    QSet<QString> seenRewardIds;
    for (const QJsonValue &value : rewards) {
        if (!value.isObject())
            continue;
        const QJsonObject reward = value.toObject();
        const QString rewardId =
            reward.value(QStringLiteral("rewardId")).toString().trimmed();
        const QString status =
            reward.value(QStringLiteral("status")).toString().trimmed();
        // Cancelled estimates are not payment history. Paid estimates are:
        // dropping them here was why a successful Send payment vanished from
        // this desktop the moment its refresh returned.
        if (rewardId.isEmpty() || seenRewardIds.contains(rewardId) ||
            status == QLatin1String("cancelled")) {
            continue;
        }
        seenRewardIds.insert(rewardId);
        m_contributorRewards.insert(rewardId, reward);

        const bool completed = status == QLatin1String("paid");
        if (completed)
            ++paid;
        else
            ++outstanding;
        if (!m_contributorRewardsTable)
            continue;

        const int row = m_contributorRewardsTable->rowCount();
        m_contributorRewardsTable->insertRow(row);
        const QString repository =
            reward.value(QStringLiteral("repository")).toString().trimmed();
        const QString kind =
            reward.value(QStringLiteral("sourceKind")).toString() ==
                    QLatin1String("pull")
                ? QStringLiteral("PR")
                : QStringLiteral("Issue");
        const int subjectNumber =
            reward.value(QStringLiteral("subjectNumber")).toInt();
        const QString source =
            QStringLiteral("%1 · %2 #%3")
                .arg(repository, kind)
                .arg(subjectNumber);
        auto *sourceItem = new QTableWidgetItem(source);
        sourceItem->setData(Qt::UserRole, rewardId);
        sourceItem->setToolTip(
            reward.value(QStringLiteral("subjectTitle")).toString());
        m_contributorRewardsTable->setItem(row, 0, sourceItem);

        const QString recipient =
            reward.value(QStringLiteral("recipient")).toString().trimmed();
        const QString discordIdentity =
            contributorRecipientDiscordIdentity(reward);
        auto *recipientItem =
            new QTableWidgetItem(recipient.isEmpty()
                                     ? QString::fromUtf8("\xE2\x80\x94")
                                     : QLatin1Char('@') + recipient +
                                           (discordIdentity.isEmpty()
                                                ? QString()
                                                : QString::fromUtf8(
                                                      " \xC2\xB7 ") +
                                                      discordIdentity));
        if (!discordIdentity.isEmpty()) {
            recipientItem->setToolTip(
                QStringLiteral("ForkMesh @%1\n%2")
                    .arg(recipient, discordIdentity));
        }
        m_contributorRewardsTable->setItem(row, 1, recipientItem);
        if (!recipient.isEmpty()) {
            auto *recipientCell = new QWidget;
            recipientCell->setObjectName(
                QStringLiteral("contributorRewardRecipient"));
            auto *recipientLayout = new QHBoxLayout(recipientCell);
            recipientLayout->setContentsMargins(4, 1, 8, 1);
            recipientLayout->setSpacing(6);
            auto *avatar = new QLabel;
            avatar->setObjectName(
                QStringLiteral("contributorRewardRecipientAvatar"));
            avatar->setFixedSize(20, 20);
            avatar->setPixmap(chatActorAvatar(
                QStringLiteral("user:") + recipient.toLower(), recipient, 20));
            avatar->setToolTip(QStringLiteral("@") + recipient);
            recipientLayout->addWidget(avatar);
            auto *names = new QWidget;
            auto *namesLayout = new QVBoxLayout(names);
            namesLayout->setContentsMargins(0, 0, 0, 0);
            namesLayout->setSpacing(0);
            auto *handle = new QLabel(QLatin1Char('@') + recipient);
            handle->setObjectName(
                QStringLiteral("contributorRewardRecipientHandle"));
            handle->setTextFormat(Qt::PlainText);
            handle->setToolTip(QStringLiteral("ForkMesh @") + recipient);
            namesLayout->addWidget(handle);
            if (!discordIdentity.isEmpty()) {
                auto *discord = new QLabel(discordIdentity);
                discord->setObjectName(
                    QStringLiteral("contributorRewardRecipientDiscord"));
                discord->setTextFormat(Qt::PlainText);
                discord->setToolTip(discordIdentity);
                namesLayout->addWidget(discord);
            }
            recipientLayout->addWidget(names);
            m_contributorRewardsTable->setCellWidget(row, 1, recipientCell);
        }

        m_contributorRewardsTable->setItem(
            row, 2,
            new QTableWidgetItem(
                reward.value(QStringLiteral("amountSol")).toString() +
                QStringLiteral(" SOL")));
        m_contributorRewardsTable->setItem(
            row, 3,
            new QTableWidgetItem(
                reward.value(QStringLiteral("condition"))
                    .toString()
                    .replace(QLatin1Char('_'), QLatin1Char(' '))));
        m_contributorRewardsTable->setItem(
            row, 4,
            new QTableWidgetItem(
                QString(status).replace(QLatin1Char('_'), QLatin1Char(' '))));

        const QString signature =
            reward.value(QStringLiteral("transactionSignature"))
                .toString()
                .trimmed();
        auto *transactionItem = new QTableWidgetItem(
            signature.isEmpty()
                ? QString::fromUtf8("\xE2\x80\x94")
                : ContributorPayoutReceipt::shorten(signature, 8));
        if (!signature.isEmpty()) {
            transactionItem->setData(Qt::UserRole, signature);
            transactionItem->setToolTip(
                QStringLiteral("Finalized Solana transaction\n%1")
                    .arg(signature));
        }
        m_contributorRewardsTable->setItem(row, 5, transactionItem);

        if (firstReadyRow < 0 && status == QLatin1String("ready") &&
            reward.value(QStringLiteral("amountLamports")).toDouble() > 0 &&
            !reward.value(QStringLiteral("recipientAddress"))
                 .toString()
                 .isEmpty()) {
            firstReadyRow = row;
        }
    }

    if (m_contributorRewardsTable &&
        m_contributorRewardsTable->rowCount() > 0) {
        m_contributorRewardsTable->selectRow(
            firstReadyRow >= 0 ? firstReadyRow : 0);
    }
    if (m_contributorRewardsStatus) {
        m_contributorRewardsStatus->setText(
            QStringLiteral(
                "%1 awaiting payment · %2 paid. Only ready rows can be paid; "
                "completed rows stay here with their transaction receipt.")
                .arg(outstanding)
                .arg(paid));
    }
    refreshRewardPoolControls();
}

void MainWindow::fetchContributorRewardsRequest(bool automated)
{
    if (!m_networkAccess) {
        if (automated) {
            finishContributorPayoutTransfer(
                false, QString(), 0,
                QStringLiteral("Stopped: network access is unavailable."));
        }
        return;
    }
    if (m_tipInFlight)
        return;
    QUrl endpoint = rewardPoolWorkerEndpoint(
        QStringLiteral("/api/rewards/estimates"));
    if (endpoint.isEmpty()) {
        if (m_contributorRewardsStatus)
            m_contributorRewardsStatus->setText(
                QStringLiteral("A trusted HTTPS ForkMesh relay is required."));
        if (automated) {
            finishContributorPayoutTransfer(
                false, QString(), 0,
                QStringLiteral("Stopped: a trusted HTTPS relay is required."));
        }
        return;
    }
    QString sessionToken;
    if (automated) {
        if (m_contributorPayoutRepository.isEmpty()) {
            finishContributorPayoutTransfer(
                false, QString(), 0,
                QStringLiteral(
                    "Stopped: the CFO run has no repository payout scope."));
            return;
        }
        // Bind the later public receipt to the same relay that authorized this
        // repository before any transfer is built or signed.
        m_contributorPayoutReceiptEndpoint = endpoint;
        m_contributorPayoutReceiptEndpoint.setPath(
            QStringLiteral("/api/accounts/tip-notice"));
        m_contributorPayoutReceiptEndpoint.setQuery(QString());
        m_contributorPayoutReceiptEndpoint.setFragment(QString());
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("ownerScope"), QStringLiteral("1"));
        query.addQueryItem(QStringLiteral("repository"),
                           m_contributorPayoutRepository);
        endpoint.setQuery(query);
        sessionToken = accountSessionTokenForUrl(endpoint);
        if (sessionToken.isEmpty()) {
            finishContributorPayoutTransfer(
                false, QString(), 0,
                QStringLiteral(
                    "Stopped: sign in as this repository's owner before processing payouts."));
            return;
        }
    }
    QNetworkRequest request(endpoint);
    hardenJsonRequest(request);
    if (automated) {
        request.setRawHeader("Authorization",
                             QByteArrayLiteral("Bearer ") +
                                 sessionToken.toUtf8());
    }
    if (m_contributorRewardsStatus)
        m_contributorRewardsStatus->setText(
            QStringLiteral("Loading owner-reviewed payment history…"));
    QNetworkReply *reply = m_networkAccess->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, automated] {
        const QByteArray body = boundedReplyBody(reply);
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
        if (!networkOk || !response.value(QStringLiteral("ok")).toBool()) {
            if (m_contributorRewardsStatus)
                m_contributorRewardsStatus->setText(
                    QStringLiteral("Payment history could not be loaded."));
            if (automated && m_contributorPayoutRunActive) {
                if (status == 401 || status == 403) {
                    finishContributorPayoutTransfer(
                        false, QString(), 0,
                        QStringLiteral(
                            "Stopped: the relay did not authorize this signed-in account as the repository owner."));
                    return;
                }
                if (status >= 400 && status < 500 && status != 429) {
                    finishContributorPayoutTransfer(
                        false, QString(), 0,
                        QStringLiteral(
                            "Stopped: the relay rejected the repository payout request."));
                    return;
                }
                ++m_contributorPayoutFetchAttempts;
                if (m_contributorPayoutFetchAttempts < 3 &&
                    (status == 0 || status == 429 || status >= 500)) {
                    const int delay = qMax(
                        forkmesh::tips::contributorPayoutRetryDelayMs(),
                        retryAfterOk ? qBound(1, retryAfterSeconds, 15 * 60) *
                                           1000
                                     : 0);
                    if (m_contributorAutoPayoutStatus) {
                        m_contributorAutoPayoutStatus->setText(
                            QStringLiteral(
                                "The payout list is temporarily unavailable. "
                                "CFO will retry in %1 seconds.")
                                .arg(delay / 1000));
                    }
                    QTimer::singleShot(
                        delay, this,
                        &MainWindow::fetchContributorRewardsForAutomation);
                } else {
                    finishContributorPayoutTransfer(
                        false, QString(), 0,
                        QStringLiteral(
                            "Stopped after paced retries: the payout list is unavailable."));
                }
            }
            return;
        }
        if (automated &&
            (response.value(QStringLiteral("custody")).toString() !=
                 QLatin1String("direct-owner-wallet") ||
             response.value(QStringLiteral("authorization")).toString() !=
                 QLatin1String("owner-session") ||
             response.value(QStringLiteral("repository")).toString() !=
                 m_contributorPayoutRepository)) {
            finishContributorPayoutTransfer(
                false, QString(), 0,
                QStringLiteral(
                    "Stopped: the relay did not confirm this repository's owner-scoped payout policy."));
            return;
        }
        const QJsonValue rewardsValue =
            response.value(QStringLiteral("rewards"));
        if (automated && !rewardsValue.isArray()) {
            finishContributorPayoutTransfer(
                false, QString(), 0,
                QStringLiteral(
                    "Stopped: the relay returned an invalid payout list."));
            return;
        }
        const QJsonArray rewards = rewardsValue.toArray();
        if (automated) {
            QSet<QString> seenRewardIds;
            static const QSet<QString> payoutStatuses{
                QStringLiteral("pending_merge"),
                QStringLiteral("pending_wallet"), QStringLiteral("ready"),
                QStringLiteral("submitted"), QStringLiteral("paid"),
                QStringLiteral("cancelled"),
            };
            for (const QJsonValue &value : rewards) {
                if (!value.isObject()) {
                    finishContributorPayoutTransfer(
                        false, QString(), 0,
                        QStringLiteral(
                            "Stopped: the payout list contained an invalid record."));
                    return;
                }
                const QJsonObject reward = value.toObject();
                const QString status =
                    reward.value(QStringLiteral("status")).toString();
                if (reward.value(QStringLiteral("repository"))
                        .toString()
                        .trimmed()
                        .toLower() != m_contributorPayoutRepository) {
                    finishContributorPayoutTransfer(
                        false, QString(), 0,
                        QStringLiteral(
                            "Stopped: the relay returned a payout outside this repository."));
                    return;
                }
                if (!payoutStatuses.contains(status)) {
                    finishContributorPayoutTransfer(
                        false, QString(), 0,
                        QStringLiteral(
                            "Stopped: a payout record had an unknown status."));
                    return;
                }
                const QString rewardId =
                    reward.value(QStringLiteral("rewardId")).toString();
                if (rewardId.isEmpty() || seenRewardIds.contains(rewardId)) {
                    finishContributorPayoutTransfer(
                        false, QString(), 0,
                        QStringLiteral(
                            "Stopped: payout record identities were missing or duplicated."));
                    return;
                }
                seenRewardIds.insert(rewardId);
            }
        }
        renderContributorRewards(rewards);
        if (automated && m_contributorPayoutRunActive) {
            m_contributorPayoutFetchAttempts = 0;
            continueContributorPayoutRun();
        }
    });
}

void MainWindow::paySelectedContributorReward()
{
    const QString rewardId = selectedIntentId(m_contributorRewardsTable);
    const QJsonObject reward = m_contributorRewards.value(rewardId);
    if (rewardId.isEmpty() ||
        reward.value(QStringLiteral("status")).toString()
            != QLatin1String("ready"))
        return;
    const quint64 lamports = quint64(
        reward.value(QStringLiteral("amountLamports")).toDouble());
    const QString recipient =
        reward.value(QStringLiteral("recipient")).toString();
    const QString address =
        reward.value(QStringLiteral("recipientAddress")).toString();
    if (!lamports || recipient.isEmpty() || address.isEmpty())
        return;
    sendTipToMember(recipient, address, lamports, rewardId);
}

void MainWindow::toggleContributorAutoPayouts()
{
    if (!m_contributorAutoPayoutPassphrase.isEmpty()) {
        forkmesh::rewards::secureErase(m_contributorAutoPayoutPassphrase);
        m_contributorAutoPayoutPassphrase.clear();
        m_contributorAutoPayoutWallet.clear();
        m_contributorAutoPayoutScope.clear();
        m_contributorPayoutRunActive = false;
        if (m_contributorAutoPayoutStatus) {
            m_contributorAutoPayoutStatus->setText(
                QStringLiteral(
                    "Disarmed. The wallet passphrase was wiped from memory; "
                    "scheduled CFO runs will wait for the owner."));
        }
        if (!m_contributorPayoutTeammateId.isEmpty()) {
            recordTeammateActivity(
                m_contributorPayoutTeammateId,
                QStringLiteral("Disarmed daily payouts and wiped the session unlock"));
        }
        logSystem(QStringLiteral(
            "Contributor payouts: CFO session authorization disarmed."));
        refreshRewardPoolControls();
        return;
    }
    if (!armContributorAutoPayouts())
        return;
    const forkmesh::teammates::LoadResult loaded =
        forkmesh::teammates::Store(teammatesDirPath()).load();
    const auto it = std::find_if(
        loaded.teammates.cbegin(), loaded.teammates.cend(),
        [](const forkmesh::teammates::Config &candidate) {
            return candidate.enabled &&
                   candidate.workflow ==
                       forkmesh::teammates::kWorkflowContributorPayouts &&
                   candidate.permissions.contains(
                       QStringLiteral("process-payouts"));
        });
    if (it != loaded.teammates.cend()) {
        runContributorPayoutTeammate(
            it->id, QStringLiteral("On demand · payout session armed"));
    }
}

bool MainWindow::armContributorAutoPayouts()
{
    if (!m_contributorAutoPayoutPassphrase.isEmpty())
        return true;
    const QString authorizationScope = teammatesDirPath();
    if (authorizationScope.isEmpty()) {
        if (m_contributorAutoPayoutStatus) {
            m_contributorAutoPayoutStatus->setText(
                QStringLiteral(
                    "Open a writable repository before arming CFO payouts."));
        }
        return false;
    }
    const QString vaultPath = forkmesh::tips::defaultTipVaultPath();
    QString error;
    const QString wallet =
        forkmesh::tips::linkedTipAddress(vaultPath, &error);
    if (wallet.isEmpty()) {
        if (m_contributorAutoPayoutStatus) {
            m_contributorAutoPayoutStatus->setText(
                QStringLiteral(
                    "Link a Solana wallet on your profile before arming the CFO."));
        }
        return false;
    }

    QMessageBox confirmation(this);
    confirmation.setIcon(QMessageBox::Warning);
    confirmation.setWindowTitle(QStringLiteral("Arm CFO daily payouts"));
    confirmation.setTextFormat(Qt::RichText);
    confirmation.setText(
        QStringLiteral(
            "<b>This authorizes reviewed payouts without another prompt</b>"
            "<br><br>For this app session, the CFO special agent may send only "
            "relay-reviewed contributor rewards from linked wallet"
            "<br><code>%1</code><br>up to <b>%2</b> per local day. It sends "
            "one exact transfer at a time, waits for the matching finalized "
            "transaction record, then pauses at least one minute.<br><br>"
            "The passphrase stays only in this process's memory and is wiped "
            "when you disarm or quit.")
            .arg(wallet.toHtmlEscaped(),
                 forkmesh::tips::formatLamports(
                     forkmesh::tips::contributorPayoutDailyCapLamports())
                     .toHtmlEscaped()));
    QPushButton *approve = confirmation.addButton(
        QStringLiteral("Arm for this session"), QMessageBox::AcceptRole);
    confirmation.addButton(QMessageBox::Cancel);
    confirmation.setDefaultButton(QMessageBox::Cancel);
    confirmation.exec();
    if (confirmation.clickedButton() != approve)
        return false;

    QInputDialog unlock(this);
    unlock.setWindowTitle(QStringLiteral("Unlock linked Solana wallet"));
    unlock.setLabelText(
        QStringLiteral(
            "Enter the local wallet-vault passphrase. It will be verified "
            "with an off-chain signature and held in memory only for this "
            "session's reviewed payouts."));
    unlock.setTextEchoMode(QLineEdit::Password);
    unlock.setInputMode(QInputDialog::TextInput);
    if (unlock.exec() != QDialog::Accepted)
        return false;
    QString passphrase = unlock.textValue();
    unlock.setTextValue(QString(passphrase.size(), QChar(u'\0')));
    unlock.setTextValue(QString());
    QString verifiedWallet;
    const bool verified = passphrase.size() <= 1024 &&
                          forkmesh::tips::verifyTipWalletPassphrase(
                              passphrase, vaultPath, &verifiedWallet, &error) &&
                          verifiedWallet == wallet;
    if (!verified) {
        forkmesh::rewards::secureErase(passphrase);
        if (m_contributorAutoPayoutStatus) {
            m_contributorAutoPayoutStatus->setText(
                QStringLiteral(
                    "The linked wallet could not be unlocked. CFO payouts remain disarmed."));
        }
        return false;
    }
    m_contributorAutoPayoutPassphrase = passphrase;
    m_contributorAutoPayoutWallet = verifiedWallet;
    m_contributorAutoPayoutScope = authorizationScope;
    forkmesh::rewards::secureErase(passphrase);

    const QDate today = QDate::currentDate();
    QSettings settings;
    const QDate savedDay = QDate::fromString(
        settings.value(kContributorPayoutDaySetting).toString(), Qt::ISODate);
    if (savedDay == today) {
        m_contributorAutoPayoutDay = today;
        m_contributorAutoPaidLamports = qMin(
            settings.value(kContributorPayoutPaidSetting).toULongLong(),
            forkmesh::tips::contributorPayoutDailyCapLamports());
    } else {
        m_contributorAutoPayoutDay = today;
        m_contributorAutoPaidLamports = 0;
        settings.setValue(kContributorPayoutDaySetting,
                          today.toString(Qt::ISODate));
        settings.setValue(kContributorPayoutPaidSetting, qulonglong(0));
    }
    if (m_contributorAutoPayoutStatus) {
        m_contributorAutoPayoutStatus->setText(
            QStringLiteral("Armed for this session. %1 of today's %2 limit is used.")
                .arg(forkmesh::tips::formatLamports(
                         m_contributorAutoPaidLamports),
                     forkmesh::tips::formatLamports(
                         forkmesh::tips::contributorPayoutDailyCapLamports())));
    }
    logSystem(QStringLiteral(
        "Contributor payouts: CFO session authorization armed."));
    refreshRewardPoolControls();
    return true;
}

void MainWindow::runContributorPayoutTeammate(const QString &teammateId,
                                              const QString &trigger)
{
    if (m_contributorPayoutRunActive) {
        setRepoDetailNotice(
            QStringLiteral("The CFO is already processing contributor payouts."));
        return;
    }
    m_contributorPayoutTeammateId = teammateId;
    m_contributorPayoutTeammatesDirectory = teammatesDirPath();
    m_contributorPayoutRuntimePrefix = teammateRuntimePrefix(teammateId);
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size()) {
        setRepoDetailNotice(
            QStringLiteral("The CFO needs an open repository payout scope."),
            true);
        return;
    }
    const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
    m_contributorPayoutRepository =
        (repo.owner.trimmed() + QLatin1Char('/') + repo.name.trimmed())
            .toLower();
    if (repo.owner.trimmed().isEmpty() || repo.name.trimmed().isEmpty()) {
        setRepoDetailNotice(
            QStringLiteral("The CFO needs a repository owner and name."), true);
        return;
    }
    m_contributorPayoutTrigger = trigger;
    recordTeammateActivity(
        teammateId,
        QStringLiteral("Started %1 · guarded payout reconciliation")
            .arg(trigger.left(100)),
        trigger, true);

    if (m_tipInFlight) {
        const QString waiting = QStringLiteral(
            "Deferred because another desktop wallet operation is in progress");
        recordTeammateActivity(teammateId, waiting);
        setRepoDetailNotice(waiting + QLatin1Char('.'));
        return;
    }

    // Public receipt reconciliation needs no wallet unlock. Always finish a
    // previously broadcast transaction record before asking for authority to
    // sign anything new.
    m_contributorPayoutRunActive = true;
    m_contributorPayoutFetchAttempts = 0;
    // Payouts this desktop already settled from public chain state still owe
    // the relay their notice. A run is the busiest moment this app is signed
    // in, so it is the best moment to try sending them; it blocks nothing and
    // signs nothing.
    flushSettledContributorNotices();
    if (resumePendingContributorReceipt()) {
        if (m_contributorAutoPayoutStatus) {
            m_contributorAutoPayoutStatus->setText(
                QStringLiteral(
                    "Reconciling the previously broadcast transaction before sending anything else."));
        }
        return;
    }

    if (!m_contributorAutoPayoutPassphrase.isEmpty() &&
        m_contributorAutoPayoutScope !=
            m_contributorPayoutTeammatesDirectory) {
        m_contributorPayoutRunActive = false;
        const QString waiting = QStringLiteral(
            "Waiting for payout authorization in this repository");
        if (m_contributorAutoPayoutStatus)
            m_contributorAutoPayoutStatus->setText(waiting + QLatin1Char('.'));
        recordTeammateActivity(teammateId, waiting);
        setRepoDetailNotice(
            QStringLiteral(
                "CFO payouts are armed for another repository. Disarm there, then arm again in this repository."));
        return;
    }

    if (m_contributorAutoPayoutPassphrase.isEmpty()) {
        const bool scheduled =
            trigger.startsWith(QStringLiteral("Scheduled"), Qt::CaseInsensitive);
        if (scheduled || !armContributorAutoPayouts()) {
            const QString waiting = QStringLiteral(
                "Waiting for the owner to arm the desktop-linked wallet");
            if (m_contributorAutoPayoutStatus)
                m_contributorAutoPayoutStatus->setText(waiting + QLatin1Char('.'));
            m_contributorPayoutRunActive = false;
            recordTeammateActivity(teammateId, waiting);
            setRepoDetailNotice(
                QStringLiteral(
                    "CFO schedule recorded. Arm CFO daily payouts in Control → Reward pool to authorize this app session."));
            return;
        }
    }
    fetchContributorRewardsForAutomation();
}

void MainWindow::continueContributorPayoutRun()
{
    if (!m_contributorPayoutRunActive || m_tipInFlight)
        return;
    if (m_contributorAutoPayoutPassphrase.isEmpty()) {
        finishContributorPayoutTransfer(
            false, QString(), 0,
            QStringLiteral("Stopped because the payout session was disarmed."));
        return;
    }
    if (m_contributorAutoPayoutScope !=
        m_contributorPayoutTeammatesDirectory) {
        finishContributorPayoutTransfer(
            false, QString(), 0,
            QStringLiteral(
                "Stopped because the active repository no longer matches the armed payout scope."));
        return;
    }

    const QDate today = QDate::currentDate();
    if (m_contributorAutoPayoutDay != today) {
        m_contributorAutoPayoutDay = today;
        m_contributorAutoPaidLamports = 0;
        QSettings settings;
        settings.setValue(kContributorPayoutDaySetting,
                          today.toString(Qt::ISODate));
        settings.setValue(kContributorPayoutPaidSetting, qulonglong(0));
    }
    const quint64 dailyCap =
        forkmesh::tips::contributorPayoutDailyCapLamports();
    if (m_contributorAutoPaidLamports >= dailyCap) {
        const QString detail = QStringLiteral(
            "Reached today's %1 CFO payout limit; remaining rewards are waiting.")
                                   .arg(forkmesh::tips::formatLamports(dailyCap));
        m_contributorPayoutRunActive = false;
        if (m_contributorAutoPayoutStatus)
            m_contributorAutoPayoutStatus->setText(detail);
        recordTeammateActivity(m_contributorPayoutTeammateId, detail);
        return;
    }

    const QString sender = forkmesh::tips::linkedTipAddress(
        forkmesh::tips::defaultTipVaultPath());
    if (!forkmesh::tips::isValidTipAddress(sender) ||
        sender != m_contributorAutoPayoutWallet) {
        finishContributorPayoutTransfer(
            false, QString(), 0,
            QStringLiteral(
                "Stopped because the desktop-linked wallet changed after payouts were armed."));
        return;
    }

    // Rewards this desktop has already paid and proven finalized on-chain,
    // whose notice has not reached the relay yet. The relay still publishes
    // them as ready, so without this the run would sign a second transfer for
    // work that has already been paid for. Read once, not once per reward.
    const QSet<QString> settled =
        ContributorPayoutReceipt::Settled::rewardIdSet();

    QList<forkmesh::tips::ContributorPayout> ready;
    for (auto it = m_contributorRewards.cbegin();
         it != m_contributorRewards.cend(); ++it) {
        if (it.value().value(QStringLiteral("status")).toString() !=
            QLatin1String("ready")) {
            continue;
        }
        if (settled.contains(it.key()))
            continue;
        forkmesh::tips::ContributorPayout payout;
        QString error;
        if (!forkmesh::tips::parseContributorPayout(it.value(), &payout,
                                                    &error)) {
            finishContributorPayoutTransfer(
                false, QString(), 0,
                QStringLiteral(
                    "Stopped: a ready payout failed strict local validation."));
            return;
        }
        ready.append(payout);
    }
    std::sort(ready.begin(), ready.end(),
              [](const forkmesh::tips::ContributorPayout &left,
                 const forkmesh::tips::ContributorPayout &right) {
                  if (left.createdAtMs != right.createdAtMs)
                      return left.createdAtMs < right.createdAtMs;
                  return left.rewardId < right.rewardId;
              });
    if (ready.isEmpty()) {
        // "Nothing is ready" and "everything ready was already paid, and the
        // relay has not been told" are different states, and the second one
        // still owes somebody an email. Never report it as the first.
        const QString detail =
            settled.isEmpty()
                ? QStringLiteral(
                      "Reconciliation complete · no reviewed payouts are ready")
                : QStringLiteral(
                      "Reconciliation complete · %1 paid payout(s) still "
                      "waiting to be reported to ForkMesh")
                      .arg(settled.size());
        m_contributorPayoutRunActive = false;
        if (m_contributorAutoPayoutStatus)
            m_contributorAutoPayoutStatus->setText(detail + QLatin1Char('.'));
        recordTeammateActivity(m_contributorPayoutTeammateId, detail);
        setRepoDetailNotice(
            settled.isEmpty()
                ? QStringLiteral(
                      "CFO reconciliation complete; no payouts are ready.")
                : QStringLiteral(
                      "CFO reconciliation complete. %1 payout(s) are paid "
                      "on-chain and waiting on a signed-in ForkMesh account to "
                      "be recorded.")
                      .arg(settled.size()));
        return;
    }

    const forkmesh::tips::ContributorPayout payout = ready.first();
    if (payout.lamports > dailyCap - m_contributorAutoPaidLamports) {
        const QString detail = QStringLiteral(
            "Today's CFO limit leaves insufficient room for reward %1; it is waiting.")
                                   .arg(payout.rewardId.left(32));
        m_contributorPayoutRunActive = false;
        if (m_contributorAutoPayoutStatus)
            m_contributorAutoPayoutStatus->setText(detail);
        recordTeammateActivity(m_contributorPayoutTeammateId, detail);
        return;
    }

    m_contributorPayoutCurrentRewardId = payout.rewardId;
    m_contributorPayoutCurrentLamports = payout.lamports;
    if (m_contributorAutoPayoutStatus) {
        m_contributorAutoPayoutStatus->setText(
            QStringLiteral("Processing %1 · %2 to %3 from the linked wallet.")
                .arg(payout.rewardId.left(32),
                     forkmesh::tips::formatLamports(payout.lamports),
                     payout.recipientName));
    }
    recordTeammateActivity(
        m_contributorPayoutTeammateId,
        QStringLiteral("Validating %1 · %2")
            .arg(payout.rewardId.left(32),
                 forkmesh::tips::formatLamports(payout.lamports)));
    startTipTransfer(payout.recipientName, payout.recipientAddress,
                     payout.lamports, payout.rewardId, true);
}

void MainWindow::finishContributorPayoutTransfer(
    bool paid, const QString &rewardId, quint64 lamports,
    const QString &detail)
{
    m_contributorPayoutWaitingForStartupAuth = false;
    m_tipInFlight = false;
    refreshTipWalletControls();
    const QString publicDetail = detail.trimmed().left(160);
    if (paid && !rewardId.isEmpty() &&
        rewardId == m_contributorPayoutCurrentRewardId &&
        lamports == m_contributorPayoutCurrentLamports) {
        const QDate today = QDate::currentDate();
        if (m_contributorAutoPayoutDay != today) {
            m_contributorAutoPayoutDay = today;
            m_contributorAutoPaidLamports = 0;
        }
        const quint64 cap =
            forkmesh::tips::contributorPayoutDailyCapLamports();
        m_contributorAutoPaidLamports =
            qMin(cap, m_contributorAutoPaidLamports + lamports);
        QSettings settings;
        settings.setValue(kContributorPayoutDaySetting,
                          today.toString(Qt::ISODate));
        settings.setValue(kContributorPayoutPaidSetting,
                          qulonglong(m_contributorAutoPaidLamports));
        recordTeammateActivity(
            m_contributorPayoutTeammateId,
            publicDetail.isEmpty()
                ? QStringLiteral("Paid and reconciled %1 · %2")
                      .arg(rewardId.left(32),
                           forkmesh::tips::formatLamports(lamports))
                : publicDetail);
        m_contributorPayoutCurrentRewardId.clear();
        m_contributorPayoutCurrentLamports = 0;
        if (!m_contributorPayoutRunActive ||
            m_contributorAutoPayoutPassphrase.isEmpty() ||
            m_contributorAutoPaidLamports >= cap) {
            m_contributorPayoutRunActive = false;
            refreshRewardPoolControls();
            return;
        }
        if (m_contributorAutoPayoutStatus) {
            m_contributorAutoPayoutStatus->setText(
                QStringLiteral(
                    "Payment reconciled. Waiting %1 seconds before checking for the next reward.")
                    .arg(forkmesh::tips::contributorPayoutSpacingMs() / 1000));
        }
        QTimer::singleShot(
            forkmesh::tips::contributorPayoutSpacingMs(), this,
            &MainWindow::fetchContributorRewardsForAutomation);
        refreshRewardPoolControls();
        return;
    }

    m_contributorPayoutRunActive = false;
    m_contributorPayoutCurrentRewardId.clear();
    m_contributorPayoutCurrentLamports = 0;
    if (m_contributorAutoPayoutStatus)
        m_contributorAutoPayoutStatus->setText(publicDetail);
    if (!m_contributorPayoutTeammateId.isEmpty() && !publicDetail.isEmpty())
        recordTeammateActivity(m_contributorPayoutTeammateId, publicDetail);
    refreshRewardPoolControls();
}

void MainWindow::fetchRewardPoolIntents()
{
    if (m_rewardPoolBusy)
        return;
    QString vaultError;
    const QString poolAddress =
        forkmesh::rewards::poolVaultPublicAddress(
            forkmesh::rewards::defaultPoolVaultPath(), &vaultError);
    QSettings settings;
    const QString network =
        settings.value(kRewardNetworkSetting).toString();
    const QString savedRpc = settings.value(kRewardRpcSetting).toString();
    QUrl rpc;
    QString rpcError;
    if (poolAddress.isEmpty() ||
        !forkmesh::rewards::validatePublicRpcUrl(savedRpc, &rpc,
                                                 &rpcError) ||
        !m_rewardPoolRpcEdit || !m_rewardPoolNetworkCombo ||
        m_rewardPoolRpcEdit->text().trimmed() != savedRpc ||
        m_rewardPoolNetworkCombo->currentData().toString() != network) {
        if (m_rewardPoolStatus) {
            m_rewardPoolStatus->setText(
                QStringLiteral(
                    "Fail closed: configure the encrypted pool vault and save "
                    "the current public RPC/network settings before fetching."));
        }
        refreshRewardPoolControls();
        return;
    }
    const QString signer = accountOwner().trimmed().toLower();
    if (signer.isEmpty() || !hasOwnerSigningCapability(signer) ||
        (!m_profileIdentity.isValid() && !m_profileIdentity.load()) ||
        !m_profileIdentity.isValid()) {
        if (m_rewardPoolStatus) {
            m_rewardPoolStatus->setText(
                QStringLiteral(
                    "Fail closed: sign in with the configured first-instance "
                    "owner account and its local identity key."));
        }
        return;
    }
    QUrl endpoint = rewardPoolWorkerEndpoint(
        QStringLiteral("/api/rewards/signing-jobs"));
    const QString timestamp =
        QString::number(QDateTime::currentMSecsSinceEpoch());
    const QByteArray canonical =
        forkmesh::rewards::signingJobsFetchCanonical(signer, timestamp);
    const QString signature = m_profileIdentity.signData(canonical);
    if (endpoint.isEmpty() || canonical.isEmpty() || signature.isEmpty() ||
        !m_networkAccess) {
        if (m_rewardPoolStatus) {
            m_rewardPoolStatus->setText(
                QStringLiteral(
                    "Fail closed: an HTTPS Worker relay and authorized local "
                    "first-owner identity are required."));
        }
        return;
    }
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("signer"), signer);
    query.addQueryItem(QStringLiteral("ts"), timestamp);
    query.addQueryItem(QStringLiteral("sig"), signature);
    endpoint.setQuery(query);

    QNetworkRequest request(endpoint);
    hardenJsonRequest(request);
    m_rewardPoolBusy = true;
    refreshRewardPoolControls();
    if (m_rewardPoolStatus)
        m_rewardPoolStatus->setText(
            QStringLiteral("Fetching signed-owner pending intent list…"));
    QNetworkReply *reply = m_networkAccess->get(request);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, poolAddress, network] {
        const QByteArray body = boundedReplyBody(reply);
        const bool networkOk =
            reply->error() == QNetworkReply::NoError;
        const QString failure =
            publicNetworkFailure(reply,
                                 QStringLiteral("Reward-intent fetch"));
        reply->deleteLater();
        m_rewardPoolBusy = false;
        if (!networkOk || body.isEmpty()) {
            if (m_rewardPoolStatus)
                m_rewardPoolStatus->setText(failure);
            refreshRewardPoolControls();
            return;
        }
        QJsonParseError parseError;
        const QJsonDocument document =
            QJsonDocument::fromJson(body, &parseError);
        const QJsonObject response = document.object();
        const QJsonArray intents =
            response.value(QStringLiteral("jobs")).toArray();
        if (parseError.error != QJsonParseError::NoError ||
            !response.value(QStringLiteral("ok")).toBool(false) ||
            response.value(QStringLiteral("custody")).toString() !=
                QLatin1String("external-local-signer")) {
            if (m_rewardPoolStatus) {
                m_rewardPoolStatus->setText(
                    QStringLiteral(
                        "The Worker returned an invalid external-signer job envelope; nothing was signed."));
            }
            refreshRewardPoolControls();
            return;
        }

        m_rewardPoolIntents.clear();
        if (m_rewardPoolIntentsTable)
            m_rewardPoolIntentsTable->setRowCount(0);
        int rejected = 0;
        for (const QJsonValue &value : intents) {
            if (!value.isObject()) {
                ++rejected;
                continue;
            }
            const QJsonObject object = value.toObject();
            forkmesh::rewards::RewardSigningJob intent;
            QString validationError;
            if (!forkmesh::rewards::parseRewardSigningJob(
                    object, poolAddress, network,
                    QDateTime::currentDateTimeUtc(), &intent,
                    &validationError)) {
                ++rejected;
                continue;
            }
            if (m_rewardPoolIntents.contains(intent.intentId)) {
                ++rejected;
                continue;
            }
            m_rewardPoolIntents.insert(intent.intentId, object);
            if (!m_rewardPoolIntentsTable)
                continue;
            const int row = m_rewardPoolIntentsTable->rowCount();
            m_rewardPoolIntentsTable->insertRow(row);
            auto *id = new QTableWidgetItem(
                QStringLiteral("%1 · %2")
                    .arg(intent.intentId.left(12), intent.status));
            id->setData(Qt::UserRole, intent.intentId);
            id->setToolTip(intent.intentId);
            m_rewardPoolIntentsTable->setItem(row, 0, id);
            QStringList destinations;
            for (const forkmesh::rewards::RewardTransfer &transfer :
                 std::as_const(intent.transfers)) {
                destinations.append(transfer.destinationWallet);
            }
            auto *destination =
                new QTableWidgetItem(
                    destinations.size() == 1
                        ? destinations.first()
                        : QStringLiteral("%1 reviewed recipients")
                              .arg(destinations.size()));
            destination->setToolTip(destinations.join(QLatin1Char('\n')));
            m_rewardPoolIntentsTable->setItem(row, 1, destination);
            m_rewardPoolIntentsTable->setItem(
                row, 2,
                new QTableWidgetItem(
                    formatLamports(intent.totalLamports)));
            m_rewardPoolIntentsTable->setItem(
                row, 3, new QTableWidgetItem(intent.network));
            m_rewardPoolIntentsTable->setItem(
                row, 4,
                new QTableWidgetItem(
                    QDateTime::fromMSecsSinceEpoch(intent.expiresAtMs)
                        .toLocalTime()
                        .toString(
                        QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
            QStringList nodes;
            for (const forkmesh::rewards::RewardTransfer &transfer :
                 std::as_const(intent.transfers)) {
                if (!transfer.nodeId.isEmpty())
                    nodes.append(transfer.nodeId);
            }
            m_rewardPoolIntentsTable->setItem(
                row, 5,
                new QTableWidgetItem(
                    nodes.isEmpty()
                        ? QString::fromUtf8("\xE2\x80\x94")
                        : nodes.join(QStringLiteral(", "))));
        }
        if (m_rewardPoolStatus) {
            m_rewardPoolStatus->setText(
                QStringLiteral(
                    "%1 locally verified pending intent(s); %2 malformed, "
                    "expired, mismatched, or unsupported intent(s) rejected.")
                    .arg(m_rewardPoolIntents.size())
                    .arg(rejected));
        }
        if (m_rewardPoolIntentsTable &&
            m_rewardPoolIntentsTable->rowCount() > 0)
            m_rewardPoolIntentsTable->selectRow(0);
        refreshRewardPoolControls();
        // Only ever acts on locally re-validated microjob_payout plans, and
        // only while the owner has armed this session.
        autoSignMicrojobPayouts();
    });
}

void MainWindow::reviewAndSignRewardIntent()
{
    if (m_rewardPoolBusy)
        return;
    const QString intentId =
        selectedIntentId(m_rewardPoolIntentsTable);
    if (intentId.isEmpty() || !m_rewardPoolIntents.contains(intentId))
        return;
    const QSettings settings;
    const QString network =
        settings.value(kRewardNetworkSetting).toString();
    QUrl rpc;
    QString error;
    if (!forkmesh::rewards::validatePublicRpcUrl(
            settings.value(kRewardRpcSetting).toString(), &rpc, &error)) {
        if (m_rewardPoolStatus)
            m_rewardPoolStatus->setText(error);
        return;
    }
    const QString vaultPath =
        forkmesh::rewards::defaultPoolVaultPath();
    const QString poolAddress =
        forkmesh::rewards::poolVaultPublicAddress(vaultPath, &error);
    forkmesh::rewards::RewardSigningJob intent;
    const QJsonObject object = m_rewardPoolIntents.value(intentId);
    if (poolAddress.isEmpty() ||
        !forkmesh::rewards::parseRewardSigningJob(
            object, poolAddress, network, QDateTime::currentDateTimeUtc(),
            &intent, &error)) {
        if (m_rewardPoolStatus)
            m_rewardPoolStatus->setText(
                QStringLiteral("Intent re-validation failed: %1").arg(error));
        return;
    }
    if (intent.status != QLatin1String("pending_signature")) {
        if (m_rewardPoolStatus)
            m_rewardPoolStatus->setText(
                QStringLiteral("That Worker job is already submitted."));
        return;
    }

    QStringList recipients;
    for (const forkmesh::rewards::RewardTransfer &transfer :
         std::as_const(intent.transfers)) {
        recipients.append(
            QStringLiteral("%1 — %2")
                .arg(transfer.destinationWallet,
                     formatLamports(transfer.lamports)));
    }
    QString recipientPreview;
    QStringList escapedRows;
    for (const QString &row : recipients.mid(0, 5))
        escapedRows.append(row.toHtmlEscaped());
    recipientPreview = escapedRows.join(QStringLiteral("<br>"));
    if (recipients.size() > 5) {
        recipientPreview +=
            QStringLiteral("<br>…and %1 more")
                .arg(recipients.size() - 5);
    }

    QMessageBox confirmation(this);
    confirmation.setIcon(QMessageBox::Warning);
    confirmation.setWindowTitle(
        QStringLiteral("Confirm community reward transfer"));
    confirmation.setText(
        QStringLiteral(
            "<b>Explicit owner approval required</b><br><br>"
            "Transfer a total of <b>%1</b> from public pool<br><code>%2</code>"
            "<br>through %3 exact System Program transfer(s):<br>%4<br><br>"
            "Network: %5<br>Policy: %6<br>Intent: %7<br>Expires: %8<br><br>"
            "A normal Solana network fee is additional. No other program or "
            "instruction is allowed. The transaction is submitted "
            "directly to your configured public RPC. Visual fountain effects "
            "are separate from this transaction.")
            .arg(formatLamports(intent.totalLamports).toHtmlEscaped(),
                 intent.sourceWallet.toHtmlEscaped(),
                 QString::number(intent.transfers.size()),
                 recipientPreview,
                 intent.network.toHtmlEscaped(),
                 intent.policy.toHtmlEscaped(),
                 intent.intentId.toHtmlEscaped(),
                 QDateTime::fromMSecsSinceEpoch(intent.expiresAtMs)
                     .toLocalTime()
                     .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss t"))
                     .toHtmlEscaped()));
    confirmation.setTextFormat(Qt::RichText);
    QPushButton *approve = confirmation.addButton(
        QStringLiteral("Confirm, sign and submit"), QMessageBox::AcceptRole);
    confirmation.addButton(QMessageBox::Cancel);
    confirmation.setDefaultButton(QMessageBox::Cancel);
    confirmation.exec();
    if (confirmation.clickedButton() != approve)
        return;
    prepareRewardPoolTransaction(object);
}

void MainWindow::toggleMicrojobAutoPayouts()
{
    if (!m_microjobAutoPayoutPassphrase.isEmpty()) {
        forkmesh::rewards::secureErase(m_microjobAutoPayoutPassphrase);
        m_microjobAutoPayoutPassphrase.clear();
        if (m_microjobAutoPayoutStatus) {
            m_microjobAutoPayoutStatus->setText(
                QStringLiteral(
                    "Disarmed. The passphrase was wiped from memory; microjob "
                    "plans now need the same explicit approval as every other "
                    "transfer."));
        }
        logSystem(QStringLiteral(
            "Reward pool: microjob auto-payouts disarmed."));
        refreshRewardPoolControls();
        return;
    }

    // Fail closed on exactly the same configuration the reviewed path needs,
    // before asking for anything secret.
    QString error;
    const QString vaultPath = forkmesh::rewards::defaultPoolVaultPath();
    const QString poolAddress =
        forkmesh::rewards::poolVaultPublicAddress(vaultPath, &error);
    const QSettings settings;
    QUrl rpc;
    if (poolAddress.isEmpty() ||
        !forkmesh::rewards::validatePublicRpcUrl(
            settings.value(kRewardRpcSetting).toString(), &rpc, &error)) {
        if (m_microjobAutoPayoutStatus) {
            m_microjobAutoPayoutStatus->setText(
                QStringLiteral(
                    "Configure the encrypted pool vault and save a public "
                    "RPC/network before arming auto-payouts."));
        }
        return;
    }

    QMessageBox confirmation(this);
    confirmation.setIcon(QMessageBox::Warning);
    confirmation.setWindowTitle(
        QStringLiteral("Arm microjob auto-payouts"));
    confirmation.setTextFormat(Qt::RichText);
    confirmation.setText(
        QStringLiteral(
            "<b>This signs without asking you again</b><br><br>"
            "For the rest of this session, plans of kind "
            "<code>microjob_payout</code> from public pool<br><code>%1</code>"
            "<br>will be signed and submitted automatically, up to a total of "
            "<b>%2</b> per day.<br><br>"
            "Nothing else is affected: every other reward plan still asks for "
            "explicit per-transfer approval. Your passphrase is kept only in "
            "this process's memory and is wiped when you disarm or quit.")
            .arg(poolAddress.toHtmlEscaped(),
                 formatLamports(kMicrojobAutoPayoutDailyCapLamports)
                     .toHtmlEscaped()));
    QPushButton *approve = confirmation.addButton(
        QStringLiteral("Arm for this session"), QMessageBox::AcceptRole);
    confirmation.addButton(QMessageBox::Cancel);
    confirmation.setDefaultButton(QMessageBox::Cancel);
    confirmation.exec();
    if (confirmation.clickedButton() != approve)
        return;

    QInputDialog unlock(this);
    unlock.setWindowTitle(QStringLiteral("Unlock local pool-key vault"));
    unlock.setLabelText(
        QStringLiteral(
            "Enter the local vault passphrase. It is held in memory for this "
            "session's microjob payouts only, and is never saved or "
            "transmitted."));
    unlock.setTextEchoMode(QLineEdit::Password);
    unlock.setInputMode(QInputDialog::TextInput);
    if (unlock.exec() != QDialog::Accepted)
        return;
    QString passphrase = unlock.textValue();
    unlock.setTextValue(QString(passphrase.size(), QChar(u'\0')));
    unlock.setTextValue(QString());
    if (passphrase.isEmpty())
        return;
    m_microjobAutoPayoutPassphrase = passphrase;
    forkmesh::rewards::secureErase(passphrase);
    m_microjobAutoPayoutDay = QDate::currentDate();
    m_microjobAutoPaidLamports = 0;
    if (m_microjobAutoPayoutStatus) {
        m_microjobAutoPayoutStatus->setText(
            QStringLiteral(
                "Armed. Microjob payout plans will be signed automatically "
                "until you disarm or quit."));
    }
    logSystem(QStringLiteral(
        "Reward pool: microjob auto-payouts armed for this session."));
    refreshRewardPoolControls();
    autoSignMicrojobPayouts();
}

void MainWindow::autoSignMicrojobPayouts()
{
    if (m_microjobAutoPayoutPassphrase.isEmpty() || m_rewardPoolBusy)
        return;
    const QDate today = QDate::currentDate();
    if (m_microjobAutoPayoutDay != today) {
        m_microjobAutoPayoutDay = today;
        m_microjobAutoPaidLamports = 0;
    }
    const QSettings settings;
    const QString network = settings.value(kRewardNetworkSetting).toString();
    QString error;
    const QString poolAddress = forkmesh::rewards::poolVaultPublicAddress(
        forkmesh::rewards::defaultPoolVaultPath(), &error);
    if (poolAddress.isEmpty())
        return;

    // One plan per pass. Signing sets m_rewardPoolBusy and the next fetch
    // picks up whatever is still pending, so a backlog drains without ever
    // running two signing flows against the same vault at once.
    for (auto it = m_rewardPoolIntents.constBegin();
         it != m_rewardPoolIntents.constEnd(); ++it) {
        forkmesh::rewards::RewardSigningJob parsed;
        if (!forkmesh::rewards::parseRewardSigningJob(
                it.value(), poolAddress, network,
                QDateTime::currentDateTimeUtc(), &parsed, &error))
            continue;
        if (parsed.kind != kMicrojobPayoutKind ||
            parsed.status != QLatin1String("pending_signature"))
            continue;
        if (parsed.totalLamports >
                kMicrojobAutoPayoutDailyCapLamports - m_microjobAutoPaidLamports) {
            if (m_microjobAutoPayoutStatus) {
                m_microjobAutoPayoutStatus->setText(
                    QStringLiteral(
                        "Today's %1 auto-payout limit is reached. Remaining "
                        "microjob plans are waiting for the reviewed path.")
                        .arg(formatLamports(
                            kMicrojobAutoPayoutDailyCapLamports)));
            }
            return;
        }
        m_microjobAutoPaidLamports += parsed.totalLamports;
        if (m_microjobAutoPayoutStatus) {
            m_microjobAutoPayoutStatus->setText(
                QStringLiteral(
                    "Auto-signing microjob payout %1 (%2 to %3 recipient(s)); "
                    "%4 of today's limit used.")
                    .arg(parsed.intentId.left(12),
                         formatLamports(parsed.totalLamports),
                         QString::number(parsed.transfers.size()),
                         formatLamports(m_microjobAutoPaidLamports)));
        }
        logSystem(
            QStringLiteral(
                "Reward pool: auto-signing microjob payout intent %1.")
                .arg(parsed.intentId.left(16)));
        prepareRewardPoolTransaction(
            it.value(), m_microjobAutoPayoutPassphrase);
        return;
    }
}

void MainWindow::prepareRewardPoolTransaction(
    const QJsonObject &job, const QString &sessionPassphrase)
{
    if (m_rewardPoolBusy)
        return;
    const QSettings settings;
    const QString network =
        settings.value(kRewardNetworkSetting).toString();
    QUrl rpc;
    QString error;
    if (!forkmesh::rewards::validatePublicRpcUrl(
            settings.value(kRewardRpcSetting).toString(), &rpc, &error)) {
        if (m_rewardPoolStatus)
            m_rewardPoolStatus->setText(error);
        return;
    }
    const QString vaultPath =
        forkmesh::rewards::defaultPoolVaultPath();
    const QString poolAddress =
        forkmesh::rewards::poolVaultPublicAddress(vaultPath, &error);
    forkmesh::rewards::RewardSigningJob parsedJob;
    if (poolAddress.isEmpty() ||
        !forkmesh::rewards::parseRewardSigningJob(
            job, poolAddress, network, QDateTime::currentDateTimeUtc(),
            &parsedJob, &error) ||
        parsedJob.status != QLatin1String("pending_signature")) {
        if (m_rewardPoolStatus)
            m_rewardPoolStatus->setText(
                QStringLiteral("Job re-validation failed: %1").arg(error));
        return;
    }

    QNetworkRequest request(rpc);
    hardenJsonRequest(request);
    const QJsonObject payload{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), QStringLiteral("forkmesh-reward-blockhash")},
        {QStringLiteral("method"), QStringLiteral("getLatestBlockhash")},
        {QStringLiteral("params"),
         QJsonArray{QJsonObject{
             {QStringLiteral("commitment"), QStringLiteral("finalized")}}}}};
    m_rewardPoolBusy = true;
    refreshRewardPoolControls();
    if (m_rewardPoolStatus) {
        m_rewardPoolStatus->setText(
            QStringLiteral(
                "Owner approved the public plan; fetching a recent Solana "
                "blockhash before the one-use local vault unlock…"));
    }
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, job, network, poolAddress, vaultPath,
             sessionPassphrase] {
        const QByteArray body = boundedReplyBody(reply);
        const bool networkOk =
            reply->error() == QNetworkReply::NoError;
        reply->deleteLater();
        m_rewardPoolBusy = false;
        const QJsonObject response =
            QJsonDocument::fromJson(body).object();
        const QJsonObject value =
            response.value(QStringLiteral("result"))
                .toObject()
                .value(QStringLiteral("value"))
                .toObject();
        const QString blockhash =
            value.value(QStringLiteral("blockhash")).toString();
        const QString heightText =
            jsonSlotString(
                value.value(QStringLiteral("lastValidBlockHeight")));
        bool heightOk = false;
        const quint64 lastValidBlockHeight =
            heightText.toULongLong(&heightOk);
        QString error;
        QJsonObject transactionIntent;
        if (!networkOk ||
            !response.value(QStringLiteral("error")).isUndefined() ||
            !heightOk ||
            !forkmesh::rewards::buildRewardTransactionIntent(
                job, poolAddress, network, blockhash,
                lastValidBlockHeight, QDateTime::currentDateTimeUtc(),
                &transactionIntent, &error)) {
            if (m_rewardPoolStatus) {
                m_rewardPoolStatus->setText(
                    error.isEmpty()
                        ? QStringLiteral(
                              "The Solana RPC did not return a usable recent blockhash; nothing was signed.")
                        : error);
            }
            refreshRewardPoolControls();
            return;
        }

        // An armed microjob session supplies the passphrase; every other plan
        // asks for it here, for this one approved transaction.
        QString passphrase = sessionPassphrase;
        if (passphrase.isEmpty()) {
            QInputDialog unlock(this);
            unlock.setWindowTitle(
                QStringLiteral("Unlock local pool-key vault"));
            unlock.setLabelText(
                QStringLiteral(
                    "Enter the local vault passphrase. It is used only in memory "
                    "for this one approved transaction and is never saved or transmitted."));
            unlock.setTextEchoMode(QLineEdit::Password);
            unlock.setInputMode(QInputDialog::TextInput);
            if (unlock.exec() != QDialog::Accepted)
                return;
            passphrase = unlock.textValue();
            unlock.setTextValue(
                QString(passphrase.size(), QChar(u'\0')));
            unlock.setTextValue(QString());
        }
        if (passphrase.isEmpty())
            return;

        forkmesh::rewards::SignedRewardTransaction signedTransaction;
        const bool signedOk =
            forkmesh::rewards::signRewardTransferIntent(
                transactionIntent, network, passphrase, vaultPath,
                QDateTime::currentDateTimeUtc(), &signedTransaction,
                &error);
        forkmesh::rewards::secureErase(passphrase);
        if (!signedOk) {
            if (m_rewardPoolStatus)
                m_rewardPoolStatus->setText(
                    QStringLiteral("Local signing failed: %1").arg(error));
            return;
        }
        if (signedTransaction.intentId !=
                transactionIntent.value(
                                     QStringLiteral("intentId"))
                    .toString() ||
            signedTransaction.signerPublicKey != poolAddress ||
            !forkmesh::rewards::isValidSolanaSignature(
                signedTransaction.chainSignature)) {
            if (m_rewardPoolStatus) {
                m_rewardPoolStatus->setText(
                    QStringLiteral(
                        "The local signer failed its post-signature checks; nothing was submitted."));
            }
            return;
        }
        submitRewardPoolTransaction(
            transactionIntent,
            signedTransaction.signedTransactionBase64,
            signedTransaction.chainSignature);
    });
}

void MainWindow::submitRewardPoolTransaction(
    const QJsonObject &intent, const QString &signedTransactionBase64,
    const QString &chainSignature)
{
    if (m_rewardPoolBusy)
        return;
    const QSettings settings;
    QUrl rpc;
    QString error;
    const QString network =
        settings.value(kRewardNetworkSetting).toString();
    if (!forkmesh::rewards::validatePublicRpcUrl(
            settings.value(kRewardRpcSetting).toString(), &rpc, &error) ||
        intent.value(QStringLiteral("network")).toString() != network ||
        !forkmesh::rewards::isValidSolanaSignature(chainSignature)) {
        if (m_rewardPoolStatus) {
            m_rewardPoolStatus->setText(
                QStringLiteral(
                    "Fail closed: the local RPC/network configuration changed before submission."));
        }
        return;
    }
    const QString intentId =
        intent.value(QStringLiteral("intentId")).toString();
    const QString poolAddress =
        intent.value(QStringLiteral("sourceWallet")).toString();
    QNetworkRequest request(rpc);
    hardenJsonRequest(request);
    const QJsonObject rpcPayload{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), QStringLiteral("forkmesh-reward-submit")},
        {QStringLiteral("method"), QStringLiteral("sendTransaction")},
        {QStringLiteral("params"),
         QJsonArray{
             signedTransactionBase64,
             QJsonObject{
                 {QStringLiteral("encoding"), QStringLiteral("base64")},
                 {QStringLiteral("skipPreflight"), false},
                 {QStringLiteral("preflightCommitment"),
                  QStringLiteral("finalized")},
                 {QStringLiteral("maxRetries"), 3}}}}};
    m_rewardPoolBusy = true;
    refreshRewardPoolControls();
    if (m_rewardPoolStatus) {
        m_rewardPoolStatus->setText(
            QStringLiteral(
                "Locally signed; submitting the public transaction directly "
                "to the configured Solana RPC…"));
    }
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(rpcPayload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, intentId, poolAddress, network, rpc,
             chainSignature] {
        const QByteArray body = boundedReplyBody(reply);
        const bool networkOk =
            reply->error() == QNetworkReply::NoError;
        reply->deleteLater();
        m_rewardPoolBusy = false;
        const QJsonObject response =
            QJsonDocument::fromJson(body).object();
        const QString returnedSignature =
            response.value(QStringLiteral("result")).toString();
        if (!networkOk || body.isEmpty() ||
            !response.value(QStringLiteral("error")).isUndefined() ||
            returnedSignature != chainSignature) {
            if (m_rewardPoolStatus) {
                const QJsonObject rpcError =
                    response.value(QStringLiteral("error")).toObject();
                const QString code =
                    rpcError.value(QStringLiteral("code")).toVariant().toString();
                m_rewardPoolStatus->setText(
                    code.isEmpty()
                        ? QStringLiteral(
                              "Solana RPC submission failed; no completed "
                              "transfer was recorded.")
                        : QStringLiteral(
                              "Solana RPC rejected the transaction (public "
                              "error code %1).")
                              .arg(code));
            }
            refreshRewardPoolControls();
            return;
        }
        QSettings settings;
        settings.setValue(kPendingIntentSetting, intentId);
        settings.setValue(kPendingSignatureSetting, chainSignature);
        settings.setValue(kPendingPoolSetting, poolAddress);
        settings.setValue(
            kPendingRpcSetting,
            rpc.toString(QUrl::StripTrailingSlash));
        settings.setValue(kPendingNetworkSetting, network);
        settings.setValue(
            kPendingSubmittedAtSetting,
            QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        if (m_rewardPoolStatus) {
            m_rewardPoolStatus->setText(
                QStringLiteral(
                    "Transaction submitted (%1…). Waiting for finalized "
                    "on-chain status before Worker reconciliation.")
                    .arg(chainSignature.left(12)));
        }
        logSystem(
            QStringLiteral(
                "Reward pool: public transaction %1 submitted for intent %2.")
                .arg(chainSignature.left(16), intentId.left(16)));
        refreshRewardPoolControls();
        submitRewardSignatureReceipt(intentId, chainSignature, false);
    });
}

void MainWindow::submitRewardSignatureReceipt(
    const QString &intentId, const QString &chainSignature,
    bool finalizationObserved)
{
    if (m_rewardPoolBusy)
        return;
    const QString signer = accountOwner().trimmed().toLower();
    const QString timestamp =
        QString::number(QDateTime::currentMSecsSinceEpoch());
    const QByteArray canonical =
        forkmesh::rewards::signingJobSubmitCanonical(
            signer, intentId, chainSignature, timestamp);
    const QString ownerSignature =
        m_profileIdentity.isValid()
            ? m_profileIdentity.signData(canonical)
            : QString();
    const QUrl endpoint = rewardPoolWorkerEndpoint(
        QStringLiteral("/api/rewards/signing-jobs"));
    if (canonical.isEmpty() || ownerSignature.isEmpty() ||
        endpoint.isEmpty() || !m_networkAccess ||
        !hasOwnerSigningCapability(signer)) {
        if (m_rewardPoolStatus) {
            m_rewardPoolStatus->setText(
                QStringLiteral(
                    "The public transaction was submitted, but the configured "
                    "first-owner identity cannot send its Worker receipt. The "
                    "public signature was retained for retry."));
        }
        refreshRewardPoolControls();
        return;
    }
    const QJsonObject payload{
        {QStringLiteral("signer"), signer},
        {QStringLiteral("intentId"), intentId},
        {QStringLiteral("transactionSignature"), chainSignature},
        {QStringLiteral("ts"), timestamp},
        {QStringLiteral("sig"), ownerSignature}};
    QNetworkRequest request(endpoint);
    hardenJsonRequest(request);
    m_rewardPoolBusy = true;
    refreshRewardPoolControls();
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, intentId, chainSignature,
             finalizationObserved] {
        const QByteArray body = boundedReplyBody(reply);
        const int httpStatus =
            reply->attribute(
                     QNetworkRequest::HttpStatusCodeAttribute)
                .toInt();
        const bool networkOk =
            reply->error() == QNetworkReply::NoError;
        const QString failure =
            publicNetworkFailure(
                reply, QStringLiteral("Reward signature receipt"));
        reply->deleteLater();
        m_rewardPoolBusy = false;
        const QJsonObject response =
            QJsonDocument::fromJson(body).object();
        const QJsonObject job =
            response.value(QStringLiteral("job")).toObject();
        const QString jobStatus =
            job.value(QStringLiteral("status")).toString();
        const bool completed =
            networkOk &&
            response.value(QStringLiteral("ok")).toBool(false) &&
            job.value(QStringLiteral("intentId")).toString() ==
                intentId &&
            job.value(QStringLiteral("transactionSignature")).toString() ==
                chainSignature &&
            jobStatus == QLatin1String("completed");
        const bool pending =
            networkOk &&
            response.value(QStringLiteral("ok")).toBool(false) &&
            (httpStatus == 202 ||
             response.value(QStringLiteral("status")).toString() ==
                 QLatin1String("submitted"));
        const QString workerError =
            response.value(QStringLiteral("error")).toString();
        const bool terminalFailure =
            finalizationObserved &&
            (workerError == QLatin1String(
                                "transaction_plan_mismatch") ||
             workerError == QLatin1String(
                                "intent_already_completed"));
        if (completed || terminalFailure) {
            QSettings settings;
            settings.remove(kPendingIntentSetting);
            settings.remove(kPendingSignatureSetting);
            settings.remove(kPendingPoolSetting);
            settings.remove(kPendingRpcSetting);
            settings.remove(kPendingNetworkSetting);
            settings.remove(kPendingSubmittedAtSetting);
            m_rewardPoolIntents.remove(intentId);
            if (m_rewardPoolIntentsTable) {
                for (int row =
                         m_rewardPoolIntentsTable->rowCount() - 1;
                     row >= 0; --row) {
                    if (m_rewardPoolIntentsTable->item(row, 0) &&
                        m_rewardPoolIntentsTable->item(row, 0)
                                ->data(Qt::UserRole)
                                .toString() == intentId) {
                        m_rewardPoolIntentsTable->removeRow(row);
                    }
                }
            }
            if (m_rewardPoolStatus) {
                m_rewardPoolStatus->setText(
                    completed
                        ? QStringLiteral(
                              "Completed on chain and independently reconciled "
                              "by the Worker (signature %1…).")
                              .arg(chainSignature.left(12))
                        : QStringLiteral(
                              "The finalized transaction did not match the "
                              "Worker plan and was recorded as rejected."));
            }
            logSystem(
                QStringLiteral(
                    "Reward pool: intent %1 reached terminal Worker status %2.")
                    .arg(intentId.left(16),
                         completed ? QStringLiteral("completed")
                                   : QStringLiteral("rejected")));
            refreshRewardPoolControls();
            return;
        }
        if (pending) {
            if (m_rewardPoolStatus) {
                m_rewardPoolStatus->setText(
                    QStringLiteral(
                        "Worker recorded the public signature and is awaiting "
                        "finality; the desktop is reconciling it independently."));
            }
            refreshRewardPoolControls();
            pollRewardPoolFinalization(0);
            return;
        }
        if (m_rewardPoolStatus)
            m_rewardPoolStatus->setText(failure);
        refreshRewardPoolControls();
    });
}

void MainWindow::pollRewardPoolFinalization(int attempt)
{
    if (m_rewardPoolBusy)
        return;
    QSettings settings;
    const QString intentId =
        settings.value(kPendingIntentSetting).toString();
    const QString chainSignature =
        settings.value(kPendingSignatureSetting).toString();
    const QString poolAddress =
        settings.value(kPendingPoolSetting).toString();
    const QString network =
        settings.value(kPendingNetworkSetting).toString();
    QUrl rpc;
    QString error;
    if (intentId.isEmpty() || poolAddress.isEmpty() ||
        !forkmesh::rewards::isValidSolanaSignature(chainSignature) ||
        !forkmesh::rewards::validatePublicRpcUrl(
            settings.value(kPendingRpcSetting).toString(), &rpc, &error) ||
        (network != QLatin1String("devnet") &&
         network != QLatin1String("testnet") &&
         network != QLatin1String("mainnet-beta"))) {
        if (m_rewardPoolStatus) {
            m_rewardPoolStatus->setText(
                QStringLiteral(
                    "No valid public submitted-transaction record is available to reconcile."));
        }
        refreshRewardPoolControls();
        return;
    }
    if (attempt >= kFinalizationPollLimit) {
        if (m_rewardPoolStatus) {
            m_rewardPoolStatus->setText(
                QStringLiteral(
                    "The transaction remains submitted but was not finalized "
                    "during this polling window. Use Reconcile last submission "
                    "later; no key or signed transaction was persisted."));
        }
        refreshRewardPoolControls();
        return;
    }

    QNetworkRequest request(rpc);
    hardenJsonRequest(request);
    const QJsonObject payload{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), QStringLiteral("forkmesh-reward-status")},
        {QStringLiteral("method"), QStringLiteral("getSignatureStatuses")},
        {QStringLiteral("params"),
         QJsonArray{
             QJsonArray{chainSignature},
             QJsonObject{{QStringLiteral("searchTransactionHistory"), true}}}}};
    m_rewardPoolBusy = true;
    refreshRewardPoolControls();
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, attempt, intentId, chainSignature] {
        const QByteArray body = boundedReplyBody(reply);
        const bool networkOk =
            reply->error() == QNetworkReply::NoError;
        reply->deleteLater();
        m_rewardPoolBusy = false;
        const QJsonObject response =
            QJsonDocument::fromJson(body).object();
        const QJsonArray values =
            response.value(QStringLiteral("result"))
                .toObject()
                .value(QStringLiteral("value"))
                .toArray();
        const QJsonValue statusValue =
            values.isEmpty() ? QJsonValue() : values.at(0);
        if (networkOk && response.value(QStringLiteral("error")).isUndefined() &&
            statusValue.isObject()) {
            const QJsonObject status = statusValue.toObject();
            const QString confirmation =
                status.value(QStringLiteral("confirmationStatus")).toString();
            const QString slot =
                jsonSlotString(status.value(QStringLiteral("slot")));
            if (confirmation == QLatin1String("finalized") &&
                !slot.isEmpty()) {
                const bool failed =
                    !status.value(QStringLiteral("err")).isNull() &&
                    !status.value(QStringLiteral("err")).isUndefined();
                reconcileRewardPoolIntent(
                    intentId, chainSignature,
                    failed ? QStringLiteral("failed")
                           : QStringLiteral("finalized"),
                    slot);
                return;
            }
        }
        if (m_rewardPoolStatus) {
            m_rewardPoolStatus->setText(
                QStringLiteral(
                    "Transaction %1… is not finalized yet (check %2/%3).")
                    .arg(chainSignature.left(12))
                    .arg(attempt + 1)
                    .arg(kFinalizationPollLimit));
        }
        refreshRewardPoolControls();
        if (m_rewardPoolFinalizeTimer) {
            m_rewardPoolFinalizeTimer->setProperty("attempt", attempt + 1);
            m_rewardPoolFinalizeTimer->start(kFinalizationPollMs);
        }
    });
}

void MainWindow::reconcileRewardPoolIntent(
    const QString &intentId, const QString &chainSignature,
    const QString &observedStatus, const QString &slot)
{
    if (m_rewardPoolStatus) {
        m_rewardPoolStatus->setText(
            observedStatus == QLatin1String("finalized")
                ? QStringLiteral(
                      "Finalized chain status observed at slot %1; asking the "
                      "Worker to independently verify the exact public transfer plan…")
                      .arg(slot)
                : QStringLiteral(
                      "A finalized on-chain failure was observed at slot %1; "
                      "asking the Worker to record the terminal result…")
                      .arg(slot));
    }
    submitRewardSignatureReceipt(intentId, chainSignature, true);
}

#ifdef FORKMESH_WINDOW_TESTS
QString MainWindow::testContributorRewardsStatusText() const
{
    return m_contributorRewardsStatus ? m_contributorRewardsStatus->text()
                                      : QString();
}
#endif
