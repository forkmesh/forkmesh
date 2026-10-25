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

QString makeHub(const QString &sys, const QString &controller, int bus, const QByteArray &speed)
{
    const QString hub = QStringLiteral("usb%1").arg(bus);
    const QString real = sys + QStringLiteral("/devices/") + controller + QLatin1Char('/') + hub;
    writeFile(real + QStringLiteral("/speed"), speed + '\n');
    QDir().mkpath(sys + QStringLiteral("/bus"));
    QFile::link(real, sys + QStringLiteral("/bus/") + hub);
    return real + QStringLiteral("/%1-0:1.0").arg(bus);
}

void makePort(const QString &iface, const QString &id, const QByteArray &type,
              const QString &peerIface = QString(), const QString &peer = QString())
{
    writeFile(iface + QLatin1Char('/') + id + QStringLiteral("/connect_type"), type + '\n');
    if (!peer.isEmpty()) {
        QDir().mkpath(peerIface + QLatin1Char('/') + peer);
        QFile::link(peerIface + QLatin1Char('/') + peer,
                    iface + QLatin1Char('/') + id + QStringLiteral("/peer"));
        QFile::link(iface + QLatin1Char('/') + id,
                    peerIface + QLatin1Char('/') + peer + QStringLiteral("/peer"));
        writeFile(peerIface + QLatin1Char('/') + peer + QStringLiteral("/connect_type"),
                  type + '\n');
    }
}

void setPlace(const QString &port, const QByteArray &panel, const QByteArray &vertical,
              const QByteArray &horizontal)
{
    writeFile(port + QStringLiteral("/physical_location/panel"), panel + '\n');
    writeFile(port + QStringLiteral("/physical_location/vertical_position"), vertical + '\n');
    writeFile(port + QStringLiteral("/physical_location/horizontal_position"),
              horizontal + '\n');
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

    {
        const QString uuids = root + QStringLiteral("/by-uuid");
        const QString uuid = QStringLiteral("8c5a432c-6fe4-4e47-921e-03f2430f7a56");
        QDir().mkpath(uuids);
        QFile::link(device, uuids + QLatin1Char('/') + uuid);
        const QString udev = root + QStringLiteral("/udev");
        const QString sysBlock = root + QStringLiteral("/sys-block");
        writeFile(sysBlock + QStringLiteral("/dev-nvme0n1p1/dev"), QByteArray("259:1\n"));
        writeFile(udev + QStringLiteral("/b259:1"), QByteArray("S:disk/by-label/TMP\nE:ID_FS_TYPE=ext4\n"));
        const QByteArray fstab("UUID=ab30 / ext4 errors=remount-ro 0 1\n");

        check(volume.fsType == QStringLiteral("ext4"), "the probe reads the mounted file system type");
        const fastdisk::BootMount mounted =
            fastdisk::planBootMount(volume, fstab, QStringLiteral("/media/f/TMP"), 1001, 1001, uuids,
                                    udev, sysBlock);
        check(mounted.possible() && mounted.uuid == uuid && mounted.target == mount &&
                  mounted.pass == 2,
              "a mounted TMP disk missing from fstab keeps its current mount folder");
        check(mounted.fstabLine() ==
                  QStringLiteral("UUID=%1 %2 ext4 defaults,noatime,nofail,"
                                 "x-systemd.device-timeout=10s 0 2")
                      .arg(uuid, mount),
              "the fstab line uses the UUID and nofail so a missing disk cannot stop boot");

        fastdisk::Volume unmounted = volume;
        unmounted.mountPoint.clear();
        unmounted.fsType.clear();
        const fastdisk::BootMount fresh =
            fastdisk::planBootMount(unmounted, fstab, QStringLiteral("/media/f/TMP"), 1001, 1001,
                                    uuids, udev, sysBlock);
        check(fresh.possible() && fresh.target == QStringLiteral("/media/f/TMP") &&
                  fresh.fsType == QStringLiteral("ext4"),
              "an unmounted TMP disk gets the default folder and its udev file system type");

        writeFile(udev + QStringLiteral("/b259:1"), QByteArray("E:ID_FS_TYPE=exfat\n"));
        const fastdisk::BootMount exfat =
            fastdisk::planBootMount(unmounted, fstab, QStringLiteral("/media/f/TMP"), 1001, 1002,
                                    uuids, udev, sysBlock);
        check(exfat.options.endsWith(QStringLiteral(",uid=1001,gid=1002")) && exfat.pass == 0,
              "a file system without owners is mounted for the user");

        for (const QByteArray &line :
             {QByteArray("UUID=") + uuid.toUpper().toUtf8() + " /mnt/x ext4 defaults 0 2\n",
              QByteArray("LABEL=TMP /mnt/x ext4 defaults 0 2\n"),
              device.toUtf8() + " /mnt/x ext4 defaults 0 2\n"})
            check(fastdisk::planBootMount(volume, fstab + line, QStringLiteral("/media/f/TMP"), 1001,
                                          1001, uuids, udev, sysBlock)
                          .configured,
                  "an fstab entry by UUID, label or device counts as already set up");
        check(!fastdisk::planBootMount(volume, fstab + "# UUID=" + uuid.toUtf8() + " /x ext4\n",
                                       QStringLiteral("/media/f/TMP"), 1001, 1001, uuids, udev,
                                       sysBlock)
                   .configured,
              "a commented-out fstab entry does not count");

        check(fastdisk::defaultBootTarget(QStringLiteral("f")) == QStringLiteral("/media/f/TMP") &&
                  fastdisk::defaultBootTarget(QStringLiteral("a b")) == QStringLiteral("/mnt/TMP") &&
                  fastdisk::defaultBootTarget(QStringLiteral("..")) == QStringLiteral("/mnt/TMP"),
              "the default mount folder is under /media/<user> only for a plain user name");
        check(!fastdisk::safeMountTarget(QStringLiteral("/media/f/../etc")) &&
                  !fastdisk::safeMountTarget(QStringLiteral("/media/f/My Disk")) &&
                  !fastdisk::safeUuid(QStringLiteral("x; rm")),
              "unsafe mount folders and UUIDs are refused");

        const QStringList args = fastdisk::bootMountArguments(mounted);
        check(args.size() == 7 && args.at(0) == QStringLiteral("sh") &&
                  args.at(4) == mounted.fstabLine() && args.at(5) == mount && args.at(6) == uuid,
              "the privileged helper gets the line, folder and UUID as separate arguments");
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

    const QByteArray extremeBos = QByteArray::fromHex(
        "050f2a0003071002 1ef400000a100300 0e00010aff071410 0a00010000000011 "
        "000030400a00b040 0a00");
    check(usblink::bosMbps(extremeBos) == 10000 &&
              usblink::bosMbps(QByteArray::fromHex("050f0f0001 0a1003000e00010aff07")) == 5000 &&
              usblink::bosMbps(QByteArray()) == 0,
          "the BOS SuperSpeedPlus sublink gives the drive's rated speed");
    {
        makeUsbDevice(sys, QStringLiteral("2-3"), "5000", "Extreme 55AE", " 3.20", true);
        writeFile(sys + QStringLiteral("/2-3/bos_descriptors"), extremeBos);
        writeFile(sys + QStringLiteral("/usb2/speed"), QByteArray("10000\n"));
        bool flagged = false;
        for (const auto &finding : usblink::scan(sys))
            flagged = flagged || (finding.id == QStringLiteral("2-3") &&
                                  finding.availableMbps == 10000);
        check(flagged, "a 10 Gbps drive with no hint in its name is caught by its BOS");
    }

    check(usblink::rootPortId(QStringLiteral("1-5.4")) == QStringLiteral("usb1-port5") &&
              usblink::deviceOnPort(QStringLiteral("usb2-port4")) == QStringLiteral("2-4"),
          "a device id maps to the computer port it hangs off");
    {
        const QString ports = root + QStringLiteral("/sys-ports");
        const QString chipset = QStringLiteral("pci0000:00/0000:00:14.0");
        const QString addOn = QStringLiteral("pci0000:00/0000:00:1c.4/0000:03:00.0");
        const QString i1 = makeHub(ports, chipset, 1, "480");
        const QString i2 = makeHub(ports, chipset, 2, "5000");
        const QString i3 = makeHub(ports, addOn, 3, "480");
        const QString i4 = makeHub(ports, addOn, 4, "10000");
        makePort(i2, QStringLiteral("usb2-port4"), "hotplug", i1, QStringLiteral("usb1-port4"));
        makePort(i2, QStringLiteral("usb2-port7"), "hotplug", i1, QStringLiteral("usb1-port5"));
        makePort(i2, QStringLiteral("usb2-port10"), "hardwired");
        makePort(i1, QStringLiteral("usb1-port15"), "not used");
        makePort(i4, QStringLiteral("usb4-port1"), "unknown", i3, QStringLiteral("usb3-port1"));
        makePort(i4, QStringLiteral("usb4-port2"), "unknown", i3, QStringLiteral("usb3-port2"));
        for (const QString &port : {i1 + QStringLiteral("/usb1-port4"),
                                    i1 + QStringLiteral("/usb1-port5"),
                                    i2 + QStringLiteral("/usb2-port4"),
                                    i2 + QStringLiteral("/usb2-port7")})
            setPlace(port, "top", "upper", "left");
        const QString bus = ports + QStringLiteral("/bus");
        makeUsbDevice(bus, QStringLiteral("2-4"), "5000", "SanDisk 3.2 Gen2", " 3.20", true);
        writeFile(bus + QStringLiteral("/1-5/manufacturer"), QByteArray("Logitech\n"));
        writeFile(bus + QStringLiteral("/1-5/product"), QByteArray("USB Receiver\n"));

        const auto found = usblink::rootPorts(bus);
        bool clean = found.size() == 8;
        for (const auto &port : found)
            clean = clean && port.place.isEmpty() && !port.id.endsWith(QStringLiteral("port10"));
        check(clean, "hardwired, unused and firmware-placeholder locations are left out");
        bool receiver = false;
        for (const auto &port : found)
            receiver = receiver || (port.id == QStringLiteral("usb2-port7") &&
                                    port.occupant == QStringLiteral("Logitech USB Receiver"));
        check(receiver, "a USB 2.0 device on the paired port marks the socket as taken");

        auto findings = usblink::scan(bus);
        check(findings.size() == 1 && findings.first().portMbps == 5000 &&
                  findings.first().fasterPorts.size() == 2,
              "the drive's own port speed and the two faster ports are found");
        QString text = findings.isEmpty() ? QString() : usblink::message(findings.first());
        check(text.contains(QStringLiteral("Its port only does 5 Gbps")) &&
                  text.contains(QStringLiteral("2 free 10 Gbps ports, all on a separate USB "
                                               "controller")) &&
                  text.contains(QStringLiteral("USB-C")),
              "the message counts the free fast ports on the other controller");

        const QString typec = ports + QStringLiteral("/devices/typec/port0");
        QDir().mkpath(typec);
        QFile::link(typec, i3 + QStringLiteral("/usb3-port2/connector"));
        setPlace(i4 + QStringLiteral("/usb4-port2"), "back", "lower", "right");
        findings = usblink::scan(bus);
        text = findings.isEmpty() ? QString() : usblink::message(findings.first());
        check(!findings.isEmpty() && !findings.first().fasterPorts.isEmpty() &&
                  findings.first().fasterPorts.first().id == QStringLiteral("usb4-port2") &&
                  findings.first().fasterPorts.first().usbC,
              "a Type-C connector link marks the fast port as USB-C and ranks it first");
        check(text.contains(QStringLiteral("the free 10 Gbps USB-C port on the back, lower right")),
              "the message names the USB-C port and where it is");

        writeFile(bus + QStringLiteral("/4-1/product"), QByteArray("Portable SSD\n"));
        writeFile(bus + QStringLiteral("/3-2/product"), QByteArray("Keyboard\n"));
        findings = usblink::scan(bus);
        text = findings.isEmpty() ? QString() : usblink::message(findings.first());
        check(text.contains(QStringLiteral("Every 10 Gbps port is in use by Keyboard, "
                                           "Portable SSD")),
              "the message says what is holding the fast ports");

        makeUsbDevice(bus, QStringLiteral("4-1"), "5000", "SanDisk 3.2 Gen2", " 3.20", true);
        makeUsbDevice(bus, QStringLiteral("4-2.3"), "480", "SanDisk 3.2 Gen2", " 2.10", true);
        bool cable = false;
        bool hub = false;
        for (const auto &finding : usblink::scan(bus)) {
            cable = cable || (finding.id == QStringLiteral("4-1") &&
                              usblink::message(finding).contains(QStringLiteral("cable")));
            hub = hub || (finding.id == QStringLiteral("4-2.3") &&
                          usblink::message(finding).contains(
                              QStringLiteral("the computer port behind that hub does 10 Gbps")));
        }
        check(cable, "a slow link on a fast port blames the cable");
        check(hub, "a drive behind a hub on a fast port is told to skip the hub");
    }

    std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
