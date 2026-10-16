#include "NativePackageExtension.h"

namespace {

const forkmesh::market::NativePackageRegistration videoMixDjStudioRegistration({
    QStringLiteral("builtin:video-mix-dj-studio"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Video Mix DJ Studio"),
    QStringLiteral("play"),
    QString(),
    QString(),
    {},
    QStringLiteral("/dashboard/video-mix-dj-studio"),
});

}
