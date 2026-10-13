// Password Vault client for the desktop: everything between /api/vault and a
// screen, the C++ twin of content/crypto/vault-client.js. It owns the unlocked
// state (keyset, item keys, decrypted items, pinned fingerprints) and has no
// widgets and no network of its own: the host's signed request function and a
// key-stretch function are handed in, so the same code runs under the window
// and under the test that drives it against the real Worker module.
#pragma once

#include "VaultCrypto.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

namespace forkmesh::vault {

struct VaultShare
{
    QString kind;           // "user" | "organization"
    QString name;
    QString permission;     // "read" | "write"
};

struct VaultItem
{
    QString id;
    QString owner;
    bool owned = false;
    QString permission;     // "owner" | "read" | "write"
    QString viaKind, viaName;
    int revision = 1;
    qint64 updatedAt = 0;
    QString updatedBy;
    QList<VaultShare> shares;
    QJsonObject fields;     // empty when it could not be opened
    QString error;

    bool readable() const { return error.isEmpty(); }
    QString field(const char *name) const { return fields.value(QLatin1String(name)).toString(); }
};

// One person a share would reach, with the state of their pinned fingerprint.
struct VaultRecipient
{
    QString name;
    QString state;          // new | pinned | changed | no_vault | invalid_keys
    QString fingerprint;
    Bundle bundle;
};

struct ShareOutcome
{
    QStringList sealedTo, skipped, withoutVault;
};

class VaultClient
{
public:
    using Reply = std::function<void(bool ok, const QJsonObject &payload, const QString &error)>;
    using Request = std::function<void(const QByteArray &method, const QString &path,
                                       const QJsonObject &body, Reply reply)>;
    using Stretched = std::function<void(bool ok, const QByteArray &masterKey, const QString &code)>;
    using Stretcher = std::function<void(const QString &password, const Kdf &kdf, Stretched done)>;
    // "" means success; otherwise the same error code the other clients use.
    using Done = std::function<void(const QString &code)>;

    explicit VaultClient(Request request, Stretcher stretcher = {});
    ~VaultClient();

    // Tests use the smallest stretch the suite accepts to stay quick.
    void setNewKdfForTesting(int memoryKiB, int passes);

    QString account() const { return m_account; }
    bool installed() const { return m_installed; }
    bool unlocked() const { return m_keyset.isValid(); }
    const QList<VaultItem> &items() const { return m_items; }
    const VaultItem *find(const QString &id) const;

    void status(Done done);
    void setUp(const QString &masterPassword, std::function<void(const QString &code, const QString &recoveryCode)> done);
    void unlock(const QString &masterPassword, Done done);
    void recover(const QString &recoveryCode, const QString &newMasterPassword,
                 std::function<void(const QString &code, const QString &newRecoveryCode)> done);
    void changePassword(const QString &masterPassword, Done done);
    void renewRecoveryCode(std::function<void(const QString &code, const QString &recoveryCode)> done);
    void lock();

    void load(Done done);
    void save(const QJsonObject &fields, const QString &id, std::function<void(const QString &code, const QString &id)> done);
    void remove(const QString &id, Done done);

    void previewShare(const QString &kind, const QString &name,
                      std::function<void(const QString &code, const QList<VaultRecipient> &)> done);
    void share(const QString &id, const QString &kind, const QString &name, const QString &permission,
               const QStringList &accepted, std::function<void(const QString &code, const ShareOutcome &)> done);
    void unshare(const QString &id, const QString &kind, const QString &name, Done done);
    void syncPending(std::function<void(const QString &code, int sealed)> done);
    void organizations(std::function<void(const QString &code, const QStringList &)> done);

    QString myFingerprint() const;
    void exportData(std::function<void(const QString &code, const QJsonObject &payload)> done);
    void destroy(std::function<void(const QString &code, const QJsonObject &payload)> done);

    static QJsonObject cleanFields(const QJsonObject &fields, QString *code);

private:
    struct Sealed { QJsonArray grants; QStringList skipped; };

    void call(const QByteArray &method, const QString &path, const QJsonObject &body,
              std::function<void(const QJsonObject &)> ok, Done failed);
    void stretchWith(const QString &password, const Kdf &kdf, Stretched done);
    QString adopt(const Keyset &opened);
    void rewrap(const QString *masterPassword, bool newRecovery,
                std::function<void(const QString &code, const QString &recoveryCode)> done);
    void saveTrust(Done done);
    QString trustState(const Bundle &bundle, QString *fingerprintText) const;
    Sealed grantsFor(const QString &id, const QByteArray &itemKey, const QString &kind,
                     const QString &name, const QString &permission,
                     const QList<VaultRecipient> &members, const QStringList &accepted);
    QList<VaultRecipient> recipientsFrom(const QJsonArray &members) const;
    void rotate(const QString &id, Done done);
    void forgetItemKeys();

    Request m_request;
    Stretcher m_stretcher;
    int m_newMemoryKiB = 65536, m_newPasses = 3;
    QString m_account;
    bool m_installed = false;
    QJsonObject m_server;               // the account record from GET /api/vault
    Keyset m_keyset;
    QHash<QString, QString> m_pins;     // name -> fingerprint
    QList<VaultItem> m_items;
    QHash<QString, QByteArray> m_itemKeys;
};

}  // namespace forkmesh::vault
