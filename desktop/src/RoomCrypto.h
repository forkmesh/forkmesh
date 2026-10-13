#pragma once

#include <QJsonObject>
#include <QString>

class RoomCrypto
{
public:
    RoomCrypto() = default;
    // Passphrase-free: derive the room key from a baked-in app key + the room
    // name, so every ForkMesh node joins the same shared rooms with no user
    // passphrase. The relay still only ever sees AES-256-GCM ciphertext.
    explicit RoomCrypto(const QString &roomName);
    RoomCrypto(const QString &roomName, const QString &passphrase);

    // Derives (and caches, process-wide) the key for a room without building
    // an object — so the 210k-round PBKDF2 can run on a worker ahead of the
    // ServerNode that will need it, instead of inside its constructor on the
    // GUI thread (one office channel at a time at startup and on reconnect).
    static void warmKey(const QString &roomName, const QString &passphrase);
    // Same, for the passphrase-free shared-room variant the session starts
    // with before the relay has handed over the room passphrase.
    static void warmDefaultKey(const QString &roomName);

    bool isValid() const { return !m_key.isEmpty(); }
    QString errorString() const { return m_error; }

#ifdef FORKMESH_SERVER_NODE_TESTS
    static void resetDerivationCountForTests();
    static int derivationCountForTests();
#endif

    QJsonObject encryptObject(const QJsonObject &plain) const;
    QJsonObject decryptObject(const QJsonObject &envelope) const;

private:
    QByteArray m_key;
    QString m_error;
};
