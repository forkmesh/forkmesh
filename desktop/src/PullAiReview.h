#pragma once

#include <QList>
#include <QString>
#include <QStringList>


struct AiReviewFinding {
    QString path;      // repo-relative file the issue is in
    int lineStart = 0; // first affected line (new side)
    int lineEnd = 0;   // last affected line (>= lineStart)
    QString severity;  // bug | warning | nit
    QString comment;   // what is wrong and why
    QString suggestionPatch;
};

QString buildAiReviewPrompt(const QString &title, const QString &description,
                            const QString &patch, int maxDiffChars = 120000);

QList<AiReviewFinding> parseAiReviewFindings(const QString &text,
                                             bool *ok = nullptr);

QString buildSuggestionPatch(const QStringList &original,
                             const QStringList &replacement, int lineStart);
bool parseSuggestionPatch(const QString &patch, QStringList *original,
                          QStringList *replacement);

bool applySuggestionToContent(QString *content, int lineStart,
                              const QString &patch, QString *error = nullptr);
