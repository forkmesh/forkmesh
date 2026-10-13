#pragma once

#include <QDir>
#include <QList>
#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace forkmesh::security {

enum class FindingState { Pass, Review, Warning, Critical, Unavailable };

enum class FindingFilter { All, Attention, Critical, Warning, Review, Fixable, Pass, Unavailable };

struct CheckDefinition {
    QString id;
    QString title;
    QString category;
    QString target;
    QString expected;
    QString probe;
    QString explanation;
};

struct FixAction {
    QString label;
    QString description;
    QString caution;
    QString script;
    bool elevated = true;
};

struct Finding {
    CheckDefinition check;
    FindingState state = FindingState::Unavailable;
    QString detail;
    QString usage;
    FixAction fix;
};

struct ScanContext {
    QString root = QStringLiteral("/");
    QString procRoot = QStringLiteral("/proc");
    QString home = QDir::homePath();
    QString user = qEnvironmentVariable("USER");
    QString pathVariable = qEnvironmentVariable("PATH");
};

QList<CheckDefinition> checkCatalog();
QList<Finding> scan(const ScanContext &context = {});
QString stateName(FindingState state);
int severityRank(FindingState state);
QString stateSummary(const QList<Finding> &findings);
QList<FindingFilter> filters();
QString filterName(FindingFilter filter);
bool matchesFilter(const Finding &finding, FindingFilter filter);
bool matchesQuery(const Finding &finding, const QString &query);
QString filteredSummary(const QList<Finding> &findings, int matched, const QString &query,
                        FindingFilter filter = FindingFilter::All, const QString &category = {});
QString shellQuote(const QString &value);
QString manualCommand(const FixAction &fix);

}
