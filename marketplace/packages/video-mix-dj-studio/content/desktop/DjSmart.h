#pragma once

#include <tuple>

#include "DjCore.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace forkmesh::djstudio {

struct DjPlaylist;

enum class DjSmartKind { Listening, Genre, Style, Decade, Artist };

struct DjSmartList {
    QString id;
    QString name;
    QString symbol;
    DjSmartKind kind = DjSmartKind::Listening;
    QList<DjTrack> songs;
    bool operator==(const DjSmartList &o) const { return std::tie(id, name, symbol, kind, songs) == std::tie(o.id, o.name, o.symbol, o.kind, o.songs); }
    bool operator!=(const DjSmartList &o) const { return !(*this == o); }
};

struct DjSimilarSong {
    DjTrack track;
    int score = 0;
    QStringList reasons;
    QString id() const { return track.id; }
    bool operator==(const DjSimilarSong &o) const { return std::tie(track, score, reasons) == std::tie(o.track, o.score, o.reasons); }
    bool operator!=(const DjSimilarSong &o) const { return !(*this == o); }
};

namespace smart {

inline constexpr int minimumSongs = 2;
inline constexpr int listeningLimit = 50;
inline constexpr int artistLimit = 30;
inline constexpr double rediscoverAfter = 30.0 * 24 * 60 * 60;

QList<DjSmartKind> kinds();
QString kindKey(DjSmartKind kind);
QString kindTitle(DjSmartKind kind);
QString text(const QString &raw);
QString artist(const DjTrack &track);
QString artistKey(const DjTrack &track);
std::optional<int> decade(const DjTrack &track);
QStringList categories(const DjTrack &track);
QList<DjSmartList> lists(const QList<DjTrack> &songs, const QList<DjPlayedSong> &library, double now);
QList<DjSimilarSong> similar(const DjTrack &seed, const QList<DjTrack> &songs);
QList<DjSimilarSong> similar(const DjTrack &seed, const QList<DjTrack> &songs,
                             const QList<DjPlaylist> &playlists, int limit = 25);
QString searchQuery(const DjTrack &seed);

}

}
