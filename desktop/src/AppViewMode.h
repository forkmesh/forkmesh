#pragma once

#include <QLatin1String>
#include <QString>

// The window's layout presets, picked from the "Change view" button in the top
// right of the chrome bar.
//
// ForkMesh's shell is fixed: a navigation rail on the left, the section stack
// in the middle, a status strip along the bottom. That is the right shape for
// browsing repositories and issues, and the wrong one for sitting with a
// running agent — there the session list, its transcript and its terminal are
// the whole job, and the rail plus the status strip are just borrowed pixels.
//
// Rather than add a second window, each mode reshapes the one shell: Focus
// drops the rail and the strip, and Workspace additionally opens Agents and
// adds a shell terminal pane beside the transcript, so the session list, the
// conversation and a terminal in the session's worktree are on screen at once.
//
// Header-only and free of Qt widgets on purpose, so the mode table (names,
// persisted keys, what each mode hides) is exercised by a test that does not
// build a MainWindow.
namespace forkmesh::ui {

enum class AppViewMode {
    Standard,  // the full shell: rail, sections, status strip
    Focus,     // the current section alone, rail and status strip hidden
    Workspace, // Agents with a shell terminal beside the transcript
};

// Every mode, in the order the switcher lists them.
constexpr AppViewMode kAppViewModes[] = {
    AppViewMode::Standard,
    AppViewMode::Focus,
    AppViewMode::Workspace,
};
constexpr int kAppViewModeCount =
    static_cast<int>(sizeof(kAppViewModes) / sizeof(kAppViewModes[0]));

// The value QSettings stores. Spelled out rather than the enum's integer so a
// mode inserted in the middle of the list later cannot silently reinterpret an
// existing user's saved preference as a different view.
inline QString appViewModeKey(AppViewMode mode)
{
    switch (mode) {
    case AppViewMode::Focus:
        return QStringLiteral("focus");
    case AppViewMode::Workspace:
        return QStringLiteral("workspace");
    case AppViewMode::Standard:
        break;
    }
    return QStringLiteral("standard");
}

// An unknown key — a downgrade to a build without the mode, a hand-edited
// config — falls back rather than leaving the window in a half-applied state.
inline AppViewMode appViewModeFromKey(const QString &key,
                                      AppViewMode fallback = AppViewMode::Standard)
{
    const QString needle = key.trimmed().toLower();
    for (AppViewMode mode : kAppViewModes) {
        if (appViewModeKey(mode) == needle)
            return mode;
    }
    return fallback;
}

inline QString appViewModeName(AppViewMode mode)
{
    switch (mode) {
    case AppViewMode::Focus:
        return QStringLiteral("Focus");
    case AppViewMode::Workspace:
        return QStringLiteral("Workspace");
    case AppViewMode::Standard:
        break;
    }
    return QStringLiteral("Standard");
}

inline QString appViewModeDescription(AppViewMode mode)
{
    switch (mode) {
    case AppViewMode::Focus:
        return QStringLiteral(
            "The current page alone - no navigation rail, no status strip.");
    case AppViewMode::Workspace:
        return QStringLiteral(
            "Agents with the session list, its transcript and its terminal "
            "side by side.");
    case AppViewMode::Standard:
        break;
    }
    return QStringLiteral(
        "The full shell: navigation rail, pages and the status strip.");
}

// Octicon shipped in resources/icons.qrc; an unshipped name would draw nothing
// at all (see tests/validate_octicon_names.py).
inline QString appViewModeIcon(AppViewMode mode)
{
    switch (mode) {
    case AppViewMode::Focus:
        return QStringLiteral("screen-full");
    case AppViewMode::Workspace:
        return QStringLiteral("terminal");
    case AppViewMode::Standard:
        break;
    }
    return QStringLiteral("device-desktop");
}

// Ctrl+Shift+L walks the list, so the keyboard reaches every mode without
// opening the menu.
inline AppViewMode nextAppViewMode(AppViewMode mode)
{
    for (int index = 0; index < kAppViewModeCount; ++index) {
        if (kAppViewModes[index] == mode)
            return kAppViewModes[(index + 1) % kAppViewModeCount];
    }
    return AppViewMode::Standard;
}

// Both reduced modes give the page the rail's and the status strip's space.
// The chrome bar itself always stays: it carries the switcher, and a mode the
// user cannot leave is a trap.
inline bool appViewModeHidesShellChrome(AppViewMode mode)
{
    return mode != AppViewMode::Standard;
}

// Only Workspace shows the Agents page's shell terminal pane. It is a terminal
// of its own, not the agent output stack's Claude Code terminal, so a shell the
// user left open is never replaced by an agent launch (or the reverse).
inline bool appViewModeShowsWorkspaceTerminal(AppViewMode mode)
{
    return mode == AppViewMode::Workspace;
}

// Workspace is a view *of* Agents, so switching into it opens that page.
inline bool appViewModeOpensAgents(AppViewMode mode)
{
    return mode == AppViewMode::Workspace;
}

} // namespace forkmesh::ui
