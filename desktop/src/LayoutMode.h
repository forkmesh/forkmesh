#pragma once

#include <QLatin1String>
#include <QSettings>
#include <QString>

namespace forkmesh::ui {

enum class LayoutMode {
    Focus,
    ControlTower,
};

inline constexpr LayoutMode kLayoutModes[] = {
    LayoutMode::Focus,
    LayoutMode::ControlTower,
};

inline constexpr char kLayoutModeSetting[] = "app/layoutMode";
inline constexpr char kLegacyWorkspaceLayoutSetting[] = "agents/workspaceLayout";
inline constexpr char kLegacyGitLayoutSetting[] = "git/layout";

inline QString layoutModeKey(LayoutMode mode)
{
    return mode == LayoutMode::ControlTower ? QStringLiteral("controlTower")
                                            : QStringLiteral("focus");
}

inline QString layoutModeName(LayoutMode mode)
{
    return mode == LayoutMode::ControlTower ? QStringLiteral("Control Tower")
                                            : QStringLiteral("Focus");
}

inline QString layoutModeDescription(LayoutMode mode)
{
    return mode == LayoutMode::ControlTower
               ? QStringLiteral("Compact and control rich, like a cockpit: the "
                                "Agents action bar, list views and detail "
                                "buttons, and the Git page with changed files "
                                "over history and the branch picker, Fetch and "
                                "Pull in the history header.")
               : QStringLiteral("One thing at a time: session cards, one "
                                "conversation header and a side panel for the "
                                "diff, the terminal and the browser, and the Git "
                                "page with Changes and History tabs beside a "
                                "column of branches, remotes, worktrees and "
                                "tags.");
}

inline LayoutMode layoutModeFromSettings(const QSettings &settings)
{
    const QString key =
        settings.value(QLatin1String(kLayoutModeSetting)).toString().trimmed();
    for (const LayoutMode mode : kLayoutModes) {
        if (key.compare(layoutModeKey(mode), Qt::CaseInsensitive) == 0)
            return mode;
    }
    const bool classicAgents =
        settings.value(QLatin1String(kLegacyWorkspaceLayoutSetting)).toString() ==
        QLatin1String("classic");
    const bool compactGit =
        settings.value(QLatin1String(kLegacyGitLayoutSetting)).toString() ==
        QLatin1String("compact");
    return classicAgents || compactGit ? LayoutMode::ControlTower : LayoutMode::Focus;
}

inline int &layoutModeState()
{
    static int state = -1;
    return state;
}

inline LayoutMode layoutMode()
{
    int &state = layoutModeState();
    if (state < 0)
        state = static_cast<int>(layoutModeFromSettings(QSettings()));
    return static_cast<LayoutMode>(state);
}

inline void setLayoutMode(LayoutMode mode)
{
    QSettings settings;
    settings.setValue(QLatin1String(kLayoutModeSetting), layoutModeKey(mode));
    settings.remove(QLatin1String(kLegacyWorkspaceLayoutSetting));
    settings.remove(QLatin1String(kLegacyGitLayoutSetting));
    layoutModeState() = static_cast<int>(mode);
}

inline bool layoutControlTower()
{
    return layoutMode() == LayoutMode::ControlTower;
}

}
