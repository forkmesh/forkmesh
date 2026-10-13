#include "UserAvatarStore.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <iostream>

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

const QString kLegacySetting = QStringLiteral("profile/avatarPng");

// Roughly the size of a real profile picture: big enough that leaving it in the
// ini file is visible in the file size assertions below.
QByteArray samplePng(char fill = 'a')
{
    return QByteArray("\x89PNG\r\n\x1a\n", 8) + QByteArray(100 * 1024, fill);
}

qint64 settingsFileSize()
{
    QSettings settings;
    settings.sync();
    return QFileInfo(settings.fileName()).size();
}

} // namespace

int main(int argc, char **argv)
{
    // Hermetic HOME so neither QSettings nor AppDataLocation touches the real
    // profile of whoever runs the suite.
    QTemporaryDir home;
    if (!home.isValid()) {
        std::cerr << "FAIL: temporary home directory is available\n";
        return 1;
    }
    qputenv("HOME", QFile::encodeName(home.path()));
    QStandardPaths::setTestModeEnabled(true);

    QCoreApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ForkMeshTest"));
    QCoreApplication::setApplicationName(QStringLiteral("UserAvatarStoreTest"));

    check(!UserAvatarStore::path().isEmpty(),
          "the store resolves a durable path when app data is writable");
    check(UserAvatarStore::load().isEmpty(),
          "a profile with no picture loads as empty");

    // Round trip.
    const QByteArray png = samplePng();
    check(UserAvatarStore::store(png), "storing a picture succeeds");
    check(UserAvatarStore::load() == png, "the stored picture round trips");
    check(QFileInfo::exists(UserAvatarStore::path()),
          "the picture is kept in its own file");

    // The whole point: the blob must not sit in the ini file, because QSettings
    // rewrites that file in full on every unrelated setValue() call.
    check(!QSettings().contains(kLegacySetting),
          "storing a picture leaves nothing behind in QSettings");
    const qint64 withPicture = settingsFileSize();
    check(withPicture < 16 * 1024,
          "the settings file does not grow by the size of the picture");

    // A settings write stays cheap no matter how big the picture is.
    QSettings().setValue(QStringLiteral("unrelated/key"), 1);
    check(settingsFileSize() < 16 * 1024,
          "an unrelated setting write does not drag the picture along");

    // Migration off the legacy key.
    QFile::remove(UserAvatarStore::path());
    const QByteArray legacy = samplePng('b');
    {
        QSettings settings;
        settings.setValue(kLegacySetting, legacy);
        settings.sync();
    }
    check(settingsFileSize() > 100 * 1024,
          "the legacy layout really does inflate the settings file");
    check(UserAvatarStore::load() == legacy,
          "a legacy picture is still readable after the upgrade");
    check(QFileInfo::exists(UserAvatarStore::path()),
          "loading migrates the legacy picture into its own file");
    check(!QSettings().contains(kLegacySetting),
          "migrating removes the legacy key");
    check(settingsFileSize() < 16 * 1024,
          "migrating shrinks the settings file back down");

    // A migration that wrote the file but could not clear the key converges on
    // the next load rather than leaving the blob in the ini file forever.
    {
        QSettings settings;
        settings.setValue(kLegacySetting, legacy);
        settings.sync();
    }
    check(UserAvatarStore::load() == legacy,
          "the file wins over a leftover legacy key");
    check(!QSettings().contains(kLegacySetting),
          "a leftover legacy key is cleared on the next load");

    // Clearing.
    check(UserAvatarStore::store(QByteArray()), "clearing the picture succeeds");
    check(!QFileInfo::exists(UserAvatarStore::path()),
          "clearing removes the picture file");
    check(UserAvatarStore::load().isEmpty(), "a cleared picture loads as empty");

    check(failures == 0, "all user avatar store checks pass");
    if (failures == 0)
        std::cout << "user avatar store tests passed\n";
    return failures == 0 ? 0 : 1;
}
