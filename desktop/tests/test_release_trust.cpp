#include "ReleaseTrust.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include <openssl/evp.h>

#include <cstdio>

namespace {

int failures = 0;

void expect(bool condition, const char *what)
{
    if (condition)
        return;
    ++failures;
    std::fprintf(stderr, "FAIL %s\n", what);
}

struct TestKey {
    EVP_PKEY *key = nullptr;
    QByteArray publicKey;

    TestKey()
    {
        EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
        if (ctx && EVP_PKEY_keygen_init(ctx) == 1)
            EVP_PKEY_keygen(ctx, &key);
        EVP_PKEY_CTX_free(ctx);
        size_t length = 32;
        publicKey.resize(32);
        if (key)
            EVP_PKEY_get_raw_public_key(
                key, reinterpret_cast<unsigned char *>(publicKey.data()),
                &length);
    }

    ~TestKey() { EVP_PKEY_free(key); }

    QByteArray sign(const QByteArray &payload) const
    {
        QByteArray signature(64, '\0');
        size_t length = 64;
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        if (ctx && EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, key) == 1)
            EVP_DigestSign(
                ctx, reinterpret_cast<unsigned char *>(signature.data()),
                &length,
                reinterpret_cast<const unsigned char *>(payload.constData()),
                size_t(payload.size()));
        EVP_MD_CTX_free(ctx);
        return signature;
    }
};

QByteArray manifestBytes(const QJsonObject &overrides = {})
{
    QJsonObject manifest{
        {QStringLiteral("schema"), QStringLiteral("forkmesh-release-v2")},
        {QStringLiteral("repo"), QStringLiteral("forkmesh/forkmesh")},
        {QStringLiteral("tag"), QStringLiteral("v1.3.0")},
        {QStringLiteral("tag_commit"), QString(40, QLatin1Char('a'))},
        {QStringLiteral("build_commit"), QString(40, QLatin1Char('a'))},
        {QStringLiteral("channel"), QStringLiteral("candidate-1.3.0")},
    };
    for (auto it = overrides.begin(); it != overrides.end(); ++it)
        manifest.insert(it.key(), it.value());
    return QJsonDocument(manifest).toJson(QJsonDocument::Compact);
}

bool accepted(const TestKey &key, const QJsonObject &overrides,
              const QString &running = QStringLiteral("1.2.8"),
              const QString &channel = QStringLiteral("latest"),
              const QString &tag = QStringLiteral("v1.3.0"))
{
    const QByteArray bytes = manifestBytes(overrides);
    return forkmesh::releasetrust::checkManifest(bytes, key.sign(bytes), tag,
                                                 running, channel,
                                                 key.publicKey)
        .ok;
}

}

int main()
{
    using namespace forkmesh::releasetrust;
    const TestKey key;
    expect(key.key != nullptr, "test key generated");

    const QByteArray bytes = manifestBytes();
    const QByteArray signature = key.sign(bytes);
    const Verdict good = checkManifest(bytes, signature, QStringLiteral("v1.3.0"),
                                       QStringLiteral("1.2.8"),
                                       QStringLiteral("latest"), key.publicKey);
    expect(good.ok, "a signed newer release is accepted");
    expect(good.tagCommit == QString(40, QLatin1Char('a')),
           "the verified tag commit is returned");

    expect(!checkManifest(bytes, signature, QStringLiteral("v1.3.0"),
                          QStringLiteral("1.2.8"))
                .ok,
           "a manifest signed by another key fails the pinned key");
    QByteArray tampered = bytes;
    tampered.replace("candidate-1.3.0", "candidate-1.3.1");
    expect(!checkManifest(tampered, signature, QStringLiteral("v1.3.0"),
                          QStringLiteral("1.2.8"), QStringLiteral("latest"),
                          key.publicKey)
                .ok,
           "a changed manifest byte fails the signature");
    expect(!checkManifest(bytes, QByteArray(), QStringLiteral("v1.3.0"),
                          QStringLiteral("1.2.8"), QStringLiteral("latest"),
                          key.publicKey)
                .ok,
           "a missing signature is refused");
    expect(!checkManifest(bytes, signature.left(63), QStringLiteral("v1.3.0"),
                          QStringLiteral("1.2.8"), QStringLiteral("latest"),
                          key.publicKey)
                .ok,
           "a short signature is refused");

    expect(!accepted(key, {{QStringLiteral("repo"),
                            QStringLiteral("attacker/forkmesh")}}),
           "another repository is refused");
    expect(!accepted(key, {{QStringLiteral("schema"),
                            QStringLiteral("forkmesh-release-v1")}}),
           "another schema is refused");
    expect(!accepted(key, {{QStringLiteral("tag_commit"), QStringLiteral("--x")}}),
           "an invalid tag commit is refused");
    expect(!accepted(key, {}, QStringLiteral("1.2.8"), QStringLiteral("latest"),
                     QStringLiteral("v1.3.1")),
           "a manifest for another tag is refused");
    expect(!accepted(key, {}, QStringLiteral("1.3.0")),
           "the running version is not an update");
    expect(!accepted(key, {}, QStringLiteral("1.4.0")),
           "an older release is refused");
    expect(!accepted(key, {{QStringLiteral("channel"),
                            QStringLiteral("candidate-1.2.0")}}),
           "a candidate of another version is not latest");
    expect(!accepted(key, {{QStringLiteral("channel"), QStringLiteral("beta")}}),
           "another channel is refused");
    expect(accepted(key, {{QStringLiteral("channel"), QStringLiteral("latest")}}),
           "a manifest published as latest is accepted");
    expect(accepted(key, {{QStringLiteral("channel"), QStringLiteral("beta")}},
                    QStringLiteral("1.2.8"), QStringLiteral("beta")),
           "the wanted non-latest channel is accepted");
    expect(!accepted(key, {}, QStringLiteral("1.2.8"), QStringLiteral("beta")),
           "a candidate is not accepted on another channel");

    expect(newestReleaseTag({QStringLiteral("v1.2.9"), QStringLiteral("v1.2.10"),
                             QStringLiteral("nightly"),
                             QStringLiteral("v1.10.0-rc1")})
               == QStringLiteral("v1.2.10"),
           "tags are ordered by version, not name or date");
    expect(newestReleaseTag({QStringLiteral("v2.0.0"), QStringLiteral("v10.0.0"),
                             QStringLiteral("v9.9.9")})
               == QStringLiteral("v10.0.0"),
           "numeric components compare as numbers");
    expect(newestReleaseTag({QStringLiteral("--upload-pack=x")}).isEmpty(),
           "a non-version tag is never chosen");
    expect(compareVersions(QStringLiteral("v1.2.8"), QStringLiteral("1.2.8")) == 0,
           "a leading v is ignored");
    expect(compareVersions(QStringLiteral("1.2"), QStringLiteral("1.2.0")) == 0,
           "missing components are zero");

    QFile latest(QStringLiteral(FORKMESH_REPO_ROOT
                                "/.forkmesh/releases/latest/release.json"));
    QFile latestSig(QStringLiteral(FORKMESH_REPO_ROOT
                                   "/.forkmesh/releases/latest/release.json.sig"));
    if (latest.open(QIODevice::ReadOnly) && latestSig.open(QIODevice::ReadOnly)) {
        const QByteArray published = latest.readAll();
        const QString tag = QJsonDocument::fromJson(published)
                                .object()
                                .value(QStringLiteral("tag"))
                                .toString();
        const Verdict real = checkManifest(published, latestSig.readAll(), tag,
                                           QStringLiteral("0.0.1"));
        expect(real.ok, "the published latest manifest verifies with the "
                        "pinned release key");
    } else {
        expect(false, "the published latest manifest is readable");
    }

    if (failures)
        std::fprintf(stderr, "%d release trust check(s) failed\n", failures);
    else
        std::printf("release trust checks passed\n");
    return failures ? 1 : 0;
}
