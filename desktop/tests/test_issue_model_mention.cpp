#include "IssueModelMention.h"

#include <QCoreApplication>

#include <iostream>

using namespace forkmesh::issuemention;

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

const Mention *first(const QList<Mention> &mentions)
{
    return mentions.isEmpty() ? nullptr : &mentions.first();
}

bool asks(const QString &text)
{
    const QList<Mention> mentions = parse(text);
    return mentions.size() == 1 && mentions.first().intent == Intent::Answer;
}

bool works(const QString &text)
{
    const QList<Mention> mentions = parse(text);
    return mentions.size() == 1 && mentions.first().intent == Intent::Work;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);

    check(modelForAlias(QStringLiteral("opus")) == QLatin1String("claude-opus-5") &&
              modelForAlias(QStringLiteral("fable")) ==
                  QLatin1String("claude-fable-5") &&
              modelForAlias(QStringLiteral("Haiku")) ==
                  QLatin1String("claude-haiku-4-5") &&
              modelForAlias(QStringLiteral("sol")) == QLatin1String("gpt-5.6-sol"),
          "the short aliases people type resolve to model ids");
    check(modelForAlias(QStringLiteral("claude-fable-5")) ==
                  QLatin1String("claude-fable-5") &&
              modelForAlias(QStringLiteral("gpt-5.1")) == QLatin1String("gpt-5.1"),
          "a model id spelled out in full addresses that model, so a model "
          "newer than this build still works");
    check(modelForAlias(QStringLiteral("alice")).isEmpty() &&
              modelForAlias(QStringLiteral("forkbot")).isEmpty() &&
              modelForAlias(QString()).isEmpty() &&
              modelForAlias(QStringLiteral("claudia")).isEmpty() &&
              modelForAlias(QStringLiteral("gptr")).isEmpty(),
          "a teammate handle is not read as a model");
    check(providerForModel(QStringLiteral("claude-opus-5")) ==
                  QLatin1String("claude-code") &&
              providerForModel(QStringLiteral("gpt-5.5")) == QLatin1String("codex"),
          "each model names the agent provider that can run it");

    const QStringList offered = aliases();
    bool everyOfferedAliasDispatches = !offered.isEmpty();
    for (const QString &alias : offered)
        if (modelForAlias(alias).isEmpty())
            everyOfferedAliasDispatches = false;
    check(everyOfferedAliasDispatches &&
              offered.contains(QStringLiteral("opus")) &&
              offered.contains(QStringLiteral("fable")) &&
              offered.contains(QStringLiteral("sol")) &&
              !offered.contains(QStringLiteral("alice")),
          "the aliases offered in the composer's autocomplete are exactly the "
          "ones a mention dispatches on");

    const QList<Mention> question =
        parse(QStringLiteral("@opus why does this deadlock on startup?"));
    check(question.size() == 1 && question.first().alias == QLatin1String("opus") &&
              question.first().model == QLatin1String("claude-opus-5") &&
              question.first().request ==
                  QLatin1String("why does this deadlock on startup?") &&
              question.first().intent == Intent::Answer,
          "a question addressed to a model carries the words meant for it");

    const QList<Mention> trailing = parse(
        QStringLiteral("The retry loop never terminates here, any idea @opus"));
    check(trailing.size() == 1 &&
              trailing.first().request ==
                  QLatin1String("The retry loop never terminates here, any idea"),
          "a mention that trails the message asks about the message");

    const QList<Mention> both = parse(QStringLiteral(
        "@opus what is the root cause? @fable fix it once opus answers."));
    check(both.size() == 2 && both.at(0).alias == QLatin1String("opus") &&
              both.at(0).request == QLatin1String("what is the root cause?") &&
              both.at(0).intent == Intent::Answer &&
              both.at(1).alias == QLatin1String("fable") &&
              both.at(1).intent == Intent::Work,
          "two models in one comment each get their own request and intent");

    check(parse(QStringLiteral("mail me@opus.dev about it")).isEmpty() &&
              parse(QStringLiteral("no mentions here")).isEmpty(),
          "an address is not a mention");
    check(parse(QStringLiteral("ask @opus.")).size() == 1 &&
              parse(QStringLiteral("ask @opus.")).first().alias ==
                  QLatin1String("opus"),
          "a mention that ends a sentence keeps its alias");

    const Mention *punctuated =
        first(parse(QStringLiteral("@opus: is the mutex held across the await?")));
    check(punctuated &&
              punctuated->request ==
                  QLatin1String("is the mutex held across the await?"),
          "the punctuation separating a mention from its request is dropped");

    check(works(QStringLiteral("@fable fix this")) &&
              works(QStringLiteral("@opus please fix the null deref in Foo")) &&
              works(QStringLiteral("@opus can you fix this?")) &&
              works(QStringLiteral("@fable implement the retry backoff")) &&
              works(QStringLiteral("@opus start an agent on this")) &&
              works(QStringLiteral("@opus open a PR for it")) &&
              works(QStringLiteral("@fable work on this")) &&
              works(QStringLiteral("@opus do it")),
          "asking a model for the change starts an agent");

    check(asks(QStringLiteral("@opus how would you fix this?")) &&
              asks(QStringLiteral("@opus what is the fix here?")) &&
              asks(QStringLiteral("@opus does this need a fix?")) &&
              asks(QStringLiteral("@opus explain why the fix regressed")) &&
              asks(QStringLiteral("@opus thoughts on the patch above?")) &&
              asks(QStringLiteral("@fable summarize this thread")) &&
              asks(QStringLiteral("@opus what does the stack trace say?")),
          "asking a model about the change is still a question");

    check(!requestsWork(QString()) && !requestsWork(QStringLiteral("   ")),
          "an empty request never starts an agent");

    check(botAuthorName(QStringLiteral("Opus 5")) ==
                  QLatin1String("Opus 5 [bot]") &&
              isBotAuthorName(QStringLiteral("Opus 5 [bot]")) &&
              !isBotAuthorName(QStringLiteral("alice")),
          "a model reply is authored under a name the scan can recognise");
    check(parse(QStringLiteral("@opus said to ask @fable about the lock"))
                  .size() == 2,
          "a reply quoting another alias is only skipped by its bot author "
          "name, so that check is what stops a model-to-model loop");

    const QList<FileCitation> cited = citedFilePaths(QStringLiteral(
        "`RepoContributionSnapshot.cpp:212` fails on Qt 6.4.2, and "
        "`docs/qt-client.md:22` still says 6.4+. Same file, "
        "RepoContributionSnapshot.cpp, no line this time."));
    check(cited.size() == 2 &&
              cited.at(0).path ==
                  QLatin1String("RepoContributionSnapshot.cpp") &&
              cited.at(0).line == 212 &&
              cited.at(1).path == QLatin1String("docs/qt-client.md") &&
              cited.at(1).line == 22,
          "a cited file carries the line the thread named, a path is reported "
          "once however often the thread repeats it, and a version number is "
          "not a file");

    const QStringList tracked = {QStringLiteral("desktop/CMakeLists.txt"),
                                 QStringLiteral("world/CMakeLists.txt"),
                                 QStringLiteral("www/docs/qt-client.md"),
                                 QStringLiteral("desktop/src/MainWindow.cpp")};
    check(trackedPathFor(tracked, QStringLiteral("docs/qt-client.md")) ==
                  QLatin1String("www/docs/qt-client.md") &&
              trackedPathFor(tracked, QStringLiteral("MainWindow.cpp")) ==
                  QLatin1String("desktop/src/MainWindow.cpp") &&
              trackedPathFor(tracked, QStringLiteral("desktop/CMakeLists.txt")) ==
                  QLatin1String("desktop/CMakeLists.txt"),
          "a citation resolves to the tracked file it names, by tail or in "
          "full");
    check(trackedPathFor(tracked, QStringLiteral("CMakeLists.txt")).isEmpty() &&
              trackedPathFor(tracked, QStringLiteral("Qt 6.4.2")).isEmpty(),
          "a name two directories share, or no file at all, resolves to "
          "nothing rather than to the wrong file");

    const QString body = QStringLiteral("a\nb\nc\nd\ne");
    const QString window = numberedFileWindow(body, 3, 1, 4000);
    check(window == QLatin1String("    2| b\n    3| c\n    4| d\n"),
          "a file window is the cited line with its neighbours, numbered as "
          "the file numbers them");
    check(numberedFileWindow(body, 0, 2, 4000) ==
                  QLatin1String("    1| a\n    2| b\n    3| c\n    4| d\n") &&
              numberedFileWindow(body, 1, 5, 4000).startsWith(
                  QLatin1String("    1| a\n")) &&
              numberedFileWindow(body, 3, 1, 1).endsWith(
                  QLatin1String("[file window truncated]\n")),
          "with no line named the window takes the head of the file, it never "
          "runs off either end, and it stops at its budget");

    if (failures == 0)
        std::cout << "issue model mention tests passed\n";
    return failures == 0 ? 0 : 1;
}
