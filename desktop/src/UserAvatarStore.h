#pragma once

#include <QByteArray>
#include <QString>

// The profile picture used to live in QSettings under "profile/avatarPng".
// QSettings rewrites the *entire* ini file on every setValue(), so that one
// ~104 KB blob taxed every unrelated setting write in the app: measured against
// a real 268 KB config, a write costs 6.10 ms with the avatar in the file and
// 4.33 ms without it. Those writes happen on the GUI thread, which is why the
// stall watchdog kept catching short freezes spread across unrelated code
// (ActionStore::approve, quick-add history, chat settings). The picture lives in
// its own file now so QSettings holds only settings.
namespace UserAvatarStore {

// Absolute path of the PNG on disk. Empty when there is no writable app data
// directory, in which case the store transparently falls back to QSettings.
QString path();

// The stored picture, or empty when none is set. Migrates a legacy QSettings
// blob out of the ini file on first call, so no caller has to know about it.
QByteArray load();

// Replaces the stored picture. Returns false only on an I/O error.
bool store(const QByteArray &png);

} // namespace UserAvatarStore
