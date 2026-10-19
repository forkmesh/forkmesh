#include "ProjectAgentAccess.h"

#include "AgentGuardrails.h"
#include "AppPaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUrl>

namespace forkmesh::projects {

namespace {

constexpr qint64 kMaxProjectFileBytes = 256 * 1024;
constexpr int kMaxListEntries = 200;

const QString kAccessKey = QStringLiteral("agentAccess");

// The per-project roots ProjectRegistry creates: identity keys, settings and
// logs are the project's own, never its agents'.
const QStringList &privateRootNames()
{
    static const QStringList names{QStringLiteral("data"),  QStringLiteral("config"),
                                   QStringLiteral("cache"), QStringLiteral("state"),
                                   QStringLiteral("logs"),  QStringLiteral("ssh")};
    return names;
}

QString canonicalOrClean(const QString &path)
{
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath() : canonical);
}

bool within(const QString &path, const QString &root)
{
    return !path.isEmpty() && !root.isEmpty() &&
           (path == root || path.startsWith(root + QLatin1Char('/')));
}

QStringList folderList(const QJsonValue &value)
{
    QStringList folders;
    for (const QJsonValue &entry : value.toArray()) {
        const QString path = QDir::cleanPath(QDir::fromNativeSeparators(
            entry.toString().trimmed()));
        if (path.isEmpty() || QDir::isRelativePath(path) || folders.contains(path))
            continue;
        folders.append(path);
        if (folders.size() == kMaxListEntries)
            break;
    }
    return folders;
}

QStringList hostList(const QJsonValue &value)
{
    QStringList hosts;
    for (const QJsonValue &entry : value.toArray()) {
        const QString host = normalizeHost(entry.toString());
        if (host.isEmpty() || hosts.contains(host))
            continue;
        hosts.append(host);
        if (hosts.size() == kMaxListEntries)
            break;
    }
    return hosts;
}

QJsonObject readProjectFile(const QString &projectDir, bool *readable)
{
    QFile file(QDir(projectDir).filePath(QLatin1String(forkmesh::paths::kProjectFileName)));
    *readable = true;
    if (!file.exists())
        return {};
    if (file.size() > kMaxProjectFileBytes || !file.open(QIODevice::ReadOnly)) {
        *readable = false;
        return {};
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        *readable = false;
        return {};
    }
    return document.object();
}

AgentAccess accessFromObject(const QJsonObject &object)
{
    AgentAccess access;
    // Anything unrecognised keeps the locked default rather than opening up.
    if (object.value(QStringLiteral("folders")).toString() == QLatin1String("unrestricted"))
        access.folders = AgentAccess::Folders::Unrestricted;
    const QString network = object.value(QStringLiteral("network")).toString();
    if (network == QLatin1String("block"))
        access.network = AgentAccess::Network::Block;
    else if (network == QLatin1String("allow"))
        access.network = AgentAccess::Network::Allow;
    if (object.contains(QStringLiteral("sharedTools")))
        access.sharedTools = object.value(QStringLiteral("sharedTools")).toBool(true);
    access.readOnlyFolders = folderList(object.value(QStringLiteral("readOnlyFolders")));
    access.readWriteFolders = folderList(object.value(QStringLiteral("readWriteFolders")));
    access.allowedHosts = hostList(object.value(QStringLiteral("allowedHosts")));
    access.followGlobal = object.value(QStringLiteral("followGlobal")).toBool(false);
    for (const QJsonValue &entry : object.value(QStringLiteral("allowedDestructive")).toArray()) {
        const QString kind = entry.toString().simplified().toLower();
        if (!kind.isEmpty() && !access.allowedDestructive.contains(kind))
            access.allowedDestructive.append(kind);
    }
    for (const QJsonValue &entry : object.value(QStringLiteral("allowedDeletePaths")).toArray()) {
        const QString pattern = entry.toString().trimmed();
        if (!pattern.isEmpty() && !access.allowedDeletePaths.contains(pattern))
            access.allowedDeletePaths.append(pattern);
    }
    return access;
}

QJsonObject accessToObject(const AgentAccess &access)
{
    QJsonObject object;
    object.insert(QStringLiteral("folders"), foldersKey(access.folders));
    object.insert(QStringLiteral("sharedTools"), access.sharedTools);
    object.insert(QStringLiteral("readOnlyFolders"),
                  QJsonArray::fromStringList(folderList(
                      QJsonArray::fromStringList(access.readOnlyFolders))));
    object.insert(QStringLiteral("readWriteFolders"),
                  QJsonArray::fromStringList(folderList(
                      QJsonArray::fromStringList(access.readWriteFolders))));
    object.insert(QStringLiteral("network"), networkKey(access.network));
    object.insert(QStringLiteral("allowedHosts"),
                  QJsonArray::fromStringList(
                      hostList(QJsonArray::fromStringList(access.allowedHosts))));
    if (!access.allowedDestructive.isEmpty())
        object.insert(QStringLiteral("allowedDestructive"),
                      QJsonArray::fromStringList(access.allowedDestructive));
    if (!access.allowedDeletePaths.isEmpty())
        object.insert(QStringLiteral("allowedDeletePaths"),
                      QJsonArray::fromStringList(access.allowedDeletePaths));
    return object;
}

} // namespace

QString foldersKey(AgentAccess::Folders folders)
{
    return folders == AgentAccess::Folders::Unrestricted ? QStringLiteral("unrestricted")
                                                         : QStringLiteral("project");
}

QString networkKey(AgentAccess::Network network)
{
    switch (network) {
    case AgentAccess::Network::Block:
        return QStringLiteral("block");
    case AgentAccess::Network::Allow:
        return QStringLiteral("allow");
    case AgentAccess::Network::Ask:
        break;
    }
    return QStringLiteral("ask");
}

AgentAccess loadAgentAccess(const QString &projectDir)
{
    bool readable = true;
    const QJsonObject project = readProjectFile(projectDir, &readable);
    if (readable && !project.contains(kAccessKey)) {
        AgentAccess access;
        access.followGlobal = true;
        return access;
    }
    return accessFromObject(project.value(kAccessKey).toObject());
}

bool saveAgentAccess(const QString &projectDir, const AgentAccess &access, QString *error)
{
    QString localError;
    if (!error)
        error = &localError;
    error->clear();
    const QString path =
        QDir(projectDir).filePath(QLatin1String(forkmesh::paths::kProjectFileName));
    bool readable = true;
    QJsonObject project = readProjectFile(projectDir, &readable);
    if (!readable) {
        // Never overwrite a project file this build cannot read back.
        *error = QStringLiteral("%1 could not be read, so it was left as it is.")
                     .arg(QDir::toNativeSeparators(path));
        return false;
    }

    QJsonObject object = accessToObject(access);
    if (access.followGlobal)
        object.insert(QStringLiteral("followGlobal"), true);
    project.insert(kAccessKey, object);

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(QJsonDocument(project).toJson(QJsonDocument::Indented)) < 0 ||
        !file.commit()) {
        *error = QStringLiteral("Could not write %1.").arg(QDir::toNativeSeparators(path));
        return false;
    }
    return true;
}

QString destructiveKey(GlobalAgentRules::Destructive destructive)
{
    switch (destructive) {
    case GlobalAgentRules::Destructive::Block:
        return QStringLiteral("block");
    case GlobalAgentRules::Destructive::Allow:
        return QStringLiteral("allow");
    case GlobalAgentRules::Destructive::Ask:
        break;
    }
    return QStringLiteral("ask");
}

QString globalRulesPath(const QString &homeProjectDir)
{
    const QString organized =
        QDir(homeProjectDir).filePath(QStringLiteral("data/core/config/agent-rules.json"));
    const QString previous = QDir(homeProjectDir).filePath(QStringLiteral("config/agent-rules.json"));
    const QFileInfo oldConfig(QDir(homeProjectDir).filePath(QStringLiteral("config")));
    if (!QFileInfo::exists(organized) && oldConfig.isDir() && !oldConfig.isSymLink() &&
        QFileInfo::exists(previous))
        return previous;
    return organized;
}

GlobalAgentRules loadGlobalRules(const QString &path)
{
    GlobalAgentRules rules;
    QFile file(path);
    if (!file.exists() || file.size() > kMaxProjectFileBytes || !file.open(QIODevice::ReadOnly))
        return rules;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return rules;
    const QJsonObject object = document.object();
    rules.defaults = accessFromObject(object.value(QStringLiteral("defaults")).toObject());
    rules.blockedHosts = hostList(object.value(QStringLiteral("blockedHosts")));
    const QJsonObject tools = object.value(QStringLiteral("toolAllowlist")).toObject();
    rules.toolAllowlist = tools.value(QStringLiteral("enabled")).toBool(false);
    for (const QJsonValue &entry : tools.value(QStringLiteral("families")).toArray()) {
        const QString family = entry.toString().trimmed().toLower();
        if (!family.isEmpty() && !rules.allowedToolFamilies.contains(family))
            rules.allowedToolFamilies.append(family);
    }
    const QString destructive = object.value(QStringLiteral("destructiveCommands")).toString();
    if (destructive == QLatin1String("block"))
        rules.destructive = GlobalAgentRules::Destructive::Block;
    else if (destructive == QLatin1String("allow"))
        rules.destructive = GlobalAgentRules::Destructive::Allow;
    return rules;
}

bool saveGlobalRules(const QString &path, const GlobalAgentRules &rules, QString *error)
{
    QString localError;
    if (!error)
        error = &localError;
    error->clear();
    QJsonArray families;
    for (const QString &family : rules.allowedToolFamilies)
        families.append(family);
    const QJsonObject object{
        {QStringLiteral("defaults"), accessToObject(rules.defaults)},
        {QStringLiteral("blockedHosts"),
         QJsonArray::fromStringList(hostList(QJsonArray::fromStringList(rules.blockedHosts)))},
        {QStringLiteral("toolAllowlist"),
         QJsonObject{{QStringLiteral("enabled"), rules.toolAllowlist},
                     {QStringLiteral("families"), families}}},
        {QStringLiteral("destructiveCommands"), destructiveKey(rules.destructive)}};
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        *error = QStringLiteral("Could not create %1.")
                     .arg(QDir::toNativeSeparators(QFileInfo(path).absolutePath()));
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(QJsonDocument(object).toJson(QJsonDocument::Indented)) < 0 ||
        !file.commit()) {
        *error = QStringLiteral("Could not write %1.").arg(QDir::toNativeSeparators(path));
        return false;
    }
    return true;
}

AgentAccess effectiveAgentAccess(const AgentAccess &project, const GlobalAgentRules &rules)
{
    AgentAccess access = project;
    if (project.followGlobal) {
        access.folders = rules.defaults.folders;
        access.network = rules.defaults.network;
        access.sharedTools = rules.defaults.sharedTools;
    }
    auto merge = [](QStringList &into, const QStringList &from) {
        for (const QString &entry : from)
            if (!into.contains(entry))
                into.append(entry);
    };
    merge(access.readOnlyFolders, rules.defaults.readOnlyFolders);
    merge(access.readWriteFolders, rules.defaults.readWriteFolders);
    merge(access.allowedHosts, rules.defaults.allowedHosts);
    merge(access.allowedDestructive, rules.defaults.allowedDestructive);
    merge(access.allowedDeletePaths, rules.defaults.allowedDeletePaths);
    access.blockedHosts = rules.blockedHosts;
    return access;
}

bool gatesExternalCalls(const AgentAccess &access)
{
    return access.network != AgentAccess::Network::Allow || !access.blockedHosts.isEmpty();
}

bool hostBlocked(const QStringList &blockedHosts, const QString &host)
{
    return hostAllowed(blockedHosts, host);
}

bool webSearchAllowed(const AgentAccess &access)
{
    const QString label = QStringLiteral("web-search");
    if (hostBlocked(access.blockedHosts, label))
        return false;
    return access.network == AgentAccess::Network::Allow ||
           hostAllowed(access.allowedHosts, label);
}

QString normalizeHost(const QString &text)
{
    QString host = text.trimmed().toLower();
    if (host.isEmpty())
        return {};
    const bool wildcard = host.startsWith(QLatin1String("*."));
    if (wildcard)
        host = host.mid(2);
    if (host.contains(QLatin1String("://"))) {
        host = QUrl(host).host().toLower();
    } else {
        host = host.section(QLatin1Char('/'), 0, 0);
        host = host.section(QLatin1Char('@'), -1);
        // host:port, but leave a bare IPv6 address alone.
        if (host.startsWith(QLatin1Char('['))) {
            host = host.mid(1).section(QLatin1Char(']'), 0, 0);
        } else if (host.count(QLatin1Char(':')) == 1) {
            host = host.section(QLatin1Char(':'), 0, 0);
        }
    }
    while (host.endsWith(QLatin1Char('.')))
        host.chop(1);
    static const QRegularExpression valid(
        QStringLiteral("^[a-z0-9:]([a-z0-9._:-]{0,251}[a-z0-9])?$"));
    if (host.isEmpty() || !valid.match(host).hasMatch())
        return {};
    return wildcard ? QStringLiteral("*.") + host : host;
}

bool hostAllowed(const QStringList &allowedHosts, const QString &host)
{
    const QString target = normalizeHost(host);
    if (target.isEmpty() || target.startsWith(QLatin1String("*.")))
        return false;
    for (const QString &entry : allowedHosts) {
        const QString allowed = normalizeHost(entry);
        if (allowed.isEmpty())
            continue;
        if (allowed.startsWith(QLatin1String("*."))) {
            const QString base = allowed.mid(2);
            if (target == base || target.endsWith(QLatin1Char('.') + base))
                return true;
        } else if (target == allowed) {
            return true;
        }
    }
    return false;
}

QStringList projectAgentFolders(const QString &projectDir,
                                const QStringList &privateRoots,
                                const QString &homeDir)
{
    const QString root = canonicalOrClean(projectDir);
    const QString home = canonicalOrClean(homeDir);
    if (root.isEmpty() || !QFileInfo(root).isDir() || within(home, root))
        return {};

    QStringList privates;
    for (const QString &path : privateRoots)
        if (!path.trimmed().isEmpty())
            privates.append(canonicalOrClean(path));

    QStringList folders;
    const QFileInfoList entries = QDir(root).entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name);
    for (const QFileInfo &entry : entries) {
        if (privateRootNames().contains(entry.fileName()))
            continue;
        const QString path = canonicalOrClean(entry.absoluteFilePath());
        // A symlink out of the project is not the project's to grant.
        if (!within(path, root))
            continue;
        bool holdsPrivate = false;
        for (const QString &secret : std::as_const(privates))
            if (within(secret, path) || within(path, secret))
                holdsPrivate = true;
        if (!holdsPrivate && !folders.contains(path))
            folders.append(path);
    }
    return folders;
}

QStringList sharedToolFolders(const QString &homeDir)
{
    const QDir home(homeDir);
    QStringList folders;
    for (const char *relative :
         {".local/bin", ".local/lib", ".local/share/claude", ".cargo/bin", ".rustup",
          ".nvm", ".bun/bin", ".deno/bin", "go/bin", "sdk", "flutter", ".pyenv"}) {
        const QString path = home.filePath(QLatin1String(relative));
        if (QFileInfo(path).isDir())
            folders.append(path);
    }
    for (const char *absolute : {"/opt", "/snap", "/nix", "/usr/local"})
        if (QFileInfo(QLatin1String(absolute)).isDir())
            folders.append(QLatin1String(absolute));
    return folders;
}

QStringList sharedScratchFolders(const QString &homeDir)
{
    QStringList folders;
    const QString cache = QDir(homeDir).filePath(QStringLiteral(".cache"));
    if (QFileInfo(cache).isDir())
        folders.append(cache);
    for (const char *absolute : {"/tmp", "/var/tmp", "/dev/shm"})
        if (QFileInfo(QLatin1String(absolute)).isDir())
            folders.append(QLatin1String(absolute));
    return folders;
}

bool folderLockSupported()
{
#ifdef Q_OS_LINUX
    return true;
#else
    return false;
#endif
}

void applyAgentAccess(forkmesh::agentguard::Config &guardrails, const AgentAccess &access,
                      const QString &projectDir, const QStringList &privateRoots,
                      const QString &homeDir, bool vmActive)
{
    guardrails.projectLocked = false;
    guardrails.projectReadWrite.clear();
    guardrails.projectReadOnly.clear();
    if (access.folders != AgentAccess::Folders::Project || vmActive ||
        !folderLockSupported())
        return;
    guardrails.projectLocked = true;
    guardrails.projectReadWrite = projectAgentFolders(projectDir, privateRoots, homeDir);
    guardrails.projectReadWrite += access.readWriteFolders;
    guardrails.projectReadOnly = access.readOnlyFolders;
    // Git needs the user's identity to commit the agent's work.
    for (const char *relative : {".gitconfig", ".config/git"}) {
        const QString path = QDir(homeDir).filePath(QLatin1String(relative));
        if (QFileInfo::exists(path))
            guardrails.projectReadOnly.append(path);
    }
    if (access.sharedTools) {
        guardrails.projectReadWrite += sharedScratchFolders(homeDir);
        guardrails.projectReadOnly += sharedToolFolders(homeDir);
    }
    guardrails.projectReadWrite.removeDuplicates();
    guardrails.projectReadOnly.removeDuplicates();
}

} // namespace forkmesh::projects
