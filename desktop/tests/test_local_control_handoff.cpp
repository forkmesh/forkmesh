#include "LocalControlHandoff.h"

#include <iostream>

using forkmesh::localcontrol::handoffBranchValid;
using forkmesh::localcontrol::handoffRefNameValid;
using forkmesh::localcontrol::takeoverRepoIndex;
using forkmesh::localcontrol::takeoverSessionValid;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

int main()
{
    check(handoffBranchValid(QStringLiteral("agent/1a2b3c-fix-login-redirect")),
          "a phone agent branch is accepted");
    check(!handoffBranchValid(QStringLiteral("main")),
          "a branch outside agent/ is refused");
    check(!handoffBranchValid(QStringLiteral("agent/")),
          "agent/ alone is refused");
    check(!handoffBranchValid(QStringLiteral("agent/../main")),
          "a branch with .. is refused");
    check(!handoffBranchValid(QStringLiteral("agent/x y")),
          "a branch with a space is refused");
    check(!handoffBranchValid(QStringLiteral("agent/x.lock")),
          "a branch ending in .lock is refused");
    check(!handoffBranchValid(QStringLiteral("agent/x/")),
          "a branch ending in / is refused");
    check(!handoffBranchValid(QStringLiteral("agent/.hidden")),
          "a component starting with a dot is refused");
    check(!handoffBranchValid(QStringLiteral("agent/a:b")),
          "a refspec colon is refused");
    check(handoffRefNameValid(QStringLiteral("main")), "a base of main is accepted");
    check(handoffRefNameValid(QStringLiteral("release/1.2")),
          "a nested base is accepted");
    check(!handoffRefNameValid(QStringLiteral("-main")),
          "a base that reads as an option is refused");
    check(!handoffRefNameValid(QStringLiteral("main^{commit}")),
          "a base with revision syntax is refused");
    check(!handoffRefNameValid(QString(201, QLatin1Char('a'))),
          "a base over 200 characters is refused");
    check(takeoverSessionValid(QStringLiteral("3f9c1e2a-7b44-4c1d-9a0e-1f2b3c4d5e6f")),
          "a relay session id is accepted");
    check(!takeoverSessionValid(QString()), "an empty session id is refused");
    check(!takeoverSessionValid(QStringLiteral("../x")),
          "a session id with a path is refused");
    check(!takeoverSessionValid(QString(129, QLatin1Char('a'))),
          "a session id over 128 characters is refused");
    const QList<QPair<QString, QString>> checkouts{
        {QStringLiteral("alice"), QStringLiteral("site")},
        {},
        {QStringLiteral("forkmesh"), QStringLiteral("forkmesh")},
        {QStringLiteral("alice"), QStringLiteral("notes")},
        {QStringLiteral("bob"), QStringLiteral("notes")}};
    check(takeoverRepoIndex(checkouts, QStringLiteral("Alice"), QStringLiteral("site")) == 0,
          "the exact owner and name is taken");
    check(takeoverRepoIndex(checkouts, QStringLiteral("fmnext-n1"),
                            QStringLiteral("forkmesh")) == 2,
          "a run addressed by its core finds the only checkout of that name");
    check(takeoverRepoIndex(checkouts, QStringLiteral("carol"), QStringLiteral("notes")) == -1,
          "two owners of the same name are not guessed between");
    check(takeoverRepoIndex(checkouts, QStringLiteral("bob"), QStringLiteral("notes")) == 4,
          "an exact owner settles two of the same name");
    check(takeoverRepoIndex(checkouts, QStringLiteral("alice"), QStringLiteral("wiki")) == -1,
          "a repository without a checkout is refused");
    check(takeoverRepoIndex(checkouts, QStringLiteral("alice"), QString()) == -1,
          "an empty name matches nothing, not the blank entry");
    if (failures == 0)
        std::cout << "local control handoff: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
