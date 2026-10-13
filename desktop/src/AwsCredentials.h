#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QUrl>

class QSettings;

namespace forkmesh::aws {

inline constexpr char kSealedKey[] = "cloud/aws/sealed";
inline constexpr char kRegionKey[] = "cloud/aws/region";
inline constexpr char kFleetEnvName[] = "aws.env";

struct Credentials {
    QString accessKeyId;
    QString secretAccessKey;
    QString region;

    bool isComplete() const;
};

QStringList regions();
QString defaultRegion();
bool isValidAccessKeyId(const QString &value);
bool isValidSecretAccessKey(const QString &value);
bool isValidRegion(const QString &value);
QString validationError(const Credentials &credentials);

struct Request {
    QByteArray method;
    QUrl url;
    QList<QPair<QByteArray, QByteArray>> headers;
    QByteArray body;

    QByteArray header(const QByteArray &name) const;
};

QByteArray amzDate(const QDateTime &utc);
QByteArray signingKey(const QByteArray &secret, const QByteArray &date,
                      const QByteArray &region, const QByteArray &service);
QByteArray canonicalRequest(const Request &request, QByteArray *signedHeaders = nullptr);
QByteArray authorization(const Request &request, const QString &accessKeyId,
                         const QString &secretAccessKey, const QString &region,
                         const QString &service);
Request signRequest(Request request, const Credentials &credentials,
                    const QString &service, const QDateTime &utc);
Request callerIdentityRequest(const Credentials &credentials, const QDateTime &utc);

struct CallerIdentity {
    bool ok = false;
    QString account;
    QString arn;
    QString userId;
    QString error;
};

CallerIdentity parseCallerIdentity(int httpStatus, const QByteArray &body);

struct InstanceType {
    QString id;
    int vcpus = 0;
    int ramMb = 0;
    double hourly = 0.0;
    QString arch;
};

QList<InstanceType> instanceTypes();
QString fleetArch();
QString cheapestInstanceType(const QString &arch = fleetArch(), int minRamMb = 1024);
double monthlyEstimate(const QString &instanceType, int volumeGb = 20);

class CredentialStore {
public:
    explicit CredentialStore(QString keyPath = QString());

    static QString defaultKeyPath();
    QString keyPath() const { return m_keyPath; }

    bool save(const Credentials &credentials, QString *error = nullptr);
    Credentials load(QString *error = nullptr) const;
    bool hasStored() const;
    void clear();

private:
    QByteArray readKey(QString *error) const;
    QByteArray readOrCreateKey(QString *error);

    QString m_keyPath;
};

QString fleetEnvText(const Credentials &credentials);
bool writeFleetEnv(const QString &directory, const Credentials &credentials,
                   QString *error = nullptr);

} // namespace forkmesh::aws
