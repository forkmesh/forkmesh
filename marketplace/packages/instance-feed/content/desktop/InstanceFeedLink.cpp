#include "NativePackageExtension.h"

namespace {

const forkmesh::market::NativePackageRegistration instanceFeedRegistration({
    QStringLiteral("builtin:instance-feed"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Instance feed"),
    QStringLiteral("comment"),
    QString(),
    QString(),
    {},
    QStringLiteral("/dashboard/instance-feed"),
});

}
