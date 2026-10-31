#include "ClaudeDesktopSidebar.h"

#include "ClaudeDesktopSessions.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace forkmesh::claudedesktop {

namespace {

constexpr qint64 kRecordLimitBytes = 1024 * 1024;
constexpr qint64 kConfigLimitBytes = 16 * 1024 * 1024;

QJsonObject readObject(const QString &path, qint64 limit)
{
    QFile file(path);
    if (file.size() > limit || !file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

QJsonValue unwrap(const QJsonValue &value)
{
    if (!value.isString())
        return value;
    const QJsonDocument doc = QJsonDocument::fromJson(value.toString().toUtf8());
    if (doc.isObject())
        return doc.object();
    if (doc.isArray())
        return doc.array();
    return value;
}

QString recordKeyOf(const QString &raw)
{
    QString key = raw.trimmed();
    if (key.startsWith(QLatin1String("code:")))
        key = key.mid(5);
    return key.startsWith(QLatin1String("local_")) ? key : QString();
}

void collectKeys(const QJsonValue &value, QStringList &out)
{
    const QJsonValue v = unwrap(value);
    if (v.isString()) {
        const QString key = recordKeyOf(v.toString());
        if (!key.isEmpty() && !out.contains(key))
            out.append(key);
    } else if (v.isArray()) {
        for (const QJsonValue &item : v.toArray())
            collectKeys(item, out);
    } else if (v.isObject()) {
        const QJsonObject o = v.toObject();
        for (const QString &field : {QStringLiteral("id"), QStringLiteral("sessionId")}) {
            const QString key = recordKeyOf(o.value(field).toString());
            if (!key.isEmpty()) {
                if (!out.contains(key))
                    out.append(key);
                return;
            }
        }
        for (auto it = o.begin(); it != o.end(); ++it)
            collectKeys(it.value(), out);
    }
}

qint64 millis(const QJsonValue &value)
{
    return value.isDouble() ? qint64(value.toDouble()) : value.toString().toLongLong();
}

QString projectKey(const QString &cwd)
{
    static const QRegularExpression other(QStringLiteral("[^A-Za-z0-9]"));
    QString key = QDir::cleanPath(cwd);
    key.replace(other, QStringLiteral("-"));
    return key;
}

QString locateTranscript(const DesktopRecord &record, const QString &claudeHome,
                         const QHash<QString, QString> &known)
{
    const QString cached = known.value(record.sessionId);
    if (!cached.isEmpty() && QFileInfo(cached).isFile())
        return cached;
    if (!isSessionId(record.sessionId) || claudeHome.isEmpty())
        return QString();
    const QString projects = QDir(claudeHome).filePath(QStringLiteral("projects"));
    if (!record.cwd.isEmpty()) {
        const QString direct = QDir(projects).filePath(projectKey(record.cwd) + QLatin1Char('/') +
                                                       record.sessionId.toLower() +
                                                       QStringLiteral(".jsonl"));
        if (QFileInfo(direct).isFile())
            return direct;
    }
    return findTranscript(projects, record.sessionId);
}

struct Scope {
    QString name;
    QList<DesktopRecord> records;
};

QList<Scope> readScopes(const QString &desktopRoot)
{
    QList<Scope> scopes;
    const QDir root(QDir(desktopRoot).filePath(QStringLiteral("claude-code-sessions")));
    if (!root.exists())
        return scopes;
    for (const QFileInfo &account : root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        for (const QFileInfo &org :
             QDir(account.absoluteFilePath()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            Scope scope;
            scope.name = account.fileName() + QLatin1Char('/') + org.fileName();
            const QDir orgDir(org.absoluteFilePath());
            const QStringList waiting =
                QDir(orgDir.filePath(QStringLiteral("waiting-input")))
                    .entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
            for (const QFileInfo &file :
                 orgDir.entryInfoList({QStringLiteral("local_*.json")}, QDir::Files)) {
                const QJsonObject o = readObject(file.absoluteFilePath(), kRecordLimitBytes);
                if (o.isEmpty() || o.value(QStringLiteral("isArchived")).toBool())
                    continue;
                DesktopRecord record;
                record.key = o.value(QStringLiteral("sessionId")).toString();
                if (record.key.isEmpty())
                    record.key = file.completeBaseName();
                record.sessionId = o.value(QStringLiteral("cliSessionId")).toString().toLower();
                record.cwd = o.value(QStringLiteral("cwd")).toString();
                record.title = o.value(QStringLiteral("title")).toString().simplified();
                record.model = o.value(QStringLiteral("model")).toString();
                record.createdMs = millis(o.value(QStringLiteral("createdAt")));
                record.activityMs = std::max(record.createdMs,
                                             millis(o.value(QStringLiteral("lastActivityAt"))));
                for (const QString &mark : waiting)
                    if (mark.contains(record.key) ||
                        (!record.sessionId.isEmpty() && mark.contains(record.sessionId)))
                        record.waiting = true;
                scope.records.append(record);
            }
            scopes.append(scope);
        }
    }
    std::stable_sort(scopes.begin(), scopes.end(), [](const Scope &a, const Scope &b) {
        return a.records.size() > b.records.size();
    });
    return scopes;
}

QList<DesktopSection> scopeSections(const QJsonObject &prefs, const QString &scope)
{
    const QJsonObject all = unwrap(prefs.value(QStringLiteral("dframe-code-sections"))).toObject();
    QJsonArray raw = all.value(scope).toObject().value(QStringLiteral("sections")).toArray();
    QList<QJsonObject> ordered;
    for (const QJsonValue &value : raw)
        ordered.append(value.toObject());
    std::stable_sort(ordered.begin(), ordered.end(), [](const QJsonObject &a, const QJsonObject &b) {
        return a.value(QStringLiteral("order")).toDouble() <
               b.value(QStringLiteral("order")).toDouble();
    });
    const QJsonObject groups =
        unwrap(prefs.value(QStringLiteral("dframe-group-scopes"))).toObject().value(scope).toObject();
    const QJsonObject assignments = groups.value(QStringLiteral("assignments")).toObject();
    const QJsonObject order = groups.value(QStringLiteral("order")).toObject();
    QList<DesktopSection> out;
    for (const QJsonObject &o : ordered) {
        DesktopSection section;
        section.id = o.value(QStringLiteral("id")).toString();
        section.kind = o.value(QStringLiteral("kind")).toString();
        section.name = o.value(QStringLiteral("name")).toString().trimmed();
        if (section.id.isEmpty() || section.kind == QLatin1String("routines") ||
            o.value(QStringLiteral("shown")).toBool(true) == false)
            continue;
        if (section.kind == QLatin1String("manual")) {
            collectKeys(order.value(section.id), section.members);
            collectKeys(o.value(QStringLiteral("members")), section.members);
            for (auto it = assignments.begin(); it != assignments.end(); ++it) {
                const QString key = recordKeyOf(it.key());
                if (it.value().toString() == section.id && !key.isEmpty() &&
                    !section.members.contains(key))
                    section.members.append(key);
            }
        }
        out.append(section);
    }
    return out;
}

int compareRuns(const QString &a, const QString &b)
{
    int i = 0;
    int j = 0;
    while (i < a.size() && j < b.size()) {
        const bool digitA = a.at(i).isDigit();
        const bool digitB = b.at(j).isDigit();
        if (digitA && digitB) {
            int endA = i;
            int endB = j;
            while (endA < a.size() && a.at(endA).isDigit())
                ++endA;
            while (endB < b.size() && b.at(endB).isDigit())
                ++endB;
            int startA = i;
            int startB = j;
            while (startA < endA - 1 && a.at(startA) == QLatin1Char('0'))
                ++startA;
            while (startB < endB - 1 && b.at(startB) == QLatin1Char('0'))
                ++startB;
            const int lengthA = endA - startA;
            const int lengthB = endB - startB;
            if (lengthA != lengthB)
                return lengthA < lengthB ? -1 : 1;
            const int cmp = QStringView(a).mid(startA, lengthA).compare(
                QStringView(b).mid(startB, lengthB));
            if (cmp != 0)
                return cmp;
            i = endA;
            j = endB;
            continue;
        }
        const QChar ca = a.at(i).toCaseFolded();
        const QChar cb = b.at(j).toCaseFolded();
        if (ca != cb)
            return ca < cb ? -1 : 1;
        ++i;
        ++j;
    }
    if (i < a.size())
        return 1;
    if (j < b.size())
        return -1;
    return 0;
}

} // namespace

DesktopIndex readDesktopIndex(const QString &desktopRoot, const QString &claudeHome,
                              const QHash<QString, QString> &knownTranscripts)
{
    DesktopIndex index;
    if (desktopRoot.isEmpty())
        return index;
    const QList<Scope> scopes = readScopes(desktopRoot);
    const QJsonObject config = readObject(
        QDir(desktopRoot).filePath(QStringLiteral("claude_desktop_config.json")), kConfigLimitBytes);
    const QJsonObject prefs = config.value(QStringLiteral("preferences"))
                                  .toObject()
                                  .value(QStringLiteral("epitaxyPrefs"))
                                  .toObject();
    QSet<QString> sectionIds;
    for (const Scope &scope : scopes) {
        for (DesktopRecord record : scope.records) {
            record.transcript = locateTranscript(record, claudeHome, knownTranscripts);
            if (!record.transcript.isEmpty())
                record.writtenMs = QFileInfo(record.transcript).lastModified().toMSecsSinceEpoch();
            index.records.append(record);
        }
        for (const DesktopSection &section : scopeSections(prefs, scope.name)) {
            if (sectionIds.contains(section.id)) {
                for (DesktopSection &kept : index.sections)
                    if (kept.id == section.id)
                        for (const QString &member : section.members)
                            if (!kept.members.contains(member))
                                kept.members.append(member);
                continue;
            }
            sectionIds.insert(section.id);
            index.sections.append(section);
        }
    }
    collectKeys(prefs.value(QStringLiteral("starred-local-code-sessions")), index.pinned);
    return index;
}

QString folderLabel(const QString &cwd)
{
    const QString clean = QDir::cleanPath(cwd.trimmed());
    if (clean.isEmpty() || clean == QLatin1String("."))
        return QString();
    const QString name = QFileInfo(clean).fileName();
    return name.isEmpty() ? clean : name;
}

bool naturalLess(const QString &a, const QString &b)
{
    const int cmp = compareRuns(a, b);
    return cmp != 0 ? cmp < 0 : a < b;
}

QList<SidebarGroup> arrangeSidebar(const DesktopIndex &index, const QList<SidebarItem> &items)
{
    QHash<QString, QList<int>> byRecord;
    for (int i = 0; i < items.size(); ++i)
        if (!items.at(i).recordKey.isEmpty())
            byRecord[items.at(i).recordKey].append(i);

    QList<DesktopSection> sections = index.sections;
    bool hasPinned = false;
    bool hasSessions = false;
    for (const DesktopSection &section : std::as_const(sections)) {
        hasPinned = hasPinned || section.kind == QLatin1String("pinned");
        hasSessions = hasSessions || section.kind == QLatin1String("sessions");
    }
    if (!hasPinned && !index.pinned.isEmpty())
        sections.prepend({QStringLiteral("pinned"), QStringLiteral("pinned"), QString(), {}});
    if (!hasSessions)
        sections.append({QStringLiteral("sessions"), QStringLiteral("sessions"), QString(), {}});

    QSet<int> claimed;
    QHash<QString, QList<int>> sectionItems;
    const auto claim = [&](const QString &sectionId, const QStringList &keys) {
        QList<int> &list = sectionItems[sectionId];
        for (const QString &key : keys)
            for (int item : byRecord.value(key))
                if (!claimed.contains(item)) {
                    claimed.insert(item);
                    list.append(item);
                }
    };
    for (const DesktopSection &section : std::as_const(sections))
        if (section.kind == QLatin1String("pinned"))
            claim(section.id, index.pinned);
    for (const DesktopSection &section : std::as_const(sections))
        if (section.kind == QLatin1String("manual"))
            claim(section.id, section.members);

    const auto byRecency = [&items](int a, int b) { return items.at(a).at > items.at(b).at; };
    QList<SidebarGroup> out;
    for (const DesktopSection &section : std::as_const(sections)) {
        if (section.kind == QLatin1String("sessions")) {
            QHash<QString, QList<int>> folders;
            for (int i = 0; i < items.size(); ++i)
                if (!claimed.contains(i))
                    folders[items.at(i).folder].append(i);
            QStringList labels = folders.keys();
            std::sort(labels.begin(), labels.end(), [](const QString &a, const QString &b) {
                if (a.isEmpty() != b.isEmpty())
                    return b.isEmpty();
                return naturalLess(a, b);
            });
            for (const QString &label : std::as_const(labels)) {
                SidebarGroup group;
                group.key = QStringLiteral("@folder:") + label;
                group.label = label.isEmpty() ? QStringLiteral("Other sessions") : label;
                group.items = folders.value(label);
                std::stable_sort(group.items.begin(), group.items.end(), byRecency);
                out.append(group);
            }
            continue;
        }
        const QList<int> list = sectionItems.value(section.id);
        if (list.isEmpty())
            continue;
        SidebarGroup group;
        group.key = QStringLiteral("@claude:") + section.id;
        group.label = section.kind == QLatin1String("pinned") ? QStringLiteral("Pinned")
                                                              : section.name;
        group.items = list;
        if (section.kind == QLatin1String("manual"))
            std::stable_sort(group.items.begin(), group.items.end(), byRecency);
        out.append(group);
    }
    return out;
}

} // namespace forkmesh::claudedesktop
