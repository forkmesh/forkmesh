#include "SourceDir.h"

namespace forkmesh {

const char *sourceDirUtf8()
{
    return FORKMESH_SOURCE_DIR;
}

QString sourceDir()
{
    return QString::fromUtf8(FORKMESH_SOURCE_DIR);
}

} // namespace forkmesh
