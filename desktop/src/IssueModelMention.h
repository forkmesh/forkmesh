#pragma once

#include <QList>
#include <QString>
#include <QStringList>

// @model mentions inside an issue thread.
//
// Writing "@opus why does this deadlock?" in an issue body or comment addresses
// that model directly: the repository host reads the thread, asks the model,
// and posts the answer back as a signed comment, so the whole exchange stays on
// the issue where every node can read it. Asking for work instead of an opinion
// ("@fable fix this", "@opus open a PR for it") starts a coding agent on the
// issue with that model rather than writing a reply.
//
// The text handling lives in its own QtCore-only module so it can be unit
// tested without a widget stack: forkmesh-tests links Core/Network/Concurrent
// only, while the mention aliases, the ask/work decision, and the file
// citations the answer is built from are exactly the parts worth pinning down
// with tests.
namespace forkmesh::issuemention {

// What the mention asked the model for.
enum class Intent {
    Answer, // reply in the thread
    Work,   // start a coding agent on the issue
};

struct Mention {
    QString alias;    // what was typed after '@', lowercased ("opus")
    QString model;    // resolved model id ("claude-opus-4-8")
    QString provider; // agent provider that runs the model ("claude-code")
    QString request;  // the words addressed to the model
    Intent intent = Intent::Answer;
};

// Resolve one @alias to a model id, or an empty string when the alias names
// something other than a model (a teammate, an org, a stray email fragment).
// Both the short aliases people actually type ("opus", "sonnet", "sol") and
// full model ids ("claude-fable-5", "gpt-5.1") resolve, so a model this build
// has never heard of still works as long as it is spelled out.
QString modelForAlias(const QString &alias);

// The short aliases above, in line-up order, for the composer's @-autocomplete.
// Only the names a mention actually dispatches on, so a bot offered in the
// popup is a bot that answers.
QStringList aliases();

// The agent provider that can run a model id, for startAgentForIssue().
QString providerForModel(const QString &model);

// Every model mention in one issue body or comment, in the order they appear.
// Each mention carries the text addressed to it: everything up to the next
// model mention, or — when the mention trails the message ("...this makes no
// sense @opus") — the message itself with the mentions removed.
QList<Mention> parse(const QString &text);

// Whether a request asks the model to change the code rather than to answer.
// Polite lead-ins are stripped first, so "can you fix this?" reads as the
// request for work it is while "how would you fix this?" stays a question.
bool requestsWork(const QString &request);

// Display author for a comment this node posts on a model's behalf. The name
// is display metadata only (authorName sits outside the signed canonical
// string), but the [bot] suffix is what stops a reply that happens to quote
// another @alias from starting an endless model-to-model thread.
QString botAuthorName(const QString &modelLabel);
bool isBotAuthorName(const QString &authorName);

// A file the thread points at: "RepoContributionSnapshot.cpp:212",
// "docs/qt-client.md:22". A mentioned model is shown these files as they stand
// now, which is how "does line 212 still say that?" gets a real answer.
struct FileCitation {
    QString path; // exactly as the thread spelled it
    int line = 0; // 0 when no line was named
};

// Every file citation in one blob of thread text, in the order they appear and
// deduplicated by path, bounded so one link-heavy comment cannot fill the
// answer. Version numbers ("Qt 6.4.2") are dropped here because they would
// otherwise take those slots; everything else path-shaped is kept, and it is
// trackedPathFor() that separates real files from prose by asking the
// repository.
QList<FileCitation> citedFilePaths(const QString &text);

// The tracked path a citation names, or empty when the repository has no
// unambiguous answer. A thread rarely spells out the full path, so a tail match
// counts: "RepoContributionSnapshot.cpp" finds "desktop/src/..." while a name
// two directories share ("CMakeLists.txt") resolves to nothing rather than to
// the wrong file.
QString trackedPathFor(const QStringList &trackedPaths, const QString &cited);

// A window of a file around the line a thread cited, numbered so the model can
// say "line 212" and mean the line the reporter meant. line 0 takes the head of
// the file instead. Bounded by maxChars, which is what keeps one enormous file
// from crowding out the commit history beside it.
QString numberedFileWindow(const QString &body, int line, int radius,
                           int maxChars);

} // namespace forkmesh::issuemention
