#include "NativePackageExtension.h"

namespace {

const forkmesh::market::NativePackageRegistration workflowsRegistration({
    QStringLiteral("builtin:workflows"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Workflows"),
    QStringLiteral("workflow"),
    QString(),
    QString(),
    {},
    QStringLiteral("/dashboard/workflows"),
});

} // namespace
