#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

// Password-derived symmetric encryption for "coves" — encrypted file vaults that
// a team shares by passing one password out-of-band. The relay and the public
// catalog only ever see AES-256-GCM ciphertext.
// Unlike RoomCrypto (whose key derives from a fixed room name), a cove carries a
// random per-cove salt in its envelope, so the same password yields a different
// key for every cove. A 256-bit AES key is quantum-resistant: Grover's algorithm
// only halves the brute-force exponent to ~128 bits, which stays out of reach.
class CoveCrypto
{
public:
    static int defaultRounds();
    static QByteArray randomSalt();
    static QByteArray randomKey();
    // Cryptographically random bytes (used for decoy grant slots).
    static QByteArray randomBytes(int count);

    // Derive the AES key from a password, the cove's salt, and its stored rounds.
    CoveCrypto(const QString &password, const QByteArray &salt, int rounds);
    static CoveCrypto withKey(const QByteArray &key);

    bool isValid() const { return !m_key.isEmpty(); }
    QString errorString() const { return m_error; }

    // Encrypt opaque bytes to a {"nonce","tag","body"} base64 object (the value
    // stored under "cipher" in a.cove file). Returns {} on failure.
    QJsonObject encrypt(const QByteArray &plaintext) const;
    // Decrypt a {"nonce","tag","body"} object. Returns the plaintext bytes, or an
    // empty QByteArray when the password/key is wrong (GCM tag mismatch) or the
    // envelope is malformed.
    QByteArray decrypt(const QJsonObject &cipher) const;

private:
    CoveCrypto() = default;

    QByteArray m_key;
    QString m_error;
};
