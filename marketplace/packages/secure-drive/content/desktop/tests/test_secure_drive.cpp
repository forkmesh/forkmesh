#include "SecureDriveCrypto.h"
#include "SecureDriveFormats.h"
#include "SecureDriveStore.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cstdio>

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

bool save(Drive &drive, const QString &id, const QByteArray &bytes)
{
    QByteArray copy = bytes;
    QBuffer buffer(&copy);
    buffer.open(QIODevice::ReadOnly);
    Failure failure;
    return drive.saveFile(id, &buffer, &failure);
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

bool folderLeaks(const QString &folder, const QList<QByteArray> &needles)
{
    for (const QFileInfo &entry : QDir(folder).entryInfoList(QDir::Files | QDir::Hidden)) {
        if (!entry.fileName().endsWith(QLatin1String(".fmsd")))
            return true;
        QFile file(entry.filePath());
        file.open(QIODevice::ReadOnly);
        const QByteArray bytes = file.readAll();
        for (const QByteArray &needle : needles)
            if (bytes.contains(needle))
                return true;
    }
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

void driveFlow()
{
    QTemporaryDir root;
    const QString folder = root.filePath(QStringLiteral("router-usb"));
    QDir().mkpath(folder);

    LocalState alice(root.filePath(QStringLiteral("alice")));
    Failure failure;
    check(alice.create(kPass, QStringLiteral("Office laptop"), &failure), "alice sets up her computer");
    check(!LocalState(root.filePath(QStringLiteral("x"))).create(QStringLiteral("short"), {}, &failure)
              && failure.code == QLatin1String("passphrase_too_short"),
          "a short passphrase is refused");
    QString aliceSlot;
    check(Drive::create(&alice, folder, QStringLiteral("Team share"), &aliceSlot, &failure), "alice creates a drive");
    Drive aliceDrive(&alice, aliceSlot);
    check(aliceDrive.scan(&failure), "alice scans the drive");
    check(hasAction(aliceDrive, "drive_created") && hasAction(aliceDrive, "drive_opened"),
          "creation and connection are logged");
    QString again;
    check(!Drive::create(&alice, folder, {}, &again, &failure), "the same folder is not created twice");

    const QByteArray letter = "<html><body>Quarterly secret plan</body></html>";
    QString letterId;
    check(add(aliceDrive, QStringLiteral("plan.html"), letter, &letterId), "alice adds a document");
    check(readAll(aliceDrive, letterId) == letter, "alice reads it back");
    QByteArray big(3 * 1024 * 1024 + 7, '\0');
    for (int i = 0; i < big.size(); ++i)
        big[i] = char((i * 31) & 0xff);
    QString bigId;
    check(add(aliceDrive, QStringLiteral("photo.raw"), big, &bigId), "alice adds a large file");
    check(readAll(aliceDrive, bigId) == big, "a multi-chunk file round-trips");
    QString emptyId;
    check(add(aliceDrive, QStringLiteral("empty.txt"), QByteArray(), &emptyId), "an empty file is stored");
    check(readAll(aliceDrive, emptyId).isEmpty(), "and reads back empty");
    check(aliceDrive.files().size() == 3, "three files are listed");
    const FileEntry *planEntry = aliceDrive.file(letterId);
    check(planEntry && planEntry->kind == QLatin1String("document") && planEntry->opens == 1,
          "the document is typed and its open counted");
    check(!folderLeaks(folder, {"Quarterly", "plan.html", "Team share", "Office laptop", "photo.raw"}),
          "the drive holds only opaque encrypted objects");

    LocalState bob(root.filePath(QStringLiteral("bob")));
    check(bob.create(kPass, QStringLiteral("Front desk"), &failure), "bob sets up his computer");
    QString code, inviteId;
    check(aliceDrive.createInvite(&code, &inviteId, &failure), "alice creates an invitation");
    QString bobSlot, bobSafety;
    check(Drive::requestJoin(&bob, folder, code, &bobSlot, &bobSafety, &failure), "bob asks to join");
    Drive bobDrive(&bob, bobSlot);
    check(bobDrive.scan(&failure) && bob.membership(bobSlot)->isPending(), "bob waits for approval");
    check(bobDrive.files().isEmpty(), "bob sees nothing before approval");

    check(aliceDrive.scan(&failure) && aliceDrive.requests().size() == 1, "alice sees bob's request");
    check(aliceDrive.requests().value(0).safety == bobSafety, "both screens show the same safety number");
    check(aliceDrive.requests().value(0).name == QLatin1String("Front desk"), "the request names bob's computer");
    check(aliceDrive.approve(aliceDrive.requests().value(0).object, &failure), "alice approves bob");

    check(bobDrive.scan(&failure) && bob.membership(bobSlot)->isMember(), "bob becomes a member");
    check(bobDrive.files().size() == 3, "bob sees the files");
    check(bob.membership(bobSlot)->label == QLatin1String("Team share"), "bob learns the drive's name");
    check(readAll(bobDrive, letterId) == letter, "bob opens alice's document");
    check(bobDrive.devices().size() == 2, "the roster lists both computers");
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
    const QString wrongCode = newInviteCode(&otherSecret);
    QString mallorySlot;
    check(Drive::requestJoin(&mallory, folder, wrongCode, &mallorySlot, nullptr, &failure),
          "a stranger can drop a request");
    check(aliceDrive.scan(&failure) && aliceDrive.requests().isEmpty(), "a request without a valid code is invisible");
    QString usedSlot;
    check(Drive::requestJoin(&mallory, QDir(folder).filePath(QStringLiteral("..")) + QStringLiteral("/router-usb"),
                             code, &usedSlot, nullptr, &failure) == false,
          "the same folder cannot be joined twice from one computer");

    check(save(aliceDrive, letterId, "<p>alice</p>") && save(bobDrive, letterId, "<p>bob</p>"),
          "both computers save at once");
    check(aliceDrive.scan(&failure) && aliceDrive.file(letterId)->conflicts == 1,
          "a concurrent save is counted as a conflict and both versions are kept");

    QString tamperId;
    add(aliceDrive, QStringLiteral("ledger.csv"), "a,b\n1,2\n", &tamperId);
    {
        QFile blob(QDir(folder).filePath(aliceDrive.file(tamperId)->current()->blob));
        blob.open(QIODevice::ReadWrite);
        blob.seek(20);
        const char byte = blob.peek(1).at(0);
        blob.write(QByteArray(1, char(byte ^ 0x40)));
    }
    Failure readFailure;
    check(readAll(aliceDrive, tamperId, -1, &readFailure) == "<failed>"
              && readFailure.code == QLatin1String("content_damaged"),
          "a changed byte in a file's ciphertext is refused");
    check(hasAction(aliceDrive, "integrity_alert"), "the refusal is logged");

    {
        QFile planted(QDir(folder).filePath(randomId() + QStringLiteral(".fmsd")));
        planted.open(QIODevice::WriteOnly);
        planted.write(QByteArray("FMSD\x01\x03", 6) + randomBytes(200));
    }
    check(aliceDrive.scan(&failure) && hasIssue(aliceDrive, "unreadable_objects"), "a planted object is reported");

    QString bobSegment = bob.membership(bobSlot)->segment;
    QByteArray bobOld;
    {
        QFile segment(QDir(folder).filePath(bobSegment));
        segment.open(QIODevice::ReadOnly);
        bobOld = segment.readAll();
    }
    check(bobDrive.note(QStringLiteral("log_exported"), {}, &failure), "bob writes another record");
    check(aliceDrive.scan(&failure), "alice sees it");
    {
        QFile segment(QDir(folder).filePath(bobSegment));
        segment.open(QIODevice::WriteOnly | QIODevice::Truncate);
        segment.write(bobOld);
    }
    check(aliceDrive.scan(&failure) && hasIssue(aliceDrive, "rolled_back"), "rolling bob's log back is detected");
    check(bobDrive.scan(&failure) && hasIssue(bobDrive, "own_records_missing"), "bob notices his own records are gone");

    alice.lock();
    check(!alice.isUnlocked() && alice.drives.isEmpty(), "locking forgets every key");
    check(!alice.unlock(QStringLiteral("wrong passphrase!"), &failure) && failure.code == QLatin1String("wrong_passphrase"),
          "a wrong passphrase does not unlock");
    check(alice.unlock(kPass, &failure) && alice.drives.size() == 1, "the right passphrase unlocks the drive list");
    check(alice.changePassphrase(kPass, QStringLiteral("another long passphrase"), &failure),
          "the passphrase can be changed");
    alice.lock();
    check(alice.unlock(QStringLiteral("another long passphrase"), &failure), "the new passphrase unlocks");
    Drive aliceAgain(&alice, aliceSlot);
    check(aliceAgain.scan(&failure), "alice reconnects");

    LocalState carol(root.filePath(QStringLiteral("carol")));
    carol.create(kPass, QStringLiteral("Spare tablet"), &failure);
    QString carolCode, carolSlot;
    aliceAgain.createInvite(&carolCode, nullptr, &failure);
    Drive::requestJoin(&carol, folder, carolCode, &carolSlot, nullptr, &failure);
    aliceAgain.scan(&failure);
    check(aliceAgain.requests().size() == 1 && aliceAgain.approve(aliceAgain.requests().value(0).object, &failure),
          "alice approves carol");
    Drive carolDrive(&carol, carolSlot);
    check(carolDrive.scan(&failure) && readAll(carolDrive, bigId) == big, "carol reads the large file");

    const QString oldBlob = aliceAgain.file(bigId)->current()->blob;
    int progressCalls = 0;
    check(aliceAgain.removeDevice(carol.deviceId, [&progressCalls](int, int) { ++progressCalls; }, &failure),
          "alice removes carol");
    check(progressCalls > 0, "re-encryption reports progress");
    check(!QFile::exists(QDir(folder).filePath(oldBlob)), "content under the old key is gone");
    check(aliceAgain.scan(&failure) && readAll(aliceAgain, bigId) == big, "alice still reads it");
    check(bobDrive.scan(&failure) && readAll(bobDrive, bigId) == big, "bob picks up the new key");
    check(bob.membership(bobSlot)->epochs.size() == 2, "bob holds both keys");

    carolDrive.scan(&failure);
    check(readAll(carolDrive, bigId) == "<failed>", "carol cannot open anything stored after her removal");
    check(hasIssue(carolDrive, "unreadable_objects"), "carol cannot read the new activity");
    carolDrive.note(QStringLiteral("drive_opened"), {}, &failure);
    check(aliceAgain.scan(&failure) && hasIssue(aliceAgain, "after_removal"),
          "records carol writes after removal are ignored and reported");
    bool removed = false;
    for (const DeviceInfo &device : aliceAgain.devices())
        if (device.id == carol.deviceId)
            removed = device.removed;
    check(removed, "the roster marks carol removed");

    check(aliceAgain.renameFile(letterId, QStringLiteral("plan-v2.html"), &failure), "rename");
    check(aliceAgain.file(letterId)->name == QLatin1String("plan-v2.html"), "the new name shows");
    check(aliceAgain.restoreVersion(letterId, 0, &failure) && readAll(aliceAgain, letterId) == letter,
          "an older version can be restored");
    const QString letterBlob = aliceAgain.file(letterId)->current()->blob;
    check(aliceAgain.deleteFile(letterId, &failure) && !aliceAgain.file(letterId), "delete");
    check(!QFile::exists(QDir(folder).filePath(letterBlob)), "a deleted file's content is removed");
    check(!folderLeaks(folder, {"Quarterly", "Revised", "plan-v2", "Spare tablet", "Front desk"}),
          "after all of that the drive still holds only opaque objects");
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

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    Failure failure;
    if (!cryptoAvailable(&failure)) {
        std::printf("skipped: %s\n", qPrintable(failure.code));
        return 0;
    }
    cryptoPrimitives();
    driveFlow();
    sheets();
    drawings();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
