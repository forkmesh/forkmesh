#include "NativePackageExtension.h"

namespace {

const forkmesh::market::NativePackageRegistration usageRegistration({
    QStringLiteral("builtin:usage"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Usage"),
    QStringLiteral("pulse"),
    QString(),
    QString(),
    {},
    QStringLiteral("/dashboard/usage"),
});

}
