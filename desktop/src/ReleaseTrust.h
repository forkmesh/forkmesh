#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace forkmesh::releasetrust {

QByteArray pinnedPublicKey();

QString officialRepository();

bool verifyEd25519(const QByteArray &rawPublicKey, const QByteArray &payload,
                   const QByteArray &signature);

QList<int> parseVersion(const QString &text);

int compareVersions(const QString &left, const QString &right);

QString newestReleaseTag(const QStringList &tags);

bool isCommitId(const QString &text);

bool channelAccepts(const QString &wantedChannel, const QString &manifestChannel,
                    const QString &tag);

struct Verdict {
    bool ok = false;
    QString reason;
    QJsonObject manifest;
    QString tagCommit;
};

Verdict checkManifest(const QByteArray &manifestBytes,
                      const QByteArray &signature, const QString &expectedTag,
                      const QString &runningVersion,
                      const QString &channel = QStringLiteral("latest"),
                      const QByteArray &publicKey = pinnedPublicKey());

}
