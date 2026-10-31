#include "NoteComments.h"

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

QJsonObject comment(const QString &id, const QString &parent,
                    const QString &quote = QString(), int start = 0,
                    qint64 created = 1, qint64 resolved = 0,
                    const QString &prefix = QString(),
                    const QString &suffix = QString())
{
    return QJsonObject{{QStringLiteral("id"), id},
                       {QStringLiteral("parentId"), parent},
                       {QStringLiteral("author"), QStringLiteral("ana")},
                       {QStringLiteral("body"), QStringLiteral("text")},
                       {QStringLiteral("quote"), quote},
                       {QStringLiteral("prefix"), prefix},
                       {QStringLiteral("suffix"), suffix},
                       {QStringLiteral("start"), start},
                       {QStringLiteral("end"), start + int(quote.size())},
                       {QStringLiteral("createdAt"), double(created)},
                       {QStringLiteral("resolvedAt"), double(resolved)}};
}

void testAnchorScoring()
{
    const QString text = QStringLiteral("cat one. the cat sat. a cat ran.");
    CommentAnchor anchor = findCommentAnchor(text, QStringLiteral("cat"),
                                             QStringLiteral("the "),
                                             QStringLiteral(" sat"), 0);
    check(anchor.start == 13 && anchor.end == 16,
          "prefix and suffix pick the occurrence they surround");
    anchor = findCommentAnchor(text, QStringLiteral("cat"), QStringLiteral("zz"),
                               QStringLiteral("zz"), 26);
    check(anchor.start == 24,
          "without matching context the occurrence nearest start wins");
    anchor = findCommentAnchor(text, QStringLiteral("cat"), QStringLiteral("a "),
                               QStringLiteral("zz"), 0);
    check(anchor.start == 24,
          "a matching prefix (2) beats a closer occurrence with none");
    anchor = findCommentAnchor(text, QStringLiteral("cat"), QStringLiteral("zz"),
                               QStringLiteral(" ran"), 0);
    check(anchor.start == 24, "a matching suffix alone scores 2 as well");
    anchor = findCommentAnchor(text, QStringLiteral("cat"), QStringLiteral("the "),
                               QStringLiteral(" ran"), 0);
    check(anchor.start == 13,
          "equal scores fall back to the distance from start");
    check(!findCommentAnchor(text, QStringLiteral("dog"), {}, {}, 0).found(),
          "a quote no longer in the note is not found");
    check(!findCommentAnchor(text, QString(), {}, {}, 0).found(),
          "an empty quote anchors nothing");
    anchor = findCommentAnchor(QStringLiteral("aaaa"), QStringLiteral("aa"),
                               QStringLiteral("a"), QStringLiteral("a"), 0);
    check(anchor.start == 1, "overlapping occurrences are all considered");
}

void testUtf16Offsets()
{
    const QString text = QStringLiteral("\U0001F600 hi there \U0001F600 hi");
    const CommentAnchor anchor =
        findCommentAnchor(text, QStringLiteral("hi"), QStringLiteral("\U0001F600 "),
                          QStringLiteral(" there"), 0);
    check(anchor.start == 3 && anchor.end == 5,
          "offsets count UTF-16 code units, so an emoji counts as two");
    const CommentQuote quote = commentQuoteAt(text, 3, 5);
    check(quote.prefix == QStringLiteral("\U0001F600 ") && quote.start == 3,
          "the captured prefix keeps the whole surrogate pair");
    const QString emojis = QString(40, QLatin1Char('x')) +
        QStringLiteral("\U0001F600") + QString(31, QLatin1Char('y')) +
        QStringLiteral("Q");
    const int at = int(emojis.indexOf(QLatin1Char('Q')));
    const CommentQuote cut = commentQuoteAt(emojis, at, at + 1);
    check(cut.prefix.size() == 31 && !cut.prefix.at(0).isLowSurrogate(),
          "a context window never starts in the middle of a surrogate pair");
}

void testQuoteCapture()
{
    const QString text = QString(40, QLatin1Char('a')) +
        QStringLiteral("  the passage ") + QString(40, QLatin1Char('b'));
    const int start = 40;
    const int end = start + int(QStringLiteral("  the passage ").size());
    const CommentQuote quote = commentQuoteAt(text, start, end);
    check(quote.quote == QStringLiteral("the passage"),
          "surrounding blanks are trimmed from the quote");
    check(quote.start == 42 && quote.end == 53,
          "start and end follow the trimmed quote");
    check(quote.prefix.size() == kCommentContext &&
              quote.prefix.endsWith(QStringLiteral("aa  ")),
          "the prefix is the 32 code units before the quote");
    check(quote.suffix.size() == kCommentContext &&
              quote.suffix.startsWith(QStringLiteral(" bb")),
          "the suffix is the 32 code units after the quote");
    const QJsonObject fields = commentQuoteFields(quote);
    check(fields.value(QStringLiteral("quote")).toString() == quote.quote &&
              fields.value(QStringLiteral("start")).toInt() == 42 &&
              fields.value(QStringLiteral("end")).toInt() == 53 &&
              fields.contains(QStringLiteral("prefix")) &&
              fields.contains(QStringLiteral("suffix")),
          "the request carries quote, prefix, suffix, start and end");
    check(commentQuoteFields(CommentQuote()).isEmpty(),
          "a whole-note comment sends no anchor fields");
    const CommentQuote edge = commentQuoteAt(QStringLiteral("hello"), 0, 5);
    check(edge.prefix.isEmpty() && edge.suffix.isEmpty(),
          "context is shorter at the edges of the note");
    const CommentQuote found = commentQuoteForText(
        QStringLiteral("one **bold** two"), QStringLiteral(" two "));
    check(found.quote == QStringLiteral("two") && found.start == 13,
          "text picked in Preview is located in the Markdown");
    check(commentQuoteForText(QStringLiteral("one **bold** two"),
                              QStringLiteral("one bold"))
              .isEmpty(),
          "rendered text that differs from the Markdown is not guessed");
}

void testPreviewSearchText()
{
    check(previewSearchText(QStringLiteral("## A **bold** title")) ==
              QStringLiteral("A bold title"),
          "heading markers and emphasis are dropped for the Preview search");
    check(previewSearchText(QStringLiteral("- see [the docs](https://x.y)")) ==
              QStringLiteral("see the docs"),
          "list markers and link targets are dropped");
    check(previewSearchText(QStringLiteral("a\nthe longer line")) ==
              QStringLiteral("the longer line"),
          "the longest line of a multi-line quote is searched");
    check(previewSearchText(QStringLiteral("**")).isEmpty(),
          "nothing worth searching gives an empty string");
}

void testThreads()
{
    const QString markdown = QStringLiteral("alpha beta gamma delta");
    const QJsonArray comments{
        comment(QStringLiteral("t-gamma"), {}, QStringLiteral("gamma"), 11, 1),
        comment(QStringLiteral("t-whole2"), {}, {}, 0, 5),
        comment(QStringLiteral("t-alpha"), {}, QStringLiteral("alpha"), 0, 2),
        comment(QStringLiteral("t-gone"), {}, QStringLiteral("omega"), 3, 0),
        comment(QStringLiteral("t-whole1"), {}, {}, 0, 3),
        comment(QStringLiteral("t-done"), {}, QStringLiteral("beta"), 6, 4, 99),
        comment(QStringLiteral("r-2"), QStringLiteral("t-alpha"), {}, 0, 9),
        comment(QStringLiteral("r-1"), QStringLiteral("t-alpha"), {}, 0, 8),
        comment(QStringLiteral("r-orphan"), QStringLiteral("missing"), {}, 0, 9),
    };
    const QList<CommentThread> threads = commentThreads(comments, markdown);
    QStringList order;
    for (const CommentThread &thread : threads)
        order << thread.id();
    check(order == QStringList({QStringLiteral("t-whole1"),
                                QStringLiteral("t-whole2"),
                                QStringLiteral("t-alpha"),
                                QStringLiteral("t-gamma"),
                                QStringLiteral("t-gone"),
                                QStringLiteral("t-done")}),
          "open threads: whole note first, then document order, detached "
          "last, resolved after all open threads");
    check(threads.at(2).replies.size() == 2 &&
              threads.at(2).replies.at(0).value(QStringLiteral("id")).toString() ==
                  QStringLiteral("r-1"),
          "replies group under their thread, oldest first");
    check(threads.at(4).place == CommentPlace::Detached &&
              !threads.at(4).anchor.found(),
          "a thread whose quote is gone is detached");
    check(threads.at(0).place == CommentPlace::WholeNote,
          "a thread with no quote is about the whole note");
    check(threads.at(5).resolved, "a thread with resolvedAt is resolved");
    check(openThreadCount(threads) == 5, "the open count leaves resolved out");
    const QString before = commentThreadsSignature(threads);
    const QList<CommentThread> edited = commentThreads(
        comments, QStringLiteral("alpha beta gamma delta, more text"));
    check(commentThreadsSignature(edited) == before,
          "typing that keeps every anchor leaves the signature unchanged");
    const QList<CommentThread> removed =
        commentThreads(comments, QStringLiteral("alpha beta delta"));
    check(commentThreadsSignature(removed) != before,
          "deleting a quoted passage changes the signature");
    check(commentsButtonLabel(3) == QStringLiteral("Comments 3") &&
              commentsButtonLabel(0) == QStringLiteral("Comments"),
          "the toolbar button shows the open-thread count");
    check(resolvedFolderLabel(2) == QStringLiteral("Resolved (2)"),
          "resolved threads fold under Resolved (n)");
}

void testPermissions()
{
    check(canResolveThread(true, QStringLiteral("bo"), QStringLiteral("ana")),
          "owners and editors may resolve any thread");
    check(canResolveThread(false, QStringLiteral("Ana"), QStringLiteral("ana")),
          "the thread's author may resolve it, names compared without case");
    check(!canResolveThread(false, QStringLiteral("bo"), QStringLiteral("ana")),
          "a viewer cannot resolve someone else's thread");
    check(!sameAccount(QString(), QString()),
          "an unknown account is nobody's author");
    check(canDeleteComment(QStringLiteral("Ana"), QStringLiteral("ana"), false),
          "you can delete your own comment");
    check(canDeleteComment(QStringLiteral("bo"), QStringLiteral("ana"), true),
          "the note's owner can delete anyone's comment");
    check(!canDeleteComment(QStringLiteral("bo"), QStringLiteral("ana"), false),
          "an editor or viewer cannot delete someone else's comment");
    const QJsonArray thread{comment(QStringLiteral("t"), {}),
                            comment(QStringLiteral("r1"), QStringLiteral("t")),
                            comment(QStringLiteral("r2"), QStringLiteral("t")),
                            comment(QStringLiteral("x"), QStringLiteral("other"))};
    check(replyCount(thread, QStringLiteral("t")) == 2,
          "the replies of a thread are counted");
    check(deleteCommentQuestion(true, 2, true, QStringLiteral("ana")) ==
              QStringLiteral("Delete this comment and its 2 replies?"),
          "deleting a thread names how many replies go with it");
    check(deleteCommentQuestion(true, 1, true, QStringLiteral("ana")) ==
              QStringLiteral("Delete this comment and its reply?"),
          "a single reply is named in the singular");
    check(deleteCommentQuestion(true, 0, false, QStringLiteral("bo")) ==
              QStringLiteral("Delete this comment by @bo?"),
          "an owner deleting someone else's comment is told whose it is");
    check(deleteCommentQuestion(false, 0, true, QStringLiteral("ana")) ==
              QStringLiteral("Delete this reply?"),
          "a reply asks about the reply only");
}

void testMentions()
{
    const QString body = QStringLiteral("hi @ana and @Bo, mail x@ana.com @zed-");
    const QList<MentionSpan> spans =
        mentionSpans(body, {QStringLiteral("ana"), QStringLiteral("bo")});
    check(spans.size() == 2, "only kept mentions are highlighted, not emails");
    check(spans.value(0).start == 3 && spans.value(0).length == 4,
          "a mention span covers the @ and the name");
    check(spans.value(1).name == QStringLiteral("bo"),
          "mentions match without case");
    check(mentionSpans(body, {}).isEmpty(),
          "no kept mentions means nothing is highlighted");
    const QString html = commentBodyHtml(QStringLiteral("<b> @ana\nok"),
                                         {QStringLiteral("ana")},
                                         QStringLiteral("#123456"));
    check(html.contains(QStringLiteral("&lt;b&gt;")) &&
              html.contains(QStringLiteral("color:#123456")) &&
              html.contains(QStringLiteral("@ana</span>")) &&
              html.contains(QStringLiteral("<br>ok")),
          "the body is escaped, mentions coloured and line breaks kept");

    MentionQuery query = mentionQueryAt(QStringLiteral("hello @an"), 9);
    check(query.active() && query.at == 6 && query.prefix == QStringLiteral("an"),
          "typing @an asks for names starting with an");
    check(mentionQueryAt(QStringLiteral("hello @"), 7).prefix.isEmpty() &&
              mentionQueryAt(QStringLiteral("hello @"), 7).active(),
          "a bare @ offers everyone");
    check(!mentionQueryAt(QStringLiteral("mail x@an"), 9).active(),
          "an @ inside a word is not a mention");
    check(!mentionQueryAt(QStringLiteral("@ana done"), 9).active(),
          "a finished word after the mention closes the suggestions");
    const QStringList people{QStringLiteral("ana"), QStringLiteral("bob"),
                             QStringLiteral("dana"), QStringLiteral("me")};
    check(mentionMatches(people, QStringLiteral("an"), QStringLiteral("me")) ==
              QStringList({QStringLiteral("ana"), QStringLiteral("dana")}),
          "names starting with the text come before names containing it");
    check(!mentionMatches(people, QString(), QStringLiteral("ME"))
               .contains(QStringLiteral("me")),
          "you are not offered to yourself");
}

void testErrorsAndTimes()
{
    check(commentError(QStringLiteral("too_many_comments"), {}).contains(
              QStringLiteral("1000")),
          "the comment cap is explained");
    check(commentError(QStringLiteral("something"), QStringLiteral("fallback")) ==
              QStringLiteral("fallback"),
          "unknown errors use the fallback text");
    const qint64 now = 1'800'000'000'000;
    check(commentTimeLabel(now - 5'000, now) == QStringLiteral("just now"),
          "a fresh comment says just now");
    check(commentTimeLabel(now - 5 * 60'000, now) == QStringLiteral("5 min ago"),
          "minutes are counted");
    check(commentTimeLabel(now - 3 * 3'600'000, now) == QStringLiteral("3 h ago"),
          "hours are counted");
    check(commentTimeLabel(0, now).isEmpty(), "no time gives no label");
}

void testPresence()
{
    const QJsonArray raw{
        QJsonObject{{QStringLiteral("clientId"), QStringLiteral("c1")},
                    {QStringLiteral("name"), QStringLiteral("zoe")},
                    {QStringLiteral("state"), QStringLiteral("viewing")},
                    {QStringLiteral("via"), QStringLiteral("live")}},
        QJsonObject{{QStringLiteral("clientId"), QStringLiteral("c2")},
                    {QStringLiteral("name"), QStringLiteral("ana")},
                    {QStringLiteral("state"), QStringLiteral("editing")},
                    {QStringLiteral("via"), QStringLiteral("poll")}},
        QJsonObject{{QStringLiteral("clientId"), QStringLiteral("c3")},
                    {QStringLiteral("name"), QStringLiteral("bo")},
                    {QStringLiteral("state"), QStringLiteral("left")}},
        QJsonObject{{QStringLiteral("name"), QStringLiteral("nobody")}},
        QJsonObject{{QStringLiteral("clientId"), QStringLiteral("c4")},
                    {QStringLiteral("state"), QStringLiteral("odd")}},
    };
    const QList<NotePeer> peers = notePeers(raw);
    check(peers.size() == 3, "left peers and peers without a clientId are dropped");
    check(peers.value(0).name == QStringLiteral("ana") && peers.value(0).editing(),
          "editors come first");
    check(peers.value(1).name == QStringLiteral("Collaborator") &&
              peers.value(1).state == QStringLiteral("viewing"),
          "a nameless peer is a Collaborator and an unknown state is viewing");
    check(notePeerLabel(peers.value(0)) == QStringLiteral("ana · editing") &&
              notePeerLabel(peers.value(2)) == QStringLiteral("zoe · viewing"),
          "each face is labelled name · state");

    check(notePresenceLine(peers) == QStringLiteral("ana is editing…"),
          "an editor is named first");
    const NotePeer viewer{QStringLiteral("x"), QStringLiteral("ana"),
                          QStringLiteral("viewing"), QStringLiteral("poll")};
    check(notePresenceLine({viewer}) == QStringLiteral("Live · ana is viewing"),
          "a single viewer is named");
    NotePeer second = viewer;
    second.clientId = QStringLiteral("y");
    NotePeer third = viewer;
    third.clientId = QStringLiteral("z");
    check(notePresenceLine({viewer, second, third}) ==
              QStringLiteral("Live · 3 others here"),
          "several viewers are counted");
    NotePeer editor = viewer;
    editor.state = QStringLiteral("editing");
    NotePeer editor2 = editor;
    editor2.name = QStringLiteral("bo");
    check(notePresenceLine({editor, editor2}) ==
              QStringLiteral("ana and bo are editing…"),
          "two editors are both named");
    NotePeer editor3 = editor;
    editor3.name = QStringLiteral("cy");
    check(notePresenceLine({editor, editor2, editor3}) ==
              QStringLiteral("ana and 2 others are editing…"),
          "more editors are counted");
    check(notePresenceLine({}).isEmpty(), "nobody here shows no line");

    check(notePresenceOverflow(4).isEmpty() &&
              notePresenceOverflow(6) == QStringLiteral("+2"),
          "four faces, then +N");
    const qint64 now = 1'000'000;
    check(notePresenceState(true, now - 9'000, now) == QStringLiteral("editing"),
          "typing in the last 10 s is editing");
    check(notePresenceState(true, now - 10'000, now) == QStringLiteral("viewing"),
          "10 s without typing is viewing");
    check(notePresenceState(false, now - 1'000, now) == QStringLiteral("viewing"),
          "someone who cannot edit is always viewing");
    check(notePresenceState(true, 0, now) == QStringLiteral("viewing"),
          "never typed is viewing");

    check(noteInitials(QStringLiteral("ana")) == QStringLiteral("A"),
          "a one-word name gives one initial");
    check(noteInitials(QStringLiteral("ana-maria lopez")) == QStringLiteral("AM"),
          "names split on spaces, dashes and underscores");
    check(noteInitials(QStringLiteral("!!")) == QStringLiteral("?"),
          "a name with no letters shows ?");
    check(noteAvatarIndex(QStringLiteral("ana"), 8) ==
              noteAvatarIndex(QStringLiteral("ana"), 8),
          "the face colour is stable for a name");
    check(noteAvatarIndex(QStringLiteral("ab"), 8) == int((97u * 31u + 98u) % 8u),
          "the colour hash is the web page's hash * 31 + code point");
}

}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testAnchorScoring();
    testUtf16Offsets();
    testQuoteCapture();
    testPreviewSearchText();
    testThreads();
    testPermissions();
    testMentions();
    testErrorsAndTimes();
    testPresence();
    if (failures == 0)
        std::puts("note comments: all checks passed");
    return failures == 0 ? 0 : 1;
}
