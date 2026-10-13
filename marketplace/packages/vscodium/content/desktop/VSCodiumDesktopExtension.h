#ifndef FORKMESH_VSCODIUM_DESKTOP_EXTENSION_H
#define FORKMESH_VSCODIUM_DESKTOP_EXTENSION_H

#include "DesktopPackageExtension.h"

class VSCodiumDesktopExtension final
    : public forkmesh::market::DesktopPackageExtension
{
public:
    QString packageId() const override;
    void activate(const forkmesh::market::DesktopPackageContext &context) override;
    void deactivate() override;
    QList<forkmesh::market::DesktopBranchAction> branchActions() const override;

private:
    void openBranch(const forkmesh::market::DesktopBranchTarget &target) const;

    forkmesh::market::DesktopPackageContext m_context;
};

namespace forkmesh::market {
DesktopPackageExtension *createVSCodiumDesktopExtension();
}

#endif
