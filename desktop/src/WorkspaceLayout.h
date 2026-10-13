#pragma once

#include "DesignTokens.h"

#include <QLatin1String>
#include <QSettings>
#include <QString>

namespace forkmesh::ui {

inline constexpr char kWorkspaceLayoutSetting[] = "agents/workspaceLayout";

inline int &workspaceLayoutState()
{
    static int state = -1;
    return state;
}

inline bool workspaceLayoutClassic()
{
    int &state = workspaceLayoutState();
    if (state < 0)
        state = QSettings()
                        .value(QLatin1String(kWorkspaceLayoutSetting))
                        .toString() == QLatin1String("classic")
                    ? 1
                    : 0;
    return state == 1;
}

inline void setWorkspaceLayoutClassic(bool classic)
{
    QSettings().setValue(QLatin1String(kWorkspaceLayoutSetting),
                         classic ? QStringLiteral("classic")
                                 : QStringLiteral("new"));
    workspaceLayoutState() = classic ? 1 : 0;
}

inline bool workspaceLayoutNew()
{
    return Theme::isNext() && !workspaceLayoutClassic();
}

}
