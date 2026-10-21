#pragma once

#include <QChar>
#include <QHash>
#include <QString>
#include <QVector>

// The one reaction vocabulary, shared by the chat UI and every wire format.
//
// ForkMesh chat stores a reaction as a stable slug ("like"): rows render it as
// hand-drawn art instead of trusting the host font, and mesh peers exchange the
// slug. Discord addresses a reaction only by its literal emoji, so a slug that
// reached its REST path came back as "Unknown Emoji" and the reaction was lost
// — and because the reactions Discord reported were spelled the other way, a
// reaction this node had already added never matched its own bookkeeping, so
// clicking a chip could only ever add again, never undo.
//
// Both spellings live in one table so that translation is total: adding a
// picker entry without an emoji here is visible at the table, not as a failed
// request on someone's message. Lives in a header so the network layer can
// translate without linking the Gui-only painters in ReactionEmoji.cpp, and so
// the chat transport tests can pin the round trip without the whole app.
namespace reactions {

struct CatalogEntry {
    QString value; // wire value stored in reaction payloads ("like")
    QString label; // human-readable name ("Like")
    QString emoji; // literal unicode every other chat service speaks
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
        {QStringLiteral("like"), QStringLiteral("Like"),
         detail::emojiFrom(0x1F44D)},
        {QStringLiteral("love"), QStringLiteral("Love"),
         detail::emojiFrom(0x2764, 0xFE0F)},
        {QStringLiteral("laugh"), QStringLiteral("Laugh"),
         detail::emojiFrom(0x1F602)},
        {QStringLiteral("celebrate"), QStringLiteral("Celebrate"),
         detail::emojiFrom(0x1F389)},
        {QStringLiteral("hot"), QStringLiteral("Hot"),
         detail::emojiFrom(0x1F525)},
        {QStringLiteral("thanks"), QStringLiteral("Thanks"),
         detail::emojiFrom(0x1F64F)},
        {QStringLiteral("smile"), QStringLiteral("Smile"),
         detail::emojiFrom(0x1F642)},
        {QStringLiteral("wink"), QStringLiteral("Wink"),
         detail::emojiFrom(0x1F609)},
        {QStringLiteral("cool"), QStringLiteral("Cool"),
         detail::emojiFrom(0x1F60E)},
        {QStringLiteral("thinking"), QStringLiteral("Thinking"),
         detail::emojiFrom(0x1F914)},
        {QStringLiteral("surprised"), QStringLiteral("Surprised"),
         detail::emojiFrom(0x1F62E)},
        {QStringLiteral("sad"), QStringLiteral("Sad"),
         detail::emojiFrom(0x1F622)},
        {QStringLiteral("angry"), QStringLiteral("Angry"),
         detail::emojiFrom(0x1F621)},
        {QStringLiteral("dislike"), QStringLiteral("Dislike"),
         detail::emojiFrom(0x1F44E)},
        {QStringLiteral("eyes"), QStringLiteral("Eyes"),
         detail::emojiFrom(0x1F440)},
        {QStringLiteral("star"), QStringLiteral("Star"),
         detail::emojiFrom(0x2B50)},
        {QStringLiteral("rocket"), QStringLiteral("Rocket"),
         detail::emojiFrom(0x1F680)},
        {QStringLiteral("check"), QStringLiteral("Done"),
         detail::emojiFrom(0x2705)},
        // The gratitude group (web "Say thanks"); "thankyou" is the comic
        // "THANK YOU" sticker, spelled as a speech balloon for text-only
        // services.
        {QStringLiteral("hearthands"), QStringLiteral("Heart hands"),
         detail::emojiFrom(0x1FAF6)},
        {QStringLiteral("sparkleheart"), QStringLiteral("Sparkling heart"),
         detail::emojiFrom(0x1F496)},
        {QStringLiteral("handshake"), QStringLiteral("Handshake"),
         detail::emojiFrom(0x1F91D)},
        {QStringLiteral("glowstar"), QStringLiteral("Glowing star"),
         detail::emojiFrom(0x1F31F)},
        {QStringLiteral("trophy"), QStringLiteral("Trophy"),
         detail::emojiFrom(0x1F3C6)},
        {QStringLiteral("thankyou"), QStringLiteral("Thank you"),
         detail::emojiFrom(0x1F4AC)},
    };
    return kCatalog;
}

// Variation selectors ride along with whoever added the reaction: Discord
// reports the heart as U+2764 U+FE0F, older ForkMesh rows stored the bare
// U+2764. Both have to land on one slug or the same reaction shows up twice.
inline QString withoutVariationSelectors(const QString &emoji)
{
    QString bare;
    bare.reserve(emoji.size());
    for (const QChar ch : emoji) {
        if (ch != QChar(0xFE0F) && ch != QChar(0xFE0E))
            bare.append(ch);
    }
    return bare;
}

namespace detail {

inline const QHash<QString, QString> &byValue()
{
    static const QHash<QString, QString> kByValue = [] {
        QHash<QString, QString> map;
        for (const CatalogEntry &entry : catalog())
            map.insert(entry.value, entry.emoji);
        return map;
    }();
    return kByValue;
}

} // namespace detail

// True when this build can spell the slug for a unicode-only service. False for
// anything already unicode, and for a slug no longer in the catalog.
inline bool hasUnicode(const QString &value)
{
    return detail::byValue().contains(value);
}

// Slug -> literal emoji. Anything already unicode passes through untouched, so
// a reaction another client invented still reaches the service it came from.
inline QString unicodeForValue(const QString &value)
{
    return detail::byValue().value(value, value);
}

// Literal emoji -> slug, keeping the original spelling when this build has no
// drawing for it (the chip then renders the emoji as text).
inline QString valueForUnicode(const QString &emoji)
{
    static const QHash<QString, QString> kByEmoji = [] {
        QHash<QString, QString> map;
        for (const CatalogEntry &entry : catalog())
            map.insert(withoutVariationSelectors(entry.emoji), entry.value);
        return map;
    }();
    return kByEmoji.value(withoutVariationSelectors(emoji), emoji);
}

} // namespace reactions
