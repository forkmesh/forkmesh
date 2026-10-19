#include "NativePackageExtension.h"

namespace {

const forkmesh::market::NativePackageRegistration feedRegistration({
    QStringLiteral("builtin:feed"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Network feed"),
    QStringLiteral("broadcast"),
    QString(),
    QString(),
    {},
    QStringLiteral("/dashboard/feed"),
});

}
