#include "PullAiReview.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

namespace {

QStringList contentLines(const QString &content)
{
    QStringList lines = content.split(QLatin1Char('\n'));
    if (!lines.isEmpty() && lines.last().isEmpty())
        lines.removeLast();
    return lines;
}

} // namespace

QString buildAiReviewPrompt(const QString &title, const QString &description,
                            const QString &patch, int maxDiffChars)
{
    QString diff = patch;
    bool truncated = false;
    if (diff.size() > maxDiffChars) {
        int cut = diff.lastIndexOf(QLatin1Char('\n'), maxDiffChars);
        if (cut <= 0)
            cut = maxDiffChars;
        diff.truncate(cut);
        truncated = true;
    }

    QString prompt;
    prompt += QStringLiteral(
        "You are a rigorous code reviewer. Review the following pull request "
        "diff and report genuine problems the change introduces: bugs, logic "
        "errors, security issues, data loss, race conditions, resource leaks, "
        "broken error handling and clear correctness hazards. Do not report "
        "style preferences, and do not comment on code the diff merely moves.");
    prompt += QStringLiteral("\n\nPull request: %1\n").arg(title);
    if (!description.trimmed().isEmpty())
        prompt += QStringLiteral("\n%1\n").arg(description.trimmed());
    prompt += QStringLiteral(
        "\nThe diff (unified format; @@ hunk headers give the line numbers of "
        "the file after the change):\n----- BEGIN DIFF -----\n%1\n"
        "----- END DIFF -----\n").arg(diff);
    if (truncated)
        prompt += QStringLiteral(
            "(The diff was truncated to fit the request; review what is "
            "shown.)\n");
    prompt += QStringLiteral(
        "\nReply with ONLY a JSON array - no prose, no markdown fences. Each "
        "element must be:\n"
        "{\"path\": \"<repo-relative file path>\",\n"
        " \"line_start\": <first affected line in the file AFTER the change>,\n"
        " \"line_end\": <last affected line>,\n"
        " \"severity\": \"bug\" | \"warning\" | \"nit\",\n"
        " \"comment\": \"<what is wrong and why, in 1-3 sentences>\",\n"
        " \"original\": \"<the exact text of lines line_start..line_end as "
        "they read after the change, or null>\",\n"
        " \"fix\": \"<replacement for exactly those lines that fixes the "
        "problem, or null when no safe self-contained fix exists>\"}\n"
        "Only report issues in lines this diff adds or modifies. \"original\" "
        "and \"fix\" must be provided together or both be null. Use \\n for "
        "line breaks inside strings. If the change introduces no issues, "
        "reply with [].");
    return prompt;
}

QList<AiReviewFinding> parseAiReviewFindings(const QString &text, bool *ok)
{
    if (ok)
        *ok = false;
    QList<AiReviewFinding> findings;

    QJsonArray arr;
    const QJsonDocument doc = QJsonDocument::fromJson(text.trimmed().toUtf8());
    if (doc.isArray()) {
        arr = doc.array();
    } else if (doc.isObject() &&
               doc.object().value(QLatin1String("findings")).isArray()) {
        arr = doc.object().value(QLatin1String("findings")).toArray();
    } else {
        const int open = text.indexOf(QLatin1Char('['));
        const int close = text.lastIndexOf(QLatin1Char(']'));
        if (open < 0 || close <= open)
            return findings;
        const QJsonDocument inner =
            QJsonDocument::fromJson(text.mid(open, close - open + 1).toUtf8());
        if (!inner.isArray())
            return findings;
        arr = inner.array();
    }
    if (ok)
        *ok = true;

    for (const QJsonValue &v : std::as_const(arr)) {
        if (!v.isObject())
            continue;
        const QJsonObject o = v.toObject();
        AiReviewFinding f;
        f.path = o.value(QLatin1String("path")).toString().trimmed();
        f.lineStart = o.value(QLatin1String("line_start"))
                          .toInt(o.value(QLatin1String("line")).toInt());
        f.lineEnd = o.value(QLatin1String("line_end")).toInt(f.lineStart);
        if (f.lineEnd < f.lineStart)
            f.lineEnd = f.lineStart;
        f.severity = o.value(QLatin1String("severity")).toString().trimmed();
        if (f.severity != QLatin1String("bug") &&
            f.severity != QLatin1String("nit"))
            f.severity = QStringLiteral("warning");
        f.comment = o.value(QLatin1String("comment")).toString().trimmed();
        if (f.path.isEmpty() || f.comment.isEmpty() || f.lineStart <= 0)
            continue;
        const QJsonValue original = o.value(QLatin1String("original"));
        const QJsonValue fix = o.value(QLatin1String("fix"));
        if (original.isString() && fix.isString() &&
            !original.toString().isEmpty()) {
            const QStringList origLines =
                original.toString().split(QLatin1Char('\n'));
            if (origLines.size() == f.lineEnd - f.lineStart + 1) {
                QStringList fixLines = fix.toString().split(QLatin1Char('\n'));
                if (fixLines.size() == 1 && fixLines.first().isEmpty())
                    fixLines.clear(); // "" fix = delete the lines
                f.suggestionPatch =
                    buildSuggestionPatch(origLines, fixLines, f.lineStart);
            }
        }
        findings.append(f);
    }
    return findings;
}

QString buildSuggestionPatch(const QStringList &original,
                             const QStringList &replacement, int lineStart)
{
    if (original.isEmpty())
        return QString();
    QStringList out;
    out << QStringLiteral("@@ -%1,%2 +%1,%3 @@")
               .arg(lineStart)
               .arg(original.size())
               .arg(replacement.size());
    for (const QString &line : original)
        out << QLatin1Char('-') + line;
    for (const QString &line : replacement)
        out << QLatin1Char('+') + line;
    return out.join(QLatin1Char('\n'));
}

bool parseSuggestionPatch(const QString &patch, QStringList *original,
                          QStringList *replacement)
{
    if (original)
        original->clear();
    if (replacement)
        replacement->clear();
    for (const QString &line : patch.split(QLatin1Char('\n'))) {
        if (line.startsWith(QLatin1String("@@")))
            continue;
        if (line.startsWith(QLatin1Char('-'))) {
            if (original)
                original->append(line.mid(1));
        } else if (line.startsWith(QLatin1Char('+'))) {
            if (replacement)
                replacement->append(line.mid(1));
        }
    }
    return original && !original->isEmpty();
}

bool applySuggestionToContent(QString *content, int lineStart,
                              const QString &patch, QString *error)
{
    if (!content)
        return false;
    QStringList original;
    QStringList replacement;
    if (!parseSuggestionPatch(patch, &original, &replacement)) {
        if (error)
            *error = QStringLiteral("The suggestion carries no original lines "
                                    "to replace.");
        return false;
    }
    QStringList lines = contentLines(*content);
    const auto matchesAt = [&](int start0) {
        if (start0 < 0 || start0 + original.size() > lines.size())
            return false;
        for (int i = 0; i < original.size(); ++i)
            if (lines.at(start0 + i) != original.at(i))
                return false;
        return true;
    };
    int start0 = lineStart - 1;
    if (!matchesAt(start0)) {
        int found = -1;
        for (int at = 0; at + original.size() <= lines.size(); ++at) {
            if (!matchesAt(at))
                continue;
            if (found >= 0) {
                if (error)
                    *error = QStringLiteral(
                        "The lines this fix replaces appear more than once in "
                        "the file - apply it by hand.");
                return false;
            }
            found = at;
        }
        if (found < 0) {
            if (error)
                *error = QStringLiteral(
                    "The file no longer matches the lines this fix replaces - "
                    "it may have changed since the review.");
            return false;
        }
        start0 = found;
    }
    const bool trailingNewline = content->endsWith(QLatin1Char('\n'));
    QStringList out = lines.mid(0, start0);
    out += replacement;
    out += lines.mid(start0 + original.size());
    *content = out.join(QLatin1Char('\n'));
    if (trailingNewline)
        content->append(QLatin1Char('\n'));
    return true;
}
