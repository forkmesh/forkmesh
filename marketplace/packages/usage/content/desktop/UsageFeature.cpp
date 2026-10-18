#include "NativePackageExtension.h"

#include <QLabel>
#include <QVBoxLayout>

namespace {

class UsageFeature final : public forkmesh::market::NativePackageWidget
{
public:
    UsageFeature(const forkmesh::market::NativePackageHost &host, QWidget *parent)
        : forkmesh::market::NativePackageWidget(parent)
    {
        setObjectName(QStringLiteral("usageSection"));
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        QWidget *page = host.agentUsagePage ? host.agentUsagePage(this) : nullptr;
        if (page) {
            layout->addWidget(page);
            return;
        }
        auto *notice = new QLabel(
            QStringLiteral("Usage needs permission to read this desktop's Claude Code "
                           "and Codex usage."),
            this);
        notice->setObjectName(QStringLiteral("statusLine"));
        notice->setWordWrap(true);
        notice->setContentsMargins(20, 18, 20, 20);
        layout->addWidget(notice, 0, Qt::AlignTop);
    }

    void refresh() override {}
};

const forkmesh::market::NativePackageRegistration usageRegistration({
    QStringLiteral("builtin:usage"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Usage"),
    QStringLiteral("pulse"),
    QString(),
    QString(),
    [](const forkmesh::market::NativePackageHost &host, QWidget *parent) {
        return new UsageFeature(host, parent);
    },
});

}
