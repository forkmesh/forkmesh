#include "NoteTools.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>

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

const QString kId = QStringLiteral("0123456789abcdef0123456789abcdef");

void testAppLinks()
{
    check(noteIdFromLink(QStringLiteral("forkmesh://note/") + kId, {}) == kId,
          "forkmesh://note/<id> opens that note");
    check(noteIdFromLink(QStringLiteral("forkmesh://note/") + kId + "/", {}) ==
              kId,
          "a trailing slash on the app link is accepted");
    check(noteIdFromLink(QStringLiteral("forkmesh://note/") + kId.toUpper(),
                         {}) == kId,
          "an upper-case id is folded to the server's lower-case form");
    check(noteIdFromLink(QStringLiteral("forkmesh://note/abc"), {}).isEmpty(),
          "a short id is refused");
    check(noteIdFromLink(QStringLiteral("forkmesh://note/") + kId + "?x=1", {})
              .isEmpty(),
          "an app link with a query is refused");
    check(noteIdFromLink(QStringLiteral("forkmesh://issue/") + kId, {})
              .isEmpty(),
          "another forkmesh:// kind is not a note");
    check(noteIdFromLink(QStringLiteral("forkmesh://note/") + kId + "/extra",
                         {})
              .isEmpty(),
          "extra path segments are refused");
}

void testWebLinks()
{
    const QStringList hosts{QStringLiteral("app.forkmesh.com")};
    check(noteIdFromLink(
              QStringLiteral("https://app.forkmesh.com/dashboard/notes?note=") +
                  kId,
              hosts) == kId,
          "the dashboard ?note= link opens the note");
    check(noteIdFromLink(
              QStringLiteral("https://APP.forkmesh.com/dashboard/notes/") + kId,
              hosts) == kId,
          "the /dashboard/notes/<id> form opens the note");
    check(noteIdFromLink(
              QStringLiteral("https://app.forkmesh.com/dashboard/notes/?note=") +
                  kId + "&x=1",
              hosts) == kId,
          "a trailing slash and extra parameters are tolerated");
    check(noteIdFromLink(
              QStringLiteral("https://elsewhere.example/dashboard/notes?note=") +
                  kId,
              hosts)
              .isEmpty(),
          "a link to another host is not opened in the app");
    check(noteIdFromLink(
              QStringLiteral("https://app.forkmesh.com/dashboard/notes?note=") +
                  kId,
              {})
              .isEmpty(),
          "without a relay host no web link is trusted");
    check(parseNoteLink(QStringLiteral("https://elsewhere.example/dashboard/"
                                       "notes?note=") +
                        kId)
                  .host == QStringLiteral("elsewhere.example"),
          "the parsed host is reported for the caller to check");
    check(noteIdFromLink(QStringLiteral("https://app.forkmesh.com/notes/") + kId,
                         hosts)
              .isEmpty(),
          "the public page is not a dashboard link");
    check(noteIdFromLink(
              QStringLiteral("https://app.forkmesh.com/dashboard/notes"), hosts)
              .isEmpty(),
          "the notes page without an id opens nothing");
    check(noteIdFromLink(
              QStringLiteral("ftp://app.forkmesh.com/dashboard/notes?note=") +
                  kId,
              hosts)
              .isEmpty(),
          "only http and https web links are read");
    check(noteIdFromLink(
              QStringLiteral("https://u:p@app.forkmesh.com/dashboard/notes?note=") +
                  kId,
              hosts)
              .isEmpty(),
          "a link with credentials is refused");
}

void testCommentLinks()
{
    const QString cid = QStringLiteral("fedcba9876543210fedcba9876543210");
    const QStringList hosts{QStringLiteral("app.forkmesh.com")};
    const NoteLink link = trustedNoteLink(
        QStringLiteral("https://app.forkmesh.com/dashboard/notes?note=") + kId +
            "&comment=" + cid.toUpper(),
        hosts);
    check(link.id == kId && link.comment == cid,
          "a mention link carries the comment to open");
    check(trustedNoteLink(QStringLiteral("https://app.forkmesh.com/dashboard/"
                                         "notes?note=") +
                              kId + "&comment=nope",
                          hosts)
                  .comment.isEmpty(),
          "a malformed comment id is ignored but the note still opens");
    check(trustedNoteLink(QStringLiteral("https://elsewhere.example/dashboard/"
                                         "notes?note=") +
                              kId + "&comment=" + cid,
                          hosts)
              .id.isEmpty(),
          "a comment link to another host is not trusted");
    check(parseNoteLink(QStringLiteral("forkmesh://note/") + kId).comment.isEmpty(),
          "an app link has no comment");
}

void testBuiltLinks()
{
    const QUrl base(QStringLiteral("https://app.forkmesh.com/api/repositories?x=1"));
    check(noteAccessLink(base, kId) ==
              QStringLiteral("https://app.forkmesh.com/dashboard/notes?note=") +
                  kId,
          "the link for people with access is the dashboard note link");
    check(notePublicLink(base, kId) ==
              QStringLiteral("https://app.forkmesh.com/notes/") + kId,
          "the public link is /notes/<id>");
    check(noteIdFromLink(noteAccessLink(base, kId),
                         {QStringLiteral("app.forkmesh.com")}) == kId,
          "a copied link opens again in the app");
    check(noteAccessLink(base, QString()).isEmpty(),
          "a note without a cloud id has no link");
}

void testConversationRefs()
{
    ConversationRef ref = parseConversationRef(
        QStringLiteral("acme/tools#12"), QStringLiteral("pull"));
    check(ref.isValid() && ref.owner == QStringLiteral("acme") &&
              ref.repo == QStringLiteral("tools") &&
              ref.kind == QStringLiteral("pull") && ref.number == 12,
          "owner/repo#N takes the chosen kind");
    ref = parseConversationRef(
        QStringLiteral("https://forkmesh.com/acme/tools/issues/7"),
        QStringLiteral("pull"));
    check(ref.isValid() && ref.kind == QStringLiteral("issue") &&
              ref.number == 7,
          "an issue URL is an issue whatever kind is chosen");
    ref = parseConversationRef(
        QStringLiteral("https://forkmesh.com/acme/tools/pulls/3"),
        QStringLiteral("issue"));
    check(ref.isValid() && ref.kind == QStringLiteral("pull"),
          "a pulls URL is a pull request");
    ref = parseConversationRef(
        QStringLiteral("/acme/tools/discussions/4"), QStringLiteral("issue"));
    check(ref.isValid() && ref.kind == QStringLiteral("discussion"),
          "a site-relative discussion path is read");
    ref = parseConversationRef(
        QStringLiteral("https://forkmesh.com/acme/tools?tab=issues#9"),
        QStringLiteral("pull"));
    check(ref.isValid() && ref.kind == QStringLiteral("issue") &&
              ref.number == 9,
          "the repository tab form is read");
    ref = parseConversationRef(QStringLiteral("forkmesh://pull/acme/tools/5"),
                               QStringLiteral("issue"));
    check(ref.isValid() && ref.kind == QStringLiteral("pull") &&
              ref.number == 5,
          "a desktop forkmesh:// reference is read");
    check(!parseConversationRef(QStringLiteral("acme/tools"),
                                QStringLiteral("issue"))
               .isValid(),
          "a repository without a number is refused");
    check(!parseConversationRef(QStringLiteral("acme/tools#0"),
                                QStringLiteral("issue"))
               .isValid(),
          "number zero is refused");
    check(!parseConversationRef(QStringLiteral("acme/tools#1000000000"),
                                QStringLiteral("issue"))
               .isValid(),
          "a number past the server's limit is refused");
    const QJsonObject link{{QStringLiteral("owner"), QStringLiteral("acme")},
                           {QStringLiteral("repo"), QStringLiteral("tools")},
                           {QStringLiteral("kind"), QStringLiteral("pull")},
                           {QStringLiteral("number"), 5}};
    check(conversationLabel(link) == QStringLiteral("Pull request acme/tools#5"),
          "a linked pull request is labelled");
    check(conversationHref(link, QUrl(QStringLiteral("https://x.test/api"))) ==
              QStringLiteral("forkmesh://pull/acme/tools/5"),
          "a linked pull request opens inside the app");
    QJsonObject discussion = link;
    discussion.insert(QStringLiteral("kind"), QStringLiteral("discussion"));
    check(conversationHref(discussion,
                           QUrl(QStringLiteral("https://x.test/api"))) ==
              QStringLiteral("https://x.test/acme/tools/discussions/5"),
          "a linked discussion opens on the relay's site");
    QJsonObject upper = link;
    upper.insert(QStringLiteral("owner"), QStringLiteral("ACME"));
    check(sameConversation(link, upper) && !sameConversation(link, discussion),
          "linked items compare by owner, repo, kind and number");
}

void testExport()
{
    check(exportFileName(QStringLiteral("Release plan: Q4!")) ==
              QStringLiteral("release-plan-q4.md"),
          "the export name is the web's slug");
    check(exportFileName(QStringLiteral("  ")) == QStringLiteral("note.md"),
          "an untitled export is note.md");
    check(exportFileName(QStringLiteral("v1.2_notes")) ==
              QStringLiteral("v1.2_notes.md"),
          "dots and underscores are kept");
    check(exportMarkdown(QStringLiteral("Plan"), QStringLiteral("- a")) ==
              QStringLiteral("# Plan\n\n- a"),
          "the export starts with the title as a heading");
    check(exportMarkdown(QString(), QString()) ==
              QStringLiteral("# Untitled note\n\n"),
          "an untitled export still has a heading");
}

void testShareHelpers()
{
    check(shareQuery(QStringLiteral("  @Alice ")) == QStringLiteral("alice"),
          "a share query drops @ and case like the server");
    check(!shareQueryWorthAsking(QString()), "an empty query is not sent");
    check(!shareQueryWorthAsking(QString(64, QLatin1Char('a'))),
          "a query the server would refuse is not sent");
    check(shareQueryWorthAsking(QStringLiteral("al")), "a short query is sent");
    check(sharedNotice(QStringLiteral("bob")) ==
              QStringLiteral("Shared with bob. Copy the link below to send it."),
          "the post-share notice matches the web");
    check(shareError(QStringLiteral("principal_not_found"),
                     QStringLiteral("bob"), QStringLiteral("x"))
              .contains(QStringLiteral("bob")),
          "an unknown name is named in the error");
    check(shareError(QStringLiteral("other"), QStringLiteral("bob"),
                     QStringLiteral("fallback")) == QStringLiteral("fallback"),
          "other errors keep the relay's wording");
    check(principalTypeLabel(QStringLiteral("organization")) ==
                  QStringLiteral("Organization") &&
              principalTypeLabel(QStringLiteral("user")) ==
                  QStringLiteral("User"),
          "each person shows their type");
}

void testFilters()
{
    check(noteFilters() == QStringList({QStringLiteral("all"),
                                        QStringLiteral("mine"),
                                        QStringLiteral("shared"),
                                        QStringLiteral("public")}),
          "the filters are All, Mine, Shared, Public");
    check(noteFilterLabel(QStringLiteral("shared"), 3) ==
              QStringLiteral("Shared 3"),
          "a filter shows its count");
    check(noteMatchesFilter(QStringLiteral("mine"), QStringLiteral("owner"),
                            QStringLiteral("private")) &&
              !noteMatchesFilter(QStringLiteral("mine"),
                                 QStringLiteral("editor"),
                                 QStringLiteral("private")),
          "Mine is the notes you own");
    check(noteMatchesFilter(QStringLiteral("shared"), QStringLiteral("viewer"),
                            QStringLiteral("private")) &&
              !noteMatchesFilter(QStringLiteral("shared"),
                                 QStringLiteral("owner"),
                                 QStringLiteral("private")),
          "Shared is the notes others shared with you");
    check(noteMatchesFilter(QStringLiteral("public"), QStringLiteral("owner"),
                            QStringLiteral("public")) &&
              !noteMatchesFilter(QStringLiteral("public"),
                                 QStringLiteral("owner"),
                                 QStringLiteral("private")),
          "Public is the published notes");
    check(noteMatchesFilter(QStringLiteral("all"), QStringLiteral("viewer"),
                            QStringLiteral("private")),
          "All shows everything");
    check(noteMatchesSearch(QStringLiteral("PLAN"), QStringLiteral("Launch plan"),
                            QString()) &&
              noteMatchesSearch(QStringLiteral("rollout"), QStringLiteral("x"),
                                QStringLiteral("the rollout steps")) &&
              !noteMatchesSearch(QStringLiteral("zzz"), QStringLiteral("x"),
                                 QStringLiteral("y")) &&
              noteMatchesSearch(QStringLiteral("  "), QStringLiteral("x"),
                                QString()),
          "search reads the title and the body, ignoring case");
    check(noteSearchQuery(QStringLiteral("  Plan ")) == QStringLiteral("plan"),
          "the cloud search query is trimmed and lower-cased like the server");
    check(!noteSearchWorthAsking(QString()) &&
              !noteSearchWorthAsking(QString(201, QLatin1Char('a'))) &&
              noteSearchWorthAsking(QString(200, QLatin1Char('a'))),
          "an empty or over-long search is not sent to the relay");
    check(noteEmptyListText(QStringLiteral("all"), QStringLiteral("q")) ==
              QStringLiteral("No notes match “q”."),
          "an empty search names the query");
    check(noteCanEdit(QStringLiteral("editor")) &&
              !noteCanEdit(QStringLiteral("viewer")) &&
              !noteCanEdit(QStringLiteral("public")),
          "owners and editors can edit, viewers cannot");
}

void testConflict()
{
    check(isVersionConflict(QJsonObject{
              {QStringLiteral("error"), QStringLiteral("version_conflict")},
              {QStringLiteral("note"),
               QJsonObject{{QStringLiteral("version"), 4}}}}),
          "a 409 with the current note is a conflict to resolve");
    check(!isVersionConflict(QJsonObject{
              {QStringLiteral("error"), QStringLiteral("version_conflict")}}),
          "a conflict without the current note cannot be resolved");
    check(!isVersionConflict(QJsonObject{
              {QStringLiteral("error"), QStringLiteral("note_not_found")},
              {QStringLiteral("note"), QJsonObject{}}}),
          "other errors are not conflicts");
}

}

qint64 localMs(int year, int month, int day, int hour, int minute)
{
    return QDateTime(QDate(year, month, day), QTime(hour, minute))
        .toMSecsSinceEpoch();
}

void testListRow()
{
    const QDateTime now(QDate(2026, 10, 31), QTime(18, 0));
    check(noteEditStamp(localMs(2026, 10, 31, 15, 14), now) ==
              QStringLiteral("Today 3:14 PM"),
          "an edit today reads Today and the time");
    check(noteEditStamp(localMs(2026, 10, 30, 9, 2), now) ==
              QStringLiteral("Yesterday 9:02 AM"),
          "an edit yesterday reads Yesterday and the time");
    check(noteEditStamp(localMs(2026, 10, 29, 15, 14), now) ==
              QStringLiteral("Oct 29, 3:14 PM"),
          "an older edit this year shows the date and time");
    check(noteEditStamp(localMs(2025, 10, 29, 15, 14), now) ==
              QStringLiteral("Oct 29, 2025"),
          "an edit in an earlier year shows the date with the year");
    check(noteEditStamp(localMs(2026, 1, 1, 0, 5), now) ==
              QStringLiteral("Jan 1, 12:05 AM"),
          "midnight reads 12 AM");
    check(noteEditStamp(0, now).isEmpty(), "no edit time gives no stamp");
    check(noteLastEditLabel(localMs(2026, 10, 31, 15, 14), QStringLiteral("ana"),
                            QStringLiteral("me"), now) ==
              QStringLiteral("Today 3:14 PM · by ana"),
          "someone else's edit is named");
    check(noteLastEditLabel(localMs(2026, 10, 31, 15, 14), QStringLiteral("Me"),
                            QStringLiteral("me"), now) ==
              QStringLiteral("Today 3:14 PM"),
          "your own edit is not named");
    check(noteLastEditTooltip(localMs(2026, 10, 29, 15, 14), QStringLiteral("ana"),
                              QStringLiteral("me")) ==
              QStringLiteral("Last edited Thursday, October 29, 2026, 3:14 PM by ana"),
          "the tooltip has the full date and time");

    check(noteStorageBadge(QStringLiteral("local")) == QStringLiteral("Local") &&
              noteStorageBadge(QStringLiteral("cloud")) == QStringLiteral("Cloud") &&
              noteStorageBadge(QStringLiteral("both")) ==
                  QStringLiteral("Local + cloud"),
          "storage badges");
    const QJsonArray shares{
        QJsonObject{{QStringLiteral("type"), QStringLiteral("user")},
                    {QStringLiteral("name"), QStringLiteral("ana")}},
        QJsonObject{{QStringLiteral("type"), QStringLiteral("organization")},
                    {QStringLiteral("name"), QStringLiteral("acme")}},
        QJsonObject{{QStringLiteral("name"), QString()}}};
    check(noteVisibilityBadge(QJsonObject{{QStringLiteral("visibility"),
                                           QStringLiteral("public")}},
                              true) == QStringLiteral("Public"),
          "a published note is Public");
    check(noteVisibilityBadge(QJsonObject{{QStringLiteral("shares"), shares}}, true) ==
              QStringLiteral("Shared"),
          "a note shared with someone is Shared");
    check(noteVisibilityBadge(QJsonObject{}, true) == QStringLiteral("Private") &&
              noteVisibilityBadge(QJsonObject{{QStringLiteral("shares"), shares}},
                                  false) == QStringLiteral("Private"),
          "an unshared or not-yet-synced note is Private");
    const QList<NoteSharePerson> people = noteSharePeople(shares);
    check(people.size() == 2 && !people.at(0).organization &&
              people.at(1).organization,
          "share avatars know organizations, nameless shares are skipped");
    check(noteShareOverflow(3).isEmpty() && noteShareOverflow(5) == QStringLiteral("+2"),
          "three avatars, then +N");

    const QJsonObject unread{{QStringLiteral("unread"), true}};
    check(noteUnread(unread, QStringLiteral("cloud")) &&
              noteUnread(unread, QStringLiteral("both")),
          "cloud and Local + cloud notes follow the cloud's unread flag");
    check(!noteUnread(unread, QStringLiteral("local")),
          "local-only notes are never unread");
    check(noteUnread(QJsonObject{{QStringLiteral("unreadComments"), 2}},
                     QStringLiteral("cloud")),
          "new comments make a note unread");
    const QJsonObject read = noteMarkedRead(
        QJsonObject{{QStringLiteral("unread"), true},
                    {QStringLiteral("unreadComments"), 3}},
        42);
    check(!noteUnread(read, QStringLiteral("cloud")) &&
              read.value(QStringLiteral("myLastViewedAt")).toDouble() == 42,
          "opening a note clears unread locally at once");
    const QJsonObject kept = noteKeepFreshness(
        QJsonObject{{QStringLiteral("id"), kId}},
        QJsonObject{{QStringLiteral("openComments"), 4},
                    {QStringLiteral("lastEditedBy"), QStringLiteral("ana")}},
        QStringLiteral("me"));
    check(kept.value(QStringLiteral("openComments")).toInt() == 4 &&
              kept.value(QStringLiteral("lastEditedBy")).toString() ==
                  QStringLiteral("me"),
          "a save keeps the comment count and makes you the last editor");
    const QJsonArray access{
        QJsonObject{{QStringLiteral("type"), QStringLiteral("user")},
                    {QStringLiteral("name"), QStringLiteral("ana")},
                    {QStringLiteral("role"), QStringLiteral("editor")}},
        QJsonObject{{QStringLiteral("type"), QStringLiteral("organization")},
                    {QStringLiteral("name"), QStringLiteral("acme")},
                    {QStringLiteral("role"), QStringLiteral("viewer")}}};
    check(noteAccessHeader(QStringLiteral("owner"), access, QStringLiteral("Shared"),
                           QStringLiteral("Cloud"), localMs(2026, 10, 29, 15, 14),
                           QStringLiteral("ana"), QStringLiteral("me")) ==
              QStringList({QStringLiteral("You own this note"),
                           QStringLiteral("Shared with ana (can edit), acme "
                                          "(organization, can view)"),
                           QStringLiteral("Shared · Cloud"),
                           QStringLiteral("Last edited Thursday, October 29, "
                                          "2026, 3:14 PM by ana")}),
          "the row menu header names your access, the people, the badges and "
          "the last edit");
    check(noteRoleLine(QStringLiteral("editor")) == QStringLiteral("You can edit") &&
              noteRoleLine(QStringLiteral("viewer")) == QStringLiteral("View only"),
          "editors and viewers are told what they can do");
    check(noteAccessHeader(QStringLiteral("owner"), {}, QStringLiteral("Private"),
                           QStringLiteral("Local"), 0, {}, {}) ==
              QStringList({QStringLiteral("You own this note"),
                           QStringLiteral("Not shared"),
                           QStringLiteral("Private · Local")}),
          "a note nobody else has, with no edit time, has three lines");
    check(noteCommentCountLabel(1) == QStringLiteral("1 open comment") &&
              noteCommentCountLabel(3) == QStringLiteral("3 open comments"),
          "comment count wording");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testAppLinks();
    testWebLinks();
    testCommentLinks();
    testBuiltLinks();
    testConversationRefs();
    testExport();
    testShareHelpers();
    testFilters();
    testConflict();
    testListRow();
    if (failures == 0)
        std::puts("note tools: all checks passed");
    return failures == 0 ? 0 : 1;
}
