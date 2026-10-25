#pragma once

#include <tuple>

#include "DjCore.h"

#include <QByteArray>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <optional>

namespace forkmesh::djstudio {

struct DjAutoDjCandidate {
    QString id;
    QString title;
    QString author;
    double seconds = 0;
    double views = 0;
    std::optional<double> likes;
    int rank = 0;
    bool operator==(const DjAutoDjCandidate &o) const { return std::tie(id, title, author, seconds, views, likes, rank) == std::tie(o.id, o.title, o.author, o.seconds, o.views, o.likes, o.rank); }
    bool operator!=(const DjAutoDjCandidate &o) const { return !(*this == o); }
    DjSearchResult result() const { return DjSearchResult{id, title, author, seconds}; }
};

struct DjAutoDjLikes {
    double likes = 0;
    double views = 0;
    bool operator==(const DjAutoDjLikes &o) const { return std::tie(likes, views) == std::tie(o.likes, o.views); }
    bool operator!=(const DjAutoDjLikes &o) const { return !(*this == o); }
};

namespace autodj {

inline constexpr double shortest = 60;
inline constexpr double longest = 12 * 60;
inline constexpr int checked = 6;
inline constexpr double likeShare = 0.01;
inline constexpr int sessionLimit = 200;

QList<DjAutoDjCandidate> candidates(const QByteArray &data);
double count(const QString &text);
QUrl likesUrl(const QUrl &instance, const QString &id);
std::optional<DjAutoDjLikes> likes(const QByteArray &data);
QString songKey(const QString &title);
bool isSong(const DjAutoDjCandidate &candidate);
bool isOriginal(const DjAutoDjCandidate &candidate);
QList<DjAutoDjCandidate> eligible(const QList<DjAutoDjCandidate> &found, const DjTrack &seed,
                                  const QSet<QString> &avoid, const QStringList &heard);
double score(const DjAutoDjCandidate &candidate, int among);
QList<DjAutoDjCandidate> ranked(const QList<DjAutoDjCandidate> &found);
std::optional<DjAutoDjCandidate> next(const QList<DjAutoDjCandidate> &ranked, const QSet<QString> &loaded,
                                      const QSet<QString> &skip, const QStringList &played);
QStringList remember(const QString &id, const QStringList &played);

}

}
