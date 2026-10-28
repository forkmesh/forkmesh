#include "NativePackageExtension.h"

namespace {

const forkmesh::market::NativePackageRegistration invoicesRegistration({
    QStringLiteral("builtin:invoices"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Invoices"),
    QStringLiteral("credit-card"),
    QString(),
    QString(),
    {},
    QStringLiteral("/dashboard/invoices"),
});

} // namespace
