#pragma once

#include <QLatin1String>
#include <QSettings>
#include <QString>

namespace forkmesh::ui {

inline constexpr char kGitLayoutSetting[] = "git/layout";

inline int &gitLayoutState()
{
    static int state = -1;
    return state;
}

inline bool gitLayoutCompact()
{
    int &state = gitLayoutState();
    if (state < 0)
        state = QSettings().value(QLatin1String(kGitLayoutSetting)).toString() ==
                        QLatin1String("compact")
                    ? 1
                    : 0;
    return state == 1;
}

inline void setGitLayoutCompact(bool compact)
{
    QSettings().setValue(QLatin1String(kGitLayoutSetting),
                         compact ? QStringLiteral("compact") : QStringLiteral("new"));
    gitLayoutState() = compact ? 1 : 0;
}

}
