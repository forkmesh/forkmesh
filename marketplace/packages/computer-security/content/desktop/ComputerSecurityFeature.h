#pragma once

#include "NativePackageExtension.h"
#include "SecurityScanner.h"

#include <QList>
#include <QString>

#include <functional>

class QButtonGroup;
class QComboBox;
class QLabel;
class QPushButton;
class QThread;
class QTreeWidget;
class QWidget;

namespace forkmesh::security {

class ComputerSecurityFeature final : public forkmesh::market::NativePackageWidget
{
public:
    explicit ComputerSecurityFeature(const forkmesh::market::NativePackageHost &host, QWidget *parent = nullptr);
    ~ComputerSecurityFeature() override;

    void refresh() override;
    void setHostSearchQuery(const QString &query) override;
    QList<forkmesh::market::NativePackageSearchHit>
    searchHits(const QString &query, int limit) const override;
    void activateSearchHit(const QString &id) override;

private:
    void render();
    void showDetails(const QString &id);
    void applyFix(const QString &id);
    void copyCommand(const QString &id);
    const Finding *findingById(const QString &id) const;
    bool matches(const Finding &finding, bool useFilter = true) const;

    QList<Finding> m_findings;
    QString m_query;
    QString m_selectedId;
    QString m_category;
    FindingFilter m_filter = FindingFilter::All;
    std::function<void()> m_searchIndexChanged;
    QLabel *m_summary = nullptr;
    QLabel *m_note = nullptr;
    QLabel *m_empty = nullptr;
    QLabel *m_status = nullptr;
    QButtonGroup *m_filterGroup = nullptr;
    QComboBox *m_categories = nullptr;
    QTreeWidget *m_tree = nullptr;
    QPushButton *m_refresh = nullptr;
    QWidget *m_details = nullptr;
    QLabel *m_detailTitle = nullptr;
    QLabel *m_detailState = nullptr;
    QLabel *m_detailResult = nullptr;
    QLabel *m_detailExplanation = nullptr;
    QLabel *m_usageHeading = nullptr;
    QLabel *m_detailUsage = nullptr;
    QLabel *m_detailFix = nullptr;
    QLabel *m_detailCommand = nullptr;
    QPushButton *m_fixButton = nullptr;
    QPushButton *m_copyButton = nullptr;
    QThread *m_scanThread = nullptr;
    bool m_fixRunning = false;
};

}
