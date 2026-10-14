#include "NativePackageExtension.h"

namespace {

const forkmesh::market::NativePackageRegistration faceChangerRegistration({
    QStringLiteral("builtin:face-changer"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Face Changer"),
    QStringLiteral("person"),
    QString(),
    QString(),
    {},
    QStringLiteral("/dashboard/face-changer"),
});

}
