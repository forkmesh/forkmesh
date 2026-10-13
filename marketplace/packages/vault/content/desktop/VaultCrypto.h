// Password Vault cryptography for the desktop: suite "fmv1", the same formats
// the browser writes (content/crypto/vault-crypto.js). Qt Core and OpenSSL
// only, so it is tested without a window.
//
// OpenSSL 3.5 supplies every primitive: Argon2id, HKDF-SHA-256, AES-256-GCM,
// X25519, Ed25519, ML-KEM-768 and SHA3-256. Nothing is reimplemented here;
// tests/fixtures/interop-from-js.json proves the two suites agree byte for
// byte, in both directions.
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace forkmesh::vault {

inline constexpr char kSuite[] = "fmv1";

// A failure carries the same code the JavaScript suite throws, so all three
// clients explain one problem in one way.
struct Failure
{
    QString code;
    bool failed() const { return !code.isEmpty(); }
};

struct Kdf
{
    int memoryKiB = 65536;      // RFC 9106's second recommended setting
    int passes = 3;
    int parallelism = 1;
    QByteArray salt;

    QJsonObject toJson() const;
    // Refuses anything under the floor (19 MiB, 2 passes): a server that hands
    // back weaker numbers is refused, never obeyed.
    static bool fromJson(const QJsonObject &json, Kdf *kdf, Failure *failure);
    static Kdf fresh();
};

// Wipes itself on destruction. Holds the raw keys of one unlocked account.
class Keyset
{
public:
    Keyset() = default;
    ~Keyset();
    Keyset(const Keyset &) = default;
    Keyset &operator=(const Keyset &) = default;

    QByteArray vaultKey;        // 32, wraps the owner's own item keys
    QByteArray x25519;          // 32, classical half of the share KEM
    QByteArray mlkemSeed;       // 64, d || z of the ML-KEM-768 key
    QByteArray ed25519;         // 32, signs the bundle and every share

    bool isValid() const;
    void clear();
    QJsonObject toJson() const;
    static bool fromJson(const QJsonObject &json, Keyset *keyset, Failure *failure);
    static bool generate(Keyset *keyset, Failure *failure);
};

// What an account publishes; verified before anything is sealed to it.
struct Bundle
{
    QString account;
    QByteArray x25519;
    QByteArray mlkem768;
    QByteArray ed25519;
    QByteArray signature;

    QJsonObject toJson() const;
    static bool fromJson(const QJsonObject &json, const QString &account,
                         Bundle *bundle, Failure *failure);
};

QString toBase64Url(const QByteArray &bytes);
bool fromBase64Url(const QString &text, QByteArray *bytes);
void wipe(QByteArray &bytes);
QByteArray randomBytes(int length);
bool cryptoAvailable(Failure *failure);

// Argon2id over the NFKC form of the password. Seconds of work: call it off
// the GUI thread.
bool stretch(const QString &password, const Kdf &kdf, QByteArray *masterKey, Failure *failure);

bool wrapKeyset(const QByteArray &secret, const QString &purpose, const QString &account,
                const Keyset &keyset, QString *blob, Failure *failure);
bool unwrapKeyset(const QByteArray &secret, const QString &purpose, const QString &account,
                  const QString &blob, Keyset *keyset, Failure *failure);

QString newRecoveryCode();
bool recoverySecret(const QString &code, QByteArray *secret, Failure *failure);

bool publicBundle(const Keyset &keyset, const QString &account, Bundle *bundle, Failure *failure);
bool fingerprint(const Bundle &bundle, QString *text, Failure *failure);

QString newItemId();
bool sealItem(const QByteArray &itemKey, const QString &itemId, const QJsonObject &fields,
              QString *blob, Failure *failure);
bool openItem(const QByteArray &itemKey, const QString &itemId, const QString &blob,
              QJsonObject *fields, Failure *failure);
bool wrapItemKey(const Keyset &keyset, const QString &itemId, const QByteArray &itemKey,
                 QString *blob, Failure *failure);
bool unwrapItemKey(const Keyset &keyset, const QString &itemId, const QString &blob,
                   QByteArray *itemKey, Failure *failure);
bool sealPrivate(const Keyset &keyset, const QString &label, const QJsonObject &value,
                 QString *blob, Failure *failure);
bool openPrivate(const Keyset &keyset, const QString &label, const QString &blob,
                 QJsonObject *value, Failure *failure);

// A share: the item key sealed to one recipient with X25519 + ML-KEM-768
// (X-Wing combiner) and signed by the sender.
bool sealGrant(const Keyset &sender, const QString &senderAccount, const Bundle &recipient,
               const QString &itemId, const QByteArray &itemKey, const QString &permission,
               QJsonObject *grant, Failure *failure);
bool openGrant(const Keyset &recipient, const QString &recipientAccount, const Bundle &sender,
               const QString &itemId, const QJsonObject &grant, const QString &permission,
               QByteArray *itemKey, Failure *failure);

struct PasswordOptions
{
    int length = 24;
    bool lower = true, upper = true, digits = true, symbols = true;
};
QString generatePassword(const PasswordOptions &options = {});

// The host a saved login belongs to ("" unless https, or http on localhost),
// and the address the Open button may visit.
QString loginHost(const QString &url);
QString loginHref(const QString &url);

}  // namespace forkmesh::vault
