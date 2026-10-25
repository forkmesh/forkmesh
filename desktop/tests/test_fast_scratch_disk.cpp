#include "AgentScratch.h"
#include "FastScratchDisk.h"
#include "UsbLinkCheck.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cstdio>

using namespace forkmesh;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char *what)
{
    ++g_checks;
    if (ok) {
        std::printf("PASS %s\n", what);
    } else {
        ++g_failures;
        std::printf("FAIL %s\n", what);
    }
}

void writeFile(const QString &path, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(content);
}

void makeAgentRoot(const QString &root)
{
    writeFile(root + QStringLiteral("/.gitignore"), agentscratch::markerText().toUtf8());
}

QByteArray interfaceDescriptor(quint8 cls, quint8 subclass, quint8 protocol)
{
    QByteArray bytes(9, '\0');
    bytes[0] = 9;
    bytes[1] = 4;
    bytes[5] = char(cls);
    bytes[6] = char(subclass);
    bytes[7] = char(protocol);
    return bytes;
}

void makeUsbDevice(const QString &sys, const QString &id, const QByteArray &speed,
                   const QByteArray &product, const QByteArray &version, bool uas)
{
    const QString dir = sys + QLatin1Char('/') + id;
    writeFile(dir + QStringLiteral("/speed"), speed + '\n');
    writeFile(dir + QStringLiteral("/manufacturer"), QByteArray("SanDisk\n"));
    writeFile(dir + QStringLiteral("/product"), product + '\n');
    writeFile(dir + QStringLiteral("/version"), version + '\n');
    writeFile(dir + QLatin1Char('/') + id + QStringLiteral(":1.0/bInterfaceClass"),
              QByteArray("08\n"));
    QByteArray descriptors(18, '\0');
    descriptors[0] = 18;
    descriptors[1] = 1;
    descriptors += interfaceDescriptor(8, 6, 0x50);
    if (uas)
        descriptors += interfaceDescriptor(8, 6, 0x62);
    writeFile(dir + QStringLiteral("/descriptors"), descriptors);
}

}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir sandbox;
    if (!sandbox.isValid())
        return 2;
    const QString root = QFileInfo(sandbox.path()).canonicalFilePath();
    const QString home = root + QStringLiteral("/home");
    QDir().mkpath(home);
    qputenv("HOME", home.toLocal8Bit());
    for (const char *variable : {"FORKMESH_HOME", "FORKMESH_PROJECTS_DIR", "FORKMESH_DATA_DIR",
                                 "FORKMESH_CONFIG_DIR", "FORKMESH_PROJECT_DIR",
                                 "XDG_DATA_HOME", "XDG_CONFIG_HOME"})
        qunsetenv(variable);

    const QString labels = root + QStringLiteral("/by-label");
    const QString device = root + QStringLiteral("/dev-nvme0n1p1");
    const QString mount = root + QStringLiteral("/media/TMP");
    const QString mounts = root + QStringLiteral("/mounts");
    writeFile(device, QByteArray());
    QDir().mkpath(labels);
    QDir().mkpath(mount);

    {
        const fastdisk::Volume none = fastdisk::probe(labels, mounts);
        check(!none.present() && !none.usable(), "no TMP label means no fast disk");
    }

    QFile::link(device, labels + QStringLiteral("/TMP"));
    {
        writeFile(mounts, QByteArray("/dev/sda2 / ext4 rw 0 0\n"));
        const fastdisk::Volume unmounted = fastdisk::probe(labels, mounts);
        check(unmounted.present() && !unmounted.mounted(),
              "a TMP label that is not mounted is present but unused");
    }

    QByteArray escapedMount = mount.toUtf8();
    writeFile(mounts, QByteArray("/dev/sda2 / ext4 rw 0 0\n") + device.toUtf8() + ' ' +
                          escapedMount + " ext4 rw,nosuid 0 0\n");
    const fastdisk::Volume volume = fastdisk::probe(labels, mounts);
    check(volume.usable() && volume.mountPoint == mount, "a mounted TMP label is found");

    {
        const auto parsed = fastdisk::parseMounts("/dev/x /media/f/My\\040Disk ext4 rw 0 0\n");
        check(parsed.size() == 1 && parsed.first().target == QStringLiteral("/media/f/My Disk"),
              "mount paths with spaces are unescaped");
    }

    check(fastdisk::activate(volume, false).isEmpty() && fastdisk::activeRoot().isEmpty(),
          "turned off in settings means not active");
    const QString active = fastdisk::activate(volume, true);
    check(active == mount + QStringLiteral("/ForkMesh") && QFileInfo(active).isDir(),
          "activating creates the ForkMesh folder on the TMP disk");

    const QString worktrees = root + QStringLiteral("/repo/.worktrees");
    makeAgentRoot(worktrees);
    const QString worktree = worktrees + QStringLiteral("/2600-task");
    writeFile(worktree + QStringLiteral("/.git"), QByteArray("gitdir: x\n"));

    const QString scratch = agentscratch::folderFor(worktree);
    check(scratch.startsWith(fastdisk::scratchBase(active) + QLatin1Char('/')),
          "agent scratch goes on the TMP disk when it is active");
    check(agentscratch::foldersFor(worktree).size() == 2 &&
              agentscratch::foldersFor(worktree).last() ==
                  worktrees + QStringLiteral("/.scratch/2600-task"),
          "the system-disk scratch folder is still known for cleanup");

    const QStringList env = agentscratch::environment(scratch, worktree);
    check(env.contains(QStringLiteral("FORKMESH_AGENT_SCRATCH=") + scratch) &&
              env.contains(QStringLiteral("CCACHE_DIR=") + fastdisk::ccacheDir(active)),
          "the agent environment points scratch and ccache at the TMP disk");
    const QString keyDir = QFileInfo(QFileInfo(scratch).absolutePath()).absolutePath();
    check(agentscratch::isAgentRoot(keyDir), "the TMP scratch parent carries the agent marker");
    check(fastdisk::readOwner(keyDir) == QDir::cleanPath(worktrees),
          "the TMP scratch parent records which worktree root it belongs to");

    writeFile(scratch + QStringLiteral("/build/out.o"), QByteArray("obj"));
    const QString orphan = QFileInfo(scratch).absolutePath() + QStringLiteral("/2599-gone");
    writeFile(orphan + QStringLiteral("/build/out.o"), QByteArray("obj"));
    const QString oldTemp = scratch + QStringLiteral("/tmp/old.txt");
    writeFile(oldTemp, QByteArray("x"));
    {
        QFile file(oldTemp);
        file.open(QIODevice::ReadWrite);
        file.setFileTime(QDateTime::currentDateTime().addDays(-3),
                         QFileDevice::FileModificationTime);
    }
    const QString freshTemp = scratch + QStringLiteral("/tmp/new.txt");
    writeFile(freshTemp, QByteArray("x"));

    const QString staleKey = fastdisk::scratchBase(active) + QStringLiteral("/deadbeef");
    writeFile(staleKey + QLatin1Char('/') + fastdisk::ownerFileName(),
              (root + QStringLiteral("/missing/.worktrees\n")).toUtf8());
    writeFile(staleKey + QStringLiteral("/.scratch/x/file"), QByteArray("x"));

    const fastdisk::CleanResult cleaned =
        fastdisk::cleanUnused(active, QDateTime::currentMSecsSinceEpoch(), 24LL * 3600 * 1000);
    check(!QFileInfo::exists(orphan), "cleanup removes scratch of worktrees that are gone");
    check(!QFileInfo::exists(staleKey), "cleanup removes scratch of worktree roots that are gone");
    check(!QFileInfo::exists(oldTemp) && QFileInfo::exists(freshTemp),
          "cleanup removes day-old temp files and keeps fresh ones");
    check(QFileInfo::exists(scratch + QStringLiteral("/build/out.o")),
          "cleanup keeps the scratch of a live worktree");
    check(cleaned.failures.isEmpty(), "cleanup reports no failures");

    {
        const fastdisk::Usage held = fastdisk::usage(volume);
        check(held.hasData(), "the TMP disk reports that it holds data");
    }

    writeFile(mount + QStringLiteral("/other/file.bin"), QByteArray("x"));
    QDir().mkpath(mount + QStringLiteral("/lost+found"));
    const fastdisk::EraseResult erased = fastdisk::eraseAll(volume, labels, mounts);
    check(erased.ok, "erasing the TMP disk succeeds");
    check(!QFileInfo::exists(mount + QStringLiteral("/ForkMesh")) &&
              !QFileInfo::exists(mount + QStringLiteral("/other")),
          "erasing removes everything on the TMP disk");
    check(QFileInfo(mount + QStringLiteral("/lost+found")).isDir(),
          "erasing keeps lost+found");
    check(!fastdisk::usage(volume).hasData(), "an erased TMP disk reports no data");

    {
        QFile::remove(labels + QStringLiteral("/TMP"));
        QFile::link(device, labels + QStringLiteral("/DATA"));
        writeFile(mount + QStringLiteral("/keep.txt"), QByteArray("keep"));
        const fastdisk::EraseResult refused = fastdisk::eraseAll(volume, labels, mounts);
        check(!refused.ok && QFileInfo::exists(mount + QStringLiteral("/keep.txt")),
              "erasing refuses a disk that is no longer labelled TMP");
        QFile::remove(labels + QStringLiteral("/DATA"));
        QFile::link(device, labels + QStringLiteral("/TMP"));
    }
    {
        fastdisk::Volume other = volume;
        other.mountPoint = root + QStringLiteral("/elsewhere");
        const fastdisk::EraseResult refused = fastdisk::eraseAll(other, labels, mounts);
        check(!refused.ok && QFileInfo::exists(mount + QStringLiteral("/keep.txt")),
              "erasing refuses when the disk moved since it was checked");
    }

    fastdisk::setActiveRoot(QString());
    check(agentscratch::folderFor(worktree) == worktrees + QStringLiteral("/.scratch/2600-task"),
          "without the TMP disk scratch stays next to the worktrees");
    check(agentscratch::foldersFor(worktree).size() == 1, "only one scratch place without TMP");

    const QString sys = root + QStringLiteral("/sys-usb");
    writeFile(sys + QStringLiteral("/usb1/speed"), QByteArray("480\n"));
    writeFile(sys + QStringLiteral("/usb2/speed"), QByteArray("20000\n"));
    makeUsbDevice(sys, QStringLiteral("1-5.4"), "480", "SanDisk 3.2 Gen2", " 2.10", true);
    makeUsbDevice(sys, QStringLiteral("2-8"), "10000", "Extreme 3.2 Gen2", " 3.20", true);
    makeUsbDevice(sys, QStringLiteral("1-2"), "480", "Cruzer Blade", " 2.00", false);
    {
        const auto findings = usblink::scan(sys);
        check(findings.size() == 1 && findings.first().id == QStringLiteral("1-5.4"),
              "only the fast drive on a slow link is reported");
        check(!findings.isEmpty() && findings.first().availableMbps == 10000 &&
                  findings.first().speedMbps == 480,
              "the report says what the drive could run at");
        check(!findings.isEmpty() &&
                  usblink::message(findings.first()).contains(QStringLiteral("480 Mbps")),
              "the message names the slow speed");
    }
    {
        writeFile(sys + QStringLiteral("/usb2/speed"), QByteArray("5000\n"));
        makeUsbDevice(sys, QStringLiteral("2-8"), "5000", "Extreme 3.2 Gen2", " 3.20", true);
        const auto findings = usblink::scan(sys);
        bool flagged = false;
        for (const auto &finding : findings)
            flagged = flagged || finding.id == QStringLiteral("2-8");
        check(!flagged, "a drive already on the fastest port the computer has is not reported");
    }
    check(usblink::capabilityFromName(QStringLiteral("Portable SSD Gen 2x2")) == 20000 &&
              usblink::capabilityFromName(QStringLiteral("USB 3.0 Disk")) == 5000 &&
              usblink::capabilityFromName(QStringLiteral("Flash Drive")) == 0,
          "drive names map to their rated speed");

    std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
