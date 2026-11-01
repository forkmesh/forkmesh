#include "NotePages.h"
#include "NoteTools.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>

#include <cstdio>

using namespace forkmesh::notes;

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

const QString kPage = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
const QString kNote = QStringLiteral("0123456789abcdef0123456789abcdef");

QJsonObject entry(const QString &id, const QString &kind, int depth,
                  const QString &label = QString(), const QString &noteId = QString())
{
    return QJsonObject{{QStringLiteral("id"), id},
                       {QStringLiteral("kind"), kind},
                       {QStringLiteral("depth"), depth},
                       {QStringLiteral("label"), label},
                       {QStringLiteral("noteId"), noteId}};
}

QStringList ids(const QJsonArray &entries)
{
    QStringList result;
    for (const QJsonValue &value : entries)
        result << value.toObject().value(QStringLiteral("id")).toString();
    return result;
}

QList<int> depths(const QJsonArray &entries)
{
    QList<int> result;
    for (const QJsonValue &value : entries)
        result << outlineDepth(value.toObject());
    return result;
}

void testEmoji()
{
    check(isValidNoteEmoji(QString()), "an empty emoji clears it");
    check(isValidNoteEmoji(QStringLiteral(" 📝 ")), "blanks around an emoji are trimmed");
    check(isValidNoteEmoji(QStringLiteral("👍🏽")), "a skin tone is allowed");
    check(isValidNoteEmoji(QStringLiteral("🇸🇪")), "a flag is allowed");
    check(isValidNoteEmoji(QStringLiteral("👩‍💻")), "a ZWJ sequence is allowed");
    check(isValidNoteEmoji(QStringLiteral("❤️")), "a variation selector is allowed");
    check(isValidNoteEmoji(QStringLiteral("1️⃣")) && isValidNoteEmoji(QStringLiteral("#⃣")),
          "digits and # count as keycaps when U+20E3 follows");
    check(!isValidNoteEmoji(QStringLiteral("1")) && !isValidNoteEmoji(QStringLiteral("*")),
          "a bare digit or * is not an emoji");
    check(!isValidNoteEmoji(QStringLiteral("a")) && !isValidNoteEmoji(QStringLiteral("📝a")),
          "ASCII letters are refused");
    check(!isValidNoteEmoji(QStringLiteral("📝 📝")), "inner whitespace is refused");
    check(!isValidNoteEmoji(QStringLiteral("📝\u0007")), "control characters are refused");
    check(!isValidNoteEmoji(QStringLiteral("é")), "a letter outside ASCII is refused");
    check(!isValidNoteEmoji(QStringLiteral("!")), "ASCII punctuation is refused");
    check(isValidNoteEmoji(QString(16, QChar(0x2605))) &&
              !isValidNoteEmoji(QString(17, QChar(0x2605))),
          "at most 16 code points");
    check(!isValidNoteEmoji(QString::fromUcs4(U"🏴🏴🏴🏴🏴🏴🏴🏴🏴🏴🏴🏴🏴🏴🏴🏴🏴")),
          "17 four-byte code points pass neither limit");
    const QString heavy = QString::fromUcs4(U"😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀");
    check(heavy.toUtf8().size() == 64 && isValidNoteEmoji(heavy),
          "64 UTF-8 bytes is the limit and still allowed");
    bool allValid = true;
    for (const QString &value : commonNoteEmoji())
        allValid = allValid && isValidNoteEmoji(value);
    check(allValid, "every emoji in the picker's grid passes the rule");
    check(noteDisplayTitle(QStringLiteral("📝"), QStringLiteral("Plan")) ==
              QStringLiteral("📝 Plan") &&
              noteDisplayTitle(QString(), QStringLiteral("Plan")) == QStringLiteral("Plan") &&
              noteDisplayTitle(QStringLiteral("📝"), QString()) ==
                  QStringLiteral("📝 Untitled note"),
          "the emoji goes before the title");
}

void testOutlineRules()
{
    const QJsonArray base{entry(QStringLiteral("aaaaaaaa"), QStringLiteral("section"), 0),
                          entry(QStringLiteral("bbbbbbbb"), QStringLiteral("note"), 1),
                          entry(QStringLiteral("cccccccc"), QStringLiteral("note"), 1),
                          entry(QStringLiteral("dddddddd"), QStringLiteral("note"), 0)};
    check(depths(normalizedOutline(QJsonArray{
              entry(QStringLiteral("aaaaaaaa"), QStringLiteral("note"), 2),
              entry(QStringLiteral("bbbbbbbb"), QStringLiteral("note"), 3),
              entry(QStringLiteral("cccccccc"), QStringLiteral("note"), 9)})) ==
              QList<int>({0, 1, 2}),
          "the first entry sits at 0 and each is at most one deeper than the one before");
    const QJsonArray fixedIds = normalizedOutline(QJsonArray{
        entry(QStringLiteral("short"), QStringLiteral("note"), 0),
        entry(QStringLiteral("dup-dup-1"), QStringLiteral("odd"), 0),
        entry(QStringLiteral("dup-dup-1"), QStringLiteral("link"), 0)});
    check(isOutlineId(ids(fixedIds).at(0)) && ids(fixedIds).at(1) == QStringLiteral("dup-dup-1") &&
              ids(fixedIds).at(2) != QStringLiteral("dup-dup-1"),
          "bad and repeated ids get fresh 8-32 character ids");
    check(fixedIds.at(1).toObject().value(QStringLiteral("kind")).toString() ==
              QStringLiteral("note"),
          "an unknown kind is read as a note");
    check(!canIndentOutline(base, 0), "the first entry cannot be indented");
    check(!canIndentOutline(base, 1), "an entry already under its neighbour stays");
    check(canIndentOutline(base, 2), "an entry level with the one above can indent");
    check(depths(indentOutline(base, 2, 1)) == QList<int>({0, 1, 2, 0}),
          "indent moves one level in");
    check(depths(indentOutline(base, 1, -1)) == QList<int>({0, 0, 1, 0}),
          "outdent moves one level out and keeps the next entry valid");
    check(!canOutdentOutline(base, 0), "depth 0 cannot be outdented");
    QJsonArray deep{entry(QStringLiteral("aaaaaaaa"), QStringLiteral("note"), 0),
                    entry(QStringLiteral("bbbbbbbb"), QStringLiteral("note"), 1),
                    entry(QStringLiteral("cccccccc"), QStringLiteral("note"), 2),
                    entry(QStringLiteral("dddddddd"), QStringLiteral("note"), 3),
                    entry(QStringLiteral("eeeeeeee"), QStringLiteral("note"), 3)};
    check(!canIndentOutline(deep, 4), "nothing goes past depth 3");
    check(ids(moveOutline(base, 3, -1)) ==
              QStringList({QStringLiteral("aaaaaaaa"), QStringLiteral("bbbbbbbb"),
                           QStringLiteral("dddddddd"), QStringLiteral("cccccccc")}),
          "move up swaps with the entry above");
    check(depths(moveOutline(base, 1, -1)) == QList<int>({0, 0, 1, 0}),
          "moving to the top drops the entry to depth 0");
    check(moveOutline(base, 0, -1) == base && moveOutline(base, 3, 1) == base,
          "moves past either end do nothing");
    check(ids(removeOutline(base, 0)).size() == 3 &&
              depths(removeOutline(base, 0)) == QList<int>({0, 1, 0}),
          "removing a section lifts the entries under it");
    const QJsonObject added = outlineEntry(QStringLiteral("note"), 0, QString(), kNote);
    const QJsonArray inserted = insertOutline(base, 0, added);
    check(ids(inserted).at(1) == added.value(QStringLiteral("id")).toString() &&
              depths(inserted).at(1) == 1,
          "a note added under a section goes inside it");
    check(insertOutline(base, -1, added).last().toObject().value(QStringLiteral("id")) ==
              added.value(QStringLiteral("id")),
          "with nothing picked a new entry goes at the end");
    QJsonArray full;
    for (int i = 0; i < kMaxOutlineEntries; ++i)
        full.append(outlineEntry(QStringLiteral("note"), 0, QString(), kNote));
    check(insertOutline(full, 0, added).size() == kMaxOutlineEntries,
          "a page holds at most 300 entries");
    check(isOutlineUrl(QStringLiteral("https://forkmesh.com/docs")) &&
              !isOutlineUrl(QStringLiteral("http://forkmesh.com")) &&
              !isOutlineUrl(QStringLiteral("https://user@forkmesh.com")) &&
              !isOutlineUrl(QStringLiteral("javascript:alert(1)")),
          "links must be https:// with a host");
    const QJsonObject link = outlineEntry(QStringLiteral("link"), 5, QStringLiteral("Docs"),
                                          QString(), QStringLiteral(" https://x.y "));
    check(isOutlineId(link.value(QStringLiteral("id")).toString()) &&
              outlineDepth(link) == 3 &&
              link.value(QStringLiteral("url")).toString() == QStringLiteral("https://x.y"),
          "new entries get an id, a depth of 0-3 and a trimmed url");
}

void testOutlineView()
{
    QJsonArray outline{entry(QStringLiteral("s1aaaaaa"), QStringLiteral("section"), 0,
                             QStringLiteral("Plans")),
                       entry(QStringLiteral("n1aaaaaa"), QStringLiteral("note"), 0, {}, kNote),
                       entry(QStringLiteral("n2aaaaaa"), QStringLiteral("note"), 1, {},
                             QStringLiteral("ffffffffffffffffffffffffffffffff")),
                       entry(QStringLiteral("s2aaaaaa"), QStringLiteral("section"), 0,
                             QStringLiteral("Later")),
                       entry(QStringLiteral("n3aaaaaa"), QStringLiteral("note"), 1, {}, kNote)};
    check(visibleOutlineRows(outline, {}) == QList<int>({0, 1, 2, 3, 4}),
          "everything shows when nothing is collapsed");
    check(visibleOutlineRows(outline, {QStringLiteral("s1aaaaaa")}) == QList<int>({0, 3, 4}),
          "a collapsed section hides what follows until the next section at its level");
    check(visibleOutlineRows(outline, {QStringLiteral("s2aaaaaa")}) == QList<int>({0, 1, 2, 3}),
          "collapsing the last section hides its indented notes");
    QJsonObject collapsedSection = outline.at(0).toObject();
    collapsedSection.insert(QStringLiteral("collapsed"), true);
    outline.replace(0, collapsedSection);
    check(collapsedOutlineSections(outline) == QSet<QString>({QStringLiteral("s1aaaaaa")}),
          "the saved collapsed flag starts sections closed");
    const QJsonObject notes{{kNote, QJsonObject{{QStringLiteral("title"), QStringLiteral("Plan")},
                                                {QStringLiteral("emoji"), QStringLiteral("📌")}}}};
    check(outlineEntryLabel(outline.at(1).toObject(), notes, true) ==
              QStringLiteral("📌 Plan") &&
              outlineEntryLabel(outline.at(1).toObject(), notes, false) ==
                  QStringLiteral("Plan"),
          "notes show their emoji unless the page turns it off");
    check(outlineEntryLocked(outline.at(2).toObject(), notes) &&
              outlineEntryLabel(outline.at(2).toObject(), notes, true) ==
                  QStringLiteral("Locked note"),
          "a note left out of the answer is locked");
    QJsonObject renamed = outline.at(1).toObject();
    renamed.insert(QStringLiteral("label"), QStringLiteral("Roadmap"));
    check(outlineEntryLabel(renamed, notes, true) == QStringLiteral("📌 Roadmap"),
          "a label overrides the note's title");
    check(outlineNoteIds(outline, notes) == QStringList({kNote}) &&
              firstOpenableNote(outline, notes) == kNote,
          "content lists each openable note once, in outline order");
}

void testPageFields()
{
    const QJsonObject fields = pageEditFields(QJsonObject{
        {QStringLiteral("title"), QString(250, QLatin1Char('t'))},
        {QStringLiteral("accent"), QStringLiteral("neon")},
        {QStringLiteral("layout"), QStringLiteral("grid")}});
    check(fields.value(QStringLiteral("title")).toString().size() == kMaxPageTitle,
          "the title is cut at 200 characters");
    check(fields.value(QStringLiteral("accent")).toString() == QStringLiteral("blue") &&
              fields.value(QStringLiteral("layout")).toString() == QStringLiteral("outline") &&
              fields.value(QStringLiteral("showEmoji")).toBool(),
          "unknown colour and layout fall back; show emoji defaults on");
    check(pageAccents().size() == 8 && pageAccentHex(QStringLiteral("teal")).startsWith('#'),
          "the eight colours of the spec");
    check(pageTitle(QJsonObject{}) == QStringLiteral("Untitled page"),
          "an empty title reads Untitled page");
    check(pageEntriesLabel(1) == QStringLiteral("1 entry") &&
              pageEntriesLabel(4) == QStringLiteral("4 entries"),
          "entry count wording");
    check(pageError(QStringLiteral("too_many_entries"), {}).contains(QStringLiteral("300")),
          "the entry cap is explained");
}

void testPageLinks()
{
    const QStringList hosts{QStringLiteral("app.forkmesh.com")};
    const NoteLink page = trustedNoteLink(
        QStringLiteral("https://app.forkmesh.com/dashboard/notes?page=") + kPage, hosts);
    check(page.isValid() && page.page == kPage && page.id.isEmpty(),
          "?page=<id> opens the page");
    const NoteLink picked = trustedNoteLink(
        QStringLiteral("https://app.forkmesh.com/dashboard/notes?page=") + kPage.toUpper() +
            "&note=" + kNote,
        hosts);
    check(picked.page == kPage && picked.id == kNote,
          "&note= picks a note on the page");
    check(!trustedNoteLink(QStringLiteral("https://elsewhere.example/dashboard/notes?page=") +
                               kPage,
                           hosts)
               .isValid(),
          "a page link to another host is not trusted");
    check(!parseNoteLink(QStringLiteral("https://app.forkmesh.com/dashboard/notes?page=nope"))
               .isValid(),
          "a malformed page id is not a link");
    check(parseNoteLink(QStringLiteral("https://app.forkmesh.com/dashboard/notes/") + kNote +
                        "?page=" + kPage)
              .page.isEmpty(),
          "the page parameter only counts on /dashboard/notes");
    check(noteIdFromLink(QStringLiteral("https://app.forkmesh.com/dashboard/notes?page=") +
                             kPage,
                         hosts)
              .isEmpty(),
          "a page link is not mistaken for a note");
    check(pageAccessLink(QUrl(QStringLiteral("https://app.forkmesh.com/api/x")), kPage, kNote) ==
              QStringLiteral("https://app.forkmesh.com/dashboard/notes?page=") + kPage +
                  "&note=" + kNote,
          "the page link names the page and the picked note");
}

}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testEmoji();
    testOutlineRules();
    testOutlineView();
    testPageFields();
    testPageLinks();
    if (failures == 0)
        std::puts("note pages: all checks passed");
    return failures == 0 ? 0 : 1;
}
