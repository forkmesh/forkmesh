#include "NoteRequestProof.h"

#include <QCoreApplication>
#include <QFile>

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

void testListIsBoundToMethodAndPath()
{
    check(noteRequestResource(QByteArrayLiteral("GET"),
                              QStringLiteral("/api/notes"), {}) ==
              QStringLiteral("GET\n/api/notes"),
          "a list request signs its method and path only");
    check(noteRequestResource(QByteArrayLiteral("GET"),
                              QStringLiteral("/api/notes/") +
                                  QString(32, QLatin1Char('b')),
                              QByteArrayLiteral("ignored")) ==
              QStringLiteral("GET\n/api/notes/") + QString(32, QLatin1Char('b')),
          "a read request never signs a body");
}

void testMutationsBindTheExactBody()
{
    check(noteRequestResource(QByteArrayLiteral("POST"),
                              QStringLiteral("/api/notes"),
                              QByteArrayLiteral("{\"title\":\"a\"}")) ==
              QStringLiteral("POST\n/api/notes\n"
                             "c66a6f9e3a6339f6c80c5fb40a9c15c1"
                             "1878e8aae519469861478ff8643b0f4f"),
          "a create request signs the sha256 of its exact JSON body");
    check(noteRequestResource(QByteArrayLiteral("DELETE"),
                              QStringLiteral("/api/notes/x"), {}) ==
              QStringLiteral("DELETE\n/api/notes/x\n"
                             "e3b0c44298fc1c149afbf4c8996fb924"
                             "27ae41e4649b934ca495991b7852b855"),
          "a bodyless mutation signs the digest of the empty body");
}

void testProofMatchesTheServer()
{
    QFile entry(QStringLiteral(FORKMESH_APP_ENTRY));
    check(entry.open(QIODevice::ReadOnly), "app/src/entry.py is readable");
    const QByteArray source = entry.readAll();
    check(source.contains(
              QByteArrayLiteral("NOTES_REQUEST_PROOF = \"") +
              kNotesRequestProof.toLatin1() + QByteArrayLiteral("\"")),
          "the desktop signs with the proof name the notes API verifies");
}

}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testListIsBoundToMethodAndPath();
    testMutationsBindTheExactBody();
    testProofMatchesTheServer();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("note request proof tests passed\n");
    return 0;
}
