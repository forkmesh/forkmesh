#include "PublicMirrorRuntime.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>

#include <cstdio>

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

bool writeFile(const QString &path, const QByteArray &bytes,
               bool executable = false)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        file.write(bytes) != bytes.size()) {
        return false;
    }
    file.close();
    if (executable) {
        return file.setPermissions(
            QFileDevice::ReadOwner | QFileDevice::WriteOwner |
            QFileDevice::ExeOwner);
    }
    return true;
}

bool git(const QString &path, const QStringList &arguments,
         QByteArray *output = nullptr)
{
    QProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("GIT_AUTHOR_NAME"),
                       QStringLiteral("Public Mirror Test"));
    environment.insert(QStringLiteral("GIT_AUTHOR_EMAIL"),
                       QStringLiteral("public-mirror@example.invalid"));
    environment.insert(QStringLiteral("GIT_COMMITTER_NAME"),
                       QStringLiteral("Public Mirror Test"));
    environment.insert(QStringLiteral("GIT_COMMITTER_EMAIL"),
                       QStringLiteral("public-mirror@example.invalid"));
    process.setProcessEnvironment(environment);
    process.start(QStringLiteral("git"),
                  QStringList{QStringLiteral("-C"), path} + arguments);
    if (!process.waitForFinished(30000) || process.exitCode() != 0)
        return false;
    if (output)
        *output = process.readAllStandardOutput();
    return true;
}

bool excludesGroupAndOther(const QFileInfo &info)
{
    const auto permissions = info.permissions();
    return !(permissions &
             (QFileDevice::ReadGroup | QFileDevice::WriteGroup |
              QFileDevice::ExeGroup | QFileDevice::ReadOther |
              QFileDevice::WriteOther | QFileDevice::ExeOther));
}

QByteArray readAll(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    const QByteArray unsortedRefs =
        QByteArrayLiteral(
            "0000000000000000000000000000000000000000 refs/tags/zeta\n"
            "ffffffffffffffffffffffffffffffffffffffff refs/heads/alpha\n"
            "1111111111111111111111111111111111111111 refs/tags/beta\n");
    const QByteArray canonicalRefs =
        QByteArrayLiteral(
            "ffffffffffffffffffffffffffffffffffffffff refs/heads/alpha\n"
            "1111111111111111111111111111111111111111 refs/tags/beta\n"
            "0000000000000000000000000000000000000000 refs/tags/zeta");
    check(
        PublicMirrorRuntime::refsSha256FromForEachRef(unsortedRefs) ==
            QString::fromLatin1(
                QCryptographicHash::hash(canonicalRefs,
                                         QCryptographicHash::Sha256)
                    .toHex()),
        "public ref fingerprints are canonicalized by refname");

    QTemporaryDir root;
    check(root.isValid(), "temporary root is available");
    QString error;

    const QString cleanupRoot = root.filePath(QStringLiteral("runtime"));
    check(QDir().mkpath(
              QDir(cleanupRoot).filePath(
                  QStringLiteral("forkmesh-mirror-runtime-stale_1/repository-0"))) &&
              QDir().mkpath(
                  QDir(cleanupRoot).filePath(
                      QStringLiteral("forkmesh-public-mirror-Ab12Cd/repository.git"))) &&
              QDir().mkpath(
                  QDir(cleanupRoot).filePath(
                      QStringLiteral("ForkMesh-Zz91Qq/repository.git"))) &&
              QDir().mkpath(
                  QDir(cleanupRoot).filePath(
                      QStringLiteral("ForkMesh-Keep01/operator-data"))) &&
              QDir().mkpath(
                  QDir(cleanupRoot).filePath(
                      QStringLiteral("operator-data"))),
          "stale mirror cleanup fixture is created");
    QFile cleanupPermissions(cleanupRoot);
    check(cleanupPermissions.setPermissions(
              QFileDevice::ReadOwner | QFileDevice::WriteOwner |
              QFileDevice::ExeOwner),
          "stale mirror cleanup root is owner-only");
    check(PublicMirrorRuntime::cleanupStaleTemporaryDirectories(
              cleanupRoot, &error) == 3 &&
              QFileInfo::exists(
                  QDir(cleanupRoot).filePath(QStringLiteral("operator-data"))) &&
              QFileInfo::exists(
                  QDir(cleanupRoot).filePath(QStringLiteral("ForkMesh-Keep01"))),
          "startup removes only exact stale mirror materializations");

    const QString toolsRoot = root.filePath(QStringLiteral("tools"));
    check(QDir().mkpath(toolsRoot), "fake tool directory is created");
    const QString fakeAge =
        QDir(toolsRoot).filePath(QStringLiteral("age"));
    const QString fakeKeygen =
        QDir(toolsRoot).filePath(QStringLiteral("age-keygen"));
    const QByteArray ageScript = R"PY(#!/usr/bin/env python3
import pathlib
import sys

HEADER = b"age-encryption.org/v1\n"
args = sys.argv[1:]
if "--encrypt" in args:
    output = pathlib.Path(args[args.index("--output") + 1])
    plain = sys.stdin.buffer.read()
    output.write_bytes(HEADER + bytes(value ^ 0xA5 for value in plain))
    raise SystemExit(0)
if "--decrypt" in args:
    raw = pathlib.Path(args[-1]).read_bytes()
    if not raw.startswith(HEADER):
        raise SystemExit(2)
    sys.stdout.buffer.write(bytes(value ^ 0xA5 for value in raw[len(HEADER):]))
    raise SystemExit(0)
raise SystemExit(2)
)PY";
    const QByteArray keygenScript = R"PY(#!/usr/bin/env python3
import pathlib
import sys

SECRET = "AGE-SECRET-KEY-1QQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQ"
RECIPIENT = "age1qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq"
args = sys.argv[1:]
if args[:1] == ["-o"] and len(args) == 2:
    pathlib.Path(args[1]).write_text(
        "# created by deterministic ForkMesh test helper\n" + SECRET + "\n",
        encoding="ascii",
    )
    raise SystemExit(0)
if args[:1] == ["-y"] and len(args) == 2:
    print(RECIPIENT)
    raise SystemExit(0)
raise SystemExit(2)
)PY";
    check(writeFile(fakeAge, ageScript, true),
          "fake age tool is created");
    check(writeFile(fakeKeygen, keygenScript, true),
          "fake age-keygen tool is created");

    PublicMirrorRuntime::Tools tools;
    tools.age = fakeAge;
    tools.ageKeygen = fakeKeygen;
    tools.tar = QStringLiteral("tar");
    tools.git = QStringLiteral("git");
    error.clear();
    check(PublicMirrorRuntime::toolingAvailable(tools, &error),
          "all encrypted public mirror tools are available");
    PublicMirrorRuntime::Tools missing = tools;
    missing.age = root.filePath(QStringLiteral("missing-age"));
    check(!PublicMirrorRuntime::toolingAvailable(missing, &error),
          "missing age fails closed");
    error.clear();

    const QString source = root.filePath(QStringLiteral("source"));
    check(QDir().mkpath(source), "public source directory is created");
    check(git(source, {QStringLiteral("init"), QStringLiteral("-q"),
                       QStringLiteral("-b"), QStringLiteral("main")}),
          "public source repository is initialized");
    const QByteArray firstPlaintext =
        QByteArrayLiteral("public-repository-content-v1");
    check(writeFile(QDir(source).filePath(QStringLiteral("README.md")),
                    firstPlaintext) &&
              git(source, {QStringLiteral("add"), QStringLiteral("README.md")}) &&
              git(source, {QStringLiteral("commit"), QStringLiteral("-q"),
                           QStringLiteral("-m"), QStringLiteral("initial")}),
          "public source commit is created");

    const QString archiveRoot =
        root.filePath(QStringLiteral("encrypted-public-mirrors"));
    const QString vaultPath =
        root.filePath(QStringLiteral("identity/public-age-vault.json"));
    const QByteArray vaultSecret =
        QByteArrayLiteral("0123456789abcdef0123456789abcdef"
                          "device-bound-public-age-vault-secret");
    auto created = PublicMirrorRuntime::syncRepository(
        source, archiveRoot, vaultPath, vaultSecret, {}, tools, &error);
    if (!created.isValid())
        std::fprintf(stderr, "public sync error: %s\n",
                     error.toUtf8().constData());
    check(created.isValid() && created.created && error.isEmpty(),
          "public repository seals into an age archive and authenticates a temporary reopen");
    check(PublicMirrorRuntime::isArchiveId(created.metadata.archiveId) &&
              created.metadata.ciphertextSha256.size() == 64 &&
              created.metadata.expectedRefsSha256.size() == 64 &&
              created.metadata.keyReference ==
                  PublicMirrorRuntime::keyReference(
                      created.metadata.archiveId),
          "public archive metadata is complete and opaque");
    check(PublicMirrorRuntime::repositoryRefsSha256(source, tools, &error) ==
              created.metadata.expectedRefsSha256,
          "an open mirror can cheaply prove its source refs are unchanged");

    const QString ciphertext = PublicMirrorRuntime::ciphertextPath(
        archiveRoot, created.metadata.archiveId);
    const QByteArray cipherBytes = readAll(ciphertext);
    const QByteArray vaultBytes = readAll(vaultPath);
    check(cipherBytes.startsWith(QByteArrayLiteral("age-encryption.org/v1\n")) &&
              !cipherBytes.contains(firstPlaintext) &&
              !vaultBytes.contains(firstPlaintext) &&
              !vaultBytes.contains(QByteArrayLiteral("AGE-SECRET-KEY-1")),
          "durable archive and identity vault contain no repository or age-identity plaintext");
    check(excludesGroupAndOther(QFileInfo(ciphertext)) &&
              excludesGroupAndOther(QFileInfo(vaultPath)) &&
              excludesGroupAndOther(
                  QFileInfo(QFileInfo(vaultPath).absolutePath())),
          "ciphertext, vault, and vault directory are owner-only");

    QByteArray shown;
    check(git(created.materialization->repositoryPath(),
              {QStringLiteral("show"),
               QStringLiteral("refs/heads/main:README.md")},
              &shown) &&
              shown.trimmed() == firstPlaintext,
          "authenticated temporary materialization contains the expected commit");
    check(!created.materialization->repositoryPath().startsWith(
              QDir::cleanPath(archiveRoot) + QLatin1Char('/')),
          "plaintext materialization is outside durable archive storage");

    QStringList unchangedProgress;
    auto unchanged = PublicMirrorRuntime::syncRepository(
        source, archiveRoot, vaultPath, vaultSecret,
        created.metadata.archiveId, tools, &error,
        [&](const QString &line) { unchangedProgress.append(line); });
    check(unchanged.isValid() && !unchanged.created &&
              unchanged.metadata.ciphertextSha256 ==
                  created.metadata.ciphertextSha256 &&
              unchangedProgress.contains(QStringLiteral(
                  "Repository unchanged — reopening the sealed archive…")) &&
              !unchangedProgress.contains(QStringLiteral(
                  "Copying the repository to seal…")),
          "an unchanged source reopens its verified seal without making a mirror clone");
    unchanged.materialization.reset();

    const QString firstMaterialization =
        created.materialization->repositoryPath();
    created.materialization.reset();
    check(!QFileInfo::exists(firstMaterialization),
          "destroying a public materialization removes its plaintext");
    auto reopened = PublicMirrorRuntime::materialize(
        archiveRoot, vaultPath, vaultSecret, created.metadata.archiveId,
        tools, &error);
    check(reopened && reopened->isValid(),
          "the device-bound vault can reopen the encrypted archive");
    check(!PublicMirrorRuntime::materialize(
              archiveRoot, vaultPath, QByteArray(64, 'x'),
              created.metadata.archiveId, tools, &error),
          "a wrong device secret cannot decrypt the age identity vault");

    QTemporaryDir gatewayDestination;
    const QJsonObject gatewayRequest{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("type"),
         QStringLiteral("forkmesh.repository-archive-materialize")},
        {QStringLiteral("scheme"),
         QStringLiteral("age-encrypted-tar-v1")},
        {QStringLiteral("ciphertextPath"), ciphertext},
        {QStringLiteral("ciphertextSha256"),
         created.metadata.ciphertextSha256},
        {QStringLiteral("keyReference"),
         created.metadata.keyReference},
        {QStringLiteral("destination"), gatewayDestination.path()},
    };
    const QJsonObject gatewayResponse =
        PublicMirrorRuntime::materializeGatewayRequest(
            gatewayRequest, archiveRoot, vaultPath, vaultSecret,
            tools, &error);
    check(gatewayResponse.value(QStringLiteral("ok")).toBool() &&
              gatewayResponse.value(QStringLiteral("repositoryPath"))
                      .toString() ==
                  QLatin1String("repository.git") &&
              QFileInfo(gatewayDestination.filePath(
                            QStringLiteral("repository.git")))
                  .isDir(),
          "gateway materialization validates the complete external-command request");
    QJsonObject wrongRequest = gatewayRequest;
    wrongRequest.insert(QStringLiteral("ciphertextSha256"),
                        QString(64, QLatin1Char('0')));
    QTemporaryDir wrongDestination;
    wrongRequest.insert(QStringLiteral("destination"),
                        wrongDestination.path());
    check(PublicMirrorRuntime::materializeGatewayRequest(
              wrongRequest, archiveRoot, vaultPath, vaultSecret,
              tools, &error)
              .isEmpty(),
          "gateway materialization rejects a mismatched ciphertext digest");

    check(git(source,
              {QStringLiteral("update-ref"),
               QStringLiteral("refs/remotes/origin/main"),
               QStringLiteral("HEAD")}) &&
              git(source,
                  {QStringLiteral("branch"),
                   QStringLiteral("agent/local-only")}),
          "managed checkout has an origin view and a local agent branch");
    error.clear();
    auto managed = PublicMirrorRuntime::syncManagedCheckout(
        source, root.filePath(QStringLiteral("managed-archive")),
        root.filePath(QStringLiteral("identity/managed-vault.json")),
        vaultSecret, {}, tools, &error);
    check(managed.isValid() &&
              git(managed.materialization->repositoryPath(),
                  {QStringLiteral("show-ref"), QStringLiteral("--verify"),
                   QStringLiteral("refs/heads/main")}) &&
              !git(managed.materialization->repositoryPath(),
                   {QStringLiteral("show-ref"), QStringLiteral("--verify"),
                    QStringLiteral("refs/heads/agent/local-only")}),
          "managed mirror seals fetched origin branches without exposing local agent refs");

    QStringList managedProgress;
    auto managedUnchanged = PublicMirrorRuntime::syncManagedCheckout(
        source, root.filePath(QStringLiteral("managed-archive")),
        root.filePath(QStringLiteral("identity/managed-vault.json")),
        vaultSecret, managed.metadata.archiveId, tools, &error,
        [&](const QString &line) { managedProgress.append(line); });
    check(managedUnchanged.isValid() &&
              managedUnchanged.metadata.ciphertextSha256 ==
                  managed.metadata.ciphertextSha256 &&
              managedProgress.contains(QStringLiteral(
                  "Repository unchanged — reopening the sealed archive…")) &&
              !managedProgress.contains(QStringLiteral(
                  "Copying the managed checkout…")),
          "an unchanged managed checkout skips its canonical mirror clone");
    check(git(source,
              {QStringLiteral("branch"), QStringLiteral("-D"),
               QStringLiteral("agent/local-only")}) &&
              git(source,
                  {QStringLiteral("update-ref"), QStringLiteral("-d"),
                   QStringLiteral("refs/remotes/origin/main")}),
          "managed-checkout-only refs are removed before the ordinary race test");

    const QByteArray secondPlaintext =
        QByteArrayLiteral("public-repository-content-v2");
    const QString archiveId = created.metadata.archiveId;
    const QString firstDigest = created.metadata.ciphertextSha256;
    QStringList racedProgress;
    bool racedSourceUpdate = false;
    bool racedSourceUpdateOk = false;
    auto updated = PublicMirrorRuntime::syncRepository(
        source, archiveRoot, vaultPath, vaultSecret, archiveId,
        tools, &error, [&](const QString &line) {
            racedProgress.append(line);
            if (racedSourceUpdate ||
                line != QStringLiteral(
                    "Repository unchanged — reopening the sealed archive…"))
                return;
            racedSourceUpdate = true;
            racedSourceUpdateOk =
                writeFile(QDir(source).filePath(QStringLiteral("README.md")),
                          secondPlaintext) &&
                git(source,
                    {QStringLiteral("add"), QStringLiteral("README.md")}) &&
                git(source,
                    {QStringLiteral("commit"), QStringLiteral("-q"),
                     QStringLiteral("-m"), QStringLiteral("update")});
        });
    check(racedSourceUpdate && racedSourceUpdateOk && updated.isValid() &&
              !updated.created &&
              updated.metadata.archiveId == archiveId &&
              updated.metadata.ciphertextSha256 != firstDigest &&
              updated.metadata.expectedRefsSha256 !=
                  created.metadata.expectedRefsSha256 &&
              racedProgress.contains(QStringLiteral(
                  "Repository refs changed during verification — rebuilding the seal…")) &&
              racedProgress.contains(QStringLiteral(
                  "Copying the repository to seal…")),
          "a ref update racing the fast reopen falls back to a fresh atomic seal");
    shown.clear();
    check(git(updated.materialization->repositoryPath(),
              {QStringLiteral("show"),
               QStringLiteral("refs/heads/main:README.md")},
              &shown) &&
              shown.trimmed() == secondPlaintext,
          "updated encrypted archive reopens at the new commit");

    QByteArray tampered = readAll(ciphertext);
    if (tampered.size() > 40)
        tampered[tampered.size() / 2] ^= 1;
    check(writeFile(ciphertext, tampered),
          "test can tamper with durable ciphertext");
    check(!PublicMirrorRuntime::materialize(
              archiveRoot, vaultPath, vaultSecret, archiveId,
              tools, &error),
          "ciphertext tampering is rejected before age is invoked");
    error.clear();
    auto recovered = PublicMirrorRuntime::syncRepository(
        source, archiveRoot, vaultPath, vaultSecret, archiveId,
        tools, &error);
    check(recovered.isValid() &&
              recovered.metadata.archiveId == archiveId &&
              QString::fromLatin1(
                  QCryptographicHash::hash(readAll(ciphertext),
                                           QCryptographicHash::Sha256)
                      .toHex()) ==
                  recovered.metadata.ciphertextSha256,
          "an authenticated source can rebuild a damaged archive in place");

    const QString managedRoot =
        root.filePath(QStringLiteral("legacy-mirrors"));
    const QString legacy =
        QDir(managedRoot).filePath(QStringLiteral("demo.git"));
    check(QDir().mkpath(legacy), "legacy managed mirror is created");
    check(PublicMirrorRuntime::removeManagedPlaintextMirror(
              legacy, managedRoot, &error) &&
              !QFileInfo::exists(legacy),
          "authenticated migration can remove a managed legacy plaintext mirror");
    const QString outside =
        root.filePath(QStringLiteral("outside.git"));
    check(QDir().mkpath(outside) &&
              !PublicMirrorRuntime::removeManagedPlaintextMirror(
                  outside, managedRoot, &error) &&
              QFileInfo::exists(outside),
          "legacy cleanup refuses paths outside the managed root");

    if (failures == 0)
        std::printf("public mirror runtime tests passed\n");
    return failures == 0 ? 0 : 1;
}
