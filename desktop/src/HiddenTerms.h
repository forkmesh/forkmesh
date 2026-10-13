#pragma once

#include <QByteArray>
#include <QMap>
#include <QReadWriteLock>
#include <QSettings>
#include <QString>
#include <QStringList>

#include <algorithm>

namespace forkmesh::hiddenterms {

inline constexpr qsizetype kMinVariableValueLength = 8;

inline QString hideVariablesKey()
{
    return QStringLiteral("search/hideVariableValues");
}

inline QReadWriteLock &termsLock()
{
    static QReadWriteLock lock;
    return lock;
}

inline QStringList &termsStorage()
{
    static QStringList terms;
    return terms;
}

inline QStringList &variableValuesStorage()
{
    static QStringList values;
    return values;
}

inline void setTerms(const QStringList &terms)
{
    QWriteLocker locker(&termsLock());
    termsStorage() = terms;
}

inline QStringList terms()
{
    QReadLocker locker(&termsLock());
    return termsStorage();
}

inline QStringList variableTerms(const QMap<QString, QString> &variables)
{
    QStringList out;
    for (const QString &raw : variables) {
        const QString value = raw.trimmed();
        if (value.size() < kMinVariableValueLength)
            continue;
        if (!out.contains(value, Qt::CaseInsensitive))
            out.append(value);
    }
    std::sort(out.begin(), out.end(),
              [](const QString &a, const QString &b) { return a.size() > b.size(); });
    return out;
}

inline void setVariableValues(const QStringList &values)
{
    QWriteLocker locker(&termsLock());
    variableValuesStorage() = values;
}

inline qsizetype variableValueCount()
{
    QReadLocker locker(&termsLock());
    return variableValuesStorage().size();
}

inline bool hidesVariables()
{
    return QSettings().value(hideVariablesKey(), true).toBool();
}

inline void refreshVariableValues(const QMap<QString, QString> &variables)
{
    setVariableValues(hidesVariables() ? variableTerms(variables) : QStringList());
}

inline QStringList activeTerms()
{
    QReadLocker locker(&termsLock());
    return variableValuesStorage() + termsStorage();
}

inline bool hides(const QString &text)
{
    for (const QString &term : activeTerms())
        if (!term.isEmpty() && text.contains(term, Qt::CaseInsensitive))
            return true;
    return false;
}

inline QString redact(QString text)
{
    for (const QString &term : activeTerms())
        if (!term.isEmpty())
            text.replace(term, QStringLiteral("[hidden]"), Qt::CaseInsensitive);
    return text;
}

inline QByteArray redact(const QByteArray &bytes)
{
    const QStringList list = activeTerms();
    if (list.isEmpty())
        return bytes;
    const QString text = QString::fromUtf8(bytes);
    const QString clean = redact(text);
    return clean == text ? bytes : clean.toUtf8();
}

} // namespace forkmesh::hiddenterms
