#pragma once

#include "RepoSecurity.h"

#include <QFrame>
#include <QList>
#include <QString>

#include <functional>

class QLabel;
class QPushButton;
class QVBoxLayout;

namespace forkmesh::ui {

QString secretScanIgnoreMarker();
QList<RepoSecurityFinding> secretFindingsByLine(const QList<RepoSecurityFinding> &findings);
QList<RepoSecurityFinding> secretFindingsWithout(const QList<RepoSecurityFinding> &findings,
                                                 const QList<RepoSecurityFinding> &done);

class SecretDiffBanner : public QFrame
{
public:
    explicit SecretDiffBanner(QWidget *parent = nullptr);

    void setFindings(const QList<RepoSecurityFinding> &findings, bool canEdit);
    void clearFindings();
    void removeFindings(const QList<RepoSecurityFinding> &done);
    void setOuterMargins(int left, int top, int right, int bottom);
    const QList<RepoSecurityFinding> &findings() const { return m_findings; }

    std::function<void(const QList<RepoSecurityFinding> &)> onIgnore;

    static constexpr int kMaxRows = 8;

private:
    void restyle();
    void rebuildRows();
    void requestIgnore(const QList<RepoSecurityFinding> &findings);

    QList<RepoSecurityFinding> m_findings;
    bool m_canEdit = false;
    bool m_dark = false;
    QString m_margins = QStringLiteral("0px");
    QLabel *m_title = nullptr;
    QLabel *m_body = nullptr;
    QPushButton *m_ignoreAll = nullptr;
    QVBoxLayout *m_layout = nullptr;
    QWidget *m_rows = nullptr;
};

}
