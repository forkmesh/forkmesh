#include "AwsCredentials.h"
#include "AwsSettingsPanel.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimeZone>

#include <cstdio>
#include <memory>

using namespace forkmesh::aws;

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

const QByteArray kVectorSecret = QByteArrayLiteral("wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY");
const QByteArray kVectorDate = QByteArrayLiteral("20150830T123600Z");

QString accessKey()
{
    return QStringLiteral("AKIA") + QStringLiteral("ABCDEFGHIJKLMNOP");
}

QString secretKey()
{
    return QStringLiteral("abcdEFGHijklMNOPqrstUVWXyz0123456789+/AB");
}

QByteArray vectorSignature(const QByteArray &method, const QString &url,
                           QList<QPair<QByteArray, QByteArray>> headers,
                           const QByteArray &body = QByteArray(),
                           const QString &service = QStringLiteral("service"))
{
    Request request;
    request.method = method;
    request.url = QUrl(url);
    request.headers = headers;
    request.body = body;
    const QByteArray value = authorization(request, QStringLiteral("AKIDEXAMPLE"),
                                           QString::fromLatin1(kVectorSecret),
                                           QStringLiteral("us-east-1"), service);
    return value.mid(value.indexOf("Signature=") + 10);
}

void testSignatureVectors()
{
    const QPair<QByteArray, QByteArray> host{"Host", "example.amazonaws.com"};
    const QPair<QByteArray, QByteArray> date{"X-Amz-Date", kVectorDate};
    check(vectorSignature("GET", QStringLiteral("https://example.amazonaws.com/"), {host, date}) ==
              "5fa00fa31553b73ebf1942676e86291e8372ff2a2260956d9b8aae1d763fbf31",
          "get-vanilla");
    check(vectorSignature("POST", QStringLiteral("https://example.amazonaws.com/"),
                          {host, date}) ==
              "5da7c1a2acd57cee7505fc6676e4e544621c30862966e37dddb68e92efbe5d6b",
          "post-vanilla");
    check(vectorSignature("GET",
                          QStringLiteral("https://example.amazonaws.com/?Param2=value2&Param1=value1"),
                          {host, date}) ==
              "b97d918cfa904a5beff61c982a1b6f458b799221646efd99d3219ec94cdf2500",
          "get-vanilla-query-order-key-case");
    check(vectorSignature("POST", QStringLiteral("https://example.amazonaws.com/"),
                          {{"Content-Type", "application/x-www-form-urlencoded"}, host, date},
                          "Param1=value1") ==
              "ff11897932ad3f4e8b18135d722051e5ac45fc38421b1da7b9d196a0fe09473a",
          "post-x-www-form-urlencoded");
    check(vectorSignature("GET",
                          QStringLiteral("https://iam.amazonaws.com/?Action=ListUsers&Version=2010-05-08"),
                          {{"Host", "iam.amazonaws.com"},
                           {"Content-Type", "application/x-www-form-urlencoded; charset=utf-8"},
                           date},
                          QByteArray(), QStringLiteral("iam")) ==
              "5d672d79c15b13162d9279b0855cfba6789a8edb4c82c400e06b5924a6f2b5d7",
          "documented IAM example");
    check(signingKey(kVectorSecret, "20120215", "us-east-1", "iam").toHex() ==
              "f4780e2d9f65fa895f9c67b32ce1baf0b0d8a43505a000a1a9e090d414db404d",
          "signing key derivation");
}

void testCallerIdentityRequestAndParsing()
{
    const Credentials credentials{accessKey(), secretKey(), QStringLiteral("eu-west-1")};
    const Request request = callerIdentityRequest(
        credentials, QDateTime(QDate(2026, 9, 30), QTime(12, 0, 0), QTimeZone::utc()));
    check(request.method == "POST" &&
              request.url.toString() == QLatin1String("https://sts.eu-west-1.amazonaws.com/"),
          "STS regional endpoint");
    check(request.body == "Action=GetCallerIdentity&Version=2011-06-15", "STS body");
    check(request.header("x-amz-date") == "20260930T120000Z", "STS date header");
    const QByteArray auth = request.header("authorization");
    check(auth.startsWith("AWS4-HMAC-SHA256 Credential=" + accessKey().toLatin1() +
                          "/20260930/eu-west-1/sts/aws4_request, "),
          "STS credential scope");
    check(auth.contains("SignedHeaders=content-type;host;x-amz-date, Signature="),
          "STS signed headers");
    check(!auth.contains(secretKey().toLatin1()), "the secret never leaves in a header");

    const CallerIdentity ok = parseCallerIdentity(
        200, "<GetCallerIdentityResponse xmlns=\"https://sts.amazonaws.com/doc/2011-06-15/\">"
             "<GetCallerIdentityResult><Arn>arn:aws:iam::123456789012:user/fleet</Arn>"
             "<UserId>AIDAEXAMPLE</UserId><Account>123456789012</Account>"
             "</GetCallerIdentityResult></GetCallerIdentityResponse>");
    check(ok.ok && ok.account == QLatin1String("123456789012") &&
              ok.arn == QLatin1String("arn:aws:iam::123456789012:user/fleet"),
          "STS success parses the account");
    const CallerIdentity refused = parseCallerIdentity(
        403, "<ErrorResponse><Error><Type>Sender</Type><Code>InvalidClientTokenId</Code>"
             "<Message>The security token included in the request is invalid.</Message>"
             "</Error></ErrorResponse>");
    check(!refused.ok && refused.error.startsWith(QLatin1String("InvalidClientTokenId: ")),
          "STS error code and message");
    check(!parseCallerIdentity(200, "<html>proxy</html>").ok, "no account is not a success");
    check(!parseCallerIdentity(200, "<x><Account>12</Account></x>").ok,
          "a malformed account id is refused");
}

void testInstanceTypes()
{
    check(fleetArch() == QLatin1String("x86_64"), "fleet arch");
    check(cheapestInstanceType() == QLatin1String("t3a.micro"), "cheapest fleet-ready type");
    check(cheapestInstanceType(QStringLiteral("x86_64"), 512) == QLatin1String("t3a.nano"),
          "cheapest tiny x86 type");
    check(cheapestInstanceType(QStringLiteral("arm64"), 512) == QLatin1String("t4g.nano"),
          "cheapest tiny arm type");
    check(qAbs(monthlyEstimate(QStringLiteral("t3a.micro")) - 12.11) < 0.001,
          "monthly estimate includes IPv4 and gp3");
    check(monthlyEstimate(QStringLiteral("m7i.large")) == 0.0, "unknown type has no price");
}

QByteArray settingsBytes()
{
    QSettings settings;
    settings.sync();
    QFile file(settings.fileName());
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

void testStore(const QString &root)
{
    const QString keyPath = root + QStringLiteral("/data/secrets/cloud-credentials.key");
    CredentialStore store(keyPath);
    check(!store.hasStored(), "empty store");
    QString error;
    check(!store.save(Credentials{QStringLiteral("nope"), secretKey(), QStringLiteral("us-east-1")},
                      &error) &&
              !error.isEmpty(),
          "malformed key id refused");
    check(store.save(Credentials{accessKey(), secretKey(), QStringLiteral("eu-west-2")}, &error),
          "credentials saved");
    const QByteArray raw = settingsBytes();
    check(!raw.isEmpty(), "settings file written");
    check(!raw.contains(secretKey().toLatin1()) && !raw.contains(accessKey().toLatin1()),
          "no plaintext credential in the settings file");
    check(QSettings().value(QLatin1String(kSealedKey)).toString().startsWith(QLatin1String("v1:")),
          "sealed blob is versioned");
    const QFileInfo keyInfo(keyPath);
    check(keyInfo.exists() && keyInfo.size() == 32, "key file holds 32 bytes");
    check(keyInfo.permissions() ==
              (QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadUser |
               QFileDevice::WriteUser),
          "key file is owner-only");
    const QFileInfo keyDir(QFileInfo(keyPath).absolutePath());
    check(!(keyDir.permissions() & (QFileDevice::ReadOther | QFileDevice::ReadGroup)),
          "key folder is owner-only");
    const Credentials loaded = CredentialStore(keyPath).load(&error);
    check(loaded.accessKeyId == accessKey() && loaded.secretAccessKey == secretKey() &&
              loaded.region == QLatin1String("eu-west-2"),
          "credentials round-trip");

    QString otherError;
    const Credentials wrongKey =
        CredentialStore(root + QStringLiteral("/elsewhere.key")).load(&otherError);
    check(wrongKey.secretAccessKey.isEmpty() && otherError.contains(QLatin1String("missing")),
          "no key file, no secret");
    QSettings settings;
    QString sealed = settings.value(QLatin1String(kSealedKey)).toString();
    QByteArray bytes = QByteArray::fromBase64(sealed.mid(3).toLatin1());
    bytes[bytes.size() - 1] = char(bytes.at(bytes.size() - 1) ^ 1);
    settings.setValue(QLatin1String(kSealedKey),
                      QStringLiteral("v1:") + QString::fromLatin1(bytes.toBase64()));
    QString tamperError;
    check(CredentialStore(keyPath).load(&tamperError).secretAccessKey.isEmpty() &&
              tamperError.contains(QLatin1String("decrypted")),
          "a tampered blob is refused");
    settings.setValue(QLatin1String(kSealedKey), sealed);
    store.clear();
    check(!store.hasStored() && CredentialStore(keyPath).load().secretAccessKey.isEmpty(),
          "clear forgets the secret");

    const QString fleet = root + QStringLiteral("/fleet");
    check(writeFleetEnv(fleet, Credentials{accessKey(), secretKey(), QStringLiteral("eu-west-2")},
                        &error),
          "aws.env written");
    QFile env(fleet + QStringLiteral("/aws.env"));
    check(env.open(QIODevice::ReadOnly) &&
              env.readAll() == fleetEnvText(Credentials{accessKey(), secretKey(),
                                                        QStringLiteral("eu-west-2")})
                                   .toUtf8(),
          "aws.env content");
    check(!(QFileInfo(env.fileName()).permissions() &
            (QFileDevice::ReadGroup | QFileDevice::ReadOther)),
          "aws.env is owner-only");
    check(!writeFleetEnv(QString(), Credentials{accessKey(), secretKey(),
                                                QStringLiteral("eu-west-2")}),
          "no folder, no file");
}

template <typename T>
T *child(QWidget *panel, const char *name)
{
    return panel->findChild<T *>(QString::fromLatin1(name));
}

void testPanel(const QString &root)
{
    const QString keyPath = root + QStringLiteral("/panel/cloud-credentials.key");
    const QString fleetDir = root + QStringLiteral("/panel-fleet");
    auto requests = std::make_shared<QList<Request>>();
    auto reply = std::make_shared<HttpReply>();
    PanelOptions options;
    options.keyPath = keyPath;
    options.clock = [] {
        return QDateTime(QDate(2026, 9, 30), QTime(8, 30, 0), QTimeZone::utc());
    };
    options.fleetConfigDirectory = [fleetDir] { return fleetDir; };
    options.transport = [requests, reply](const Request &request, QObject *, ReplyHandler done) {
        requests->append(request);
        done(*reply);
    };
    std::unique_ptr<QWidget> panel(buildSettingsPanel(options));
    auto *access = child<QLineEdit>(panel.get(), "awsAccessKeyId");
    auto *secret = child<QLineEdit>(panel.get(), "awsSecretAccessKey");
    auto *region = child<QComboBox>(panel.get(), "awsRegion");
    auto *status = child<QLabel>(panel.get(), "awsStatus");
    check(access && secret && region && status, "panel widgets exist");
    if (!access || !secret || !region || !status)
        return;
    check(secret->echoMode() == QLineEdit::Password, "secret is a password field");
    check(region->findData(QStringLiteral("us-east-1")) >= 0 && region->count() >= 10,
          "region dropdown");
    check(child<QLabel>(panel.get(), "awsCheapest")->text().contains(QLatin1String("t3a.micro")),
          "panel shows the cheapest type");

    child<QPushButton>(panel.get(), "awsTest")->click();
    check(requests->isEmpty() && status->text().contains(QLatin1String("access key ID")),
          "Test refuses an empty key without a request");

    access->setText(accessKey());
    secret->setText(secretKey());
    region->setCurrentIndex(region->findData(QStringLiteral("ap-southeast-2")));
    child<QPushButton>(panel.get(), "awsSave")->click();
    check(status->text().startsWith(QLatin1String("Saved (encrypted)")), "Save reports success");
    check(secret->text().isEmpty(), "the secret field is cleared after saving");
    const QByteArray raw = settingsBytes();
    check(!raw.contains(secretKey().toLatin1()) && !raw.contains(accessKey().toLatin1()),
          "panel save leaves no plaintext in settings");

    std::unique_ptr<QWidget> reopened(buildSettingsPanel(options));
    auto *access2 = child<QLineEdit>(reopened.get(), "awsAccessKeyId");
    auto *secret2 = child<QLineEdit>(reopened.get(), "awsSecretAccessKey");
    auto *region2 = child<QComboBox>(reopened.get(), "awsRegion");
    auto *status2 = child<QLabel>(reopened.get(), "awsStatus");
    check(access2->text() == accessKey(), "reopened panel shows the key id");
    check(secret2->text().isEmpty() &&
              secret2->placeholderText().contains(QLatin1String("stored")),
          "reopened panel never shows the secret");
    check(region2->currentData().toString() == QLatin1String("ap-southeast-2"),
          "reopened panel keeps the region");

    reply->status = 200;
    reply->body = "<GetCallerIdentityResponse><GetCallerIdentityResult>"
                  "<Arn>arn:aws:iam::210987654321:user/forkmesh-fleet</Arn>"
                  "<UserId>AIDA2</UserId><Account>210987654321</Account>"
                  "</GetCallerIdentityResult></GetCallerIdentityResponse>";
    child<QPushButton>(reopened.get(), "awsTest")->click();
    check(requests->size() == 1, "Test sends one request");
    if (!requests->isEmpty()) {
        const Request &sent = requests->constLast();
        check(sent.url.host() == QLatin1String("sts.ap-southeast-2.amazonaws.com"),
              "Test calls STS in the chosen region");
        check(sent.body.contains("Action=GetCallerIdentity"), "Test is read-only");
        check(sent.header("authorization").contains(
                  "/20260930/ap-southeast-2/sts/aws4_request"),
              "Test signs with the stored secret and the clock");
        Request copy = sent;
        copy.headers.removeIf([](const QPair<QByteArray, QByteArray> &header) {
            return header.first.compare("authorization", Qt::CaseInsensitive) == 0;
        });
        check(sent.header("authorization") ==
                  authorization(copy, accessKey(), secretKey(),
                                QStringLiteral("ap-southeast-2"), QStringLiteral("sts")),
              "the signature verifies with the real secret");
    }
    check(status2->text().contains(QLatin1String("210987654321")) &&
              status2->text().contains(QLatin1String("forkmesh-fleet")),
          "Test shows the account id");

    reply->status = 403;
    reply->body = "<ErrorResponse><Error><Code>SignatureDoesNotMatch</Code>"
                  "<Message>bad</Message></Error></ErrorResponse>";
    child<QPushButton>(reopened.get(), "awsTest")->click();
    check(status2->text().contains(QLatin1String("SignatureDoesNotMatch")),
          "Test shows the AWS error");
    check(!status2->text().contains(secretKey()), "status never shows the secret");

    child<QPushButton>(reopened.get(), "awsExport")->click();
    QFile env(fleetDir + QStringLiteral("/aws.env"));
    check(env.open(QIODevice::ReadOnly) &&
              env.readAll().contains("AWS_REGION=ap-southeast-2\n"),
          "Write aws.env uses the stored credentials");

    child<QPushButton>(reopened.get(), "awsForget")->click();
    check(CredentialStore(keyPath).load().secretAccessKey.isEmpty(), "Forget clears the store");
}

} // namespace

int main(int argc, char **argv)
{
    QTemporaryDir sandbox;
    if (!sandbox.isValid())
        return 2;
    const QString home = sandbox.path() + QStringLiteral("/home");
    QDir().mkpath(home + QStringLiteral("/.config"));
    QDir().mkpath(home + QStringLiteral("/.local/share"));
    QDir().mkpath(home + QStringLiteral("/.cache"));
    qputenv("HOME", home.toUtf8());
    qputenv("XDG_CONFIG_HOME", (home + QStringLiteral("/.config")).toUtf8());
    qputenv("XDG_DATA_HOME", (home + QStringLiteral("/.local/share")).toUtf8());
    qputenv("XDG_CACHE_HOME", (home + QStringLiteral("/.cache")).toUtf8());
    qunsetenv("FORKMESH_HOME");
    qunsetenv("FORKMESH_PROJECTS_DIR");
    qunsetenv("FMNEXT_CONFIG_DIR");
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ForkMeshAwsTest"));
    QCoreApplication::setApplicationName(QStringLiteral("ForkMeshAwsTest"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       sandbox.path() + QStringLiteral("/settings"));
    check(QSettings().fileName().startsWith(sandbox.path()), "settings live in the sandbox");

    testSignatureVectors();
    testCallerIdentityRequestAndParsing();
    testInstanceTypes();
    testStore(sandbox.path());
    testPanel(sandbox.path());

    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("aws credentials tests passed\n");
    return 0;
}
