#include "VSCodiumDesktopExtension.h"

#include "ProcessReaper.h"

#include <QFileInfo>
#include <QStandardPaths>

namespace {

QString codiumExecutable()
{
    QString codium = QStandardPaths::findExecutable(QStringLiteral("codium"));
    if (codium.isEmpty())
        codium = QStandardPaths::findExecutable(QStringLiteral("vscodium"));
#ifdef Q_OS_MACOS
    if (codium.isEmpty()) {
        const QString cli = QStringLiteral(
            "/Applications/VSCodium.app/Contents/Resources/app/bin/codium");
        if (QFileInfo::exists(cli))
            codium = cli;
    }
#endif
    return codium;
}

} // namespace

QString VSCodiumDesktopExtension::packageId() const
{
    return QStringLiteral("vscodium");
}

void VSCodiumDesktopExtension::activate(
    const forkmesh::market::DesktopPackageContext &context)
{
    m_context = context;
}

void VSCodiumDesktopExtension::deactivate()
{
    m_context = {};
}

QList<forkmesh::market::DesktopBranchAction>
VSCodiumDesktopExtension::branchActions() const
{
    forkmesh::market::DesktopBranchAction action;
    action.label = QStringLiteral("Codium");
    action.icon = QStringLiteral("code");
    action.toolTip = QStringLiteral("Open this branch's checkout in VSCodium");
    action.trigger = [this](const forkmesh::market::DesktopBranchTarget &target) {
        openBranch(target);
    };
    return {action};
}

void VSCodiumDesktopExtension::openBranch(
    const forkmesh::market::DesktopBranchTarget &target) const
{
    const auto notify = [this](const QString &text, bool error) {
        if (m_context.notify)
            m_context.notify(text, error);
    };
    if (target.directory.isEmpty()) {
        notify(QStringLiteral("No local checkout to open."), true);
        return;
    }
    const QString codium = codiumExecutable();
    if (codium.isEmpty()) {
        notify(QString::fromUtf8("VSCodium not found \xE2\x80\x94 install it and "
                                 "ensure \"codium\" is on PATH."),
               true);
        return;
    }
    if (forkmesh::processreaper::startReleased(codium, {target.directory}))
        notify(QString::fromUtf8("Opening %1 in VSCodium\xE2\x80\xA6")
                   .arg(target.branch),
               false);
    else
        notify(QStringLiteral("Could not launch VSCodium."), true);
}

namespace forkmesh::market {

DesktopPackageExtension *createVSCodiumDesktopExtension()
{
    return new VSCodiumDesktopExtension;
}

} // namespace forkmesh::market
