#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

// Private mirrors use AES-256-GCM content keys wrapped with both X25519 and
// ML-KEM-768 through HKDF-SHA256. Mirrors and the relay receive no private key
// material; opening a wrap requires both KEM results.
class MirrorCrypto
{
public:
    struct Identity {
        QByteArray x25519Pub;  // 32 bytes
        QByteArray x25519Priv; // 32 bytes
        QByteArray mlkemPub;   // 1184 bytes
        QByteArray mlkemPriv;  // 2400 bytes

        bool isValid() const;
        QJsonObject publicBundle() const;
        // A short, stable id for this identity derived from its public keys, so a
        // recipient can find its own wrap entry in an envelope without trial
        // decryption. Matches publicKeyId(publicBundle()).
        QString keyId() const;
    };

    static Identity generateIdentity();

    static bool postQuantumAvailable(QString *reason = nullptr);

    // The key id for a public bundle (base64url SHA-256 of its raw public keys).
    static QString publicKeyId(const QJsonObject &publicBundle);

    // Seal opaque archive bytes to a set of recipient public bundles. The result
    // is an envelope JSON:
    // {"kind":"forkmesh.mirror","v":1,"alg":"x25519+mlkem768/aes256gcm",
    // "nonce","tag","body", // archive under the content key
    // "recipients":[ {kid,x25519,mlkem768,nonce,tag,key},... ]}
    // Returns {} on failure (and sets *error when non-null).
    static QJsonObject sealArchive(const QByteArray &plaintext,
                                   const QList<QJsonObject> &recipientBundles,
                                   QString *error = nullptr);

    // Open an envelope with the recipient's identity: recover the content key
    // from this identity's wrap entry and decrypt the archive. Returns the
    // plaintext bytes, or {} when this identity is not a recipient / the envelope
    // is malformed / a tag mismatches (and sets *error when non-null).
    static QByteArray openArchive(const QJsonObject &envelope,
                                  const Identity &me,
                                  QString *error = nullptr);
    // Each recipient entry contains the ephemeral X25519 public key, ML-KEM
    // ciphertext, and authenticated wrapped content key. The relay can
    // validate/route the envelope but has no private material with which to
    // open it.
    static QJsonObject sealOwnerPayload(const QByteArray &plaintext,
                                        const QJsonObject &recipientBundle,
                                        QString *error = nullptr);

    // Seal to every enrolled owner device at once: the sealing device's own
    // bundle first, companion devices (e.g. the mobile node) after it. Order
    // is preserved so the envelope's first recipient identifies the producer;
    // the Worker bounds the list and refuses duplicate recipient ids.
    static QJsonObject sealOwnerPayload(const QByteArray &plaintext,
                                        const QList<QJsonObject> &recipientBundles,
                                        QString *error = nullptr);

    // Open an owner payload sealed to any set of owner devices this identity
    // is part of. Malformed framing, a recipient mismatch, or any GCM/KEM
    // authentication failure returns an empty byte array and sets *error when
    // supplied.
    static QByteArray openOwnerPayload(const QJsonObject &envelope,
                                       const Identity &me,
                                       QString *error = nullptr);

    // The first recipient's id — the device that sealed the envelope.
    static QString ownerPayloadKeyId(const QJsonObject &envelope);
    // Every recipient id in envelope order; empty when the framing is invalid.
    static QStringList ownerPayloadKeyIds(const QJsonObject &envelope);
};
