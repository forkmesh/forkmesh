// The desktop's fmv1 suite against the browser's: opens everything in
// tests/fixtures/interop-from-js.json, round-trips its own output, and refuses
// every tampering the JavaScript suite refuses. `--write <file>` emits the
// mirror-image fixture (interop-from-cpp.json) that the JavaScript test opens.
#include "VaultCrypto.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <cstdio>

#include <openssl/crypto.h>

using namespace forkmesh::vault;

void vaultClientFlow();                     // test_vault_client_flow.cpp

namespace {
int failures = 0;
int checks = 0;
}  // namespace

void check(bool ok, const char *name)
{
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n", name);
    }
}

namespace {

QJsonObject readJson(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

}  // namespace

QString fixturesDir()
{
    const QByteArray overridden = qgetenv("FORKMESH_VAULT_FIXTURES");
    if (!overridden.isEmpty())
        return QString::fromLocal8Bit(overridden);
    // __FILE__ is absolute under CMake; a hand compile may leave it relative,
    // so fall back to walking up from the working directory.
    const QString beside = QDir(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath()
                                + QStringLiteral("/../../../tests/fixtures")).canonicalPath();
    if (!beside.isEmpty() && QFileInfo::exists(beside + QStringLiteral("/interop-from-js.json")))
        return beside;
    for (QDir dir = QDir::current();; ) {
        for (const QString &candidate : {QStringLiteral("tests/fixtures"),
                                         QStringLiteral("marketplace/packages/vault/tests/fixtures")}) {
            if (QFileInfo::exists(dir.filePath(candidate + QStringLiteral("/interop-from-js.json"))))
                return dir.filePath(candidate);
        }
        if (!dir.cdUp())
            return beside;
    }
}

namespace {

Keyset keysetOf(const QJsonObject &account)
{
    Keyset keyset;
    Failure failure;
    check(Keyset::fromJson(account.value(QStringLiteral("keyset")).toObject(), &keyset, &failure),
          "fixture keyset parses");
    return keyset;
}

Bundle bundleOf(const QJsonObject &account, const QString &name)
{
    Bundle bundle;
    Failure failure;
    check(Bundle::fromJson(account.value(QStringLiteral("bundle")).toObject(), name, &bundle, &failure),
          "fixture bundle verifies");
    return bundle;
}

void opensWhatTheBrowserWrites(const QJsonObject &v)
{
    Failure failure;
    Kdf kdf;
    check(Kdf::fromJson(v.value(QStringLiteral("kdf")).toObject(), &kdf, &failure), "kdf parses");
    QByteArray masterKey;
    check(stretch(v.value(QStringLiteral("password")).toString(), kdf, &masterKey, &failure)
              && masterKey.toHex() == v.value(QStringLiteral("masterKey")).toString().toLatin1(),
          "Argon2id master key matches (NFKC password)");

    const QJsonObject accounts = v.value(QStringLiteral("accounts")).toObject();
    const QJsonObject aliceJson = accounts.value(QStringLiteral("alice")).toObject();
    const QJsonObject bobJson = accounts.value(QStringLiteral("bob")).toObject();
    const Keyset alice = keysetOf(aliceJson), bob = keysetOf(bobJson);
    const Bundle aliceBundle = bundleOf(aliceJson, QStringLiteral("alice"));
    const Bundle bobBundle = bundleOf(bobJson, QStringLiteral("bob"));

    Keyset opened;
    check(unwrapKeyset(masterKey, QStringLiteral("master"), QStringLiteral("alice"),
                       v.value(QStringLiteral("wrappedKeyset")).toString(), &opened, &failure)
              && opened.toJson() == alice.toJson(),
          "keyset opens under the master password");
    Failure wrong;
    check(!unwrapKeyset(masterKey, QStringLiteral("master"), QStringLiteral("bob"),
                        v.value(QStringLiteral("wrappedKeyset")).toString(), &opened, &wrong)
              && wrong.code == QLatin1String("wrong_password"),
          "a keyset blob served to another account is refused");

    QByteArray recovery;
    check(recoverySecret(v.value(QStringLiteral("recoveryCode")).toString(), &recovery, &failure)
              && recovery.toHex() == v.value(QStringLiteral("recoverySecret")).toString().toLatin1(),
          "recovery code decodes");
    check(recoverySecret(v.value(QStringLiteral("recoveryCode")).toString().toLower().replace(
                             QLatin1Char('-'), QLatin1Char(' ')).replace(QLatin1Char('0'), QLatin1Char('o')),
                         &recovery, &failure)
              && recovery.toHex() == v.value(QStringLiteral("recoverySecret")).toString().toLatin1(),
          "a loosely typed recovery code decodes the same");
    check(unwrapKeyset(recovery, QStringLiteral("recovery"), QStringLiteral("alice"),
                       v.value(QStringLiteral("wrappedRecovery")).toString(), &opened, &failure)
              && opened.vaultKey == alice.vaultKey,
          "keyset opens under the recovery code");

    for (const auto &[name, keyset, json] : {std::tuple{QStringLiteral("alice"), alice, aliceJson},
                                             std::tuple{QStringLiteral("bob"), bob, bobJson}}) {
        Bundle derived;
        QString print;
        const QJsonObject published = json.value(QStringLiteral("bundle")).toObject();
        check(publicBundle(keyset, name, &derived, &failure)
                  && toBase64Url(derived.x25519) == published.value(QStringLiteral("x25519")).toString()
                  && toBase64Url(derived.mlkem768) == published.value(QStringLiteral("mlkem768")).toString()
                  && toBase64Url(derived.ed25519) == published.value(QStringLiteral("ed25519")).toString(),
              "public keys derive from the keyset (ML-KEM from its seed)");
        check(fingerprint(derived, &print, &failure)
                  && print == json.value(QStringLiteral("fingerprint")).toString(),
              "fingerprint matches");
        Bundle reparsed;
        check(Bundle::fromJson(derived.toJson(), name, &reparsed, &failure), "our own bundle verifies");
    }

    const QJsonObject item = v.value(QStringLiteral("item")).toObject();
    const QString itemId = item.value(QStringLiteral("id")).toString();
    QByteArray itemKey;
    check(unwrapItemKey(alice, itemId, item.value(QStringLiteral("wrappedKey")).toString(), &itemKey, &failure)
              && itemKey.toHex() == item.value(QStringLiteral("key")).toString().toLatin1(),
          "item key unwraps");
    QJsonObject fields;
    check(openItem(itemKey, itemId, item.value(QStringLiteral("body")).toString(), &fields, &failure)
              && fields == item.value(QStringLiteral("fields")).toObject(),
          "item fields open, Unicode and escapes intact");
    QJsonObject trust;
    check(openPrivate(alice, QStringLiteral("trust"),
                      v.value(QStringLiteral("trust")).toObject().value(QStringLiteral("blob")).toString(),
                      &trust, &failure)
              && trust == v.value(QStringLiteral("trust")).toObject().value(QStringLiteral("value")).toObject(),
          "trust record opens");

    const QJsonObject grants = v.value(QStringLiteral("grants")).toObject();
    for (const QString &permission : {QStringLiteral("read"), QStringLiteral("write")}) {
        QByteArray shared;
        check(openGrant(bob, QStringLiteral("bob"), aliceBundle, itemId,
                        grants.value(permission).toObject(), permission, &shared, &failure)
                  && shared == itemKey,
              "a grant opens for its recipient");
    }

    // Every binding a share carries is enforced.
    const QJsonObject readGrant = grants.value(QStringLiteral("read")).toObject();
    QByteArray leaked;
    const auto refused = [&](const char *name, const Keyset &who, const QString &account,
                             const Bundle &from, const QString &id, const QJsonObject &grant,
                             const QString &permission) {
        Failure why;
        check(!openGrant(who, account, from, id, grant, permission, &leaked, &why)
                  && why.code == QLatin1String("invalid_share_signature"),
              name);
    };
    refused("a grant upgraded to write", bob, QStringLiteral("bob"), aliceBundle, itemId, readGrant,
            QStringLiteral("write"));
    refused("a grant moved to another item", bob, QStringLiteral("bob"), aliceBundle, newItemId(),
            readGrant, QStringLiteral("read"));
    refused("a grant replayed to another recipient", alice, QStringLiteral("alice"), aliceBundle,
            itemId, readGrant, QStringLiteral("read"));
    Bundle forged = bobBundle;
    forged.account = QStringLiteral("alice");
    refused("a grant from a forged sender", bob, QStringLiteral("bob"), forged, itemId, readGrant,
            QStringLiteral("read"));
    for (const QString &field : {QStringLiteral("x25519"), QStringLiteral("mlkem768"), QStringLiteral("wrapped")}) {
        QByteArray bytes;
        fromBase64Url(readGrant.value(field).toString(), &bytes);
        bytes[3] = char(bytes.at(3) ^ 1);
        QJsonObject tampered = readGrant;
        tampered.insert(field, toBase64Url(bytes));
        refused("a tampered grant", bob, QStringLiteral("bob"), aliceBundle, itemId, tampered,
                QStringLiteral("read"));
    }
    QJsonObject relabelled = bobJson.value(QStringLiteral("bundle")).toObject();
    relabelled.insert(QStringLiteral("account"), QStringLiteral("mallory"));
    Bundle unused;
    Failure why;
    check(!Bundle::fromJson(relabelled, QStringLiteral("mallory"), &unused, &why)
              && why.code == QLatin1String("invalid_public_keys"),
          "a bundle re-labelled for another account is refused");
    QJsonObject swapped = bobJson.value(QStringLiteral("bundle")).toObject();
    swapped.insert(QStringLiteral("mlkem768"),
                   aliceJson.value(QStringLiteral("bundle")).toObject().value(QStringLiteral("mlkem768")));
    check(!Bundle::fromJson(swapped, QStringLiteral("bob"), &unused, &why),
          "a bundle with a swapped ML-KEM key is refused");

    QJsonObject other;
    Failure moved;
    check(!openItem(itemKey, newItemId(), item.value(QStringLiteral("body")).toString(), &other, &moved)
              && moved.code == QLatin1String("decryption_failed"),
          "an item blob moved to another id is refused");
}

void refusesWeakStretching()
{
    Kdf kdf = Kdf::fresh();
    check(kdf.memoryKiB == 65536 && kdf.passes == 3 && kdf.parallelism == 1 && kdf.salt.size() == 16,
          "the default stretch is Argon2id, 64 MiB, 3 passes");
    for (const auto &[field, value] : {std::pair{QStringLiteral("memoryKiB"), QJsonValue(1024)},
                                       std::pair{QStringLiteral("passes"), QJsonValue(1)},
                                       std::pair{QStringLiteral("algorithm"), QJsonValue(QStringLiteral("pbkdf2"))},
                                       std::pair{QStringLiteral("version"), QJsonValue(16)},
                                       std::pair{QStringLiteral("salt"), QJsonValue(QStringLiteral("AAAA"))},
                                       std::pair{QStringLiteral("memoryKiB"), QJsonValue(65536.5)}}) {
        QJsonObject json = kdf.toJson();
        json.insert(field, value);
        Kdf parsed;
        Failure why;
        check(!Kdf::fromJson(json, &parsed, &why) && why.code == QLatin1String("unsafe_kdf_parameters"),
              "weak or malformed stretch settings are refused");
    }
}

void reportsMissingPrimitives()
{
    Failure why;
    const bool available = cryptoAvailable(&why);
    check(available == (OpenSSL_version_num() >= 0x30500000L),
          "Argon2id and ML-KEM-768 are reported available exactly when OpenSSL 3.5+ is loaded");
    check(available ? !why.failed()
                    : why.code == QLatin1String("argon2id_unavailable")
                          || why.code == QLatin1String("mlkem_unavailable"),
          "a missing primitive has its own failure code");
    if (!available) {
        Keyset keyset;
        QByteArray master;
        Failure generateWhy, stretchWhy;
        check(!Keyset::generate(&keyset, &generateWhy) && generateWhy.code == why.code
                  && !stretch(QStringLiteral("password"), Kdf::fresh(), &master, &stretchWhy)
                  && stretchWhy.code == why.code,
              "without the primitives nothing is created or unlocked");
    }
}

void helpers()
{
    QSet<QString> seen;
    for (int i = 0; i < 200; ++i) {
        const QString password = generatePassword({16});
        bool lower = false, upper = false, digit = false, symbol = false, confusing = false;
        for (const QChar c : password) {
            lower = lower || c.isLower();
            upper = upper || c.isUpper();
            digit = digit || c.isDigit();
            symbol = symbol || !c.isLetterOrNumber();
            confusing = confusing || QStringLiteral("lIO01").contains(c);
        }
        check(password.size() == 16 && lower && upper && digit && symbol && !confusing,
              "generated password shape");
        seen.insert(password);
    }
    check(seen.size() == 200, "generated passwords do not repeat");

    QByteArray back;
    Failure failure;
    const QString code = newRecoveryCode();
    check(code.size() == 52 + 12 && recoverySecret(code, &back, &failure) && back.size() == 32,
          "a fresh recovery code decodes");

    check(loginHost(QStringLiteral("https://www.Bank.Example/x")) == QLatin1String("bank.example"), "host");
    check(loginHost(QStringLiteral("bank.example:8443/x")) == QLatin1String("bank.example"), "host:port");
    check(loginHost(QStringLiteral("http://bank.example")).isEmpty(), "plain http has no host to fill");
    check(loginHost(QStringLiteral("http://localhost:3000/a")) == QLatin1String("localhost"), "localhost");
    for (const QString &bad : {QStringLiteral("javascript:alert(1)"), QStringLiteral("data:text/html,x"),
                               QStringLiteral("ftp://x.y"), QString()})
        check(loginHref(bad).isEmpty(), "only https addresses are ever opened");
    QByteArray junk;
    check(!fromBase64Url(QStringLiteral("not base64!"), &junk) && !fromBase64Url(QStringLiteral("ab+/"), &junk),
          "only the url-safe alphabet decodes");
}

// A vault made here, with its test-only secrets beside it, for the browser
// suite to open (tests/interop_check.js).
QJsonObject writesWhatTheBrowserOpens()
{
    Failure failure;
    const QString password = QString::fromUtf8("correct horse battery staple \xE2\x84\xAB");
    Kdf kdf = Kdf::fresh();
    kdf.memoryKiB = 19456;
    kdf.passes = 2;
    QByteArray masterKey, recovery;
    Keyset alice, bob;
    Bundle aliceBundle, bobBundle;
    QString alicePrint, bobPrint, wrappedKeyset, wrappedRecovery, body, wrappedKey, trustBlob;
    const QString recoveryCode = newRecoveryCode(), itemId = newItemId();
    const QByteArray itemKey = randomBytes(32);
    const QJsonObject fields{{QStringLiteral("title"), QString::fromUtf8("Registrar \xE2\x80\x94 prod")},
                             {QStringLiteral("username"), QStringLiteral("ops@acme.test")},
                             {QStringLiteral("password"), QString::fromUtf8("p\xC3\xA4ss \"quoted\" \\ w\xC3\xB6rd")},
                             {QStringLiteral("url"), QStringLiteral("https://registrar.example/login")},
                             {QStringLiteral("notes"), QStringLiteral("line one\nline two")}};
    bool ok = stretch(password, kdf, &masterKey, &failure) && Keyset::generate(&alice, &failure)
              && Keyset::generate(&bob, &failure)
              && publicBundle(alice, QStringLiteral("alice"), &aliceBundle, &failure)
              && publicBundle(bob, QStringLiteral("bob"), &bobBundle, &failure)
              && fingerprint(aliceBundle, &alicePrint, &failure) && fingerprint(bobBundle, &bobPrint, &failure)
              && wrapKeyset(masterKey, QStringLiteral("master"), QStringLiteral("alice"), alice, &wrappedKeyset, &failure)
              && recoverySecret(recoveryCode, &recovery, &failure)
              && wrapKeyset(recovery, QStringLiteral("recovery"), QStringLiteral("alice"), alice, &wrappedRecovery, &failure)
              && sealItem(itemKey, itemId, fields, &body, &failure)
              && wrapItemKey(alice, itemId, itemKey, &wrappedKey, &failure);
    const QJsonObject trustValue{{QStringLiteral("pins"), QJsonObject{{QStringLiteral("bob"), bobPrint}}}};
    ok = ok && sealPrivate(alice, QStringLiteral("trust"), trustValue, &trustBlob, &failure);
    QJsonObject grants;
    for (const QString &permission : {QStringLiteral("read"), QStringLiteral("write")}) {
        QJsonObject grant;
        ok = ok && sealGrant(alice, QStringLiteral("alice"), bobBundle, itemId, itemKey, permission, &grant, &failure);
        grants.insert(permission, grant);
        QByteArray opened;
        check(ok && openGrant(bob, QStringLiteral("bob"), aliceBundle, itemId, grant, permission, &opened, &failure)
                  && opened == itemKey,
              "our own grant round-trips");
    }
    check(ok, "a vault is made end to end");
    QJsonObject reopened;
    check(openItem(itemKey, itemId, body, &reopened, &failure) && reopened == fields, "our own item round-trips");
    QByteArray bodyBytes;
    fromBase64Url(body, &bodyBytes);
    check(bodyBytes.size() % 256 == 12 + 16, "items are padded to 256-byte steps");

    const auto account = [](const Keyset &keyset, const Bundle &bundle, const QString &print) {
        return QJsonObject{{QStringLiteral("keyset"), keyset.toJson()},
                           {QStringLiteral("bundle"), bundle.toJson()},
                           {QStringLiteral("fingerprint"), print}};
    };
    return {{QStringLiteral("format"), QStringLiteral("forkmesh-vault-interop-v1")},
            {QStringLiteral("suite"), QLatin1String(kSuite)},
            {QStringLiteral("madeBy"), QStringLiteral("cpp")},
            {QStringLiteral("warning"), QStringLiteral("Test keys only. Nothing here protects anything.")},
            {QStringLiteral("password"), password},
            {QStringLiteral("kdf"), kdf.toJson()},
            {QStringLiteral("masterKey"), QString::fromLatin1(masterKey.toHex())},
            {QStringLiteral("accounts"), QJsonObject{{QStringLiteral("alice"), account(alice, aliceBundle, alicePrint)},
                                                     {QStringLiteral("bob"), account(bob, bobBundle, bobPrint)}}},
            {QStringLiteral("wrappedKeyset"), wrappedKeyset},
            {QStringLiteral("recoveryCode"), recoveryCode},
            {QStringLiteral("recoverySecret"), QString::fromLatin1(recovery.toHex())},
            {QStringLiteral("wrappedRecovery"), wrappedRecovery},
            {QStringLiteral("item"), QJsonObject{{QStringLiteral("id"), itemId},
                                                 {QStringLiteral("key"), QString::fromLatin1(itemKey.toHex())},
                                                 {QStringLiteral("fields"), fields},
                                                 {QStringLiteral("body"), body},
                                                 {QStringLiteral("wrappedKey"), wrappedKey}}},
            {QStringLiteral("trust"), QJsonObject{{QStringLiteral("value"), trustValue}, {QStringLiteral("blob"), trustBlob}}},
            {QStringLiteral("grants"), grants}};
}
}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    reportsMissingPrimitives();
    if (!cryptoAvailable(nullptr)) {
        std::printf("%d checks, %d failures\n", checks, failures);
        return failures == 0 ? 0 : 1;
    }
    const QString dir = fixturesDir();
    const QJsonObject fromBrowser = readJson(dir + QStringLiteral("/interop-from-js.json"));
    check(fromBrowser.value(QStringLiteral("format")).toString() == QLatin1String("forkmesh-vault-interop-v1"),
          "the browser's fixture is found");
    if (failures == 0)
        opensWhatTheBrowserWrites(fromBrowser);
    // The phone's output, and our own earlier output, so a change here
    // cannot strand data another client or an older build wrote.
    for (const char *name : {"/interop-from-dart.json", "/interop-from-cpp.json"}) {
        const QJsonObject other = readJson(dir + QLatin1String(name));
        if (!other.isEmpty())
            opensWhatTheBrowserWrites(other);
    }
    refusesWeakStretching();
    helpers();
    const QJsonObject made = writesWhatTheBrowserOpens();
    opensWhatTheBrowserWrites(made);
    vaultClientFlow();

    const QStringList arguments = app.arguments();
    const int flag = int(arguments.indexOf(QStringLiteral("--write")));
    if (flag >= 0 && flag + 1 < arguments.size() && failures == 0) {
        QFile out(arguments.at(flag + 1));
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return 2;
        out.write(QJsonDocument(made).toJson(QJsonDocument::Indented));
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
