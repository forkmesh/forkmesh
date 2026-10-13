#pragma once

#include <QString>

// The footer prompt composer's unsent text, kept on disk while it is typed so a
// restart or crash mid-prompt puts it back in the box on the next launch. It is
// written on nearly every keystroke, so it lives in its own small file: a
// QSettings write rewrites the entire ini file on the GUI thread.
namespace PromptDraftStore {

// Absolute path of the draft on disk. Empty when there is no writable app data
// directory, in which case nothing is stored.
QString path();

// The saved draft, or empty when there is none.
QString load();

// Replaces the saved draft; blank text removes it. Returns false only on an
// I/O error.
bool store(const QString &text);

} // namespace PromptDraftStore
