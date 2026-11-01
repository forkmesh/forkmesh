#include "NativePackageExtension.h"

namespace {

const forkmesh::market::NativePackageRegistration spreadsheetsRegistration({
    QStringLiteral("builtin:spreadsheets"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Spreadsheets"),
    QStringLiteral("table"),
    QString(),
    QString(),
    {},
    QStringLiteral("/dashboard/spreadsheets"),
});

} // namespace
