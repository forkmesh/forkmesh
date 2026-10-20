#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QHostAddress>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

namespace forkmesh::nexus {

struct KnownService {
    quint16 port = 0;
    QString name;
    QString title;
    bool cleartext = false;
    bool sensitive = false;
};

const QList<KnownService> &serviceCatalog();
const KnownService *serviceForPort(quint16 port);

enum class PortPreset { Quick, Common, Web, RemoteAccess, FileSharing, Mail, SmartHome, Media, Databases, WellKnown };

QList<PortPreset> portPresets();
QString presetTitle(PortPreset preset);
QList<quint16> presetPorts(PortPreset preset);

struct PortParse {
    QList<quint16> ports;
    QString error;
};

PortParse parsePorts(const QString &text);
QString describePorts(const QList<quint16> &ports);

using Network = QPair<QHostAddress, int>;

struct TargetParse {
    QList<QHostAddress> addresses;
    QString hostname;
    bool isRange = false;
    QString error;
};

TargetParse parseTarget(const QString &text, int maximumHosts);
QList<Network> parseNetworks(const QString &text, QString *error = nullptr);
QString describeNetworks(const QList<Network> &networks);

enum class TargetScope { Loopback, LocalNetwork, AuthorizedNetwork, PublicInternet };

TargetScope classify(const QHostAddress &address, const QList<Network> &authorized = {});
QString scopeTitle(TargetScope scope);

struct ScanPolicy {
    bool allowPublicTargets = false;
    QList<Network> authorizedNetworks;
    int maximumHosts = 256;
    int maximumPublicHosts = 4;
    int maximumProbes = 70000;
};

enum class DecisionKind { Allowed, NeedsAttestation, Denied };

struct Decision {
    DecisionKind kind = DecisionKind::Denied;
    QString message;
};

Decision evaluate(const ScanPolicy &policy, const QList<QHostAddress> &addresses, int portCount, bool isRange, bool attested);

enum class PortState { Open, Closed, NoResponse };

QString stateTitle(PortState state);
PortState stateFromName(const QString &name);
QString stateName(PortState state);

struct ServiceGuess {
    QString name;
    QString detail;
    bool confirmed = false;
};

ServiceGuess identifyService(quint16 port, const QByteArray &banner);
bool sendsHttpProbe(quint16 port);

struct PortResult {
    QString address;
    quint16 port = 0;
    PortState state = PortState::NoResponse;
    QString service;
    QString detail;
    bool confirmed = false;
    int latencyMs = -1;
};

struct ScanSession {
    QString id;
    QString target;
    QString ports;
    QDateTime startedAt;
    QDateTime finishedAt;
    bool cancelled = false;
    int probes = 0;
    QList<PortResult> results;
};

int openCount(const ScanSession &session);
QJsonObject scanToJson(const ScanSession &session);
ScanSession scanFromJson(const QJsonObject &object);
QByteArray scanCsv(const ScanSession &session);
QStringList portChanges(const ScanSession &current, const ScanSession *previous);

struct DnsRecord {
    QString name;
    quint16 type = 0;
    QString target;
    QHostAddress address;
    quint16 port = 0;
    QStringList text;
};

constexpr quint16 DnsTypeA = 1;
constexpr quint16 DnsTypePtr = 12;
constexpr quint16 DnsTypeTxt = 16;
constexpr quint16 DnsTypeAaaa = 28;
constexpr quint16 DnsTypeSrv = 33;

QByteArray mdnsQuery(const QStringList &names, quint16 type, quint16 id = 0);
QList<DnsRecord> parseDnsMessage(const QByteArray &message);

QStringList defaultServiceTypes();
QString serviceTypeTitle(const QString &type);

struct DiscoveredDevice {
    QString name;
    QStringList hostnames;
    QStringList addresses;
    QStringList services;
    QList<quint16> ports;
    QDateTime lastSeen;
};

QList<DiscoveredDevice> devicesFromRecords(const QList<DnsRecord> &records);
QJsonObject deviceToJson(const DiscoveredDevice &device);
DiscoveredDevice deviceFromJson(const QJsonObject &object);

struct HeaderFinding {
    QString severity;
    QString text;
};

QList<HeaderFinding> headerFindings(const QList<QPair<QByteArray, QByteArray>> &headers, bool https);

struct NexusSettings {
    int timeoutMs = 1500;
    int concurrency = 48;
    bool allowPublicTargets = false;
    QString authorizedNetworks;
};

constexpr int MaximumStoredScans = 50;

struct NexusArchive {
    NexusSettings settings;
    QList<ScanSession> scans;
    QList<DiscoveredDevice> devices;
};

QJsonObject archiveToJson(const NexusArchive &archive);
NexusArchive archiveFromJson(const QJsonObject &object);
ScanPolicy policyFor(const NexusSettings &settings);
QString suggestedLocalNetwork(const QList<Network> &interfaceAddresses);

}
