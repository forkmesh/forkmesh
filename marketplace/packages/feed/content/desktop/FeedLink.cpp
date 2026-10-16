#include "NativePackageExtension.h"

namespace {

const forkmesh::market::NativePackageRegistration feedRegistration({
    QStringLiteral("builtin:feed"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Feed"),
    QStringLiteral("broadcast"),
    QString(),
    QString(),
    {},
    QStringLiteral("/dashboard/feed"),
});

}
