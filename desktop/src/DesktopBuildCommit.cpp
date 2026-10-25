#include "DesktopBuildCommit.h"

#if __has_include("ForkMeshBuildCommit.h")
#include "ForkMeshBuildCommit.h"
#endif
#ifndef FORKMESH_BUILD_COMMIT
#define FORKMESH_BUILD_COMMIT "unknown"
#endif

QString forkmesh::desktopBuildCommit()
{
    return QStringLiteral(FORKMESH_BUILD_COMMIT).trimmed().toLower();
}
