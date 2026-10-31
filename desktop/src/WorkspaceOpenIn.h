#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

namespace forkmesh::openin {

struct Editor {
    QString label;
    QString program;
};

struct Candidate {
    const char *label;
    QStringList programs;
};

inline QList<Candidate> candidates()
{
    return {
        {"VS Code", {QStringLiteral("code")}},
        {"VSCodium", {QStringLiteral("codium"), QStringLiteral("vscodium")}},
        {"Cursor", {QStringLiteral("cursor")}},
        {"Zed", {QStringLiteral("zed"), QStringLiteral("zeditor")}},
        {"Windsurf", {QStringLiteral("windsurf")}},
    };
}

inline QList<Editor> availableEditors(const std::function<QString(const QString &)> &find)
{
    QList<Editor> out;
    if (!find)
        return out;
    for (const Candidate &candidate : candidates()) {
        for (const QString &program : candidate.programs) {
            const QString path = find(program);
            if (path.isEmpty())
                continue;
            out.append({QString::fromLatin1(candidate.label), path});
            break;
        }
    }
    return out;
}

}
