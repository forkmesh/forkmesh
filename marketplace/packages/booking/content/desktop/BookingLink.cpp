#include "NativePackageExtension.h"

namespace {

const forkmesh::market::NativePackageRegistration bookingRegistration({
    QStringLiteral("builtin:booking"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Booking"),
    QStringLiteral("calendar"),
    QString(),
    QString(),
    {},
    QStringLiteral("/dashboard/booking"),
});

} // namespace
