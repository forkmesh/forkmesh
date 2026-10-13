#pragma once

// File-local helpers shared by the MainWindow feature files that were split
// out of MainWindowChat.cpp (hosts, Vultr mirrors, host install, nodes,
// users, network diagnostics and the navigation rail). Header-only and in an
// anonymous namespace, exactly as they were when one translation unit held
// all of their callers. Include after MainWindowInternal.h.

#include "ForkMeshIdentity.h"
#include "MainWindowInternal.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QHeaderView>
#include <QJsonObject>
#include <QPushButton>
#include <QRegularExpression>
#include <QTableWidget>

namespace {

inline bool verifiedFlagshipMirrorHealth(const QJsonObject &payload,
                                         const QString &expectedNode,
                                         const QString &nonce,
                                         qint64 issuedAt)
{
    const QJsonObject proof =
        payload.value(QStringLiteral("repositoryProof")).toObject();
    const QJsonObject challenge =
        payload.value(QStringLiteral("challenge")).toObject();
    const QString node =
        payload.value(QStringLiteral("node")).toString().trimmed().toLower();
    const QByteArray signedMessage =
        QByteArrayLiteral("forkmesh-https-health-repository-v1\n") +
        node.toUtf8() + '\n' + nonce.toUtf8() + '\n' +
        QByteArray::number(issuedAt) +
        QByteArrayLiteral("\nforkmesh\nforkmesh\n1\n") +
        proof.value(QStringLiteral("integrity")).toString().toUtf8() + '\n' +
        proof.value(QStringLiteral("refsSha256")).toString().toUtf8() + '\n' +
        proof.value(QStringLiteral("operationsSha256")).toString().toUtf8();
    const QString messageSha256 = QString::fromLatin1(
        QCryptographicHash::hash(signedMessage, QCryptographicHash::Sha256)
            .toHex());
    return node == expectedNode.trimmed().toLower() &&
           proof.value(QStringLiteral("owner")).toString() ==
               QLatin1String("forkmesh") &&
           proof.value(QStringLiteral("repository")).toString() ==
               QLatin1String("forkmesh") &&
           proof.value(QStringLiteral("available")).toBool() &&
           proof.value(QStringLiteral("integrity")).toString() ==
               QLatin1String("ok") &&
           // A valid signature over an empty repository is still an empty
           // mirror: it serves nothing, so it is neither deploy-verified nor
           // fleet-healthy (2026-08-19: empty mirrors counted as healthy
           // capacity and the reconciler destroyed the last data-bearing one).
           forkmesh::control::mirrorRepositoryProofServesContent(
               proof.value(QStringLiteral("refsSha256")).toString()) &&
           challenge.value(QStringLiteral("messageType")).toString() ==
               QLatin1String("forkmesh-https-health-repository-v1") &&
           challenge.value(QStringLiteral("nonce")).toString() == nonce &&
           qint64(challenge.value(QStringLiteral("issuedAt")).toDouble()) ==
               issuedAt &&
           challenge.value(QStringLiteral("messageSha256")).toString() ==
               messageSha256 &&
           ForkMeshIdentity::verifySignature(
               payload.value(QStringLiteral("publicKey")).toString(),
               challenge.value(QStringLiteral("signature")).toString(),
               signedMessage);
}

inline QPushButton *makeInlineHelpButton(const QString &accessibleName,
                                         const QString &helpText,
                                         QWidget *parent = nullptr)
{
    auto *button = new QPushButton(parent);
    button->setObjectName(QStringLiteral("inlineHelpButton"));
    button->setAccessibleName(accessibleName);
    button->setAccessibleDescription(helpText);
    button->setToolTip(
        QStringLiteral("<div style='width: 420px; white-space: normal;'>%1</div>")
            .arg(helpText.toHtmlEscaped()));
    button->setCursor(Qt::PointingHandCursor);
    button->setFlat(true);
    button->setFixedSize(24, 24);
    forkmesh::ui::setOcticon(button, "info", 14);
    return button;
}

const QString kVultrProvisionSetting =
    QStringLiteral("hosts/vultrProvision/v1");

const char *const kSolanaRpcEndpoints[] = {
    "https://api.mainnet-beta.solana.com",
    "https://solana-rpc.publicnode.com",
};

inline bool isLikelySolanaAddress(const QString &address)
{
    static const QRegularExpression re(
        QStringLiteral("^[1-9A-HJ-NP-Za-km-z]{32,44}$"));
    return re.match(address.trimmed()).hasMatch();
}

inline QString formatSolanaBalance(qint64 lamports)
{
    return QStringLiteral("%1 SOL").arg(lamports / 1000000000.0, 0, 'f', 9);
}

inline void networkPrepareFullTable(QTableWidget *table)
{
    table->setTextElideMode(Qt::ElideNone);
    table->setWordWrap(false);
    table->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    table->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    table->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    table->verticalHeader()->setDefaultSectionSize(24);
    // These diagnostics grids are deliberately single-line.  Asking the item
    // delegate to shape every cell again on each telemetry tick made
    // resizeRowsToContents() block the GUI thread for hundreds of milliseconds.
    table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
}

inline QString hostInstallEventTimestamp()
{
    return QDateTime::currentDateTime().toString(Qt::ISODate);
}

inline QString timestampedHostInstallEvent(const QString &message)
{
    return QStringLiteral("[%1] %2").arg(hostInstallEventTimestamp(), message);
}

} // namespace
