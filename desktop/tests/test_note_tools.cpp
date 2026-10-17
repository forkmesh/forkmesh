#include "NoteTools.h"

#include <QCoreApplication>

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

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testAppLinks();
    testWebLinks();
    testBuiltLinks();
    testConversationRefs();
    testExport();
    testShareHelpers();
    testFilters();
    testConflict();
    if (failures == 0)
        std::puts("note tools: all checks passed");
    return failures == 0 ? 0 : 1;
}
