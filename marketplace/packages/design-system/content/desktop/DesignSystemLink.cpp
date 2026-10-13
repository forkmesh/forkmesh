#include "NativePackageExtension.h"

namespace {

const forkmesh::market::NativePackageRegistration designSystemRegistration({
    QStringLiteral("builtin:design-system"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Design"),
    QStringLiteral("palette"),
    QString(),
    QString(),
    {},
    QStringLiteral("/design"),
});

} // namespace
