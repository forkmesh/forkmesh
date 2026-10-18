#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace forkmesh::securedrive {

inline constexpr char kSuite[] = "fmsd1";
inline constexpr int kKeyBytes = 32;
inline constexpr int kMlkemPublicBytes = 1184;
inline constexpr int kMlkemCiphertextBytes = 1088;

struct Failure
{
    QString code;
    bool failed() const { return !code.isEmpty(); }
};

bool fail(Failure *failure, const char *code);

struct Kdf
{
    int memoryKiB = 65536;
    int passes = 3;
    int parallelism = 1;
    QByteArray salt;

    QJsonObject toJson() const;
    static bool fromJson(const QJsonObject &json, Kdf *kdf, Failure *failure);
    static Kdf fresh();
};

class DeviceKeys
{
public:
    DeviceKeys() = default;
    ~DeviceKeys();
    DeviceKeys(const DeviceKeys &) = default;
    DeviceKeys &operator=(const DeviceKeys &) = default;

    QByteArray x25519;
    QByteArray mlkemSeed;
    QByteArray ed25519;

    bool isValid() const;
    void clear();
    QJsonObject toJson() const;
    static bool fromJson(const QJsonObject &json, DeviceKeys *keys, Failure *failure);
    static bool generate(DeviceKeys *keys, Failure *failure);
};

struct PublicKeys
{
    QByteArray x25519;
    QByteArray mlkem768;
    QByteArray ed25519;

    bool isValid() const;
    QJsonObject toJson() const;
    static bool fromJson(const QJsonObject &json, PublicKeys *keys);
};

QString toBase64Url(const QByteArray &bytes);
bool fromBase64Url(const QString &text, QByteArray *bytes);
void wipe(QByteArray &bytes);
QByteArray randomBytes(int length);
QString randomId();
bool cryptoAvailable(Failure *failure);

QByteArray sha256(const QByteArray &bytes);
bool hkdf(const QByteArray &secret, const QString &info, QByteArray *out, int length = kKeyBytes);
QByteArray hmac(const QByteArray &key, const QByteArray &message);
bool constantTimeEqual(const QByteArray &left, const QByteArray &right);

bool aeadSeal(const QByteArray &key, const QByteArray &nonce, const QByteArray &aad,
              const QByteArray &plaintext, QByteArray *sealed);
bool aeadOpen(const QByteArray &key, const QByteArray &nonce, const QByteArray &aad,
              const QByteArray &sealed, QByteArray *plaintext);

bool stretch(const QString &passphrase, const Kdf &kdf, QByteArray *key, Failure *failure);

bool publicKeys(const DeviceKeys &keys, PublicKeys *out, Failure *failure);
bool sign(const QByteArray &ed25519, const QByteArray &message, QByteArray *signature);
bool verify(const QByteArray &ed25519Public, const QByteArray &message, const QByteArray &signature);

bool sealToDevice(const PublicKeys &recipient, const QByteArray &label, const QByteArray &plaintext,
                  QByteArray *sealed, Failure *failure);
bool openFromDevice(const DeviceKeys &recipient, const QByteArray &label, const QByteArray &sealed,
                    QByteArray *plaintext, Failure *failure);

QString safetyNumber(const QByteArray &context, const PublicKeys &keys);

QString newInviteCode(QByteArray *secret);
bool parseInviteCode(const QString &code, QByteArray *secret);

qint64 paddedSize(qint64 length);

}  // namespace forkmesh::securedrive
