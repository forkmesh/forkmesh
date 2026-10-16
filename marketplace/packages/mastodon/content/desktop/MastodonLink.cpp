#include "NativePackageExtension.h"

namespace {

const forkmesh::market::NativePackageRegistration mastodonRegistration({
    QStringLiteral("builtin:mastodon"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Mastodon"),
    QStringLiteral("mastodon"),
    QString(),
    QString(),
    {},
    QStringLiteral("/dashboard/mastodon"),
});

}
