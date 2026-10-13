#ifndef FORKMESH_DESKTOP_PACKAGE_EXTENSION_H
#define FORKMESH_DESKTOP_PACKAGE_EXTENSION_H

#include <QIcon>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QtGlobal>

#include <functional>
#include <memory>

class QNetworkAccessManager;
class QWidget;

namespace forkmesh::market {

struct DesktopLogCategory {
    QString badge;
    QString accent;
    QString icon;
};

struct DesktopPackageContext {
    QWidget *window = nullptr;
    QNetworkAccessManager *network = nullptr;
    std::function<void(const QString &, const QString &)> logActivity;
    std::function<void(const QString &, bool)> notify;
    std::function<bool()> darkTheme;
    std::function<QIcon(const QString &, int)> icon;
};

struct DesktopBranchTarget {
    QString branch;
    QString directory;
};

struct DesktopBranchAction {
    QString label;
    QString icon;
    QString toolTip;
    std::function<void(const DesktopBranchTarget &)> trigger;
};

// The desktop is only a receiver for package contributions. A native package
// owns its runtime, settings, widgets and resources and exposes them through
// this deliberately service-neutral contract.
class DesktopPackageExtension
{
public:
    virtual ~DesktopPackageExtension() = default;

    virtual QString packageId() const = 0;
    virtual QList<DesktopLogCategory> logCategories() const { return {}; }
    virtual QString logPageTitle() const { return {}; }
    virtual QString logPageToolTip() const { return {}; }

    virtual void activate(const DesktopPackageContext &context) = 0;
    virtual void deactivate() = 0;
    virtual QWidget *createLogPage(QWidget *parent) { Q_UNUSED(parent); return nullptr; }
    virtual QWidget *logPage() const { return nullptr; }
    virtual void refresh() {}
    virtual QJsonObject remoteSnapshot() const { return {}; }
    virtual QList<DesktopBranchAction> branchActions() const { return {}; }
};

std::unique_ptr<DesktopPackageExtension>
createDesktopPackageExtension(const QString &entrypoint);
bool hasDesktopPackageExtension(const QString &entrypoint);

} // namespace forkmesh::market

#endif
