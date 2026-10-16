#include "NativePackageExtension.h"

namespace {

const forkmesh::market::NativePackageRegistration feedRegistration({
    QStringLiteral("builtin:feed"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Feed"),
    QStringLiteral("rss"),
    QString(),
    QString(),
    {},
    QStringLiteral("/dashboard/feed"),
});

}
