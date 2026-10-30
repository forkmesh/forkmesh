#include "CorePackageServices.h"

#include <algorithm>

namespace forkmesh {

void CorePackageServices::add(const QString &id, const QStringList &packages, Hook start,
                              Hook stop)
{
    if (id.isEmpty() || isKnown(id))
        return;
    m_services.push_back({id, packages, std::move(start), std::move(stop), false});
}

void CorePackageServices::sync(const InstalledCheck &installed)
{
    if (!installed)
        return;
    m_lastCheck = installed;
    if (m_syncing) {
        m_resync = true;
        return;
    }
    m_syncing = true;
    do {
        m_resync = false;
        const InstalledCheck check = m_lastCheck;
        for (std::size_t index = 0; index < m_services.size(); ++index) {
            const bool wanted =
                std::any_of(m_services[index].packages.cbegin(),
                            m_services[index].packages.cend(), check);
            if (wanted == m_services[index].running)
                continue;
            m_services[index].running = wanted;
            const Hook hook = wanted ? m_services[index].start : m_services[index].stop;
            if (hook)
                hook();
        }
    } while (m_resync);
    m_syncing = false;
}

void CorePackageServices::stopAll()
{
    for (auto it = m_services.rbegin(); it != m_services.rend(); ++it) {
        if (!it->running)
            continue;
        it->running = false;
        if (it->stop)
            it->stop();
    }
}

bool CorePackageServices::isRunning(const QString &id) const
{
    const Service *service = find(id);
    return service && service->running;
}

bool CorePackageServices::isKnown(const QString &id) const
{
    return find(id) != nullptr;
}

QStringList CorePackageServices::packagesFor(const QString &id) const
{
    const Service *service = find(id);
    return service ? service->packages : QStringList();
}

QStringList CorePackageServices::services() const
{
    QStringList ids;
    for (const Service &service : m_services)
        ids.append(service.id);
    return ids;
}

QStringList CorePackageServices::running() const
{
    QStringList ids;
    for (const Service &service : m_services) {
        if (service.running)
            ids.append(service.id);
    }
    return ids;
}

const CorePackageServices::Service *CorePackageServices::find(const QString &id) const
{
    for (const Service &service : m_services) {
        if (service.id == id)
            return &service;
    }
    return nullptr;
}

}
