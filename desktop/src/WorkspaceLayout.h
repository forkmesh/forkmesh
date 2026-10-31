#pragma once

#include "DesignTokens.h"
#include "LayoutMode.h"

namespace forkmesh::ui {

inline bool workspaceLayoutClassic()
{
    return layoutControlTower();
}

inline bool workspaceLayoutNew()
{
    return Theme::isNext() && !workspaceLayoutClassic();
}

}
