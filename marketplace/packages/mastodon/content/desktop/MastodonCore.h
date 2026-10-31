#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>

namespace forkmesh::mastodon {

inline const QString kScopes = QStringLiteral("read write follow");
inline const QString kClientName = QStringLiteral("ForkMesh Desktop");
inline const QString kClientWebsite = QStringLiteral("https://forkmesh.com");
inline const QString kOutOfBand = QStringLiteral("urn:ietf:wg:oauth:2.0:oob");
inline const QString kTagScheme = QStringLiteral("fm-tag");
inline const QString kAccountScheme = QStringLiteral("fm-account");

struct SavedAccount {
    QString instance;
    QString token;
    QString clientId;
    QString clientSecret;
    QString id;
    QString acct;
    QString displayName;
    QString avatar;

    QString key() const;
    QString handle() const;
    QJsonObject toJson() const;
    static SavedAccount fromJson(const QJsonObject &object);
    bool valid() const;
};

QList<SavedAccount> accountsFromJson(const QByteArray &json);
QByteArray accountsToJson(const QList<SavedAccount> &accounts);

QString normalizeInstance(const QString &raw);
QString instanceHost(const QString &instance);
bool safeWebUrl(const QUrl &url);

struct RichText {
    QString html;
    QString plain;
};

RichText renderContent(const QString &html, const QJsonArray &mentions = {},
                       const QJsonArray &tags = {});
QString plainText(const QString &html);
QString escapeText(const QString &text);
QString elide(const QString &text, int length);

QString relativeTime(const QDateTime &when, const QDateTime &now);
QString relativeTime(const QString &iso, const QDateTime &now);
QString countText(qint64 count);

QUrl linkTarget(const QByteArray &linkHeader, const QString &relation);
QString codeFromCallback(const QString &pasted);
QUrlQuery callbackQuery(const QByteArray &requestHead);
QString randomToken(int bytes = 32);
QString pkceChallenge(const QString &verifier);
QUrl authorizeUrl(const QString &instance, const QString &clientId,
                  const QString &redirect, const QString &state,
                  const QString &challenge);

QString displayName(const QJsonObject &account);
QString fullHandle(const QJsonObject &account, const QString &instance);
QJsonObject shownStatus(const QJsonObject &status);
QString visibilityLabel(const QString &visibility);
QString visibilityIcon(const QString &visibility);
QStringList visibilityValues();
QString notificationSentence(const QString &type);
QString filterHideReason(const QJsonObject &status, const QString &context,
                         bool *hide);

int statusLength(const QString &text);

}
