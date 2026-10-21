#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

class ForkMeshIdentity;

struct DiscussionEvent {
    QString type;
    QString id;
    QString author;
    QString authorName;
    qint64 ts = 0;
    QString title;
    QString category;
    QString target;
    QString targetAuthor;
    QString status; // "status" events: open | closed | archived
    QString body;
    QString sig;

    QJsonObject toJson() const;
    static DiscussionEvent fromJson(const QJsonObject &obj);
};

struct Discussion {
    int number = 0;
    QString title;
    QString category;
    QString status = "open"; // open | closed | archived
    qint64 createdAt = 0;
    QString author;
    QString authorName;
    QList<DiscussionEvent> events;
    DiscussionEvent answer;

    qint64 updatedAt() const;
    int commentCount() const;
    bool answerable() const;
    QString answerId() const;
};

class DiscussionStore
{
public:
    DiscussionStore(QString workTreePath, QString mirrorPath,
                    const ForkMeshIdentity *identity,
                    QString authorName = QString());

    bool canWrite() const;
    QList<Discussion> loadAll(QString *error = nullptr) const;

    int createDiscussion(const QString &title, const QString &body,
                         const QString &category, QString *error = nullptr);
    bool addComment(int number, const QString &body, QString *error = nullptr);
    bool deleteComment(int number, const QString &eventId, QString *error = nullptr);
    bool setStatus(int number, const QString &status, QString *error = nullptr);
    bool markAnswer(int number, const QString &commentId, QString *error = nullptr);
    bool mayMarkAnswer(const Discussion &discussion) const;
    bool deleteDiscussion(int number, QString *error = nullptr);
    bool applyRemoteEvent(int number, const DiscussionEvent &ev,
                          const QString &titleIfNew,
                          QString *error = nullptr,
                          bool ownerApproved = false);

    DiscussionEvent makeSignedEvent(int number, DiscussionEvent ev) const;
    static QByteArray canonicalString(int number, const DiscussionEvent &ev,
                                      const QString &repoKey = QString());
    void setSigningRepository(const QString &owner, const QString &name);
    static QString contentForSigning(const DiscussionEvent &ev);
    static QStringList categories();
    static QString normalizedCategory(const QString &category);
    static QString answerCategory();

private:
    QString discussionsDir() const;
    QString discussionDir(int number) const;
    int nextNumber() const;
    bool readDiscussionFile(int number, Discussion &out) const;
    bool writeDiscussionFile(const Discussion &discussion, QString *error) const;
    bool writeAnswerFile(int number, const DiscussionEvent &answer,
                         QString *error) const;
    bool applyAnswer(const Discussion &discussion, DiscussionEvent answer,
                     QString *error) const;
    bool commit(const QString &message, QString *error) const;
    QByteArray showFromMirror(const QString &repoRelPath, bool *ok) const;
    QString mirrorRef() const;
    QList<Discussion> loadFromMirror(QString *error) const;

    QString m_workTree;
    QString m_mirror;
    const ForkMeshIdentity *m_identity;
    QString m_authorName;
    QString m_signingRepoKey;
};
