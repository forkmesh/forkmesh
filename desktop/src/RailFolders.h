#pragma once

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

namespace RailFolders {

constexpr int kMaxNameLength = 32;
constexpr int kMaxFolders = 32;
inline const QString kKeyPrefix = QStringLiteral("folder:");

struct Folder {
    QString id;
    QString name;
    QStringList members;
};

using State = QList<Folder>;

inline QString normalizeName(const QString &name)
{
    QString normalized = name.simplified();
    if (normalized.size() > kMaxNameLength)
        normalized.truncate(kMaxNameLength);
    return normalized;
}

inline QString railKey(const QString &id)
{
    return kKeyPrefix + id;
}

inline bool isFolderKey(const QString &key)
{
    return key.startsWith(kKeyPrefix);
}

inline int indexOf(const State &state, const QString &id)
{
    for (int i = 0; i < state.size(); ++i)
        if (state.at(i).id == id)
            return i;
    return -1;
}

inline QString folderOf(const State &state, const QString &key)
{
    if (key.isEmpty())
        return QString();
    for (const Folder &folder : state)
        if (folder.members.contains(key))
            return folder.id;
    return QString();
}

inline State parse(const QString &json)
{
    State state;
    const QJsonArray folders =
        QJsonDocument::fromJson(json.toUtf8()).array();
    QSet<QString> ids;
    QSet<QString> seen;
    for (const QJsonValue &value : folders) {
        if (state.size() >= kMaxFolders)
            break;
        const QJsonObject object = value.toObject();
        Folder folder;
        folder.id = object.value(QStringLiteral("id")).toString().trimmed();
        folder.name = normalizeName(object.value(QStringLiteral("name")).toString());
        if (folder.id.isEmpty() || ids.contains(folder.id))
            continue;
        if (folder.name.isEmpty())
            folder.name = QStringLiteral("Folder");
        for (const QJsonValue &member :
             object.value(QStringLiteral("members")).toArray()) {
            const QString key = member.toString();
            if (key.isEmpty() || isFolderKey(key) || seen.contains(key))
                continue;
            seen.insert(key);
            folder.members.append(key);
        }
        ids.insert(folder.id);
        state.append(folder);
    }
    return state;
}

inline QString serialize(const State &state)
{
    QJsonArray folders;
    for (const Folder &folder : state) {
        folders.append(QJsonObject{
            {QStringLiteral("id"), folder.id},
            {QStringLiteral("name"), folder.name},
            {QStringLiteral("members"), QJsonArray::fromStringList(folder.members)},
        });
    }
    return QString::fromUtf8(
        QJsonDocument(folders).toJson(QJsonDocument::Compact));
}

inline QString create(State &state, const QString &name)
{
    const QString normalized = normalizeName(name);
    if (normalized.isEmpty() || state.size() >= kMaxFolders)
        return QString();
    int next = 1;
    while (indexOf(state, QString::number(next)) >= 0)
        ++next;
    const QString id = QString::number(next);
    state.append(Folder{id, normalized, {}});
    return id;
}

inline bool rename(State &state, const QString &id, const QString &name)
{
    const int index = indexOf(state, id);
    const QString normalized = normalizeName(name);
    if (index < 0 || normalized.isEmpty() || state.at(index).name == normalized)
        return false;
    state[index].name = normalized;
    return true;
}

inline bool remove(State &state, const QString &id)
{
    const int index = indexOf(state, id);
    if (index < 0)
        return false;
    state.removeAt(index);
    return true;
}

inline bool move(State &state, const QString &key, const QString &folderId)
{
    if (key.isEmpty() || isFolderKey(key))
        return false;
    const int target = folderId.isEmpty() ? -1 : indexOf(state, folderId);
    if (!folderId.isEmpty() && target < 0)
        return false;
    if (folderOf(state, key) == folderId)
        return false;
    for (Folder &folder : state)
        folder.members.removeAll(key);
    if (target >= 0)
        state[target].members.append(key);
    return true;
}

}
