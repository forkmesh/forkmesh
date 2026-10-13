#include "PromptDraftStore.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <iostream>

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

} // namespace

int main(int argc, char **argv)
{
    // Hermetic HOME so neither QSettings nor AppDataLocation touches the real
    // profile of whoever runs the suite.
    QTemporaryDir home;
    if (!home.isValid()) {
        std::cerr << "FAIL: temporary home directory is available\n";
        return 1;
    }
    qputenv("HOME", QFile::encodeName(home.path()));
    QStandardPaths::setTestModeEnabled(true);

    QCoreApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ForkMeshTest"));
    QCoreApplication::setApplicationName(QStringLiteral("PromptDraftStoreTest"));

    const QString file = PromptDraftStore::path();
    check(!file.isEmpty(), "the draft resolves a path when app data is writable");
    check(PromptDraftStore::load().isEmpty(), "a fresh profile has no draft");

    // Multi-line, non-ASCII, and trailing whitespace all come back exactly as
    // typed: the cursor is restored at the end of what the user left.
    const QString typed = QStringLiteral("fix the — composer\n  keep é this \n");
    check(PromptDraftStore::store(typed), "storing a draft succeeds");
    check(QFileInfo::exists(file), "the draft is written to its own file");
    check(PromptDraftStore::load() == typed, "the draft round-trips byte for byte");

    // Every keystroke replaces the whole draft, including deletions.
    check(PromptDraftStore::store(QStringLiteral("fix")), "a shorter draft stores");
    check(PromptDraftStore::load() == QStringLiteral("fix"),
          "a shorter draft replaces the longer one");

    // The draft never lands in the ini file every setting write rewrites.
    {
        QSettings settings;
        settings.sync();
        check(settings.allKeys().isEmpty(), "the draft stays out of QSettings");
    }

    // Sending (or erasing) the prompt clears it, so the next launch starts empty.
    check(PromptDraftStore::store(QStringLiteral(" \n\t")),
          "a blank draft stores");
    check(!QFileInfo::exists(file), "a blank draft removes the file");
    check(PromptDraftStore::load().isEmpty(), "a blank draft loads as empty");
    check(PromptDraftStore::store(QString()), "clearing an absent draft succeeds");

    if (failures == 0)
        std::cout << "prompt draft store tests passed\n";
    return failures == 0 ? 0 : 1;
}
