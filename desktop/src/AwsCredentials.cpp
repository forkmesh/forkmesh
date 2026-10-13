#include "AwsCredentials.h"

#include "AppPaths.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageAuthenticationCode>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QUrlQuery>
#include <QXmlStreamReader>

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cmath>
#include <map>

namespace forkmesh::aws {

namespace {

constexpr int kKeyBytes = 32;
constexpr int kNonceBytes = 12;
constexpr int kTagBytes = 16;
constexpr char kSealPrefix[] = "v1:";
constexpr char kAssociatedData[] = "forkmesh-cloud-credentials-v1|aws";
constexpr double kIpv4Monthly = 0.005 * 730;
constexpr double kGp3GbMonthly = 0.08;

QByteArray hmac(const QByteArray &key, const QByteArray &message)
{
    return QMessageAuthenticationCode::hash(message, key, QCryptographicHash::Sha256);
}

QByteArray sha256Hex(const QByteArray &data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}

QByteArray encode(const QString &value)
{
    return QUrl::toPercentEncoding(value);
}

bool setOwnerOnly(QFileDevice &file)
{
    return file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

bool ensurePrivateDirectory(const QString &directory, QString *error)
{
    if (!QDir().mkpath(directory)) {
        if (error)
            *error = QStringLiteral("could not create %1").arg(directory);
        return false;
    }
    QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                         QFileDevice::ExeOwner);
    return true;
}

bool writePrivateFile(const QString &path, const QByteArray &bytes, QString *error)
{
    if (!ensurePrivateDirectory(QFileInfo(path).absolutePath(), error))
        return false;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || !setOwnerOnly(file)) {
        if (error)
            *error = QStringLiteral("could not write %1").arg(path);
        file.cancelWriting();
        return false;
    }
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        if (error)
            *error = QStringLiteral("could not write %1").arg(path);
        return false;
    }
    return true;
}

QByteArray seal(const QByteArray &key, const QByteArray &plain)
{
    QByteArray nonce(kNonceBytes, Qt::Uninitialized);
    if (RAND_bytes(reinterpret_cast<unsigned char *>(nonce.data()), kNonceBytes) != 1)
        return {};
    QByteArray out(plain.size() + kTagBytes, Qt::Uninitialized);
    const QByteArray aad(kAssociatedData);
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int length = 0;
    int total = 0;
    bool ok = ctx &&
              EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kNonceBytes, nullptr) == 1 &&
              EVP_EncryptInit_ex(ctx, nullptr, nullptr,
                                 reinterpret_cast<const unsigned char *>(key.constData()),
                                 reinterpret_cast<const unsigned char *>(nonce.constData())) == 1 &&
              EVP_EncryptUpdate(ctx, nullptr, &length,
                                reinterpret_cast<const unsigned char *>(aad.constData()),
                                int(aad.size())) == 1 &&
              EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char *>(out.data()), &length,
                                reinterpret_cast<const unsigned char *>(plain.constData()),
                                int(plain.size())) == 1;
    if (ok) {
        total = length;
        ok = EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(out.data()) + total,
                                 &length) == 1;
        total += length;
    }
    if (ok)
        ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, kTagBytes, out.data() + total) == 1;
    if (ctx)
        EVP_CIPHER_CTX_free(ctx);
    if (!ok)
        return {};
    out.resize(total + kTagBytes);
    return nonce + out;
}

QByteArray openSealed(const QByteArray &key, const QByteArray &sealed)
{
    if (sealed.size() <= kNonceBytes + kTagBytes)
        return {};
    const QByteArray nonce = sealed.left(kNonceBytes);
    const QByteArray cipher = sealed.mid(kNonceBytes, sealed.size() - kNonceBytes - kTagBytes);
    QByteArray tag = sealed.right(kTagBytes);
    QByteArray plain(cipher.size(), Qt::Uninitialized);
    const QByteArray aad(kAssociatedData);
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int length = 0;
    int total = 0;
    bool ok = ctx &&
              EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kNonceBytes, nullptr) == 1 &&
              EVP_DecryptInit_ex(ctx, nullptr, nullptr,
                                 reinterpret_cast<const unsigned char *>(key.constData()),
                                 reinterpret_cast<const unsigned char *>(nonce.constData())) == 1 &&
              EVP_DecryptUpdate(ctx, nullptr, &length,
                                reinterpret_cast<const unsigned char *>(aad.constData()),
                                int(aad.size())) == 1 &&
              EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char *>(plain.data()), &length,
                                reinterpret_cast<const unsigned char *>(cipher.constData()),
                                int(cipher.size())) == 1;
    if (ok) {
        total = length;
        ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, kTagBytes, tag.data()) == 1 &&
             EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(plain.data()) + total,
                                 &length) == 1;
        total += length;
    }
    if (ctx)
        EVP_CIPHER_CTX_free(ctx);
    if (!ok)
        return {};
    plain.resize(total);
    return plain;
}

} // namespace

bool Credentials::isComplete() const
{
    return validationError(*this).isEmpty();
}

QStringList regions()
{
    return {QStringLiteral("us-east-1"),      QStringLiteral("us-east-2"),
            QStringLiteral("us-west-1"),      QStringLiteral("us-west-2"),
            QStringLiteral("ca-central-1"),   QStringLiteral("eu-west-1"),
            QStringLiteral("eu-west-2"),      QStringLiteral("eu-west-3"),
            QStringLiteral("eu-central-1"),   QStringLiteral("eu-north-1"),
            QStringLiteral("eu-south-1"),     QStringLiteral("ap-south-1"),
            QStringLiteral("ap-northeast-1"), QStringLiteral("ap-northeast-2"),
            QStringLiteral("ap-northeast-3"), QStringLiteral("ap-southeast-1"),
            QStringLiteral("ap-southeast-2"), QStringLiteral("sa-east-1")};
}

QString defaultRegion()
{
    return QStringLiteral("us-east-1");
}

bool isValidAccessKeyId(const QString &value)
{
    static const QRegularExpression pattern(QStringLiteral("^AKIA[A-Z0-9]{16}$"));
    return pattern.match(value).hasMatch();
}

bool isValidSecretAccessKey(const QString &value)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9/+=]{30,128}$"));
    return pattern.match(value).hasMatch();
}

bool isValidRegion(const QString &value)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[a-z]{2}(-gov)?-[a-z]+-[0-9]{1,2}$"));
    return pattern.match(value).hasMatch() && !value.startsWith(QLatin1String("cn-"));
}

QString validationError(const Credentials &credentials)
{
    if (!isValidAccessKeyId(credentials.accessKeyId))
        return QStringLiteral("The access key ID must be an IAM user key (AKIA followed by 16 "
                              "letters or digits).");
    if (!isValidSecretAccessKey(credentials.secretAccessKey))
        return QStringLiteral("The secret access key is malformed.");
    if (!isValidRegion(credentials.region))
        return QStringLiteral("Choose an AWS region.");
    return QString();
}

QByteArray Request::header(const QByteArray &name) const
{
    for (const auto &pair : headers) {
        if (pair.first.compare(name, Qt::CaseInsensitive) == 0)
            return pair.second;
    }
    return {};
}

QByteArray amzDate(const QDateTime &utc)
{
    return utc.toUTC().toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'")).toLatin1();
}

QByteArray signingKey(const QByteArray &secret, const QByteArray &date,
                      const QByteArray &region, const QByteArray &service)
{
    QByteArray key = hmac(QByteArrayLiteral("AWS4") + secret, date);
    key = hmac(key, region);
    key = hmac(key, service);
    return hmac(key, QByteArrayLiteral("aws4_request"));
}

QByteArray canonicalRequest(const Request &request, QByteArray *signedHeaders)
{
    QString path = request.url.path(QUrl::FullyDecoded);
    if (path.isEmpty())
        path = QStringLiteral("/");
    QList<QPair<QByteArray, QByteArray>> query;
    for (const auto &item : QUrlQuery(request.url).queryItems(QUrl::FullyDecoded))
        query.append({encode(item.first), encode(item.second)});
    std::sort(query.begin(), query.end());
    QByteArrayList queryParts;
    for (const auto &item : query)
        queryParts.append(item.first + '=' + item.second);
    std::map<QByteArray, QByteArray> folded;
    for (const auto &pair : request.headers) {
        const QByteArray name = pair.first.trimmed().toLower();
        const QByteArray value = pair.second.simplified();
        auto found = folded.find(name);
        if (found == folded.end())
            folded.emplace(name, value);
        else
            found->second += ',' + value;
    }
    QByteArray headerText;
    QByteArrayList names;
    for (const auto &entry : folded) {
        headerText += entry.first + ':' + entry.second + '\n';
        names.append(entry.first);
    }
    const QByteArray signedList = names.join(';');
    if (signedHeaders)
        *signedHeaders = signedList;
    return request.method.toUpper() + '\n' + QUrl::toPercentEncoding(path, "/") + '\n' +
           queryParts.join('&') + '\n' + headerText + '\n' + signedList + '\n' +
           sha256Hex(request.body);
}

QByteArray authorization(const Request &request, const QString &accessKeyId,
                         const QString &secretAccessKey, const QString &region,
                         const QString &service)
{
    const QByteArray date = request.header("x-amz-date");
    QByteArray signedHeaders;
    const QByteArray canonical = canonicalRequest(request, &signedHeaders);
    const QByteArray scope = date.left(8) + '/' + region.toLatin1() + '/' +
                             service.toLatin1() + "/aws4_request";
    const QByteArray toSign = QByteArrayLiteral("AWS4-HMAC-SHA256\n") + date + '\n' + scope +
                              '\n' + sha256Hex(canonical);
    const QByteArray key = signingKey(secretAccessKey.toUtf8(), date.left(8),
                                      region.toLatin1(), service.toLatin1());
    return QByteArrayLiteral("AWS4-HMAC-SHA256 Credential=") + accessKeyId.toLatin1() + '/' +
           scope + ", SignedHeaders=" + signedHeaders +
           ", Signature=" + hmac(key, toSign).toHex();
}

Request signRequest(Request request, const Credentials &credentials, const QString &service,
                    const QDateTime &utc)
{
    if (request.header("host").isEmpty())
        request.headers.append({QByteArrayLiteral("Host"), request.url.host().toLatin1()});
    if (request.header("x-amz-date").isEmpty())
        request.headers.append({QByteArrayLiteral("X-Amz-Date"), amzDate(utc)});
    request.headers.append({QByteArrayLiteral("Authorization"),
                            authorization(request, credentials.accessKeyId,
                                          credentials.secretAccessKey, credentials.region,
                                          service)});
    return request;
}

Request callerIdentityRequest(const Credentials &credentials, const QDateTime &utc)
{
    Request request;
    request.method = QByteArrayLiteral("POST");
    request.url = QUrl(QStringLiteral("https://sts.%1.amazonaws.com/").arg(credentials.region));
    request.body = QByteArrayLiteral("Action=GetCallerIdentity&Version=2011-06-15");
    request.headers.append({QByteArrayLiteral("Content-Type"),
                            QByteArrayLiteral("application/x-www-form-urlencoded; charset=utf-8")});
    return signRequest(request, credentials, QStringLiteral("sts"), utc);
}

CallerIdentity parseCallerIdentity(int httpStatus, const QByteArray &body)
{
    CallerIdentity identity;
    QString code;
    QString message;
    QXmlStreamReader reader(body);
    while (!reader.atEnd()) {
        if (reader.readNext() != QXmlStreamReader::StartElement)
            continue;
        const QStringView name = reader.name();
        if (name == QLatin1String("Account"))
            identity.account = reader.readElementText().trimmed();
        else if (name == QLatin1String("Arn"))
            identity.arn = reader.readElementText().trimmed();
        else if (name == QLatin1String("UserId"))
            identity.userId = reader.readElementText().trimmed();
        else if (name == QLatin1String("Code") && code.isEmpty())
            code = reader.readElementText().trimmed();
        else if (name == QLatin1String("Message") && message.isEmpty())
            message = reader.readElementText().trimmed();
    }
    static const QRegularExpression accountPattern(QStringLiteral("^[0-9]{12}$"));
    if (httpStatus == 200 && !reader.hasError() &&
        accountPattern.match(identity.account).hasMatch()) {
        identity.ok = true;
        return identity;
    }
    identity.account.clear();
    if (!code.isEmpty())
        identity.error = message.isEmpty() ? code : QStringLiteral("%1: %2").arg(code, message);
    else if (httpStatus <= 0)
        identity.error = QStringLiteral("no answer from AWS");
    else
        identity.error = QStringLiteral("AWS answered HTTP %1 without an account id")
                             .arg(httpStatus);
    return identity;
}

QList<InstanceType> instanceTypes()
{
    return {
        {QStringLiteral("t4g.nano"), 2, 512, 0.0042, QStringLiteral("arm64")},
        {QStringLiteral("t4g.micro"), 2, 1024, 0.0084, QStringLiteral("arm64")},
        {QStringLiteral("t4g.small"), 2, 2048, 0.0168, QStringLiteral("arm64")},
        {QStringLiteral("t4g.medium"), 2, 4096, 0.0336, QStringLiteral("arm64")},
        {QStringLiteral("t3a.nano"), 2, 512, 0.0047, QStringLiteral("x86_64")},
        {QStringLiteral("t3a.micro"), 2, 1024, 0.0094, QStringLiteral("x86_64")},
        {QStringLiteral("t3a.small"), 2, 2048, 0.0188, QStringLiteral("x86_64")},
        {QStringLiteral("t3a.medium"), 2, 4096, 0.0376, QStringLiteral("x86_64")},
        {QStringLiteral("t3.nano"), 2, 512, 0.0052, QStringLiteral("x86_64")},
        {QStringLiteral("t3.micro"), 2, 1024, 0.0104, QStringLiteral("x86_64")},
        {QStringLiteral("t3.small"), 2, 2048, 0.0208, QStringLiteral("x86_64")},
        {QStringLiteral("t3.medium"), 2, 4096, 0.0416, QStringLiteral("x86_64")},
    };
}

QString fleetArch()
{
    return QStringLiteral("x86_64");
}

QString cheapestInstanceType(const QString &arch, int minRamMb)
{
    const InstanceType *best = nullptr;
    const QList<InstanceType> types = instanceTypes();
    for (const InstanceType &type : types) {
        if (type.arch != arch || type.ramMb < minRamMb)
            continue;
        if (!best || type.hourly < best->hourly ||
            (type.hourly == best->hourly &&
             (type.ramMb > best->ramMb || (type.ramMb == best->ramMb && type.id < best->id))))
            best = &type;
    }
    return best ? best->id : QString();
}

double monthlyEstimate(const QString &instanceType, int volumeGb)
{
    for (const InstanceType &type : instanceTypes()) {
        if (type.id == instanceType)
            return std::round((type.hourly * 730 + kIpv4Monthly + kGp3GbMonthly * volumeGb) *
                              100.0) /
                   100.0;
    }
    return 0.0;
}

CredentialStore::CredentialStore(QString keyPath)
    : m_keyPath(keyPath.isEmpty() ? defaultKeyPath() : std::move(keyPath))
{
}

QString CredentialStore::defaultKeyPath()
{
    return forkmesh::paths::dataRoot() + QStringLiteral("/secrets/cloud-credentials.key");
}

QByteArray CredentialStore::readKey(QString *error) const
{
    QFile file(m_keyPath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("the credential key %1 is missing").arg(m_keyPath);
        return {};
    }
    const QByteArray key = file.readAll();
    if (key.size() != kKeyBytes) {
        if (error)
            *error = QStringLiteral("the credential key %1 is damaged").arg(m_keyPath);
        return {};
    }
    return key;
}

QByteArray CredentialStore::readOrCreateKey(QString *error)
{
    if (QFileInfo::exists(m_keyPath))
        return readKey(error);
    QByteArray key(kKeyBytes, Qt::Uninitialized);
    if (RAND_bytes(reinterpret_cast<unsigned char *>(key.data()), kKeyBytes) != 1) {
        if (error)
            *error = QStringLiteral("no secure random bytes for the credential key");
        return {};
    }
    if (!writePrivateFile(m_keyPath, key, error))
        return {};
    return key;
}

bool CredentialStore::save(const Credentials &credentials, QString *error)
{
    const QString problem = validationError(credentials);
    if (!problem.isEmpty()) {
        if (error)
            *error = problem;
        return false;
    }
    const QByteArray key = readOrCreateKey(error);
    if (key.isEmpty())
        return false;
    const QByteArray plain = QJsonDocument(QJsonObject{
                                               {QStringLiteral("accessKeyId"),
                                                credentials.accessKeyId},
                                               {QStringLiteral("secretAccessKey"),
                                                credentials.secretAccessKey},
                                           })
                                 .toJson(QJsonDocument::Compact);
    const QByteArray sealed = seal(key, plain);
    if (sealed.isEmpty()) {
        if (error)
            *error = QStringLiteral("could not encrypt the credentials");
        return false;
    }
    QSettings settings;
    settings.setValue(QLatin1String(kSealedKey),
                      QString::fromLatin1(QByteArray(kSealPrefix) + sealed.toBase64()));
    settings.setValue(QLatin1String(kRegionKey), credentials.region);
    settings.sync();
    return settings.status() == QSettings::NoError;
}

Credentials CredentialStore::load(QString *error) const
{
    Credentials credentials;
    QSettings settings;
    credentials.region = settings.value(QLatin1String(kRegionKey)).toString();
    if (!isValidRegion(credentials.region))
        credentials.region = defaultRegion();
    const QByteArray stored = settings.value(QLatin1String(kSealedKey)).toString().toLatin1();
    if (stored.isEmpty())
        return credentials;
    if (!stored.startsWith(kSealPrefix)) {
        if (error)
            *error = QStringLiteral("the stored AWS credentials use an unknown format");
        return credentials;
    }
    const QByteArray key = readKey(error);
    if (key.isEmpty())
        return credentials;
    const QByteArray plain =
        openSealed(key, QByteArray::fromBase64(stored.mid(int(qstrlen(kSealPrefix)))));
    const QJsonObject object = QJsonDocument::fromJson(plain).object();
    if (object.isEmpty()) {
        if (error)
            *error = QStringLiteral("the stored AWS credentials could not be decrypted");
        return credentials;
    }
    credentials.accessKeyId = object.value(QStringLiteral("accessKeyId")).toString();
    credentials.secretAccessKey = object.value(QStringLiteral("secretAccessKey")).toString();
    return credentials;
}

bool CredentialStore::hasStored() const
{
    return !QSettings().value(QLatin1String(kSealedKey)).toString().isEmpty();
}

void CredentialStore::clear()
{
    QSettings settings;
    settings.remove(QLatin1String(kSealedKey));
    settings.sync();
}

QString fleetEnvText(const Credentials &credentials)
{
    return QStringLiteral("AWS_ACCESS_KEY_ID=%1\nAWS_SECRET_ACCESS_KEY=%2\nAWS_REGION=%3\n")
        .arg(credentials.accessKeyId, credentials.secretAccessKey, credentials.region);
}

bool writeFleetEnv(const QString &directory, const Credentials &credentials, QString *error)
{
    const QString problem = validationError(credentials);
    if (!problem.isEmpty()) {
        if (error)
            *error = problem;
        return false;
    }
    if (directory.trimmed().isEmpty()) {
        if (error)
            *error = QStringLiteral("no fleet config folder");
        return false;
    }
    return writePrivateFile(QDir(directory).filePath(QLatin1String(kFleetEnvName)),
                            fleetEnvText(credentials).toUtf8(), error);
}

} // namespace forkmesh::aws
