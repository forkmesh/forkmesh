#include "ReleaseTrust.h"

#include <QJsonDocument>
#include <QRegularExpression>

#include <openssl/evp.h>

namespace forkmesh::releasetrust {

QByteArray pinnedPublicKey()
{
    return QByteArray::fromBase64(
        QByteArrayLiteral("HBYOCW4qnyZkYAnEoqUrYxPiDRszjfJa+xJUeyQPUS0="));
}

QString officialRepository()
{
    return QStringLiteral("forkmesh/forkmesh");
}

bool verifyEd25519(const QByteArray &rawPublicKey, const QByteArray &payload,
                   const QByteArray &signature)
{
    if (rawPublicKey.size() != 32 || signature.size() != 64)
        return false;
    EVP_PKEY *key = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, nullptr,
        reinterpret_cast<const unsigned char *>(rawPublicKey.constData()),
        size_t(rawPublicKey.size()));
    if (!key)
        return false;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    bool ok = false;
    if (ctx && EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, key) == 1) {
        ok = EVP_DigestVerify(
                 ctx,
                 reinterpret_cast<const unsigned char *>(signature.constData()),
                 size_t(signature.size()),
                 reinterpret_cast<const unsigned char *>(payload.constData()),
                 size_t(payload.size())) == 1;
    }
    if (ctx)
        EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    return ok;
}

QList<int> parseVersion(const QString &text)
{
    static const QRegularExpression versionRe(
        QStringLiteral("\\Av?(\\d{1,6}(?:\\.\\d{1,6}){0,3})\\z"));
    const QRegularExpressionMatch match = versionRe.match(text.trimmed());
    if (!match.hasMatch())
        return {};
    QList<int> parts;
    for (const QString &part : match.captured(1).split(QLatin1Char('.')))
        parts.append(part.toInt());
    while (parts.size() < 4)
        parts.append(0);
    return parts;
}

int compareVersions(const QString &left, const QString &right)
{
    const QList<int> a = parseVersion(left);
    const QList<int> b = parseVersion(right);
    if (a.isEmpty() || b.isEmpty())
        return a.isEmpty() == b.isEmpty() ? 0 : (a.isEmpty() ? -1 : 1);
    for (int i = 0; i < a.size(); ++i) {
        if (a.at(i) != b.at(i))
            return a.at(i) < b.at(i) ? -1 : 1;
    }
    return 0;
}

QString newestReleaseTag(const QStringList &tags)
{
    QString best;
    for (const QString &raw : tags) {
        const QString tag = raw.trimmed();
        if (parseVersion(tag).isEmpty())
            continue;
        if (best.isEmpty() || compareVersions(tag, best) > 0)
            best = tag;
    }
    return best;
}

bool isCommitId(const QString &text)
{
    static const QRegularExpression commitRe(
        QStringLiteral("\\A(?:[0-9a-f]{40}|[0-9a-f]{64})\\z"));
    return commitRe.match(text).hasMatch();
}

bool channelAccepts(const QString &wantedChannel, const QString &manifestChannel,
                    const QString &tag)
{
    if (manifestChannel == wantedChannel)
        return true;
    if (wantedChannel != QStringLiteral("latest"))
        return false;
    QString version = tag.trimmed();
    if (version.startsWith(QLatin1Char('v')))
        version.remove(0, 1);
    return !version.isEmpty()
           && manifestChannel == QStringLiteral("candidate-") + version;
}

Verdict checkManifest(const QByteArray &manifestBytes,
                      const QByteArray &signature, const QString &expectedTag,
                      const QString &runningVersion, const QString &channel,
                      const QByteArray &publicKey)
{
    Verdict verdict;
    if (!verifyEd25519(publicKey, manifestBytes, signature)) {
        verdict.reason = QStringLiteral("its release.json.sig is missing or "
                                        "does not verify against the ForkMesh "
                                        "release key");
        return verdict;
    }
    const QJsonDocument document = QJsonDocument::fromJson(manifestBytes);
    if (!document.isObject()) {
        verdict.reason = QStringLiteral("its signed manifest is not a JSON object");
        return verdict;
    }
    const QJsonObject manifest = document.object();
    if (manifest.value(QStringLiteral("schema")).toString()
        != QStringLiteral("forkmesh-release-v2")) {
        verdict.reason = QStringLiteral("its signed manifest has an unsupported schema");
        return verdict;
    }
    if (manifest.value(QStringLiteral("repo")).toString() != officialRepository()) {
        verdict.reason = QStringLiteral("its signed manifest names another "
                                        "repository");
        return verdict;
    }
    const QString tag = manifest.value(QStringLiteral("tag")).toString();
    if (tag != expectedTag.trimmed()) {
        verdict.reason = QStringLiteral("its signed manifest is for %1").arg(tag);
        return verdict;
    }
    if (!channelAccepts(channel,
                        manifest.value(QStringLiteral("channel")).toString(),
                        tag)) {
        verdict.reason = QStringLiteral("its signed manifest is for another "
                                        "release channel");
        return verdict;
    }
    if (parseVersion(tag).isEmpty() || parseVersion(runningVersion).isEmpty()
        || compareVersions(tag, runningVersion) <= 0) {
        verdict.reason = QStringLiteral("it is not newer than the running "
                                        "version %1").arg(runningVersion);
        return verdict;
    }
    const QString tagCommit =
        manifest.value(QStringLiteral("tag_commit")).toString();
    if (!isCommitId(tagCommit)) {
        verdict.reason = QStringLiteral("its signed manifest has no valid tag "
                                        "commit");
        return verdict;
    }
    verdict.ok = true;
    verdict.manifest = manifest;
    verdict.tagCommit = tagCommit;
    return verdict;
}

}
