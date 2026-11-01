#pragma once

#include <QChar>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QUuid>

namespace forkmesh::notes {

constexpr int kMaxEmojiCodePoints = 16;
constexpr int kMaxEmojiBytes = 64;
constexpr int kMaxOutlineDepth = 3;
constexpr int kMaxOutlineEntries = 300;
constexpr int kMaxPageTitle = 200;
constexpr int kMaxPageDescription = 2000;

inline QString normalizedNoteEmoji(const QString &value)
{
    return value.trimmed();
}

inline bool isValidNoteEmoji(const QString &value)
{
    const QString text = normalizedNoteEmoji(value);
    if (text.isEmpty())
        return true;
    const QList<uint> points = text.toUcs4();
    if (points.size() > kMaxEmojiCodePoints ||
        text.toUtf8().size() > kMaxEmojiBytes)
        return false;
    const bool keycap = points.contains(0x20E3u);
    for (const uint point : points) {
        const char32_t c = char32_t(point);
        const QChar::Category category = QChar::category(c);
        if (QChar::isSpace(c) || category == QChar::Other_Control ||
            category == QChar::Separator_Space ||
            category == QChar::Separator_Line ||
            category == QChar::Separator_Paragraph)
            return false;
        if (point < 0x80) {
            const bool keycapBase = (point >= '0' && point <= '9') ||
                                    point == '#' || point == '*';
            if (!keycapBase || !keycap)
                return false;
            continue;
        }
        switch (category) {
        case QChar::Symbol_Math:
        case QChar::Symbol_Currency:
        case QChar::Symbol_Modifier:
        case QChar::Symbol_Other:
        case QChar::Mark_NonSpacing:
        case QChar::Mark_SpacingCombining:
        case QChar::Mark_Enclosing:
        case QChar::Other_Format:
            break;
        default:
            return false;
        }
    }
    return true;
}

inline const QStringList &commonNoteEmoji()
{
    static const QStringList emoji = {
        QStringLiteral("📝"), QStringLiteral("📌"), QStringLiteral("📎"),
        QStringLiteral("📚"), QStringLiteral("📖"), QStringLiteral("🗂️"),
        QStringLiteral("📅"), QStringLiteral("✅"), QStringLiteral("☑️"),
        QStringLiteral("⭐"), QStringLiteral("🔥"), QStringLiteral("💡"),
        QStringLiteral("🚀"), QStringLiteral("🎯"), QStringLiteral("🧠"),
        QStringLiteral("💬"), QStringLiteral("❓"), QStringLiteral("❗"),
        QStringLiteral("⚠️"), QStringLiteral("🐛"), QStringLiteral("🔧"),
        QStringLiteral("⚙️"), QStringLiteral("🔒"), QStringLiteral("🔑"),
        QStringLiteral("📈"), QStringLiteral("💰"), QStringLiteral("🧾"),
        QStringLiteral("🏠"), QStringLiteral("✈️"), QStringLiteral("🎉"),
        QStringLiteral("❤️"), QStringLiteral("😀"), QStringLiteral("🤔"),
        QStringLiteral("👋"), QStringLiteral("👍"), QStringLiteral("🌱"),
        QStringLiteral("🍎"), QStringLiteral("☕"), QStringLiteral("🎨"),
        QStringLiteral("🎵"), QStringLiteral("📷"), QStringLiteral("🌍"),
        QStringLiteral("🔬"), QStringLiteral("💻"), QStringLiteral("📦"),
        QStringLiteral("🧪"), QStringLiteral("🗓️"), QStringLiteral("1️⃣"),
    };
    return emoji;
}

inline QString noteDisplayTitle(const QString &emoji, const QString &title,
                                const QString &fallback = QStringLiteral("Untitled note"))
{
    const QString name = title.trimmed();
    const QString mark = normalizedNoteEmoji(emoji);
    if (mark.isEmpty())
        return name;
    return QStringLiteral("%1 %2").arg(mark, name.isEmpty() ? fallback : name);
}

inline const QStringList &pageAccents()
{
    static const QStringList accents{
        QStringLiteral("blue"),   QStringLiteral("purple"), QStringLiteral("pink"),
        QStringLiteral("red"),    QStringLiteral("orange"), QStringLiteral("yellow"),
        QStringLiteral("teal"),   QStringLiteral("gray")};
    return accents;
}

inline QString pageAccent(const QString &value)
{
    return pageAccents().contains(value) ? value : QStringLiteral("blue");
}

inline QString pageAccentHex(const QString &accent)
{
    const QString value = pageAccent(accent);
    if (value == QLatin1String("purple"))
        return QStringLiteral("#a371f7");
    if (value == QLatin1String("pink"))
        return QStringLiteral("#db61a2");
    if (value == QLatin1String("red"))
        return QStringLiteral("#f85149");
    if (value == QLatin1String("orange"))
        return QStringLiteral("#db6d28");
    if (value == QLatin1String("yellow"))
        return QStringLiteral("#d29922");
    if (value == QLatin1String("teal"))
        return QStringLiteral("#39c5cf");
    if (value == QLatin1String("gray"))
        return QStringLiteral("#8b949e");
    return QStringLiteral("#4493f8");
}

inline QString pageAccentLabel(const QString &accent)
{
    const QString value = pageAccent(accent);
    return value.left(1).toUpper() + value.mid(1);
}

inline QString pageLayout(const QString &value)
{
    return value == QLatin1String("continuous") ? value : QStringLiteral("outline");
}

inline QString pageLayoutLabel(const QString &layout)
{
    return pageLayout(layout) == QLatin1String("continuous")
        ? QStringLiteral("Every note, one after another")
        : QStringLiteral("Outline, one note at a time");
}

inline QString pageTitle(const QJsonObject &page)
{
    const QString title = page.value(QLatin1String("title")).toString().trimmed();
    return title.isEmpty() ? QStringLiteral("Untitled page") : title;
}

inline QString pageEntriesLabel(int entries)
{
    return entries == 1 ? QStringLiteral("1 entry")
                        : QStringLiteral("%1 entries").arg(entries);
}

inline bool isOutlineId(const QString &value)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_-]{8,32}$"));
    return pattern.match(value).hasMatch();
}

inline QString newOutlineId()
{
    return QUuid::createUuid().toString(QUuid::Id128).left(16);
}

inline bool isOutlineUrl(const QString &value)
{
    const QUrl url(value.trimmed(), QUrl::StrictMode);
    return url.isValid() && url.scheme() == QLatin1String("https") &&
           !url.host().isEmpty() && url.userInfo().isEmpty();
}

inline QJsonObject outlineEntry(const QString &kind, int depth,
                                const QString &label,
                                const QString &noteId = QString(),
                                const QString &url = QString())
{
    QJsonObject entry{{QStringLiteral("id"), newOutlineId()},
                      {QStringLiteral("kind"), kind},
                      {QStringLiteral("depth"), qBound(0, depth, kMaxOutlineDepth)},
                      {QStringLiteral("label"), label},
                      {QStringLiteral("collapsed"), false}};
    if (kind == QLatin1String("note"))
        entry.insert(QStringLiteral("noteId"), noteId);
    if (kind == QLatin1String("link"))
        entry.insert(QStringLiteral("url"), url.trimmed());
    return entry;
}

inline int outlineDepth(const QJsonObject &entry)
{
    return qBound(0, entry.value(QLatin1String("depth")).toInt(), kMaxOutlineDepth);
}

inline QString outlineKind(const QJsonObject &entry)
{
    const QString kind = entry.value(QLatin1String("kind")).toString();
    return kind == QLatin1String("section") || kind == QLatin1String("link")
        ? kind
        : QStringLiteral("note");
}

inline QJsonArray normalizedOutline(const QJsonArray &entries)
{
    QJsonArray result;
    QSet<QString> ids;
    int previous = -1;
    for (const QJsonValue &value : entries) {
        if (result.size() >= kMaxOutlineEntries)
            break;
        QJsonObject entry = value.toObject();
        QString id = entry.value(QLatin1String("id")).toString();
        while (!isOutlineId(id) || ids.contains(id))
            id = newOutlineId();
        ids.insert(id);
        entry.insert(QStringLiteral("id"), id);
        entry.insert(QStringLiteral("kind"), outlineKind(entry));
        const int depth = qMin(outlineDepth(entry), previous + 1);
        entry.insert(QStringLiteral("depth"), qMax(0, depth));
        previous = qMax(0, depth);
        result.append(entry);
    }
    return result;
}

inline bool canIndentOutline(const QJsonArray &entries, int index)
{
    if (index <= 0 || index >= entries.size())
        return false;
    const int depth = outlineDepth(entries.at(index).toObject());
    const int before = outlineDepth(entries.at(index - 1).toObject());
    return depth < kMaxOutlineDepth && depth <= before;
}

inline bool canOutdentOutline(const QJsonArray &entries, int index)
{
    return index >= 0 && index < entries.size() &&
           outlineDepth(entries.at(index).toObject()) > 0;
}

inline QJsonArray indentOutline(QJsonArray entries, int index, int delta)
{
    if (delta > 0 && !canIndentOutline(entries, index))
        return entries;
    if (delta < 0 && !canOutdentOutline(entries, index))
        return entries;
    QJsonObject entry = entries.at(index).toObject();
    entry.insert(QStringLiteral("depth"), outlineDepth(entry) + (delta > 0 ? 1 : -1));
    entries.replace(index, entry);
    return normalizedOutline(entries);
}

inline QJsonArray moveOutline(QJsonArray entries, int index, int delta)
{
    const int target = index + delta;
    if (index < 0 || index >= entries.size() || target < 0 ||
        target >= entries.size() || delta == 0)
        return entries;
    const QJsonValue moving = entries.at(index);
    entries.removeAt(index);
    entries.insert(target, moving);
    return normalizedOutline(entries);
}

inline QJsonArray removeOutline(QJsonArray entries, int index)
{
    if (index < 0 || index >= entries.size())
        return entries;
    entries.removeAt(index);
    return normalizedOutline(entries);
}

inline int outlineInsertDepth(const QJsonArray &entries, int after)
{
    if (after < 0 || after >= entries.size())
        return 0;
    const QJsonObject entry = entries.at(after).toObject();
    const int depth = outlineDepth(entry);
    return outlineKind(entry) == QLatin1String("section")
        ? qMin(depth + 1, kMaxOutlineDepth)
        : depth;
}

inline QJsonArray insertOutline(QJsonArray entries, int after, QJsonObject entry)
{
    if (entries.size() >= kMaxOutlineEntries)
        return entries;
    entry.insert(QStringLiteral("depth"), outlineInsertDepth(entries, after));
    const int at = after < 0 || after >= entries.size() ? int(entries.size())
                                                        : after + 1;
    entries.insert(at, entry);
    return normalizedOutline(entries);
}

inline QList<int> visibleOutlineRows(const QJsonArray &entries,
                                     const QSet<QString> &collapsed)
{
    QList<int> rows;
    int hideBelow = -1;
    for (int i = 0; i < entries.size(); ++i) {
        const QJsonObject entry = entries.at(i).toObject();
        const int depth = outlineDepth(entry);
        const bool section = outlineKind(entry) == QLatin1String("section");
        if (hideBelow >= 0) {
            const bool ends = depth < hideBelow || (section && depth == hideBelow);
            if (!ends)
                continue;
            hideBelow = -1;
        }
        rows.append(i);
        const QString id = entry.value(QLatin1String("id")).toString();
        if (section && collapsed.contains(id))
            hideBelow = depth;
    }
    return rows;
}

inline QSet<QString> collapsedOutlineSections(const QJsonArray &entries)
{
    QSet<QString> ids;
    for (const QJsonValue &value : entries) {
        const QJsonObject entry = value.toObject();
        if (outlineKind(entry) == QLatin1String("section") &&
            entry.value(QLatin1String("collapsed")).toBool())
            ids.insert(entry.value(QLatin1String("id")).toString());
    }
    return ids;
}

inline bool outlineEntryLocked(const QJsonObject &entry, const QJsonObject &notes)
{
    return outlineKind(entry) == QLatin1String("note") &&
           !notes.contains(entry.value(QLatin1String("noteId")).toString());
}

inline QString outlineEntryLabel(const QJsonObject &entry, const QJsonObject &notes,
                                 bool showEmoji)
{
    const QString label = entry.value(QLatin1String("label")).toString().trimmed();
    const QString kind = outlineKind(entry);
    if (kind == QLatin1String("section"))
        return label.isEmpty() ? QStringLiteral("Untitled section") : label;
    if (kind == QLatin1String("link"))
        return label.isEmpty() ? entry.value(QLatin1String("url")).toString() : label;
    const QString noteId = entry.value(QLatin1String("noteId")).toString();
    if (!notes.contains(noteId))
        return label.isEmpty() ? QStringLiteral("Locked note") : label;
    const QJsonObject note = notes.value(noteId).toObject();
    const QString title = label.isEmpty()
        ? note.value(QLatin1String("title")).toString().trimmed()
        : label;
    const QString shown = title.isEmpty() ? QStringLiteral("Untitled note") : title;
    return showEmoji
        ? noteDisplayTitle(note.value(QLatin1String("emoji")).toString(), shown)
        : shown;
}

inline QStringList outlineNoteIds(const QJsonArray &entries, const QJsonObject &notes)
{
    QStringList ids;
    for (const QJsonValue &value : entries) {
        const QJsonObject entry = value.toObject();
        const QString id = entry.value(QLatin1String("noteId")).toString();
        if (outlineKind(entry) == QLatin1String("note") && notes.contains(id) &&
            !ids.contains(id))
            ids << id;
    }
    return ids;
}

inline QString firstOpenableNote(const QJsonArray &entries, const QJsonObject &notes)
{
    return outlineNoteIds(entries, notes).value(0);
}

inline QJsonObject pageEditFields(const QJsonObject &page)
{
    QJsonObject fields;
    fields.insert(QStringLiteral("title"),
                  page.value(QLatin1String("title")).toString().left(kMaxPageTitle));
    fields.insert(QStringLiteral("emoji"), page.value(QLatin1String("emoji")).toString());
    fields.insert(QStringLiteral("description"),
                  page.value(QLatin1String("description"))
                      .toString()
                      .left(kMaxPageDescription));
    fields.insert(QStringLiteral("accent"),
                  pageAccent(page.value(QLatin1String("accent")).toString()));
    fields.insert(QStringLiteral("layout"),
                  pageLayout(page.value(QLatin1String("layout")).toString()));
    fields.insert(QStringLiteral("showEmoji"),
                  page.value(QLatin1String("showEmoji")).toBool(true));
    fields.insert(QStringLiteral("outline"),
                  normalizedOutline(page.value(QLatin1String("outline")).toArray()));
    return fields;
}

inline QString pageError(const QString &code, const QString &fallback)
{
    if (code == QLatin1String("page_not_found"))
        return QStringLiteral("That page was deleted, or you no longer have access.");
    if (code == QLatin1String("invalid_page"))
        return QStringLiteral("One of the page's fields is not valid.");
    if (code == QLatin1String("too_many_entries"))
        return QStringLiteral("A page holds at most %1 outline entries.")
            .arg(kMaxOutlineEntries);
    if (code == QLatin1String("invalid_emoji"))
        return QStringLiteral("That is not an emoji.");
    if (code == QLatin1String("note_not_found"))
        return QStringLiteral("You can only add notes you can open.");
    if (code == QLatin1String("version_conflict"))
        return QStringLiteral("Someone else changed this page. It has been "
                              "reloaded; make your change again.");
    return fallback;
}

}
