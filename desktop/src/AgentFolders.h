#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

struct AgentFolder {
    QString id;
    QString name;
    bool collapsed = false;
};

struct AgentFolderRow {
    QString folderId;
    int member = -1;
    bool header() const { return member < 0; }
};

class AgentFolders
{
public:
    static constexpr int kMaxNameLength = 48;

    static QString repoKey(const QString &owner, const QString &name);

    QJsonObject toJson() const;
    void fromJson(const QJsonObject &json);
    void load();
    void save() const;

    QList<AgentFolder> folders(const QString &repo) const;
    const AgentFolder *folder(const QString &repo, const QString &id) const;
    QString create(const QString &repo, const QString &name);
    QString findOrCreate(const QString &repo, const QString &name);
    bool rename(const QString &repo, const QString &id, const QString &name);
    bool remove(const QString &repo, const QString &id);
    bool setCollapsed(const QString &repo, const QString &id, bool collapsed);
    bool setAllCollapsed(const QString &repo, bool collapsed);
    bool move(const QString &repo, const QString &id, const QString &targetId,
              bool after);
    bool moveToEdge(const QString &repo, const QString &id, bool last);

    QString folderOf(const QString &repo, const QString &member) const;
    QStringList members(const QString &repo, const QString &folderId) const;
    bool assign(const QString &repo, const QString &member,
                const QString &folderId);
    int memberCount(const QString &repo, const QString &folderId,
                    const QStringList &members) const;

    QList<AgentFolderRow> layout(const QString &repo, const QStringList &members,
                                 bool filtering) const;

private:
    struct Repo {
        QList<AgentFolder> folders;
        QHash<QString, QString> members;
    };
    int indexOf(const QString &repo, const QString &id) const;
    QHash<QString, Repo> m_repos;
};
