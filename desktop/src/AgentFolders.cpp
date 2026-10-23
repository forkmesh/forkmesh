#include "AgentFolders.h"

#include "SettingsWrite.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <iterator>
#include <QSettings>
#include <QUuid>

namespace {

const QString kAgentFoldersSetting = QStringLiteral("agents/folders");

QString normalizedName(const QString &name)
{
    return name.simplified().left(AgentFolders::kMaxNameLength);
}

}

QString AgentFolders::repoKey(const QString &owner, const QString &name)
{
    return owner + QLatin1Char('/') + name;
}

QJsonObject AgentFolders::toJson() const
{
    QJsonObject json;
    for (auto it = m_repos.cbegin(); it != m_repos.cend(); ++it) {
        if (it->folders.isEmpty())
            continue;
        QJsonArray folders;
        for (const AgentFolder &folder : it->folders)
            folders.append(QJsonObject{{QStringLiteral("id"), folder.id},
                                       {QStringLiteral("name"), folder.name},
                                       {QStringLiteral("collapsed"),
                                        folder.collapsed}});
        QJsonObject members;
        for (auto m = it->members.cbegin(); m != it->members.cend(); ++m)
            members.insert(m.key(), m.value());
        json.insert(it.key(),
                    QJsonObject{{QStringLiteral("folders"), folders},
                                {QStringLiteral("members"), members}});
    }
    return json;
}

void AgentFolders::fromJson(const QJsonObject &json)
{
    m_repos.clear();
    for (auto it = json.constBegin(); it != json.constEnd(); ++it) {
        const QJsonObject entry = it.value().toObject();
        Repo repo;
        QSet<QString> ids;
        for (const QJsonValue &value :
             entry.value(QStringLiteral("folders")).toArray()) {
            const QJsonObject object = value.toObject();
            AgentFolder folder;
            folder.id = object.value(QStringLiteral("id")).toString();
            folder.name =
                normalizedName(object.value(QStringLiteral("name")).toString());
            folder.collapsed =
                object.value(QStringLiteral("collapsed")).toBool();
            if (folder.id.isEmpty() || ids.contains(folder.id))
                continue;
            if (folder.name.isEmpty())
                folder.name = QStringLiteral("Folder");
            ids.insert(folder.id);
            repo.folders.append(folder);
        }
        const QJsonObject members =
            entry.value(QStringLiteral("members")).toObject();
        for (auto m = members.constBegin(); m != members.constEnd(); ++m)
            if (ids.contains(m.value().toString()))
                repo.members.insert(m.key(), m.value().toString());
        if (!repo.folders.isEmpty())
            m_repos.insert(it.key(), repo);
    }
}

void AgentFolders::load()
{
    fromJson(QJsonDocument::fromJson(
                 QSettings().value(kAgentFoldersSetting).toByteArray())
                 .object());
}

void AgentFolders::save() const
{
    const QJsonObject json = toJson();
    QSettings settings;
    if (json.isEmpty())
        forkmesh::settings::drop(settings, kAgentFoldersSetting);
    else
        forkmesh::settings::put(
            settings, kAgentFoldersSetting,
            QJsonDocument(json).toJson(QJsonDocument::Compact));
}

int AgentFolders::indexOf(const QString &repo, const QString &id) const
{
    const auto it = m_repos.constFind(repo);
    if (it == m_repos.cend() || id.isEmpty())
        return -1;
    for (int i = 0; i < it->folders.size(); ++i)
        if (it->folders.at(i).id == id)
            return i;
    return -1;
}

QList<AgentFolder> AgentFolders::folders(const QString &repo) const
{
    return m_repos.value(repo).folders;
}

const AgentFolder *AgentFolders::folder(const QString &repo,
                                        const QString &id) const
{
    const int index = indexOf(repo, id);
    return index < 0 ? nullptr : &m_repos.find(repo)->folders.at(index);
}

QString AgentFolders::create(const QString &repo, const QString &name)
{
    const QString trimmed = normalizedName(name);
    if (trimmed.isEmpty())
        return QString();
    AgentFolder folder;
    folder.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    folder.name = trimmed;
    m_repos[repo].folders.append(folder);
    return folder.id;
}

QString AgentFolders::findOrCreate(const QString &repo, const QString &name)
{
    const QString trimmed = normalizedName(name);
    if (trimmed.isEmpty())
        return QString();
    for (const AgentFolder &folder : m_repos.value(repo).folders)
        if (folder.name.compare(trimmed, Qt::CaseInsensitive) == 0)
            return folder.id;
    return create(repo, trimmed);
}

bool AgentFolders::rename(const QString &repo, const QString &id,
                          const QString &name)
{
    const int index = indexOf(repo, id);
    const QString trimmed = normalizedName(name);
    if (index < 0 || trimmed.isEmpty())
        return false;
    AgentFolder &folder = m_repos[repo].folders[index];
    if (folder.name == trimmed)
        return false;
    folder.name = trimmed;
    return true;
}

bool AgentFolders::remove(const QString &repo, const QString &id)
{
    const int index = indexOf(repo, id);
    if (index < 0)
        return false;
    Repo &entry = m_repos[repo];
    entry.folders.removeAt(index);
    for (auto it = entry.members.begin(); it != entry.members.end();)
        it = it.value() == id ? entry.members.erase(it) : std::next(it);
    if (entry.folders.isEmpty())
        m_repos.remove(repo);
    return true;
}

bool AgentFolders::setCollapsed(const QString &repo, const QString &id,
                                bool collapsed)
{
    const int index = indexOf(repo, id);
    if (index < 0)
        return false;
    AgentFolder &folder = m_repos[repo].folders[index];
    if (folder.collapsed == collapsed)
        return false;
    folder.collapsed = collapsed;
    return true;
}

bool AgentFolders::setAllCollapsed(const QString &repo, bool collapsed)
{
    auto it = m_repos.find(repo);
    if (it == m_repos.end())
        return false;
    bool changed = false;
    for (AgentFolder &folder : it->folders) {
        changed |= folder.collapsed != collapsed;
        folder.collapsed = collapsed;
    }
    return changed;
}

bool AgentFolders::move(const QString &repo, const QString &id,
                        const QString &targetId, bool after)
{
    const int from = indexOf(repo, id);
    if (from < 0 || indexOf(repo, targetId) < 0 || id == targetId)
        return false;
    QList<AgentFolder> &folders = m_repos[repo].folders;
    const QList<AgentFolder> before = folders;
    const AgentFolder moving = folders.takeAt(from);
    int to = 0;
    while (to < folders.size() && folders.at(to).id != targetId)
        ++to;
    folders.insert(after ? to + 1 : to, moving);
    for (int i = 0; i < folders.size(); ++i)
        if (folders.at(i).id != before.at(i).id)
            return true;
    return false;
}

bool AgentFolders::moveToEdge(const QString &repo, const QString &id, bool last)
{
    const int from = indexOf(repo, id);
    if (from < 0)
        return false;
    QList<AgentFolder> &folders = m_repos[repo].folders;
    const int to = last ? folders.size() - 1 : 0;
    if (from == to)
        return false;
    folders.move(from, to);
    return true;
}

QString AgentFolders::folderOf(const QString &repo, const QString &member) const
{
    const auto it = m_repos.constFind(repo);
    if (it == m_repos.cend() || member.isEmpty())
        return QString();
    return it->members.value(member);
}

QStringList AgentFolders::members(const QString &repo,
                                  const QString &folderId) const
{
    QStringList keys;
    const auto it = m_repos.constFind(repo);
    if (it == m_repos.cend() || folderId.isEmpty())
        return keys;
    for (auto m = it->members.cbegin(); m != it->members.cend(); ++m)
        if (m.value() == folderId)
            keys << m.key();
    keys.sort();
    return keys;
}

bool AgentFolders::assign(const QString &repo, const QString &member,
                          const QString &folderId)
{
    if (member.isEmpty())
        return false;
    if (folderId.isEmpty()) {
        const auto it = m_repos.find(repo);
        return it != m_repos.end() && it->members.remove(member) > 0;
    }
    if (indexOf(repo, folderId) < 0)
        return false;
    Repo &entry = m_repos[repo];
    if (entry.members.value(member) == folderId)
        return false;
    entry.members.insert(member, folderId);
    return true;
}

int AgentFolders::memberCount(const QString &repo, const QString &folderId,
                              const QStringList &members) const
{
    int count = 0;
    for (const QString &member : members)
        if (!member.isEmpty() && folderOf(repo, member) == folderId)
            ++count;
    return count;
}

QList<AgentFolderRow> AgentFolders::layout(const QString &repo,
                                           const QStringList &members,
                                           bool filtering) const
{
    QList<AgentFolderRow> rows;
    const auto it = m_repos.constFind(repo);
    QHash<QString, QList<int>> grouped;
    QList<int> loose;
    for (int i = 0; i < members.size(); ++i) {
        const QString folderId =
            it == m_repos.cend() ? QString() : it->members.value(members.at(i));
        if (!folderId.isEmpty() && indexOf(repo, folderId) >= 0)
            grouped[folderId].append(i);
        else
            loose.append(i);
    }
    if (it != m_repos.cend()) {
        for (const AgentFolder &folder : it->folders) {
            const QList<int> inside = grouped.value(folder.id);
            if (filtering && inside.isEmpty())
                continue;
            rows.append({folder.id, -1});
            if (folder.collapsed && !filtering)
                continue;
            for (const int member : inside)
                rows.append({folder.id, member});
        }
    }
    for (const int member : std::as_const(loose))
        rows.append({QString(), member});
    return rows;
}
