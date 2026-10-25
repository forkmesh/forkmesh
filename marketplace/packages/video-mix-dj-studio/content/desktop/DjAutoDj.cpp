#include "DjAutoDj.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>

namespace forkmesh::djstudio {

namespace {

const QSet<QString> &noise()
{
    static const QSet<QString> words{
        QStringLiteral("official"), QStringLiteral("video"), QStringLiteral("audio"), QStringLiteral("music"),
        QStringLiteral("lyrics"), QStringLiteral("lyric"), QStringLiteral("visualizer"), QStringLiteral("visualiser"),
        QStringLiteral("hd"), QStringLiteral("hq"), QStringLiteral("4k"), QStringLiteral("mv"), QStringLiteral("m v"),
        QStringLiteral("remastered"), QStringLiteral("remaster"), QStringLiteral("explicit"), QStringLiteral("clean"),
        QStringLiteral("version"), QStringLiteral("full"), QStringLiteral("feat"), QStringLiteral("ft"),
        QStringLiteral("featuring"), QStringLiteral("topic"),
    };
    return words;
}

const QStringList &notSongs()
{
    static const QStringList words{
        QStringLiteral("got talent"), QStringLiteral("agt"), QStringLiteral("bgt"), QStringLiteral("x factor"),
        QStringLiteral("xfactor"), QStringLiteral("the voice"), QStringLiteral("american idol"), QStringLiteral("pop idol"),
        QStringLiteral("idols"), QStringLiteral("golden buzzer"), QStringLiteral("audition"), QStringLiteral("auditions"),
        QStringLiteral("judges"), QStringLiteral("simon cowell"), QStringLiteral("reaction"), QStringLiteral("reactions"),
        QStringLiteral("reacts"), QStringLiteral("reacting"), QStringLiteral("first time hearing"),
        QStringLiteral("first time listening"), QStringLiteral("vocal coach"), QStringLiteral("cover by"),
        QStringLiteral("covered by"), QStringLiteral("cover of"), QStringLiteral("acoustic cover"),
        QStringLiteral("piano cover"), QStringLiteral("guitar cover"), QStringLiteral("drum cover"),
        QStringLiteral("bass cover"), QStringLiteral("karaoke"), QStringLiteral("backing track"),
        QStringLiteral("sing along"), QStringLiteral("tutorial"), QStringLiteral("lesson"), QStringLiteral("how to play"),
        QStringLiteral("how to sing"), QStringLiteral("remix"), QStringLiteral("mashup"), QStringLiteral("nightcore"),
        QStringLiteral("sped up"), QStringLiteral("slowed"), QStringLiteral("reverb"), QStringLiteral("8d"),
        QStringLiteral("bass boosted"), QStringLiteral("parody"), QStringLiteral("medley"), QStringLiteral("interview"),
        QStringLiteral("trailer"), QStringLiteral("episode"), QStringLiteral("compilation"), QStringLiteral("highlights"),
        QStringLiteral("rehearsal"), QStringLiteral("behind the scenes"), QStringLiteral("documentary"),
        QStringLiteral("podcast"), QStringLiteral("live at"), QStringLiteral("live from"),
        QStringLiteral("live performance"), QStringLiteral("full performance"), QStringLiteral("concert"),
        QStringLiteral("tiny desk"), QStringLiteral("sings"), QStringLiteral("performs"), QStringLiteral("tribute"),
    };
    return words;
}

const QStringList &notOriginal()
{
    static const QStringList words{
        QStringLiteral("live"), QStringLiteral("cover"), QStringLiteral("covers"), QStringLiteral("acoustic"),
        QStringLiteral("unplugged"), QStringLiteral("instrumental"), QStringLiteral("karaoke"), QStringLiteral("remix"),
        QStringLiteral("session"), QStringLiteral("sessions"), QStringLiteral("demo"),
    };
    return words;
}

const QList<QPair<QString, QString>> &brackets()
{
    static const QList<QPair<QString, QString>> pairs{
        {QStringLiteral("("), QStringLiteral(")")},
        {QStringLiteral("["), QStringLiteral("]")},
        {QStringLiteral("{"), QStringLiteral("}")},
    };
    return pairs;
}

QString smartText(const QString &raw)
{
    QString spaced;
    for (const char32_t scalar : raw.toLower().toUcs4()) {
        if (QChar::isLetterOrNumber(scalar) || QChar::isMark(scalar) || scalar == U'&')
            spaced.append(QString::fromUcs4(&scalar, 1));
        else
            spaced.append(QLatin1Char(' '));
    }
    return QLatin1Char(' ') + spaced.split(QLatin1Char(' '), Qt::SkipEmptyParts).join(QLatin1Char(' ')) +
           QLatin1Char(' ');
}

QString key(const QString &text)
{
    QStringList kept;
    for (const QString &word : smartText(text).split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        if (!noise().contains(word))
            kept.append(word);
    }
    return kept.join(QLatin1Char(' '));
}

QStringList qualifiers(const QString &title)
{
    QStringList parts;
    for (const auto &[open, close] : brackets()) {
        qsizetype from = 0;
        while (true) {
            const qsizetype start = title.indexOf(open, from);
            if (start < 0)
                break;
            const qsizetype end = title.indexOf(close, start + open.size());
            if (end < 0)
                break;
            parts.append(title.mid(start + open.size(), end - start - open.size()));
            from = end + close.size();
        }
    }
    const QStringList piped = title.split(QStringLiteral(" | "));
    parts.append(piped.mid(1));
    QString dashed = piped.first();
    for (const QString &dash : {QStringLiteral(" – "), QStringLiteral(" — ")})
        dashed.replace(dash, QStringLiteral(" - "));
    parts.append(dashed.split(QStringLiteral(" - ")).mid(2));
    QStringList out;
    for (const QString &part : parts)
        out.append(smartText(part));
    return out;
}

std::optional<QList<QJsonObject>> objects(const QJsonValue &value)
{
    if (!value.isArray())
        return std::nullopt;
    QList<QJsonObject> out;
    for (const QJsonValue &row : value.toArray()) {
        if (!row.isObject())
            return std::nullopt;
        out.append(row.toObject());
    }
    return out;
}

}

namespace autodj {

QList<DjAutoDjCandidate> candidates(const QByteArray &data)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError)
        return {};
    std::optional<QList<QJsonObject>> rows;
    if (document.isObject())
        rows = objects(document.object().value(QStringLiteral("recommendedVideos")));
    if (!rows && document.isArray())
        rows = objects(QJsonValue(document.array()));
    QList<DjAutoDjCandidate> out;
    QSet<QString> seen;
    for (const QJsonObject &row : rows.value_or(QList<QJsonObject>())) {
        const QJsonValue type = row.value(QStringLiteral("type"));
        if (type.isString() && type.toString() != QLatin1String("video"))
            continue;
        QString source;
        const QJsonValue raw = row.value(QStringLiteral("videoId"));
        const QJsonValue url = row.value(QStringLiteral("url"));
        if (raw.isString()) {
            source = raw.toString();
        } else if (url.isString()) {
            source = url.toString();
            if (source.startsWith(QLatin1Char('/')))
                source.prepend(QStringLiteral("https://www.youtube.com"));
        }
        const QString id = studio::videoId(source);
        if (id.isEmpty() || seen.contains(id))
            continue;
        seen.insert(id);
        DjAutoDjCandidate candidate;
        candidate.id = id;
        candidate.title = row.value(QStringLiteral("title")).toString();
        if (candidate.title.isEmpty())
            candidate.title = QStringLiteral("Untitled video");
        candidate.author = row.value(QStringLiteral("author")).toString();
        candidate.seconds = row.value(QStringLiteral("lengthSeconds")).toDouble(0);
        const QJsonValue views = row.value(QStringLiteral("viewCount"));
        candidate.views = views.isDouble() ? views.toDouble()
                                           : count(row.value(QStringLiteral("viewCountText")).toString());
        candidate.rank = int(seen.size()) - 1;
        out.append(candidate);
    }
    return out;
}

double count(const QString &text)
{
    const QString lowered = text.toLower().remove(QLatin1Char(','));
    static const QRegularExpression number(QStringLiteral("[0-9]+(\\.[0-9]+)?\\s*[kmb]?"));
    const QRegularExpressionMatch match = number.match(lowered);
    if (!match.hasMatch())
        return 0;
    QString part = match.captured(0).remove(QLatin1Char(' '));
    const double scale = part.endsWith(QLatin1Char('k'))   ? 1e3
                         : part.endsWith(QLatin1Char('m')) ? 1e6
                         : part.endsWith(QLatin1Char('b')) ? 1e9
                                                           : 1;
    static const QRegularExpression suffix(QStringLiteral("^[kmb]+|[kmb]+$"));
    part.remove(suffix);
    bool ok = false;
    const double value = part.toDouble(&ok);
    return (ok ? value : 0) * scale;
}

QUrl likesUrl(const QUrl &instance, const QString &id)
{
    if (!studio::isVideoId(id))
        return QUrl();
    QUrl url = instance;
    QString path = instance.path();
    if (!path.endsWith(QLatin1Char('/')))
        path.append(QLatin1Char('/'));
    url.setPath(path + QStringLiteral("api/v1/videos/") + id);
    url.setQuery(QStringLiteral("fields=likeCount,viewCount"));
    return url;
}

std::optional<DjAutoDjLikes> likes(const QByteArray &data)
{
    const QJsonDocument document = QJsonDocument::fromJson(data);
    if (!document.isObject())
        return std::nullopt;
    const QJsonObject object = document.object();
    const QJsonValue likeCount = object.value(QStringLiteral("likeCount"));
    if (!likeCount.isDouble() || !(likeCount.toDouble() >= 0))
        return std::nullopt;
    return DjAutoDjLikes{likeCount.toDouble(), std::max(0.0, object.value(QStringLiteral("viewCount")).toDouble(0))};
}

QString songKey(const QString &title)
{
    QString name = title;
    for (const auto &[open, close] : brackets()) {
        while (true) {
            const qsizetype start = name.indexOf(open);
            if (start < 0)
                break;
            const qsizetype end = name.indexOf(close, start + open.size());
            if (end < 0)
                break;
            name.remove(start, end + close.size() - start);
        }
    }
    for (const QString &dash : {QStringLiteral(" - "), QStringLiteral(" – "), QStringLiteral(" — "), QStringLiteral(" | ")}) {
        const qsizetype split = name.indexOf(dash);
        if (split >= 0) {
            const QString tail = name.mid(split + dash.size());
            if (!key(tail).isEmpty())
                name = tail;
            break;
        }
    }
    return key(name);
}

bool isSong(const DjAutoDjCandidate &candidate)
{
    const QString text = smartText(candidate.title + QLatin1Char(' ') + candidate.author);
    for (const QString &word : notSongs()) {
        if (text.contains(QLatin1Char(' ') + word + QLatin1Char(' ')))
            return false;
    }
    const QStringList words = smartText(candidate.title).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    static const QStringList endings{QStringLiteral("cover"), QStringLiteral("covers"), QStringLiteral("karaoke"),
                                     QStringLiteral("instrumental")};
    if (!words.isEmpty() && endings.contains(words.last()))
        return false;
    for (const QString &part : qualifiers(candidate.title)) {
        for (const QString &word : notOriginal()) {
            if (part.contains(QLatin1Char(' ') + word + QLatin1Char(' ')))
                return false;
        }
    }
    return true;
}

bool isOriginal(const DjAutoDjCandidate &candidate)
{
    const QString author = smartText(candidate.author);
    if (author.endsWith(QLatin1String(" topic ")) || author.contains(QLatin1String("vevo")))
        return true;
    const QString title = smartText(candidate.title);
    for (const QString &word : {QStringLiteral(" official "), QStringLiteral(" remastered "), QStringLiteral(" remaster "),
                                QStringLiteral(" original ")}) {
        if (title.contains(word))
            return true;
    }
    QString name = candidate.title;
    for (const QString &dash : {QStringLiteral(" – "), QStringLiteral(" — "), QStringLiteral(" | ")})
        name.replace(dash, QStringLiteral(" - "));
    const QStringList parts = name.split(QStringLiteral(" - "));
    const QString artist = key(candidate.author);
    return parts.size() > 1 && !artist.isEmpty() && key(parts.first()) == artist;
}

QList<DjAutoDjCandidate> eligible(const QList<DjAutoDjCandidate> &found, const DjTrack &seed,
                                  const QSet<QString> &avoid, const QStringList &heard)
{
    QSet<QString> keys;
    for (const QString &title : QStringList{seed.title} + heard) {
        const QString k = songKey(title);
        if (!k.isEmpty())
            keys.insert(k);
    }
    QList<DjAutoDjCandidate> originals;
    QList<DjAutoDjCandidate> others;
    for (const DjAutoDjCandidate &candidate : found) {
        if (candidate.id == seed.id || avoid.contains(candidate.id))
            continue;
        if (candidate.seconds < shortest || candidate.seconds > longest || !isSong(candidate))
            continue;
        (isOriginal(candidate) ? originals : others).append(candidate);
    }
    QList<DjAutoDjCandidate> out;
    for (const DjAutoDjCandidate &candidate : originals + others) {
        const QString k = songKey(candidate.title);
        if (k.isEmpty()) {
            out.append(candidate);
        } else if (!keys.contains(k)) {
            keys.insert(k);
            out.append(candidate);
        }
    }
    return out;
}

double score(const DjAutoDjCandidate &candidate, int among)
{
    const double liked = candidate.likes.value_or(candidate.views * likeShare);
    const double related = among > 1 ? 1 - double(candidate.rank) / double(among - 1) : 1;
    const double ratio = candidate.likes && candidate.views > 0 ? std::min(0.1, liked / candidate.views) * 10 : 0;
    return std::log10(1 + std::max(0.0, liked)) + ratio + related * 2;
}

QList<DjAutoDjCandidate> ranked(const QList<DjAutoDjCandidate> &found)
{
    int span = 0;
    for (const DjAutoDjCandidate &candidate : found)
        span = std::max(span, candidate.rank);
    span += 1;
    QVector<int> order(found.size());
    QVector<bool> original(found.size());
    QVector<double> scores(found.size());
    for (int i = 0; i < found.size(); ++i) {
        order[i] = i;
        original[i] = isOriginal(found.at(i));
        scores[i] = score(found.at(i), span);
    }
    std::sort(order.begin(), order.end(), [&](int l, int r) {
        if (original.at(l) != original.at(r))
            return bool(original.at(l));
        if (scores.at(l) != scores.at(r))
            return scores.at(l) > scores.at(r);
        return l < r;
    });
    QList<DjAutoDjCandidate> out;
    for (int index : order)
        out.append(found.at(index));
    return out;
}

std::optional<DjAutoDjCandidate> next(const QList<DjAutoDjCandidate> &ranked, const QSet<QString> &loaded,
                                      const QSet<QString> &skip, const QStringList &played)
{
    for (const DjAutoDjCandidate &candidate : ranked) {
        if (!loaded.contains(candidate.id) && !skip.contains(candidate.id) && !played.contains(candidate.id))
            return candidate;
    }
    return std::nullopt;
}

QStringList remember(const QString &id, const QStringList &played)
{
    QStringList out{id};
    for (const QString &item : played) {
        if (item != id)
            out.append(item);
        if (out.size() >= sessionLimit)
            break;
    }
    return out;
}

}

}
