#include "DesktopPackageExtension.h"

#include <QMap>

#include <utility>

namespace forkmesh::market {
namespace {

QMap<QString, DesktopPackageExtensionFactory> &factories()
{
    static QMap<QString, DesktopPackageExtensionFactory> registry;
    return registry;
}

std::function<bool(const QString &)> &modulesProvide()
{
    static std::function<bool(const QString &)> hook;
    return hook;
}

std::function<bool(const QString &)> &modulesLoad()
{
    static std::function<bool(const QString &)> hook;
    return hook;
}

} // namespace

DesktopPackageExtensionRegistration::DesktopPackageExtensionRegistration(
    const QString &entrypoint, DesktopPackageExtensionFactory factory)
{
    if (entrypoint.isEmpty() || !factory || factories().contains(entrypoint))
        return;
    factories().insert(entrypoint, std::move(factory));
    m_accepted = true;
}

std::unique_ptr<DesktopPackageExtension>
createDesktopPackageExtension(const QString &entrypoint)
{
    if (!factories().contains(entrypoint) && modulesLoad())
        modulesLoad()(entrypoint);
    const auto found = factories().constFind(entrypoint);
    if (found == factories().cend())
        return {};
    return std::unique_ptr<DesktopPackageExtension>(found.value()());
}

bool hasDesktopPackageExtension(const QString &entrypoint)
{
    if (entrypoint.isEmpty())
        return false;
    return factories().contains(entrypoint) ||
           (modulesProvide() && modulesProvide()(entrypoint));
}

void setDesktopPackageExtensionModules(std::function<bool(const QString &)> provides,
                                       std::function<bool(const QString &)> load)
{
    modulesProvide() = std::move(provides);
    modulesLoad() = std::move(load);
}

} // namespace forkmesh::market
