#include "DjSmart.h"
#include "DjCrate.h"

#include <QHash>
#include <QMap>
#include <QSet>

#include <algorithm>

namespace forkmesh::djstudio::smart {

namespace {

struct Rule {
    QString id;
    QString name;
    QString symbol;
    QStringList words;
};

Rule rule(const char *id, const char *name, const char *symbol, std::initializer_list<const char *> words)
{
    Rule out{QString::fromUtf8(id), QString::fromUtf8(name), QString::fromUtf8(symbol), {}};
    for (const char *word : words)
        out.words.append(QString::fromUtf8(word));
    return out;
}

const QList<Rule> &genres()
{
    static const QList<Rule> rules{
        rule("house", "House", "house", {"house", "deep house", "tech house", "afro house", "progressive house"}),
        rule("techno", "Techno", "waveform.path", {"techno", "hard techno", "minimal"}),
        rule("trance", "Trance", "sparkles", {"trance", "psytrance", "uplifting"}),
        rule("dnb", "Drum & Bass", "metronome", {"drum & bass", "drum and bass", "drum n bass", "dnb", "d&b", "jungle"}),
        rule("dubstep", "Dubstep & Bass", "speaker.wave.3", {"dubstep", "riddim", "bass music", "brostep", "future bass"}),
        rule("edm", "EDM & Electro", "bolt", {"edm", "big room", "electro", "electro house", "festival", "hardstyle"}),
        rule("garage", "UK Garage & Breaks", "square.stack.3d.up", {"garage", "ukg", "2 step", "breakbeat", "breaks"}),
        rule("hiphop", "Hip-Hop & Rap", "mic", {"hip hop", "hiphop", "rap", "trap", "drill", "boom bap", "grime"}),
        rule("rnb", "R&B & Soul", "heart", {"r&b", "rnb", "soul", "neo soul", "motown"}),
        rule("pop", "Pop", "star", {"pop", "synthpop", "dance pop", "europop"}),
        rule("kpop", "K-Pop", "star.circle", {"k pop", "kpop"}),
        rule("rock", "Rock & Metal", "guitars", {"rock", "metal", "punk", "grunge", "emo", "alternative"}),
        rule("chill", "Chill & Lo-fi", "moon.stars",
             {"lofi", "lo fi", "chill", "chillhop", "chillout", "ambient", "relax", "relaxing", "study", "sleep"}),
        rule("latin", "Latin", "sun.max",
             {"reggaeton", "latin", "bachata", "salsa", "cumbia", "dembow", "merengue", "latino"}),
        rule("afro", "Afrobeats & Amapiano", "globe.europe.africa", {"afrobeats", "afrobeat", "amapiano", "afro"}),
        rule("reggae", "Reggae & Dancehall", "leaf", {"reggae", "dancehall", "ska", "dub"}),
        rule("disco", "Disco & Funk", "circle.hexagongrid", {"disco", "nu disco", "funk", "funky", "boogie"}),
        rule("jazz", "Jazz & Blues", "music.quarternote.3", {"jazz", "blues", "swing", "bossa nova"}),
        rule("classical", "Classical", "pianokeys",
             {"classical", "symphony", "sonata", "concerto", "orchestra", "beethoven", "mozart", "bach", "chopin"}),
        rule("country", "Country & Folk", "music.mic", {"country", "folk", "bluegrass", "americana"}),
    };
    return rules;
}

const QList<Rule> &styles()
{
    static const QList<Rule> rules{
        rule("remix", "Remixes & Edits", "slider.horizontal.3",
             {"remix", "edit", "bootleg", "flip", "vip", "rework", "mashup", "extended mix", "club mix"}),
        rule("sets", "DJ Sets & Mixes", "record.circle",
             {"dj set", "live set", "boiler room", "full set", "mixtape", "megamix", "essential mix", "b2b"}),
        rule("live", "Live Performances", "music.mic.circle", {"live", "concert", "tiny desk", "session", "unplugged"}),
        rule("acoustic", "Acoustic & Covers", "guitars.fill", {"acoustic", "cover", "piano version"}),
        rule("speed", "Sped Up & Slowed", "gauge.with.dots.needle.33percent", {"sped up", "slowed", "nightcore", "reverb"}),
        rule("videos", "Official Videos", "play.rectangle", {"official video", "official music video", "music video"}),
        rule("lyrics", "Lyric Videos", "text.quote", {"lyrics", "lyric video"}),
    };
    return rules;
}

QStringList words(const QString &text)
{
    QStringList parts;
    QString part;
    for (const QChar c : text) {
        if (c.isSpace()) {
            if (!part.isEmpty()) {
                parts.append(part);
                part.clear();
            }
        } else {
            part.append(c);
        }
    }
    if (!part.isEmpty())
        parts.append(part);
    return parts;
}

QString haystack(const DjTrack &track) { return text((QStringList{track.title} + track.tags).join(QLatin1Char(' '))); }

bool matches(const Rule &rule, const QString &haystack)
{
    return std::any_of(rule.words.begin(), rule.words.end(), [&](const QString &word) {
        return haystack.contains(QLatin1Char(' ') + word + QLatin1Char(' '));
    });
}

}

QList<DjSmartKind> kinds()
{
    return {DjSmartKind::Listening, DjSmartKind::Genre, DjSmartKind::Style, DjSmartKind::Decade, DjSmartKind::Artist};
}

QString kindKey(DjSmartKind kind)
{
    switch (kind) {
    case DjSmartKind::Listening:
        return QStringLiteral("listening");
    case DjSmartKind::Genre:
        return QStringLiteral("genre");
    case DjSmartKind::Style:
        return QStringLiteral("style");
    case DjSmartKind::Decade:
        return QStringLiteral("decade");
    case DjSmartKind::Artist:
        return QStringLiteral("artist");
    }
    return QString();
}

QString kindTitle(DjSmartKind kind)
{
    switch (kind) {
    case DjSmartKind::Listening:
        return QStringLiteral("Listening");
    case DjSmartKind::Genre:
        return QStringLiteral("Genres");
    case DjSmartKind::Style:
        return QStringLiteral("Styles");
    case DjSmartKind::Decade:
        return QStringLiteral("Decades");
    case DjSmartKind::Artist:
        return QStringLiteral("Artists");
    }
    return QString();
}

QString text(const QString &raw)
{
    QString spaced;
    for (const char32_t u : raw.toLower().toUcs4()) {
        if (QChar::isLetterOrNumber(u) || QChar::isMark(u) || u == U'&')
            spaced.append(QString::fromUcs4(&u, 1));
        else
            spaced.append(QLatin1Char(' '));
    }
    return QLatin1Char(' ') + spaced.split(QLatin1Char(' '), Qt::SkipEmptyParts).join(QLatin1Char(' ')) +
           QLatin1Char(' ');
}

QString artist(const DjTrack &track)
{
    QString name = track.author.trimmed();
    static const QStringList suffixes{QStringLiteral(" - Topic"), QStringLiteral("VEVO"), QStringLiteral(" Official"),
                                      QStringLiteral(" Music")};
    for (const QString &suffix : suffixes) {
        if (crate::length(name) > suffix.size() && name.toLower().endsWith(suffix.toLower())) {
            name.chop(suffix.size());
            name = name.trimmed();
        }
    }
    if (name.isEmpty()) {
        const qsizetype dash = track.title.indexOf(QLatin1String(" - "));
        if (dash >= 0) {
            const QString lead = track.title.left(dash).trimmed();
            if (crate::length(lead) <= 40)
                name = lead;
        }
    }
    return name;
}

QString artistKey(const DjTrack &track) { return text(artist(track)).trimmed(); }

std::optional<int> decade(const DjTrack &track)
{
    static const QHash<QString, int> shortForms{
        {QStringLiteral("50s"), 1950},   {QStringLiteral("60s"), 1960},   {QStringLiteral("70s"), 1970},
        {QStringLiteral("80s"), 1980},   {QStringLiteral("90s"), 1990},   {QStringLiteral("00s"), 2000},
        {QStringLiteral("2000s"), 2000}, {QStringLiteral("2010s"), 2010}, {QStringLiteral("2020s"), 2020}};
    for (const QString &word : haystack(track).split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        const bool digits = word.size() == 4 && std::all_of(word.begin(), word.end(), [](QChar c) {
                                return c.unicode() >= '0' && c.unicode() <= '9';
                            });
        if (digits) {
            const int year = word.toInt();
            if (year >= 1950 && year <= 2029)
                return year / 10 * 10;
        }
        const auto found = shortForms.constFind(word);
        if (found != shortForms.cend())
            return *found;
    }
    return std::nullopt;
}

QStringList categories(const DjTrack &track)
{
    const QString words = haystack(track);
    QStringList found;
    for (const Rule &rule : genres()) {
        if (matches(rule, words))
            found.append(QStringLiteral("genre:") + rule.id);
    }
    for (const Rule &rule : styles()) {
        if (matches(rule, words))
            found.append(QStringLiteral("style:") + rule.id);
    }
    if (const auto year = decade(track))
        found.append(QStringLiteral("decade:%1").arg(*year));
    return found;
}

QList<DjSmartList> lists(const QList<DjTrack> &input, const QList<DjPlayedSong> &library, double now)
{
    QSet<QString> seen;
    QList<DjTrack> songs;
    for (const DjTrack &song : input) {
        if (!studio::isVideoId(song.id) || seen.contains(song.id))
            continue;
        seen.insert(song.id);
        songs.append(song);
    }
    QHash<QString, DjTrack> known;
    for (const DjTrack &song : songs)
        known.insert(song.id, song);
    QList<DjPlayedSong> played;
    for (const DjPlayedSong &song : library) {
        if (known.contains(song.id))
            played.append(song);
    }
    QHash<QString, int> plays;
    for (const DjPlayedSong &song : played) {
        const auto found = plays.find(song.id);
        if (found == plays.end())
            plays.insert(song.id, song.plays);
        else
            *found = std::max(*found, song.plays);
    }
    QList<DjSmartList> result;
    const auto add = [&](const QString &id, const QString &name, const QString &symbol, DjSmartKind kind,
                         const QList<DjTrack> &tracks) {
        if (tracks.size() < minimumSongs)
            return;
        result.append(DjSmartList{id, name, symbol, kind, tracks});
    };
    const auto tracksOf = [&](const QList<int> &order, int limit) {
        QList<DjTrack> out;
        for (int i = 0; i < order.size() && i < limit; ++i) {
            const auto found = known.constFind(played.at(order.at(i)).id);
            if (found != known.cend())
                out.append(*found);
        }
        return out;
    };

    QList<int> most;
    QList<int> recent;
    for (int i = 0; i < played.size(); ++i) {
        if (played.at(i).plays > 1)
            most.append(i);
        if (played.at(i).plays > 0)
            recent.append(i);
    }
    std::sort(most.begin(), most.end(), [&](int l, int r) {
        if (played.at(l).plays != played.at(r).plays)
            return played.at(l).plays > played.at(r).plays;
        return l < r;
    });
    std::sort(recent.begin(), recent.end(), [&](int l, int r) {
        if (played.at(l).last != played.at(r).last)
            return played.at(l).last > played.at(r).last;
        return l < r;
    });
    add(QStringLiteral("listen:most"), QStringLiteral("Most Played"), QStringLiteral("flame"), DjSmartKind::Listening,
        tracksOf(most, listeningLimit));
    add(QStringLiteral("listen:recent"), QStringLiteral("Recently Played"), QStringLiteral("clock"),
        DjSmartKind::Listening, tracksOf(recent, listeningLimit));
    QList<DjTrack> fresh;
    QList<DjTrack> once;
    for (const DjTrack &song : songs) {
        const int count = plays.value(song.id, 0);
        if (count == 0)
            fresh.append(song);
        if (plays.contains(song.id) && count == 1)
            once.append(song);
    }
    add(QStringLiteral("listen:new"), QStringLiteral("Never Played"), QStringLiteral("sparkle"),
        DjSmartKind::Listening, fresh);
    add(QStringLiteral("listen:once"), QStringLiteral("Played Once"), QStringLiteral("1.circle"),
        DjSmartKind::Listening, once);
    QList<DjTrack> rediscover;
    for (const DjPlayedSong &song : played) {
        if (song.plays > 0 && song.last > 0 && now - song.last > rediscoverAfter)
            rediscover.append(known.value(song.id));
    }
    add(QStringLiteral("listen:rediscover"), QStringLiteral("Rediscover"), QStringLiteral("arrow.counterclockwise"),
        DjSmartKind::Listening, rediscover);

    QList<QSet<QString>> tagged;
    tagged.reserve(songs.size());
    for (const DjTrack &song : songs) {
        const QStringList found = categories(song);
        tagged.append(QSet<QString>(found.begin(), found.end()));
    }
    const auto within = [&](const QString &category) {
        QList<DjTrack> out;
        for (int i = 0; i < songs.size(); ++i) {
            if (tagged.at(i).contains(category))
                out.append(songs.at(i));
        }
        return out;
    };
    for (const Rule &rule : genres()) {
        add(QStringLiteral("genre:") + rule.id, rule.name, rule.symbol, DjSmartKind::Genre,
            within(QStringLiteral("genre:") + rule.id));
    }
    for (const Rule &rule : styles()) {
        add(QStringLiteral("style:") + rule.id, rule.name, rule.symbol, DjSmartKind::Style,
            within(QStringLiteral("style:") + rule.id));
    }
    QSet<int> decadeSet;
    for (const QSet<QString> &found : tagged) {
        for (const QString &category : found) {
            if (!category.startsWith(QLatin1String("decade:")))
                continue;
            bool ok = false;
            const int year = category.mid(7).toInt(&ok);
            if (ok)
                decadeSet.insert(year);
        }
    }
    QList<int> decades(decadeSet.begin(), decadeSet.end());
    std::sort(decades.begin(), decades.end());
    for (const int year : decades) {
        add(QStringLiteral("decade:%1").arg(year), QStringLiteral("%1s").arg(year), QStringLiteral("calendar"),
            DjSmartKind::Decade, within(QStringLiteral("decade:%1").arg(year)));
    }

    QHash<QString, QList<DjTrack>> artists;
    QHash<QString, QHash<QString, int>> names;
    QStringList order;
    for (const DjTrack &song : songs) {
        const QString key = artistKey(song);
        if (key.isEmpty())
            continue;
        if (!artists.contains(key))
            order.append(key);
        artists[key].append(song);
        names[key][artist(song)] += 1;
    }
    QList<int> ranked;
    for (int i = 0; i < order.size(); ++i) {
        if (artists.value(order.at(i)).size() >= minimumSongs)
            ranked.append(i);
    }
    std::sort(ranked.begin(), ranked.end(), [&](int l, int r) {
        const qsizetype left = artists.value(order.at(l)).size();
        const qsizetype right = artists.value(order.at(r)).size();
        if (left != right)
            return left > right;
        return l < r;
    });
    for (int i = 0; i < ranked.size() && i < artistLimit; ++i) {
        const QString &key = order.at(ranked.at(i));
        QString spelled = key;
        int best = -1;
        const QHash<QString, int> spellings = names.value(key);
        for (auto it = spellings.cbegin(); it != spellings.cend(); ++it) {
            if (it.value() > best || (it.value() == best && it.key() < spelled)) {
                best = it.value();
                spelled = it.key();
            }
        }
        add(QStringLiteral("artist:") + key, spelled, QStringLiteral("person"), DjSmartKind::Artist,
            artists.value(key));
    }
    return result;
}

QList<DjSimilarSong> similar(const DjTrack &seed, const QList<DjTrack> &songs)
{
    return similar(seed, songs, QList<DjPlaylist>());
}

QList<DjSimilarSong> similar(const DjTrack &seed, const QList<DjTrack> &songs, const QList<DjPlaylist> &playlists,
                             int limit)
{
    const QString seedArtist = artistKey(seed);
    const QSet<QString> tags(seed.tags.begin(), seed.tags.end());
    QList<DjPlaylist> holding;
    for (const DjPlaylist &list : playlists) {
        if (list.songs.contains(seed.id))
            holding.append(list);
    }
    const QStringList seedKinds = categories(seed);
    const QSet<QString> kindSet(seedKinds.begin(), seedKinds.end());
    QHash<QString, QString> labels;
    for (const Rule &rule : genres()) {
        if (!labels.contains(QStringLiteral("genre:") + rule.id))
            labels.insert(QStringLiteral("genre:") + rule.id, rule.name);
    }
    for (const Rule &rule : styles()) {
        if (!labels.contains(QStringLiteral("style:") + rule.id))
            labels.insert(QStringLiteral("style:") + rule.id, rule.name);
    }
    QSet<QString> seen{seed.id};
    QList<QPair<int, DjSimilarSong>> found;
    for (int offset = 0; offset < songs.size(); ++offset) {
        const DjTrack &song = songs.at(offset);
        if (seen.contains(song.id))
            continue;
        seen.insert(song.id);
        int score = 0;
        QStringList reasons;
        if (!seedArtist.isEmpty() && artistKey(song) == seedArtist) {
            score += 5;
            reasons.append(QStringLiteral("Same artist"));
        }
        for (const QString &tag : song.tags) {
            if (!tags.contains(tag))
                continue;
            score += 3;
            reasons.append(QStringLiteral("#") + tag);
        }
        for (const DjPlaylist &list : holding) {
            if (!list.songs.contains(song.id))
                continue;
            score += 2;
            reasons.append(list.name);
        }
        for (const QString &kind : categories(song)) {
            if (!kindSet.contains(kind))
                continue;
            if (kind.startsWith(QLatin1String("genre:"))) {
                score += 2;
                reasons.append(labels.value(kind, kind));
            } else if (kind.startsWith(QLatin1String("style:"))) {
                score += 1;
                reasons.append(labels.value(kind, kind));
            } else if (kind.startsWith(QLatin1String("decade:"))) {
                score += 1;
                reasons.append(kind.mid(7) + QLatin1Char('s'));
            }
        }
        if (score <= 0)
            continue;
        found.append({offset, DjSimilarSong{song, score, reasons}});
    }
    std::sort(found.begin(), found.end(), [](const auto &l, const auto &r) {
        if (l.second.score != r.second.score)
            return l.second.score > r.second.score;
        return l.first < r.first;
    });
    QList<DjSimilarSong> out;
    for (int i = 0; i < found.size() && i < limit; ++i)
        out.append(found.at(i).second);
    return out;
}

QString searchQuery(const DjTrack &seed)
{
    const QString name = artist(seed);
    const QString hay = haystack(seed);
    QString genre;
    for (const Rule &rule : genres()) {
        if (matches(rule, hay)) {
            genre = rule.name;
            break;
        }
    }
    if (!name.isEmpty())
        return name + QLatin1Char(' ') + (genre.isEmpty() ? QStringLiteral("mix") : genre);
    QString title = seed.title;
    const QList<QPair<QString, QString>> pairs{{QStringLiteral("("), QStringLiteral(")")},
                                               {QStringLiteral("["), QStringLiteral("]")}};
    for (const auto &[open, close] : pairs) {
        while (true) {
            const qsizetype start = title.indexOf(open);
            if (start < 0)
                break;
            const qsizetype end = title.indexOf(close, start + open.size());
            if (end < 0)
                break;
            title.remove(start, end + close.size() - start);
        }
    }
    return words(title).mid(0, 6).join(QLatin1Char(' '));
}

}
