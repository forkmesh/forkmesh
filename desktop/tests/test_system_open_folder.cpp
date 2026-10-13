#include "SystemOpenFolder.h"

#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>
#include <QUrl>

#include <cstdio>

using namespace forkmesh::systemopen;

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir scratch;
    if (!scratch.isValid()) {
        std::fprintf(stderr, "FAIL: no temporary dir\n");
        return 1;
    }
    const QString root = QDir::cleanPath(scratch.path());
    QDir base(root);
    base.mkpath(QStringLiteral("work/alpha/.git"));
    base.mkpath(QStringLiteral("work/alpha/src/deep"));
    base.mkpath(QStringLiteral("work/beta/.git"));
    base.mkpath(QStringLiteral("work/plain"));
    base.mkpath(QStringLiteral("bare.git/objects"));
    QFile head(root + QStringLiteral("/bare.git/HEAD"));
    head.open(QIODevice::WriteOnly);
    head.write("ref: refs/heads/main\n");
    head.close();
    QFile file(root + QStringLiteral("/work/notes.txt"));
    file.open(QIODevice::WriteOnly);
    file.close();

    const QString work = root + QStringLiteral("/work");
    const QString alpha = work + QStringLiteral("/alpha");
    const QString beta = work + QStringLiteral("/beta");
    const QString deep = alpha + QStringLiteral("/src/deep");

    check(folderFromArgument(alpha) == alpha, "an absolute folder is accepted");
    check(folderFromArgument(alpha + QStringLiteral("/src/../")) == alpha,
          "the folder is returned clean");
    check(folderFromArgument(QUrl::fromLocalFile(alpha).toString()) == alpha,
          "a file:// URL to a folder is accepted");
    check(folderFromArgument(QStringLiteral("work/alpha")).isEmpty(),
          "a relative path is refused");
    check(folderFromArgument(work + QStringLiteral("/notes.txt")).isEmpty(),
          "a regular file is refused");
    check(folderFromArgument(work + QStringLiteral("/missing")).isEmpty(),
          "a missing folder is refused");
    check(folderFromArgument(QStringLiteral("forkmesh://fork/a/b")).isEmpty(),
          "a forkmesh:// link is not a folder");
    check(folderFromArgument(QStringLiteral("https://example.com/")).isEmpty(),
          "a web URL is not a folder");
    check(folderFromArgument(QString()).isEmpty(), "an empty argument is refused");

    check(folderFromLaunchArguments({root, QStringLiteral("--project"), alpha})
              .isEmpty(),
          "argv[0] and the --project value are not folders to open");
    check(folderFromLaunchArguments({QStringLiteral("forkmesh"),
                                     QStringLiteral("--project"),
                                     QStringLiteral("dev"), beta}) == beta,
          "a folder after --project <name> is found");
    check(folderFromLaunchArguments({QStringLiteral("forkmesh")}).isEmpty(),
          "a plain launch has no folder");

    check(isGitCheckout(alpha), "a working tree with .git is a checkout");
    check(isGitCheckout(root + QStringLiteral("/bare.git")),
          "a bare repository is a checkout");
    check(!isGitCheckout(work + QStringLiteral("/plain")),
          "a plain folder is not a checkout");

    check(gitCheckoutsAt(alpha) == QStringList{alpha},
          "a checkout resolves to itself");
    check(gitCheckoutsAt(work) == (QStringList{alpha, beta}),
          "a parent folder resolves to its child checkouts");
    check(gitCheckoutsAt(deep).isEmpty(), "a subfolder has no checkouts of its own");

    check(enclosingGitCheckout(deep) == alpha,
          "a subfolder resolves to the checkout around it");
    check(!enclosingGitCheckout(work + QStringLiteral("/plain")).startsWith(root),
          "a folder beside checkouts is not inside any of them");

    const QStringList known{QString(), beta, alpha, alpha + QStringLiteral("/src")};
    check(repositoryContaining(alpha, known) == 2,
          "the repository's own folder matches it");
    check(repositoryContaining(deep, known) == 3,
          "the innermost repository around a folder wins");
    check(repositoryContaining(work, known) == -1,
          "a parent of repositories matches none of them");
    check(repositoryContaining(work + QStringLiteral("/alphabet"),
                               {alpha}) == -1,
          "a sibling sharing a name prefix does not match");

    if (failures == 0)
        std::printf("system open folder tests passed\n");
    return failures == 0 ? 0 : 1;
}
