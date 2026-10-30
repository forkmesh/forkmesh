#pragma once

#include <QString>
#include <QStringList>

#include <functional>
#include <vector>

namespace forkmesh {

class CorePackageServices
{
public:
    using Hook = std::function<void()>;
    using InstalledCheck = std::function<bool(const QString &packageId)>;

    void add(const QString &id, const QStringList &packages, Hook start, Hook stop);
    void sync(const InstalledCheck &installed);
    void stopAll();

    bool isRunning(const QString &id) const;
    bool isKnown(const QString &id) const;
    QStringList packagesFor(const QString &id) const;
    QStringList services() const;
    QStringList running() const;

private:
    struct Service {
        QString id;
        QStringList packages;
        Hook start;
        Hook stop;
        bool running = false;
    };

    const Service *find(const QString &id) const;

    std::vector<Service> m_services;
    bool m_syncing = false;
    bool m_resync = false;
    InstalledCheck m_lastCheck;
};

}
