#include "SecretDiffBanner.h"

#include "Theme.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
#include <QPushButton>
#include <QSet>
#include <QVBoxLayout>

namespace forkmesh::ui {

namespace {

bool bannerIsDark()
{
    return qApp->palette().color(QPalette::Base).lightness() < 128;
}

QPushButton *bannerButton(const QString &text, const char *objectName,
                          QWidget *parent)
{
    auto *button = new QPushButton(text, parent);
    button->setObjectName(QLatin1String(objectName));
    button->setCursor(Qt::PointingHandCursor);
    button->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    return button;
}

}

QString secretScanIgnoreMarker()
{
    return QStringLiteral("forkmesh-secret-scan:ignore-line");
}

static QString secretFindingLineKey(const RepoSecurityFinding &finding)
{
    return finding.path + QLatin1Char('\n') + QString::number(finding.line);
}

QList<RepoSecurityFinding> secretFindingsByLine(const QList<RepoSecurityFinding> &findings)
{
    QList<RepoSecurityFinding> out;
    QSet<QString> seen;
    for (const RepoSecurityFinding &finding : findings) {
        const QString key = secretFindingLineKey(finding);
        if (seen.contains(key))
            continue;
        seen.insert(key);
        out.append(finding);
    }
    return out;
}

QList<RepoSecurityFinding> secretFindingsWithout(const QList<RepoSecurityFinding> &findings,
                                                 const QList<RepoSecurityFinding> &done)
{
    QSet<QString> doneKeys;
    for (const RepoSecurityFinding &finding : done)
        doneKeys.insert(secretFindingLineKey(finding));
    QList<RepoSecurityFinding> out;
    for (const RepoSecurityFinding &finding : findings)
        if (!doneKeys.contains(secretFindingLineKey(finding)))
            out.append(finding);
    return out;
}

SecretDiffBanner::SecretDiffBanner(QWidget *parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("secretDiffBanner"));
    setAccessibleName(QStringLiteral("Possible secrets in this diff"));
    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(14, 10, 14, 10);
    m_layout->setSpacing(6);

    auto *head = new QHBoxLayout;
    head->setContentsMargins(0, 0, 0, 0);
    head->setSpacing(10);
    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("secretDiffBannerTitle"));
    m_title->setTextFormat(Qt::PlainText);
    m_title->setWordWrap(true);
    head->addWidget(m_title, 1);
    m_ignoreAll = bannerButton(QString(), "secretDiffBannerIgnoreAll", this);
    connect(m_ignoreAll, &QPushButton::clicked, this,
            [this] { requestIgnore(m_findings); });
    head->addWidget(m_ignoreAll, 0, Qt::AlignTop);
    m_layout->addLayout(head);

    m_body = new QLabel(this);
    m_body->setObjectName(QStringLiteral("secretDiffBannerBody"));
    m_body->setTextFormat(Qt::RichText);
    m_body->setWordWrap(true);
    m_body->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_body->setMinimumWidth(1);
    m_layout->addWidget(m_body);

    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    hide();
}

void SecretDiffBanner::clearFindings()
{
    setFindings({}, m_canEdit);
}

void SecretDiffBanner::removeFindings(const QList<RepoSecurityFinding> &done)
{
    setFindings(secretFindingsWithout(m_findings, done), m_canEdit);
}

void SecretDiffBanner::setOuterMargins(int left, int top, int right, int bottom)
{
    m_margins = QStringLiteral("%1px %2px %3px %4px")
                    .arg(top)
                    .arg(right)
                    .arg(bottom)
                    .arg(left);
    if (!m_findings.isEmpty())
        restyle();
}

void SecretDiffBanner::setFindings(const QList<RepoSecurityFinding> &findings,
                                   bool canEdit)
{
    const QList<RepoSecurityFinding> next = secretFindingsByLine(findings);
    const auto sameFindings = [this, &next] {
        if (next.size() != m_findings.size())
            return false;
        for (int i = 0; i < next.size(); ++i)
            if (secretFindingLineKey(next.at(i)) !=
                    secretFindingLineKey(m_findings.at(i)) ||
                next.at(i).title != m_findings.at(i).title)
                return false;
        return true;
    };
    if (sameFindings() && canEdit == m_canEdit && m_dark == bannerIsDark() &&
        isHidden() == next.isEmpty())
        return;
    m_findings = next;
    m_canEdit = canEdit;
    if (m_findings.isEmpty()) {
        hide();
        rebuildRows();
        return;
    }
    restyle();
    const int count = static_cast<int>(m_findings.size());
    m_title->setText(
        QStringLiteral("⚠  %1 possible secret%2 in this diff")
            .arg(count)
            .arg(count == 1 ? QString() : QStringLiteral("s")));
    m_body->setText(
        QStringLiteral("These added lines match credential patterns. If one is a "
                       "real key or token, remove it and rotate it before it is "
                       "pushed. If it is a fake or example value, add "
                       "<code>%1</code> to the line and the secret scanner "
                       "skips it.")
            .arg(secretScanIgnoreMarker()));
    m_ignoreAll->setText(count == 1 ? QStringLiteral("Add ignore marker")
                                    : QStringLiteral("Add ignore marker to all %1")
                                          .arg(count));
    m_ignoreAll->setEnabled(m_canEdit && static_cast<bool>(onIgnore));
    m_ignoreAll->setToolTip(
        m_canEdit ? QStringLiteral("Append \"%1\" as a comment to every flagged line")
                        .arg(secretScanIgnoreMarker())
                  : QStringLiteral("There is no checkout of these changes on this "
                                   "machine to write the marker into"));
    rebuildRows();
    show();
}

void SecretDiffBanner::restyle()
{
    const bool dark = bannerIsDark();
    m_dark = dark;
    setStyleSheet(Theme::remapSheet(
        QStringLiteral(
            "#secretDiffBanner{background:%1;border:2px solid %2;border-radius:8px;"
            "margin:%7;}"
            "#secretDiffBanner QLabel{background:transparent;color:%3;}"
            "#secretDiffBanner QLabel#secretDiffBannerTitle{font-size:15px;"
            "font-weight:700;color:%2;}"
            "#secretDiffBanner QLabel#secretDiffBannerRow{font-family:monospace;}"
            "#secretDiffBanner QPushButton{background:%4;color:#ffffff;"
            "border:1px solid %4;border-radius:6px;padding:3px 10px;"
            "font-weight:600;min-height:0;}"
            "#secretDiffBanner QPushButton:hover{background:%5;border-color:%5;}"
            "#secretDiffBanner QPushButton:disabled{background:transparent;"
            "color:%6;border-color:%6;}")
            .arg(dark ? QStringLiteral("#3c1618") : QStringLiteral("#ffebe9"),
                 dark ? QStringLiteral("#f85149") : QStringLiteral("#cf222e"),
                 dark ? QStringLiteral("#ffdcd7") : QStringLiteral("#1f2328"),
                 dark ? QStringLiteral("#da3633") : QStringLiteral("#cf222e"),
                 dark ? QStringLiteral("#f85149") : QStringLiteral("#a40e26"),
                 dark ? QStringLiteral("#8b949e") : QStringLiteral("#8c959f"),
                 m_margins)));
}

void SecretDiffBanner::rebuildRows()
{
    if (m_rows) {
        m_rows->hide();
        m_layout->removeWidget(m_rows);
        m_rows->deleteLater();
        m_rows = nullptr;
    }
    if (m_findings.isEmpty())
        return;
    m_rows = new QWidget(this);
    auto *rowsLayout = new QVBoxLayout(m_rows);
    rowsLayout->setContentsMargins(0, 2, 0, 0);
    rowsLayout->setSpacing(4);
    const int shown = qMin(static_cast<int>(m_findings.size()), kMaxRows);
    for (int i = 0; i < shown; ++i) {
        const RepoSecurityFinding finding = m_findings.at(i);
        auto *row = new QHBoxLayout;
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(10);
        auto *label = new QLabel(m_rows);
        label->setObjectName(QStringLiteral("secretDiffBannerRow"));
        label->setTextFormat(Qt::RichText);
        label->setWordWrap(true);
        label->setMinimumWidth(1);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        label->setText(QStringLiteral("<b>%1</b> &nbsp;%2:%3")
                           .arg(finding.title.toHtmlEscaped(),
                                finding.path.toHtmlEscaped())
                           .arg(finding.line));
        label->setToolTip(finding.detail);
        row->addWidget(label, 1);
        auto *button = bannerButton(QStringLiteral("Add ignore marker"),
                                    "secretDiffBannerIgnore", m_rows);
        button->setEnabled(m_canEdit && static_cast<bool>(onIgnore));
        button->setToolTip(m_ignoreAll->toolTip());
        connect(button, &QPushButton::clicked, this,
                [this, finding] { requestIgnore({finding}); });
        row->addWidget(button, 0, Qt::AlignVCenter);
        rowsLayout->addLayout(row);
    }
    if (m_findings.size() > shown) {
        auto *more = new QLabel(m_rows);
        more->setTextFormat(Qt::PlainText);
        more->setText(QStringLiteral("… and %1 more")
                          .arg(m_findings.size() - shown));
        rowsLayout->addWidget(more);
    }
    m_layout->addWidget(m_rows);
}

void SecretDiffBanner::requestIgnore(const QList<RepoSecurityFinding> &findings)
{
    if (!m_canEdit || !onIgnore || findings.isEmpty())
        return;
    onIgnore(findings);
}

}
