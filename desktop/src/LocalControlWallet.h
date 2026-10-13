#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

// The "wallet." local control family (WS19; docs/native/DESKTOP_PARITY.md
// section 19): tip and bounty signing requests the owner approves on the phone. This is the policy half, kept free of widgets so the
// tests can drive it; the requests are answered in
// MainWindowLocalControlWallet.cpp. Gate 2 creates it empty; WS19 owns it.
namespace forkmesh::localcontrol::wallet {

// The method prefix this family answers.
QString familyPrefix();

QStringList actions();
bool isKnownAction(const QString &action);

constexpr int kMaxRewards = 100;
constexpr int kMaxRecipientChars = 63;
constexpr int kMaxRewardIdChars = 128;
constexpr int kMaxTitleChars = 200;
constexpr qint64 kRequestStaleMs = 15 * 60 * 1000;

QString confirmationHolder(const QString &deviceKey, int connectionId);

bool validRecipient(const QString &name);
bool plausibleAddress(const QString &address);
bool validRewardId(const QString &rewardId);
bool parseLamports(const QJsonValue &value, quint64 *lamports);
bool lamportsInRange(quint64 lamports, quint64 minimum, quint64 maximum);

QString tipTarget(const QString &recipient, const QString &address,
                  quint64 lamports);
QString rewardTarget(const QString &rewardId);

QString solText(quint64 lamports);
QString tipSummary(const QString &recipient, const QString &address,
                   quint64 lamports);
QString rewardSummary(const QString &recipient, quint64 lamports,
                      const QString &subject);

struct RewardFacts {
    QString rewardId;
    QString recipient;
    QString address;
    quint64 lamports = 0;
    QString repository;
    QString kind;
    int number = 0;
    QString title;
};
bool rewardPayable(const RewardFacts &facts);
QJsonObject rewardRow(const RewardFacts &facts);
QJsonArray rewardRows(const QList<RewardFacts> &facts, int max = kMaxRewards);
QString rewardSubject(const RewardFacts &facts);

struct PhoneRequest {
    QString id;
    QString action;
    QString summary;
    QString status;
    qint64 requestedAtMs = 0;
};
bool requestBlocks(const PhoneRequest &request, qint64 nowMs);
QJsonObject requestRow(const PhoneRequest &request);
QString newRequestId(const QByteArray &random);

struct WalletFacts {
    bool linked = false;
    QString address;
    bool busy = false;
    int queued = 0;
    quint64 minLamports = 0;
    quint64 maxLamports = 0;
};
QJsonObject statusPayload(const WalletFacts &facts, const PhoneRequest &request,
                          qint64 nowMs);

} // namespace forkmesh::localcontrol::wallet
