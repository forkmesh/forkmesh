#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

namespace forkmesh::destructive {

QStringList kinds();
QString kindExample(const QString &kind);

struct Review {
    enum class Lean { Allow, Deny, Unsure };
    QString what;
    QString undo;
    QStringList forAllow;
    QStringList forDeny;
    Lean lean = Lean::Unsure;
    QString leanReason;
};

Review review(const QString &command, const QString &kind, qsizetype matchStart,
              qsizetype matchLength, const QHash<QString, QString> &values = {},
              const QString &scratch = QString());

QString commandHtml(const QString &command, qsizetype matchStart, qsizetype matchLength,
                    const QString &cwd, const QString &homeDir, bool dark);

QString programUrl(const QString &program);
QString subcommandUrl(const QString &program, const QStringList &operands);

} // namespace forkmesh::destructive
