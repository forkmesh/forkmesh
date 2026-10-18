#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace forkmesh::openwrt {

inline const QString kAnonymousSession = QStringLiteral("00000000000000000000000000000000");
inline constexpr int kSessionTimeoutSeconds = 3600;
inline constexpr int kApplyRollbackSeconds = 90;

enum UbusStatus {
    UbusOk = 0,
    UbusInvalidCommand = 1,
    UbusInvalidArgument = 2,
    UbusMethodNotFound = 3,
    UbusNotFound = 4,
    UbusNoData = 5,
    UbusPermissionDenied = 6,
    UbusTimeout = 7,
    UbusNotSupported = 8,
    UbusUnknownError = 9,
    UbusConnectionFailed = 10,
};

struct RpcResult {
    bool ok = false;
    int status = -1;
    QJsonObject data;
    QString error;
    bool accessDenied = false;
    bool unreachable = false;
    QString certificate;
};

QByteArray callBody(int id, const QString &session, const QString &object, const QString &method,
                    const QJsonObject &args = {});
RpcResult parseReply(const QByteArray &body, int httpStatus = 200);
QString statusMessage(int status);

struct Address {
    QUrl base;
    QString error;
    bool encrypted = false;
    bool valid() const { return error.isEmpty() && base.isValid(); }
};

Address parseAddress(const QString &input);
bool isLocalHostName(const QString &host);
QString defaultGatewayFromProcRoute(const QString &text);
QString defaultGatewayFromRouteGet(const QString &text);

struct UciSection {
    QString name;
    QString type;
    bool anonymous = false;
    int index = 0;
    QJsonObject options;

    QString option(const QString &key, const QString &fallback = {}) const;
    bool flag(const QString &key, bool fallback = false) const;
};

QList<UciSection> uciSections(const QJsonObject &values, const QString &type = {});
QString optionText(const QJsonValue &value);
QJsonValue optionValue(const QString &text, bool list);
QString sectionLabel(const UciSection &section);
bool isSecretOption(const QString &name);
bool validUciName(const QString &name);
bool validUciOptionName(const QString &name);

int pendingCount(const QJsonObject &changes);
QStringList describeChanges(const QJsonObject &changes);

struct Lease {
    qint64 expires = 0;
    QString mac;
    QString ip;
    QString hostname;
};

QList<Lease> parseLeases(const QString &text);
QString leaseExpiry(qint64 expires, qint64 nowSeconds);

struct Package {
    QString name;
    QString version;
};

struct PackageTool {
    QString program;
    bool wrapper = false;
};

QList<PackageTool> packageTools();
QList<Package> parseInstalledPackages(const QString &text);
QStringList packageArguments(const PackageTool &tool, const QString &action, const QStringList &names = {});
bool validPackageName(const QString &name);

struct Service {
    QString name;
    bool enabled = false;
    bool running = false;
    int start = 0;
};

QList<Service> parseServices(const QJsonObject &list);

struct InterfaceStatus {
    QString name;
    QString proto;
    QString device;
    bool up = false;
    qint64 uptime = 0;
    QStringList ipv4;
    QStringList ipv6;
    QStringList dns;
    QString gateway;
};

QList<InterfaceStatus> parseInterfaces(const QJsonObject &dump);

QString formatUptime(qint64 seconds);
QString formatBytes(qint64 bytes);
QString formatLoad(const QJsonArray &load);

bool validIpv4(const QString &text);
bool validNetmask(const QString &text);
bool validMac(const QString &text);
bool validPort(const QString &text);
QString ssidProblem(const QString &ssid);
QString wifiKeyProblem(const QString &encryption, const QString &key);
QString hostnameProblem(const QString &hostname);
QString passwordProblem(const QString &password, const QString &confirm);

struct Choice {
    QString value;
    QString label;
};

QList<Choice> encryptionChoices();
QList<Choice> protocolChoices();
QList<Choice> forwardProtocolChoices();
QString choiceLabel(const QList<Choice> &choices, const QString &value);

struct BlockDevice {
    QString name;
    QString device;
    QString uuid;
    QString label;
    QString type;
    QString mount;
    qint64 size = 0;
};

struct MountPoint {
    QString device;
    QString mount;
    QString type;
    qint64 size = 0;
    qint64 avail = 0;
};

struct StorageDisk {
    BlockDevice block;
    QString mount;
    qint64 size = 0;
    qint64 avail = 0;
    bool usageKnown = false;
    QString fstabSection;
    QString bootTarget;
    bool bootEnabled = false;

    QString title() const;
    bool mounted() const { return !mount.isEmpty(); }
};

struct Share {
    QString section;
    QString name;
    QString path;
    bool readOnly = false;
    bool guests = false;
};

bool isStorageDevice(const QString &name);
QList<BlockDevice> parseBlockDevices(const QJsonObject &devices);
QList<MountPoint> parseMountPoints(const QJsonObject &data);
QList<StorageDisk> storageDisks(const QList<BlockDevice> &blocks, const QList<MountPoint> &mounts, const QList<UciSection> &fstab);
QString mountPathProblem(const QString &path);
QString suggestedMountPath(const BlockDevice &block);
QStringList usbStoragePackages();
QStringList networkSharingPackages();
QString shareUrl(const QString &routerHost, const QString &shareName);

struct StorageCheck {
    bool packagesKnown = false;
    QStringList installed;
    bool blockTool = false;
    bool fstabExists = false;
    bool autoMount = false;
    bool autoMountPending = false;
    bool usbKnown = false;
    int usbControllers = 0;
    QStringList usbDevices;
    bool partitionsKnown = false;
    QStringList partitions;
    int disks = 0;
    int mounted = 0;
    QString listProblem;
    QStringList kernelLines;
};

inline const QString kFstabPath = QStringLiteral("/etc/config/fstab");
QStringList parsePartitions(const QString &text);
int usbControllerCount(const QStringList &entries);
QStringList usbDeviceEntries(const QStringList &entries);
QStringList storageKernelLines(const QString &dmesg, int limit = 20);
QStringList missingStoragePackages(const QStringList &installed);
QString fstabFromDetect(const QString &output);
QStringList outputTail(const QString &text, int count = 4);
QStringList storageFindings(const StorageCheck &check);
QString storageReport(const StorageCheck &check);
QString shareConfig(const QStringList &configs);
QString shareSectionType(const QString &config);
QList<Share> parseShares(const QList<UciSection> &sections);
QString shareNameProblem(const QString &name);
QJsonObject mountSectionValues(const BlockDevice &block, const QString &target);

QString certificateFingerprint(const QByteArray &der);
QString firmwareProblem(const QJsonObject &validation);
QString confirmationPhrase(const QString &hostname);

}
