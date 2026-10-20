#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>

namespace stickers {

struct CatalogEntry {
    QString slug;
    QString text;
    quint32 rgb;
    int tilt;
    QString fallback;
};

namespace detail {

inline QString emojiFrom(char32_t first, char32_t second = 0)
{
    const char32_t points[] = {first, second};
    return QString::fromUcs4(points, second != 0 ? 2 : 1);
}

} // namespace detail

inline const QVector<CatalogEntry> &catalog()
{
    static const QVector<CatalogEntry> kCatalog = {
        {QStringLiteral("lgtm"), QStringLiteral("LGTM"), 0x8250df, -6,
         detail::emojiFrom(0x2705)},
        {QStringLiteral("ship-it"), QStringLiteral("Ship it"), 0x0969da, 5,
         detail::emojiFrom(0x1F680)},
        {QStringLiteral("nice"), QStringLiteral("Nice!"), 0xbf3989, -4,
         detail::emojiFrom(0x1F44D)},
        {QStringLiteral("thanks"), QStringLiteral("Thanks!"), 0xbc4c00, 6,
         detail::emojiFrom(0x1F64F)},
        {QStringLiteral("on-it"), QStringLiteral("On it"), 0x57606a, -5,
         detail::emojiFrom(0x1F440)},
        {QStringLiteral("wow"), QStringLiteral("Wow!"), 0xcf222e, 4,
         detail::emojiFrom(0x1F62E)},
        {QStringLiteral("love-it"), QStringLiteral("Love it"), 0xd6336c, -3,
         detail::emojiFrom(0x2764, 0xFE0F)},
        {QStringLiteral("party"), QStringLiteral("Party!"), 0x9a6700, 7,
         detail::emojiFrom(0x1F389)},
    };
    return kCatalog;
}

inline QString prefix()
{
    return QStringLiteral("sticker:");
}

inline const CatalogEntry *find(const QString &slug)
{
    for (const CatalogEntry &entry : catalog()) {
        if (entry.slug == slug)
            return &entry;
    }
    return nullptr;
}

inline QString valueForSlug(const QString &slug)
{
    return prefix() + slug;
}

inline const CatalogEntry *entryForValue(const QString &value)
{
    if (!value.startsWith(prefix()))
        return nullptr;
    return find(value.mid(prefix().size()));
}

inline bool isSticker(const QString &value)
{
    return entryForValue(value) != nullptr;
}

inline int catalogIndex(const QString &value)
{
    const CatalogEntry *entry = entryForValue(value);
    return entry ? int(entry - catalog().constData()) : -1;
}

inline QString valueFromWire(const QJsonObject &plain)
{
    const QJsonValue sticker = plain.value(QStringLiteral("sticker"));
    if (sticker.isString() && find(sticker.toString()))
        return valueForSlug(sticker.toString());
    return plain.value(QStringLiteral("emoji")).toString();
}

inline void writeWire(QJsonObject &plain, const QString &value)
{
    if (const CatalogEntry *entry = entryForValue(value)) {
        plain.insert(QStringLiteral("emoji"), entry->fallback);
        plain.insert(QStringLiteral("sticker"), entry->slug);
        return;
    }
    plain.insert(QStringLiteral("emoji"), value);
}

} // namespace stickers
