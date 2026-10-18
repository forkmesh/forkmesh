#include "SecureDriveCrypto.h"
#include "SecureDriveFormats.h"
#include "SecureDriveStore.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTemporaryDir>

#include <cstdio>
#include <memory>
#include <thread>

using namespace forkmesh::securedrive;

namespace {

int failures = 0;
int checks = 0;

void check(bool ok, const char *name)
{
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n", name);
    }
}

const QString kPass = QStringLiteral("correct horse battery");

QByteArray readAll(Drive &drive, const QString &id, int version = -1, Failure *failure = nullptr)
{
    QByteArray out;
    QBuffer buffer(&out);
    buffer.open(QIODevice::WriteOnly);
    Failure local;
    if (!drive.readFile(id, version, &buffer, QStringLiteral("view"), failure ? failure : &local))
        return QByteArray("<failed>");
    return out;
}

bool add(Drive &drive, const QString &name, const QByteArray &bytes, QString *id)
{
    QByteArray copy = bytes;
    QBuffer buffer(&copy);
    buffer.open(QIODevice::ReadOnly);
    Failure failure;
    const bool ok = drive.addFile(name, &buffer, id, &failure);
    if (!ok)
        std::fprintf(stderr, "addFile: %s\n", qPrintable(failure.code));
    return ok;
}

bool save(Drive &drive, const QString &id, const QByteArray &bytes, const QString &base = QString())
{
    QByteArray copy = bytes;
    QBuffer buffer(&copy);
    buffer.open(QIODevice::ReadOnly);
    Failure failure;
    return drive.saveFile(id, &buffer, &failure, base);
}

bool hasIssue(const Drive &drive, const char *code)
{
    for (const Issue &issue : drive.issues())
        if (issue.code == QLatin1String(code))
            return true;
    return false;
}

bool hasAction(const Drive &drive, const char *action)
{
    for (const Record &record : drive.records())
        if (record.action == QLatin1String(action))
            return true;
    return false;
}

void cryptoPrimitives()
{
    QByteArray secret, parsed;
    const QString code = newInviteCode(&secret);
    check(code.size() == 35, "invite code has nine groups of three");
    check(parseInviteCode(code, &parsed) && parsed == secret, "invite code round-trips");
    check(parseInviteCode(code.toLower(), &parsed) && parsed == secret, "invite code ignores case");
    QString typo = code;
    typo[0] = typo.at(0) == QLatin1Char('A') ? QLatin1Char('B') : QLatin1Char('A');
    check(!parseInviteCode(typo, &parsed), "a mistyped invite code is refused");

    DeviceKeys alice, bob;
    PublicKeys alicePublic;
    check(DeviceKeys::generate(&alice, nullptr) && DeviceKeys::generate(&bob, nullptr), "device keys generate");
    check(publicKeys(alice, &alicePublic, nullptr), "public keys derive");
    QByteArray sealed, opened;
    check(sealToDevice(alicePublic, "t", "hello", &sealed, nullptr), "seal to a device");
    check(openFromDevice(alice, "t", sealed, &opened, nullptr) && opened == "hello", "the device opens it");
    check(!openFromDevice(bob, "t", sealed, &opened, nullptr), "another device cannot");
    check(!openFromDevice(alice, "u", sealed, &opened, nullptr), "a different label cannot");
    sealed[40] = char(sealed.at(40) ^ 1);
    check(!openFromDevice(alice, "t", sealed, &opened, nullptr), "a changed ciphertext is refused");

    qint64 previous = 0;
    bool monotonic = true, covers = true;
    for (qint64 n : {0LL, 1LL, 4096LL, 4097LL, 100000LL, 1048577LL, 50000000LL}) {
        const qint64 padded = paddedSize(n);
        monotonic = monotonic && padded >= previous;
        covers = covers && padded >= n && (n < 4096 || padded - n <= n / 8 + 1);
        previous = padded;
    }
    check(monotonic && covers, "padding grows with size and stays within an eighth");
}

QByteArray readContainer(const QString &path)
{
    QFile file(path);
    file.open(QIODevice::ReadOnly);
    return file.readAll();
}

void writeContainer(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    file.open(QIODevice::WriteOnly | QIODevice::Truncate);
    file.write(bytes);
}

void patchContainer(const QString &path, qint64 offset, const QByteArray &bytes)
{
    QFile file(path);
    file.open(QIODevice::ReadWrite);
    file.seek(offset);
    file.write(bytes);
}

bool onlyOneOpaqueFile(const QString &folder, const QString &container, const QList<QByteArray> &needles)
{
    const QFileInfoList entries = QDir(folder).entryInfoList(QDir::Files | QDir::Hidden);
    if (entries.size() != 1 || entries.first().absoluteFilePath() != QFileInfo(container).absoluteFilePath())
        return false;
    const QByteArray bytes = readContainer(container);
    if (bytes.size() % kPageBytes != 0)
        return false;
    for (const QByteArray &needle : needles)
        if (bytes.contains(needle))
            return false;
    return true;
}

void driveFlow()
{
    Drive::setLeaseTiming({0, 300, 1});
    QTemporaryDir root;
    const QString folder = root.filePath(QStringLiteral("router-usb"));
    QDir().mkpath(folder);
    const QString container = QDir(folder).filePath(QStringLiteral("Shared.fmsd"));

    LocalState alice(root.filePath(QStringLiteral("alice")));
    Failure failure;
    check(alice.create(kPass, QStringLiteral("Office laptop"), &failure), "alice sets up her computer");
    check(!LocalState(root.filePath(QStringLiteral("x"))).create(QStringLiteral("short"), {}, &failure)
              && failure.code == QLatin1String("passphrase_too_short"),
          "a short passphrase is refused");
    QString aliceId;
    check(Drive::create(&alice, container, QStringLiteral("Team share"), &aliceId, &failure), "alice creates a drive");
    Drive aliceDrive(&alice, aliceId);
    check(aliceDrive.scan(&failure), "alice scans the drive");
    check(hasAction(aliceDrive, "drive_created") && hasAction(aliceDrive, "drive_opened"),
          "creation and connection are logged");
    QString again;
    check(!Drive::create(&alice, container, {}, &again, &failure), "an existing file is never overwritten");
    const qint64 emptySize = QFileInfo(container).size();

    const QByteArray letter = "<html><body>Quarterly secret plan</body></html>";
    QString letterId;
    check(add(aliceDrive, QStringLiteral("plan.html"), letter, &letterId), "alice adds a document");
    check(readAll(aliceDrive, letterId) == letter, "alice reads it back");
    check(QFileInfo(container).size() == emptySize, "a small file does not change the drive's size");
    QByteArray big(3 * 1024 * 1024 + 7, '\0');
    for (int i = 0; i < big.size(); ++i)
        big[i] = char((i * 31) & 0xff);
    QString bigId;
    check(add(aliceDrive, QStringLiteral("photo.raw"), big, &bigId), "alice adds a large file");
    check(readAll(aliceDrive, bigId) == big, "a multi-page file round-trips");
    check(QFileInfo(container).size() > emptySize && QFileInfo(container).size() % kPageBytes == 0,
          "the drive grows in whole pages when it must");
    QString emptyId;
    check(add(aliceDrive, QStringLiteral("empty.txt"), QByteArray(), &emptyId), "an empty file is stored");
    check(readAll(aliceDrive, emptyId).isEmpty(), "and reads back empty");
    check(aliceDrive.files().size() == 3, "three files are listed");
    const FileEntry *planEntry = aliceDrive.file(letterId);
    check(planEntry && planEntry->kind == QLatin1String("document") && planEntry->opens == 1,
          "the document is typed and its open counted");
    check(onlyOneOpaqueFile(folder, container, {"Quarterly", "plan.html", "Team share", "Office laptop", "photo.raw"}),
          "the share holds one opaque file and nothing readable");

    LocalState bob(root.filePath(QStringLiteral("bob")));
    check(bob.create(kPass, QStringLiteral("Front desk"), &failure), "bob sets up his phone");
    QString code, inviteId;
    check(aliceDrive.createInvite(&code, &inviteId, &failure), "alice creates an invitation");
    QString bobId, bobSafety;
    check(Drive::requestJoin(&bob, container, code, &bobId, &bobSafety, &failure), "bob asks to join");
    Drive bobDrive(&bob, bobId);
    check(bobDrive.scan(&failure) && bob.membership(bobId)->isPending(), "bob waits for approval");
    check(bobDrive.files().isEmpty(), "bob sees nothing before approval");
    check(QDir(folder).entryList(QDir::Files).size() == 1, "a join request adds no file to the share");

    check(aliceDrive.scan(&failure) && aliceDrive.requests().size() == 1, "alice sees bob's request");
    check(aliceDrive.requests().value(0).safety == bobSafety, "both screens show the same safety number");
    check(aliceDrive.requests().value(0).name == QLatin1String("Front desk"), "the request names bob's device");
    check(aliceDrive.approve(aliceDrive.requests().value(0).mailbox, &failure), "alice approves bob");

    check(bobDrive.scan(&failure) && bob.membership(bobId)->isMember(), "bob becomes a member");
    check(bobDrive.files().size() == 3, "bob sees the files");
    check(bob.membership(bobId)->label == QLatin1String("Team share"), "bob learns the drive's name");
    check(readAll(bobDrive, letterId) == letter, "bob opens alice's document");
    check(bobDrive.devices().size() == 2, "the roster lists both devices");
    const QByteArray edited = "<html><body>Revised plan</body></html>";
    check(save(bobDrive, letterId, edited), "bob saves a new version");
    check(aliceDrive.scan(&failure) && readAll(aliceDrive, letterId) == edited, "alice sees bob's version");
    check(aliceDrive.file(letterId)->versions.size() == 2, "the earlier version is kept");
    check(readAll(aliceDrive, letterId, 0) == letter, "the earlier version still opens");
    check(aliceDrive.issues().isEmpty(), "an honest drive reports no problems");

    bool sawBobOpen = false;
    for (const Record &record : aliceDrive.records())
        if (record.device == bob.deviceId && record.action == QLatin1String("file_opened"))
            sawBobOpen = true;
    check(sawBobOpen, "alice's log shows bob opening the document");
    check(aliceDrive.activityCsv().contains(QStringLiteral("Front desk")), "the activity export names devices");

    LocalState mallory(root.filePath(QStringLiteral("mallory")));
    mallory.create(kPass, QStringLiteral("Stranger"), &failure);
    QByteArray otherSecret;
    QString wrongCode = newInviteCode(&otherSecret);
    while (aliceDrive.mailboxesInUse().contains(mailboxForInvite(otherSecret)))
        wrongCode = newInviteCode(&otherSecret);
    QString malloryId;
    Drive::requestJoin(&mallory, container, wrongCode, &malloryId, nullptr, &failure);
    check(aliceDrive.scan(&failure) && aliceDrive.requests().isEmpty(), "a request without a valid code is invisible");
    QString usedId;
    check(!Drive::requestJoin(&mallory, container, code, &usedId, nullptr, &failure),
          "the same drive cannot be joined twice from one device");

    bobDrive.scan(&failure);
    const QString bobStarted = bobDrive.file(letterId)->current()->blob;
    check(save(aliceDrive, letterId, "<p>alice</p>") && save(bobDrive, letterId, "<p>bob</p>", bobStarted),
          "both devices save edits they started from the same version");
    check(aliceDrive.scan(&failure) && aliceDrive.file(letterId)->conflicts == 1,
          "a save made without seeing the other is counted as a conflict and both versions are kept");

    {
        Drive::setLeaseTiming({15, 3000, 2});
        Drive aliceWriter(&alice, aliceId);
        Drive bobWriter(&bob, bobId);
        aliceWriter.scan(&failure);
        bobWriter.scan(&failure);
        bool aliceOk = true;
        std::thread other([&] {
            Failure f;
            for (int i = 0; i < 8; ++i)
                aliceOk = aliceWriter.note(QStringLiteral("log_exported"), {}, &f) && aliceOk;
        });
        bool bobOk = true;
        for (int i = 0; i < 8; ++i)
            bobOk = bobWriter.note(QStringLiteral("log_exported"), {}, &failure) && bobOk;
        other.join();
        Drive::setLeaseTiming({0, 300, 1});
        check(aliceOk && bobOk, "two devices writing at the same time both succeed");
        int exports = 0;
        check(aliceDrive.scan(&failure), "the drive reads after concurrent writes");
        for (const Record &record : aliceDrive.records())
            exports += record.action == QLatin1String("log_exported") ? 1 : 0;
        check(exports == 16 && aliceDrive.issues().isEmpty(), "the lease keeps every record from both devices");
    }

    patchContainer(container, kMailboxPage * kPageBytes + qint64(bob.membership(bobId)->mailbox) * kSlotBytes,
                   randomBytes(int(kSlotBytes)));
    check(bobDrive.scan(&failure) && hasIssue(bobDrive, "keyring_restored") && !bobDrive.removedFromDrive(),
          "an overwritten key slot does not lock a member out");
    check(bobDrive.note(QStringLiteral("log_exported"), {}, &failure) && bobDrive.scan(&failure)
              && !hasIssue(bobDrive, "keyring_restored"),
          "the member writes its key slot back");

    QString tamperId;
    add(aliceDrive, QStringLiteral("ledger.csv"), "a,b\n1,2\n", &tamperId);
    {
        const qint64 page = aliceDrive.file(tamperId)->current()->pages.first().start;
        const QByteArray bytes = readContainer(container).mid(int(page * kPageBytes + 20), 1);
        patchContainer(container, page * kPageBytes + 20, QByteArray(1, char(bytes.at(0) ^ 0x40)));
    }
    Failure readFailure;
    check(readAll(aliceDrive, tamperId, -1, &readFailure) == "<failed>"
              && readFailure.code == QLatin1String("content_damaged"),
          "a changed byte in a file's ciphertext is refused");
    check(hasAction(aliceDrive, "integrity_alert"), "the refusal is logged");

    const QByteArray beforeBob = readContainer(container);
    check(bobDrive.scan(&failure) && bobDrive.note(QStringLiteral("log_exported"), {}, &failure), "bob writes another record");
    check(aliceDrive.scan(&failure), "alice sees it");
    writeContainer(container, beforeBob);
    check(aliceDrive.scan(&failure) && (hasIssue(aliceDrive, "index_restored") || hasIssue(aliceDrive, "rolled_back")),
          "putting back an old copy of the drive file is detected");
    check(bobDrive.scan(&failure) && (hasIssue(bobDrive, "own_records_missing") || hasIssue(bobDrive, "index_restored")),
          "bob notices too");

    {
        const QByteArray bytes = readContainer(container);
        patchContainer(container, kRootAPage * kPageBytes, randomBytes(int(kRootPages * kPageBytes)));
        patchContainer(container, kRootBPage * kPageBytes, randomBytes(int(kRootPages * kPageBytes)));
        check(aliceDrive.scan(&failure) && hasIssue(aliceDrive, "index_restored") && aliceDrive.files().size() >= 3,
              "a destroyed index is rebuilt from this device's verified copy");
        check(aliceDrive.note(QStringLiteral("log_exported"), {}, &failure), "the next change writes it back");
        check(bobDrive.scan(&failure) && bobDrive.files().size() >= 3, "and the other device reads the repaired drive");
        Q_UNUSED(bytes);
    }

    alice.lock();
    check(!alice.isUnlocked() && alice.drives.isEmpty(), "locking forgets every key");
    check(!alice.unlock(QStringLiteral("wrong passphrase!"), &failure) && failure.code == QLatin1String("wrong_passphrase"),
          "a wrong passphrase does not unlock");
    check(alice.unlock(kPass, &failure) && alice.drives.size() == 1, "the right passphrase unlocks the drive list");
    check(alice.changePassphrase(kPass, QStringLiteral("another long passphrase"), &failure),
          "the passphrase can be changed");
    alice.lock();
    check(alice.unlock(QStringLiteral("another long passphrase"), &failure), "the new passphrase unlocks");
    Drive aliceAgain(&alice, aliceId);
    check(aliceAgain.scan(&failure), "alice reconnects");

    LocalState carol(root.filePath(QStringLiteral("carol")));
    carol.create(kPass, QStringLiteral("Spare tablet"), &failure);
    QString carolCode, carolId;
    aliceAgain.createInvite(&carolCode, nullptr, &failure);
    Drive::requestJoin(&carol, container, carolCode, &carolId, nullptr, &failure);
    aliceAgain.scan(&failure);
    check(aliceAgain.requests().size() == 1 && aliceAgain.approve(aliceAgain.requests().value(0).mailbox, &failure),
          "alice approves carol");
    Drive carolDrive(&carol, carolId);
    check(carolDrive.scan(&failure) && readAll(carolDrive, bigId) == big, "carol reads the large file");
    const QList<Run> oldPages = aliceAgain.file(bigId)->current()->pages;
    const QByteArray oldFirst = readContainer(container).mid(int(oldPages.first().start * kPageBytes), int(kPageBytes));

    aliceAgain.scan(&failure);
    const qsizetype issuesBefore = aliceAgain.issues().size();
    int progressCalls = 0;
    check(aliceAgain.removeDevice(carol.deviceId, [&progressCalls](int, int) { ++progressCalls; }, &failure),
          "alice removes carol");
    check(progressCalls > 0, "re-encryption reports progress");
    check(readContainer(container).mid(int(oldPages.first().start * kPageBytes), int(kPageBytes)) != oldFirst,
          "content under the old key is overwritten");
    check(aliceAgain.scan(&failure) && readAll(aliceAgain, bigId) == big, "alice still reads it");
    check(bobDrive.scan(&failure) && readAll(bobDrive, bigId) == big, "bob picks up the new key");
    check(bob.membership(bobId)->epochs.size() == 2, "bob holds both keys");
    check(!carolDrive.scan(&failure) || readAll(carolDrive, bigId) == "<failed>",
          "carol cannot open anything stored after her removal");
    check(!carolDrive.note(QStringLiteral("drive_opened"), {}, &failure), "carol can no longer write records");
    check(aliceAgain.scan(&failure) && aliceAgain.issues().size() == issuesBefore, "carol's attempts leave the drive intact");
    bool removed = false;
    for (const DeviceInfo &device : aliceAgain.devices())
        if (device.id == carol.deviceId)
            removed = device.removed;
    check(removed, "the roster marks carol removed");

    check(aliceAgain.renameFile(letterId, QStringLiteral("plan-v2.html"), &failure), "rename");
    check(aliceAgain.file(letterId)->name == QLatin1String("plan-v2.html"), "the new name shows");
    check(aliceAgain.restoreVersion(letterId, 0, &failure) && readAll(aliceAgain, letterId) == letter,
          "an older version can be restored");
    const QList<Run> letterPages = aliceAgain.file(letterId)->current()->pages;
    const QByteArray letterBefore = readContainer(container).mid(int(letterPages.first().start * kPageBytes), int(kPageBytes));
    check(aliceAgain.deleteFile(letterId, &failure) && !aliceAgain.file(letterId), "delete");
    check(readContainer(container).mid(int(letterPages.first().start * kPageBytes), int(kPageBytes)) != letterBefore,
          "a deleted file's pages are overwritten");
    check(onlyOneOpaqueFile(folder, container, {"Quarterly", "Revised", "plan-v2", "Spare tablet", "Front desk"}),
          "after all of that the share still holds one opaque file");
}

void removalWithoutFiles()
{
    Drive::setLeaseTiming({0, 300, 1});
    QTemporaryDir root;
    const QString container = root.filePath(QStringLiteral("Empty.fmsd"));
    Failure failure;
    LocalState alice(root.filePath(QStringLiteral("alice")));
    LocalState carol(root.filePath(QStringLiteral("carol")));
    alice.create(kPass, QStringLiteral("Laptop"), &failure);
    carol.create(kPass, QStringLiteral("Tablet"), &failure);
    QString aliceId, carolId, code;
    Drive::create(&alice, container, QStringLiteral("Empty"), &aliceId, &failure);
    Drive a(&alice, aliceId);
    a.scan(&failure);
    a.createInvite(&code, nullptr, &failure);
    Drive::requestJoin(&carol, container, code, &carolId, nullptr, &failure);
    a.scan(&failure);
    a.approve(a.requests().value(0).mailbox, &failure);
    Drive c(&carol, carolId);
    check(c.scan(&failure) && carol.membership(carolId)->isMember(), "carol joins an empty drive");
    check(a.scan(&failure) && a.removeDevice(carol.deviceId, {}, &failure), "alice removes her with no files to re-encrypt");
    c.scan(&failure);
    check(c.removedFromDrive() && hasIssue(c, "keyring_missing"), "no index under the old key is left for her to read");
    check(!c.note(QStringLiteral("log_exported"), {}, &failure), "so she cannot write");
    check(a.scan(&failure) && a.issues().isEmpty(), "and the drive stays clean");
}

void sheets()
{
    Sheet sheet = Sheet::fromCsv("Item,Cost\r\nPaper,4\r\nInk,\"1,5\"\r\nTotal,=SUM(B2:B3)\r\n");
    check(sheet.rows() == 4 && sheet.columns() == 2, "a CSV sheet loads");
    check(sheet.raw(2, 1) == QLatin1String("1,5"), "quoted commas survive");
    check(sheet.display(3, 1) == QLatin1String("4"), "SUM skips text");
    sheet.setRaw(2, 1, QStringLiteral("1.5"));
    check(sheet.display(3, 1) == QLatin1String("5.5"), "SUM adds numbers");
    sheet.setRaw(0, 3, QStringLiteral("=B2*2+B3^2"));
    check(sheet.display(0, 3) == QLatin1String("10.25"), "arithmetic precedence");
    sheet.setRaw(1, 3, QStringLiteral("=AVERAGE(B2:B3)"));
    check(sheet.display(1, 3) == QLatin1String("2.75"), "AVERAGE");
    sheet.setRaw(2, 3, QStringLiteral("=B2/0"));
    check(sheet.display(2, 3) == QLatin1String("#DIV/0!"), "division by zero");
    sheet.setRaw(3, 3, QStringLiteral("=D5"));
    sheet.setRaw(4, 3, QStringLiteral("=D4+1"));
    check(sheet.display(3, 3) == QLatin1String("#CYCLE!"), "a cycle is caught");
    sheet.setRaw(5, 0, QStringLiteral("=IF(B2>3,\"big\",\"small\")"));
    check(sheet.display(5, 0) == QLatin1String("big"), "IF with text");
    sheet.setRaw(5, 1, QStringLiteral("=ROUND(10/3,2)"));
    check(sheet.display(5, 1) == QLatin1String("3.33"), "ROUND");
    sheet.setRaw(5, 2, QStringLiteral("=NOPE(1)"));
    check(sheet.display(5, 2) == QLatin1String("#NAME?"), "unknown functions");
    sheet.setRaw(6, 0, QStringLiteral("line one\nline \"two\""));
    const Sheet again = Sheet::fromCsv(sheet.toCsv());
    check(again.raw(6, 0) == sheet.raw(6, 0) && again.raw(3, 1) == sheet.raw(3, 1), "CSV round-trips");
    int row = 0, column = 0;
    check(Sheet::parseCell(QStringLiteral("AB12"), &row, &column) && row == 11 && column == 27, "cell names");
    check(Sheet::columnName(27) == QLatin1String("AB"), "column names");
}

void drawings()
{
    Drawing drawing;
    drawing.width = 800;
    drawing.height = 600;
    drawing.background = QStringLiteral("#fafafa");
    Stroke stroke;
    stroke.color = QStringLiteral("#0969da");
    stroke.width = 5;
    stroke.points = {QPointF(10, 20), QPointF(30.5, 40), QPointF(50, 60)};
    drawing.strokes = {stroke, Stroke{QStringLiteral("#cf222e"), 2, {QPointF(5, 5)}}};
    Drawing parsed;
    check(Drawing::fromSvg(drawing.toSvg(), &parsed), "a drawing parses back");
    check(parsed.width == 800 && parsed.background == QLatin1String("#fafafa") && parsed.strokes.size() == 2,
          "size, background and strokes survive");
    check(parsed.strokes.first().points.size() == 3 && parsed.strokes.first().color == QLatin1String("#0969da")
              && parsed.strokes.at(1).points.size() == 1,
          "stroke points and colours survive");
    check(!Drawing::fromSvg("<svg xmlns=\"http://www.w3.org/2000/svg\"><path d=\"M0 0 L1 1\"/></svg>", &parsed),
          "an SVG this editor did not write opens as a plain file instead");
}

QString fixturesDir()
{
    const QByteArray overridden = qgetenv("FORKMESH_SECURE_DRIVE_FIXTURES");
    if (!overridden.isEmpty())
        return QString::fromLocal8Bit(overridden);
    const QString beside = QDir(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath()
                                + QStringLiteral("/../../../tests/fixtures")).canonicalPath();
    if (!beside.isEmpty())
        return beside;
    for (QDir dir = QDir::current();;) {
        const QString candidate = dir.filePath(QStringLiteral("marketplace/packages/secure-drive/tests/fixtures"));
        if (QFileInfo(candidate).isDir())
            return candidate;
        if (!dir.cdUp())
            return {};
    }
}

QJsonObject readJsonFile(const QString &path)
{
    QFile file(path);
    file.open(QIODevice::ReadOnly);
    return QJsonDocument::fromJson(file.readAll()).object();
}

QJsonObject deviceJson(const LocalState &state, const QString &driveId, const QString &containerName)
{
    QJsonObject device = state.exportDevice();
    QJsonArray drives;
    for (const QJsonValue &value : device.value(QStringLiteral("drives")).toArray()) {
        QJsonObject membership = value.toObject();
        if (membership.value(QStringLiteral("id")).toString() != driveId)
            continue;
        membership.insert(QStringLiteral("container"), containerName);
        drives.append(membership);
    }
    device.insert(QStringLiteral("drives"), drives);
    return device;
}

void zeroFreeSpace(Drive &drive, const QString &container)
{
    QFile file(container);
    file.open(QIODevice::ReadWrite);
    const qint64 pages = file.size() / kPageBytes;
    QVector<bool> used(int(pages), false);
    for (const Run &run : drive.dataRunsInUse())
        for (qint64 i = 0; i < run.count; ++i)
            used[int(run.start + i)] = true;
    const QByteArray zeroPage(int(kPageBytes), '\0');
    file.seek(kLeasePage * kPageBytes);
    file.write(zeroPage);
    const QList<int> mailboxes = drive.mailboxesInUse();
    for (int slot = 0; slot < kMailboxSlots; ++slot)
        if (!mailboxes.contains(slot)) {
            file.seek(kMailboxPage * kPageBytes + qint64(slot) * kSlotBytes);
            file.write(QByteArray(int(kSlotBytes), '\0'));
        }
    for (qint64 page = kDataPage; page < pages; ++page)
        if (!used[int(page)]) {
            file.seek(page * kPageBytes);
            file.write(zeroPage);
        }
}

QByteArray pattern(int size, int seed)
{
    QByteArray bytes(size, '\0');
    for (int i = 0; i < size; ++i)
        bytes[i] = char((i * 131 + seed * 17 + (i >> 8)) & 0xff);
    return bytes;
}

int writeFixture(const QString &directory)
{
    Drive::setLeaseTiming({0, 300, 1});
    Drive::setInviteLifetime(qint64(100) * 365 * 24 * 3600 * 1000);
    QDir().mkpath(directory);
    const QString name = QStringLiteral("interop-from-cpp");
    const QString container = QDir(directory).filePath(name + QStringLiteral(".fmsd"));
    QFile::remove(container);
    QTemporaryDir scratch;
    Failure failure;
    LocalState alice(scratch.filePath(QStringLiteral("alice")));
    LocalState bob(scratch.filePath(QStringLiteral("bob")));
    LocalState carol(scratch.filePath(QStringLiteral("carol")));
    alice.create(kPass, QStringLiteral("Office laptop"), &failure);
    bob.create(kPass, QStringLiteral("Front desk phone"), &failure);
    carol.create(kPass, QStringLiteral("Spare tablet"), &failure);
    QString aliceId;
    if (!Drive::create(&alice, container, QStringLiteral("Interop share"), &aliceId, &failure)) {
        std::fprintf(stderr, "create: %s\n", qPrintable(failure.code));
        return 1;
    }
    Drive a(&alice, aliceId);
    a.scan(&failure);
    QString notes, budget, sketch, empty, photo, old;
    add(a, QStringLiteral("Notes.html"), "<html><body><h1>Notes</h1><p>First draft</p></body></html>", &notes);
    add(a, QStringLiteral("Budget.csv"), "Item,Cost\r\nPaper,4.5\r\nInk,19.99\r\nTotal,\"=SUM(B2:B3)\"\r\n", &budget);
    Drawing drawing;
    drawing.strokes = {Stroke{QStringLiteral("#0969da"), 6, {QPointF(100, 100), QPointF(500, 100), QPointF(500, 400)}}};
    add(a, QStringLiteral("Sketch.svg"), drawing.toSvg(), &sketch);
    add(a, QStringLiteral("empty.txt"), QByteArray(), &empty);
    add(a, QStringLiteral("photo.bin"), pattern(40000, 3), &photo);
    add(a, QStringLiteral("old.txt"), "to be deleted", &old);
    a.renameFile(budget, QStringLiteral("Budget 2027.csv"), &failure);
    a.deleteFile(old, &failure);

    QString code;
    a.createInvite(&code, nullptr, &failure);
    QString bobId;
    Drive::requestJoin(&bob, container, code, &bobId, nullptr, &failure);
    a.scan(&failure);
    a.approve(a.requests().value(0).mailbox, &failure);
    Drive b(&bob, bobId);
    b.scan(&failure);
    save(b, notes, "<html><body><h1>Notes</h1><p>Second draft from the phone</p></body></html>");
    readAll(b, sketch);

    QString carolCode, carolId, carolSafety;
    a.scan(&failure);
    a.createInvite(&carolCode, nullptr, &failure);
    Drive::requestJoin(&carol, container, carolCode, &carolId, &carolSafety, &failure);
    QString openCode;
    a.scan(&failure);
    a.createInvite(&openCode, nullptr, &failure);
    a.scan(&failure);
    b.scan(&failure);
    if (!a.issues().isEmpty() || !b.issues().isEmpty() || a.requests().size() != 1) {
        std::fprintf(stderr, "fixture drive is not clean\n");
        return 1;
    }
    zeroFreeSpace(a, container);
    a.scan(&failure);
    b.scan(&failure);
    if (!a.issues().isEmpty() || !b.issues().isEmpty()) {
        std::fprintf(stderr, "zeroing free space damaged the fixture\n");
        return 1;
    }

    QJsonArray files;
    for (const FileEntry &entry : a.files()) {
        const FileVersion *v = entry.current();
        files.append(QJsonObject{{QStringLiteral("id"), entry.id},
                                 {QStringLiteral("name"), entry.name},
                                 {QStringLiteral("kind"), entry.kind},
                                 {QStringLiteral("versions"), int(entry.versions.size())},
                                 {QStringLiteral("size"), double(v->size)},
                                 {QStringLiteral("sha256"), v->sha256}});
    }
    QJsonObject safety;
    for (const DeviceInfo &device : a.devices())
        safety.insert(device.id, a.deviceSafetyNumber(device.id));
    const JoinRequest request = a.requests().value(0);
    const QString driveId = alice.membership(aliceId)->driveId;
    QJsonObject fixture{
        {QStringLiteral("format"), QStringLiteral("forkmesh-secure-drive-interop-v1")},
        {QStringLiteral("writer"), QStringLiteral("cpp")},
        {QStringLiteral("container"), name + QStringLiteral(".fmsd")},
        {QStringLiteral("passphrase"), kPass},
        {QStringLiteral("drive"), driveId},
        {QStringLiteral("label"), QStringLiteral("Interop share")},
        {QStringLiteral("members"),
         QJsonArray{deviceJson(alice, aliceId, name + QStringLiteral(".fmsd")),
                    deviceJson(bob, bobId, name + QStringLiteral(".fmsd"))}},
        {QStringLiteral("pending"), deviceJson(carol, carolId, name + QStringLiteral(".fmsd"))},
        {QStringLiteral("joinRequest"),
         QJsonObject{{QStringLiteral("mailbox"), request.mailbox},
                     {QStringLiteral("name"), request.name},
                     {QStringLiteral("safety"), request.safety}}},
        {QStringLiteral("openInviteCode"), openCode},
        {QStringLiteral("files"), files},
        {QStringLiteral("deletedFiles"), QJsonArray{QStringLiteral("old.txt")}},
        {QStringLiteral("minimumRecords"), int(a.records().size())},
        {QStringLiteral("deviceSafety"), safety}};
    QSaveFile out(QDir(directory).filePath(name + QStringLiteral(".json")));
    out.open(QIODevice::WriteOnly);
    out.write(QJsonDocument(fixture).toJson(QJsonDocument::Indented));
    out.commit();
    std::printf("wrote %s\n", qPrintable(QDir(directory).filePath(name + QStringLiteral(".json"))));
    return 0;
}

void readsFixture(const QString &path)
{
    const QJsonObject fixture = readJsonFile(path);
    const QString writer = fixture.value(QStringLiteral("writer")).toString();
    const QByteArray label = ("fixture from " + writer).toUtf8();
    const auto checkF = [&label](bool ok, const char *what) {
        const QByteArray text = label + ": " + what;
        check(ok, text.constData());
    };
    checkF(fixture.value(QStringLiteral("format")).toString() == QLatin1String("forkmesh-secure-drive-interop-v1"),
           "format");
    QTemporaryDir scratch;
    const QString container = scratch.filePath(QStringLiteral("drive.fmsd"));
    QFile::copy(QFileInfo(path).dir().filePath(fixture.value(QStringLiteral("container")).toString()), container);
    QFile::setPermissions(container, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    const QString pass = fixture.value(QStringLiteral("passphrase")).toString();
    const auto withContainer = [&container](QJsonObject device) {
        QJsonArray drives;
        for (const QJsonValue &value : device.value(QStringLiteral("drives")).toArray()) {
            QJsonObject membership = value.toObject();
            membership.insert(QStringLiteral("container"), container);
            drives.append(membership);
        }
        device.insert(QStringLiteral("drives"), drives);
        return device;
    };
    const QJsonArray members = fixture.value(QStringLiteral("members")).toArray();
    QList<std::shared_ptr<LocalState>> states;
    QList<std::shared_ptr<Drive>> drives;
    for (int i = 0; i < members.size(); ++i) {
        auto state = std::make_shared<LocalState>(scratch.filePath(QStringLiteral("member%1").arg(i)));
        Failure failure;
        checkF(state->importDevice(withContainer(members.at(i).toObject()), pass, &failure), "imports a member");
        if (state->drives.isEmpty())
            return;
        auto drive = std::make_shared<Drive>(state.get(), state->drives.first().id);
        checkF(drive->scan(&failure), "a member opens the drive");
        checkF(drive->issues().isEmpty(), "the drive verifies with no problems");
        checkF(drive->records().size() >= fixture.value(QStringLiteral("minimumRecords")).toInt(), "every record is read");
        checkF(state->drives.first().label == fixture.value(QStringLiteral("label")).toString(), "the drive name");
        const QJsonArray expected = fixture.value(QStringLiteral("files")).toArray();
        checkF(drive->files().size() == expected.size(), "the file list");
        for (const QJsonValue &value : expected) {
            const QJsonObject want = value.toObject();
            const FileEntry *entry = drive->file(want.value(QStringLiteral("id")).toString());
            checkF(entry && entry->name == want.value(QStringLiteral("name")).toString()
                       && entry->kind == want.value(QStringLiteral("kind")).toString()
                       && entry->versions.size() == want.value(QStringLiteral("versions")).toInt(),
                   "a file's name, kind and versions");
            if (!entry)
                continue;
            const QByteArray content = readAll(*drive, entry->id);
            checkF(QString::fromLatin1(QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex())
                       == want.value(QStringLiteral("sha256")).toString(),
                   "a file's content decrypts to the written bytes");
        }
        const QJsonObject safety = fixture.value(QStringLiteral("deviceSafety")).toObject();
        for (auto it = safety.constBegin(); it != safety.constEnd(); ++it)
            checkF(drive->deviceSafetyNumber(it.key()) == it.value().toString(), "safety numbers agree");
        states.append(state);
        drives.append(drive);
    }
    if (drives.size() < 2)
        return;
    Failure failure;
    const QJsonObject request = fixture.value(QStringLiteral("joinRequest")).toObject();
    drives.first()->scan(&failure);
    const QList<JoinRequest> requests = drives.first()->requests();
    checkF(requests.size() == 1 && requests.first().mailbox == request.value(QStringLiteral("mailbox")).toInt()
               && requests.first().safety == request.value(QStringLiteral("safety")).toString()
               && requests.first().name == request.value(QStringLiteral("name")).toString(),
           "the pending join request and its safety number");
    QString added;
    checkF(add(*drives.at(1), QStringLiteral("from-cpp.txt"), "written by the desktop", &added), "a member adds a file");
    const QString firstId = fixture.value(QStringLiteral("files")).toArray().first().toObject().value(QStringLiteral("id")).toString();
    checkF(save(*drives.at(1), firstId, "changed by the desktop"), "a member saves a version");
    checkF(drives.first()->approve(request.value(QStringLiteral("mailbox")).toInt(), &failure), "the pending device is approved");
    auto pendingState = std::make_shared<LocalState>(scratch.filePath(QStringLiteral("pending")));
    checkF(pendingState->importDevice(withContainer(fixture.value(QStringLiteral("pending")).toObject()), pass, &failure),
           "imports the pending device");
    if (pendingState->drives.isEmpty())
        return;
    Drive pending(pendingState.get(), pendingState->drives.first().id);
    checkF(pending.scan(&failure) && pendingState->drives.first().isMember(), "the pending device joins");
    checkF(readAll(pending, added) == "written by the desktop" && readAll(pending, firstId) == "changed by the desktop",
           "it reads what was written");
    checkF(drives.first()->scan(&failure) && drives.first()->issues().isEmpty() && pending.issues().isEmpty(),
           "the drive still verifies after writes from this implementation");
}

void readsEveryFixture()
{
    const QString dir = fixturesDir();
    const QStringList found = QDir(dir).entryList({QStringLiteral("interop-from-*.json")}, QDir::Files);
    check(found.contains(QStringLiteral("interop-from-cpp.json")), "the desktop's own fixture is present");
    for (const QString &name : found)
        readsFixture(QDir(dir).filePath(name));
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    Failure failure;
    if (!cryptoAvailable(&failure)) {
        std::printf("skipped: %s\n", qPrintable(failure.code));
        return 0;
    }
    if (argc == 3 && QByteArray(argv[1]) == "--write-fixture")
        return writeFixture(QString::fromLocal8Bit(argv[2]));
    cryptoPrimitives();
    driveFlow();
    removalWithoutFiles();
    sheets();
    drawings();
    readsEveryFixture();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
