#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>

class ForkMeshIdentity;

// One entry in a project's append-only, signed history. Different event types
// use different fields. Signatures are raw Ed25519 over an explicit canonical
// string, matching the worker's ed25519_verify.
struct ProjectEvent {
    QString type;          // open | title | edit | status | dates | milestone | issues | delete
    QString id;
    QString author;        // signer pubkey (base64url)
    QString authorName;
    qint64 ts = 0;
    QString title;         // open/title
    QString body;          // open/edit description
    QString status;        // status: open|closed
    qint64 startDate = 0;  // dates: planned start (epoch ms, 0 = unset)
    qint64 endDate = 0;    // dates: planned end (epoch ms, 0 = unset)
    QString milestone;     // milestone (empty = none)
    QList<int> issues;     // issues: linked issue numbers
    QString target;        // delete ("self" tombstones the project)
    QString sig;

    QJsonObject toJson() const;
    static ProjectEvent fromJson(const QJsonObject &obj);
};

// A single project: derived metadata plus the full signed event log.
struct Project {
    int number = 0;
    QString title;
    QString body;
    QString status = "open";
    qint64 startDate = 0; // planned start (epoch ms, 0 = unset)
    qint64 endDate = 0;   // planned end (epoch ms, 0 = unset)
    QString milestone;    // linked milestone (drives overall progress)
    QList<int> issues;    // linked issue numbers
    qint64 createdAt = 0;
    QString author;
    QString authorName;
    QList<ProjectEvent> events;

    QJsonObject toJson() const;
    static Project fromJson(const QJsonObject &obj);
    bool isDeleted() const; // a delete event targeting "self" tombstones it
};

class ProjectStore
{
public:
    ProjectStore(QString workTreePath, QString mirrorPath,
                 const ForkMeshIdentity *identity, QString authorName = QString());

    bool canWrite() const;

    QList<Project> loadAll(QString *error = nullptr) const;

    int createProject(const QString &title, const QString &body,
                      qint64 startDate, qint64 endDate, const QString &milestone,
                      const QList<int> &issues, QString *error = nullptr);
    bool setTitle(int number, const QString &newTitle, QString *error = nullptr);
    bool setBody(int number, const QString &newBody, QString *error = nullptr);
    bool setStatus(int number, const QString &status, QString *error = nullptr);
    bool setDates(int number, qint64 startDate, qint64 endDate,
                  QString *error = nullptr);
    bool setMilestone(int number, const QString &milestone, QString *error = nullptr);
    bool setIssues(int number, const QList<int> &issues, QString *error = nullptr);
    bool tombstoneProject(int number, QString *error = nullptr);

    // Build a signed event with the node identity. Fills id/author/authorName/ts/sig.
    ProjectEvent makeSignedEvent(int number, ProjectEvent ev) const;

    // The exact bytes an event's signature commits to. Public + static so it can
    // be unit-tested and kept byte-identical to the worker's verifier.
    static QByteArray canonicalString(int number, const ProjectEvent &ev);
    static QString contentForSigning(const ProjectEvent &ev);

private:
    QString projectsDir() const;                // <workTree>/.forkmesh/projects
    QString projectDir(int number) const;       // <workTree>/.forkmesh/projects/<n>
    QString projectFilePath(int number) const;  // .../<n>/project-<n>.json
    bool readProjectFile(int number, Project &out) const;
    bool writeProjectFile(const Project &project, QString *error) const;
    void recomputeMetadata(Project &project) const; // fold events into fields
    int nextNumber() const;
    bool commit(const QString &message, QString *error) const;

    QList<Project> loadFromMirror(QString *error) const;
    QString mirrorRef() const;

    QString m_workTree;
    QString m_mirror;
    const ForkMeshIdentity *m_identity;
    QString m_authorName;

    mutable bool m_mirrorRefResolved = false;
    mutable QString m_cachedMirrorRef;
};
