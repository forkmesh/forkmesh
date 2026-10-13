#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <utility>

namespace forkmesh::commandvars {

inline const QString ScratchName = QStringLiteral("FORKMESH_AGENT_SCRATCH");

struct Environment {
    QHash<QString, QString> values;
    QStringList hidden;
    bool sent = false;
};

struct Variable {
    enum class Source { Command, Environment, Hidden, Unset, Unknown };
    QString name;
    QString assigned;
    QString value;
    bool known = false;
    Source source = Source::Unknown;
};

QList<Variable> variables(const QString &command, const Environment &environment);
QHash<QString, QString> valuesAt(const QString &command, const Environment &environment,
                                 qsizetype position);
QString trustedScratch(const Environment &environment);
bool deletesOnlyInside(const QString &command, const QString &root);
bool pathInside(const QString &path, const QString &root);

QStringList removedPaths(const QString &command, const Environment &environment,
                         const QList<std::pair<qsizetype, qsizetype>> &spans);
QString expandDeletePattern(const QString &pattern, const QString &scratch);
QString deletePatternProblem(const QString &pattern);
bool deletePatternMatches(const QString &pattern, const QString &path, const QString &scratch);
bool protectedDeletePath(const QString &path);
QString uncoveredDeletePath(const QStringList &patterns, const QStringList &paths,
                            const QString &scratch);
bool deletesAllowed(const QStringList &patterns, const QStringList &paths,
                    const QString &scratch);
QStringList suggestedDeletePatterns(const QStringList &paths, const QString &scratch);

} // namespace forkmesh::commandvars
