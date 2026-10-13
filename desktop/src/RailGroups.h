#pragma once

#include "RailShortcuts.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

namespace RailGroups {

enum class SortMode { Custom, Name, Used, Count };

constexpr int kMaxNameLength = 32;
constexpr int kMaxCustomGroups = 32;
inline const QString kCustomPrefix = QStringLiteral("custom-");
inline const QString kSeparatorPrefix = QStringLiteral("group:");

struct BuiltIn {
    const char *id;
    const char *title;
    QStringList keys;
};

struct Group {
    QString id;
    QString name;
    bool builtIn = false;
};

struct State {
    QList<Group> groups;
    QHash<QString, QString> assigned;
};

inline QString sortModeKey(SortMode mode)
{
    switch (mode) {
    case SortMode::Name:
        return QStringLiteral("name");
    case SortMode::Used:
        return QStringLiteral("used");
    case SortMode::Count:
        return QStringLiteral("count");
    case SortMode::Custom:
        break;
    }
    return QStringLiteral("custom");
}

inline SortMode parseSortMode(const QString &key)
{
    if (key == QLatin1String("name"))
        return SortMode::Name;
    if (key == QLatin1String("used"))
        return SortMode::Used;
    if (key == QLatin1String("count"))
        return SortMode::Count;
    return SortMode::Custom;
}

inline QString sortModeTitle(SortMode mode)
{
    switch (mode) {
    case SortMode::Name:
        return QStringLiteral("Alphabetical");
    case SortMode::Used:
        return QStringLiteral("Most used");
    case SortMode::Count:
        return QStringLiteral("By count");
    case SortMode::Custom:
        break;
    }
    return QStringLiteral("Custom order");
}

inline const QList<BuiltIn> &builtInGroups()
{
    static const QList<BuiltIn> groups = {
        {"work", "Work",
         {QStringLiteral("agents"), QStringLiteral("code"),
          QStringLiteral("git"), QStringLiteral("repos"),
          QStringLiteral("tasks"), QStringLiteral("notes"),
          QStringLiteral("calendar"), QStringLiteral("files")}},
        {"people", "People",
         {QStringLiteral("chat"), QStringLiteral("notifications"),
          QStringLiteral("team"), QStringLiteral("users")}},
        {"network", "Network",
         {QStringLiteral("network"), QStringLiteral("controlNode"),
          QStringLiteral("market"), QStringLiteral("ledger")}},
        {"insights", "Insights",
         {QStringLiteral("stats"), QStringLiteral("componentHealth"),
          QStringLiteral("memory")}},
        {"packages", "Packages", {}},
        {"tools", "Tools",
         {QStringLiteral("home"), QStringLiteral("screenshot"),
          QStringLiteral("tour"), QStringLiteral("settings")}},
        {"shortcuts", "Shortcuts", {}},
        {"more", "More", {}},
    };
    return groups;
}

inline bool isBuiltIn(const QString &id)
{
    for (const BuiltIn &group : builtInGroups())
        if (id == QLatin1String(group.id))
            return true;
    return false;
}

inline QString defaultGroupOf(const QString &key)
{
    if (key.startsWith(QLatin1String("package:")))
        return QStringLiteral("packages");
    if (RailShortcuts::isShortcutKey(key))
        return QStringLiteral("shortcuts");
    if (key.startsWith(QLatin1String("repoTab:")) ||
        key.startsWith(QLatin1String("repoAction:")) ||
        key.startsWith(QLatin1String("repoCode:")))
        return QStringLiteral("work");
    for (const BuiltIn &group : builtInGroups())
        if (group.keys.contains(key))
            return QString::fromLatin1(group.id);
    return QStringLiteral("more");
}

inline QString normalizeName(const QString &name)
{
    QString normalized = name.simplified();
    if (normalized.size() > kMaxNameLength)
        normalized.truncate(kMaxNameLength);
    return normalized;
}

inline QString separatorKey(const QString &id)
{
    return kSeparatorPrefix + id;
}

inline int indexOf(const State &state, const QString &id)
{
    for (int i = 0; i < state.groups.size(); ++i)
        if (state.groups.at(i).id == id)
            return i;
    return -1;
}

inline QString assignedGroupOf(const State &state, const QString &key)
{
    const QString id = state.assigned.value(key);
    return !id.isEmpty() && indexOf(state, id) >= 0 ? id : QString();
}

inline QString groupOf(const State &state, const QString &key)
{
    const QString assigned = assignedGroupOf(state, key);
    return assigned.isEmpty() ? defaultGroupOf(key) : assigned;
}

inline int customCount(const State &state)
{
    int count = 0;
    for (const Group &group : state.groups)
        count += group.builtIn ? 0 : 1;
    return count;
}

inline State parse(const QString &json)
{
    State state;
    const QJsonObject root = QJsonDocument::fromJson(json.toUtf8()).object();
    QSet<QString> ids;
    for (const QJsonValue &value : root.value(QStringLiteral("groups")).toArray()) {
        const QJsonObject object = value.toObject();
        Group group;
        group.id = object.value(QStringLiteral("id")).toString().trimmed();
        if (group.id.isEmpty() || ids.contains(group.id))
            continue;
        group.builtIn = isBuiltIn(group.id);
        if (group.builtIn) {
            for (const BuiltIn &builtIn : builtInGroups())
                if (group.id == QLatin1String(builtIn.id))
                    group.name = QString::fromLatin1(builtIn.title);
        } else {
            if (!group.id.startsWith(kCustomPrefix) ||
                customCount(state) >= kMaxCustomGroups)
                continue;
            group.name =
                normalizeName(object.value(QStringLiteral("name")).toString());
            if (group.name.isEmpty())
                group.name = QStringLiteral("Group");
        }
        ids.insert(group.id);
        state.groups.append(group);
    }
    for (const BuiltIn &builtIn : builtInGroups()) {
        const QString id = QString::fromLatin1(builtIn.id);
        if (!ids.contains(id))
            state.groups.append(Group{id, QString::fromLatin1(builtIn.title), true});
    }
    const QJsonObject assigned = root.value(QStringLiteral("assigned")).toObject();
    for (auto it = assigned.constBegin(); it != assigned.constEnd(); ++it) {
        const QString id = it.value().toString();
        if (!it.key().isEmpty() && indexOf(state, id) >= 0 &&
            id != defaultGroupOf(it.key()))
            state.assigned.insert(it.key(), id);
    }
    return state;
}

inline QString serialize(const State &state)
{
    QJsonArray groups;
    for (const Group &group : state.groups) {
        QJsonObject object{{QStringLiteral("id"), group.id}};
        if (!group.builtIn)
            object.insert(QStringLiteral("name"), group.name);
        groups.append(object);
    }
    QJsonObject assigned;
    for (auto it = state.assigned.constBegin(); it != state.assigned.constEnd();
         ++it)
        assigned.insert(it.key(), it.value());
    return QString::fromUtf8(
        QJsonDocument(QJsonObject{{QStringLiteral("groups"), groups},
                                  {QStringLiteral("assigned"), assigned}})
            .toJson(QJsonDocument::Compact));
}

inline bool isDefault(const State &state)
{
    if (!state.assigned.isEmpty() || customCount(state) > 0)
        return false;
    const QList<BuiltIn> &builtIns = builtInGroups();
    if (state.groups.size() != builtIns.size())
        return false;
    for (int i = 0; i < builtIns.size(); ++i)
        if (state.groups.at(i).id != QLatin1String(builtIns.at(i).id))
            return false;
    return true;
}

inline QString create(State &state, const QString &name)
{
    const QString normalized = normalizeName(name);
    if (normalized.isEmpty() || customCount(state) >= kMaxCustomGroups)
        return QString();
    int next = 1;
    while (indexOf(state, kCustomPrefix + QString::number(next)) >= 0)
        ++next;
    const QString id = kCustomPrefix + QString::number(next);
    state.groups.prepend(Group{id, normalized, false});
    return id;
}

inline bool rename(State &state, const QString &id, const QString &name)
{
    const int index = indexOf(state, id);
    const QString normalized = normalizeName(name);
    if (index < 0 || state.groups.at(index).builtIn || normalized.isEmpty() ||
        state.groups.at(index).name == normalized)
        return false;
    state.groups[index].name = normalized;
    return true;
}

inline bool remove(State &state, const QString &id)
{
    const int index = indexOf(state, id);
    if (index < 0 || state.groups.at(index).builtIn)
        return false;
    state.groups.removeAt(index);
    for (auto it = state.assigned.begin(); it != state.assigned.end();) {
        if (it.value() == id)
            it = state.assigned.erase(it);
        else
            ++it;
    }
    return true;
}

inline bool assign(State &state, const QString &key, const QString &groupId)
{
    if (key.isEmpty() || (!groupId.isEmpty() && indexOf(state, groupId) < 0))
        return false;
    const QString target =
        groupId.isEmpty() || groupId == defaultGroupOf(key) ? QString() : groupId;
    if (state.assigned.value(key) == target)
        return false;
    if (target.isEmpty())
        state.assigned.remove(key);
    else
        state.assigned.insert(key, target);
    return true;
}

inline bool moveGroup(State &state, const QString &id, int step,
                      const QSet<QString> &shown = {})
{
    const int from = indexOf(state, id);
    if (from < 0 || step == 0)
        return false;
    const int direction = step < 0 ? -1 : 1;
    int to = from + direction;
    while (to >= 0 && to < state.groups.size() && !shown.isEmpty() &&
           !shown.contains(state.groups.at(to).id))
        to += direction;
    if (to < 0 || to >= state.groups.size())
        return false;
    state.groups.move(from, to);
    return true;
}

}
