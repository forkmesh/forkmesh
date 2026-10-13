#pragma once

#include "ChatFormat.h"
#include "ChatLinkText.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

namespace forkmesh::chatblocks {

inline constexpr int kMaxBlocks = 40;
inline constexpr int kMaxFields = 10;
inline constexpr int kMaxTextChars = 3000;
inline constexpr int kMaxFieldChars = 300;

inline bool senderMayFormat(const QString &senderId)
{
    return senderId.startsWith(QLatin1String("app:")) ||
           senderId.startsWith(QLatin1String("system:"));
}

inline QString blockText(const QJsonValue &value, int limit)
{
    const QJsonValue text =
        value.isObject() ? value.toObject().value(QStringLiteral("text")) : value;
    return text.isString() ? text.toString().trimmed().left(limit) : QString();
}

inline QJsonArray sanitized(const QJsonValue &value)
{
    QJsonArray out;
    if (!value.isArray())
        return out;
    for (const QJsonValue &item : value.toArray()) {
        if (out.size() >= kMaxBlocks || !item.isObject())
            break;
        const QJsonObject block = item.toObject();
        const QString type = block.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("divider")) {
            out.append(QJsonObject{{QStringLiteral("type"), type}});
            continue;
        }
        if (type != QLatin1String("section") && type != QLatin1String("context"))
            continue;
        QJsonObject clean{{QStringLiteral("type"), type}};
        const QString text =
            blockText(block.value(QStringLiteral("text")), kMaxTextChars);
        if (!text.isEmpty())
            clean.insert(QStringLiteral("text"), text);
        QJsonArray fields;
        if (type == QLatin1String("section")) {
            for (const QJsonValue &field :
                 block.value(QStringLiteral("fields")).toArray()) {
                if (fields.size() >= kMaxFields)
                    break;
                const QString line = blockText(field, kMaxFieldChars);
                if (!line.isEmpty())
                    fields.append(line);
            }
        }
        if (!fields.isEmpty())
            clean.insert(QStringLiteral("fields"), fields);
        if (text.isEmpty() && fields.isEmpty())
            continue;
        out.append(clean);
    }
    return out;
}

enum class Tone { None, Good, Warn, Bad, Info };

inline Tone toneOf(const QString &word)
{
    static const QStringList good{
        "accepted", "delivered", "ok", "passed", "sent", "succeeded",
        "success", "done", "healthy", "up", "resolved"};
    static const QStringList warn{
        "suppressed", "soft-bounced", "warning", "skipped", "queued",
        "retrying", "pending", "degraded"};
    static const QStringList bad{
        "failed", "rejected", "bounced", "complained", "blocked", "error",
        "down", "critical"};
    static const QStringList info{"opened", "clicked", "viewed", "noted"};
    static const QRegularExpression letter(QStringLiteral("[a-z]"));
    QString key = word.trimmed().toLower();
    const int start = key.indexOf(letter);
    if (start > 0)
        key = key.mid(start);
    if (good.contains(key))
        return Tone::Good;
    if (warn.contains(key))
        return Tone::Warn;
    if (bad.contains(key))
        return Tone::Bad;
    if (info.contains(key))
        return Tone::Info;
    return Tone::None;
}

struct Palette {
    QString card, border, muted, good, warn, bad, info;
};

inline Palette paletteFor(bool dark)
{
    if (dark)
        return {"#161b22", "#30363d", "#8b949e", "#3fb950", "#d29922",
                "#f85149", "#58a6ff"};
    return {"#f6f8fa", "#d0d7de", "#59636e", "#1a7f37", "#9a6700", "#cf222e",
            "#0969da"};
}

inline QString headlineHtml(const QString &text, const Palette &palette)
{
    const QStringList parts = text.split(QStringLiteral(" · "));
    QStringList pieces;
    for (int index = 0; index < parts.size(); ++index) {
        const QString part = parts.at(index).trimmed();
        if (part.isEmpty())
            continue;
        const QString escaped = part.toHtmlEscaped();
        if (index == 0) {
            pieces.append(QStringLiteral("<span style='font-weight:600'>%1</span>")
                              .arg(escaped));
            continue;
        }
        const Tone tone = toneOf(part);
        if (tone == Tone::None) {
            pieces.append(QStringLiteral("<span style='color:%1'>%2</span>")
                              .arg(palette.muted, escaped));
            continue;
        }
        const QString color = tone == Tone::Good   ? palette.good
                              : tone == Tone::Warn ? palette.warn
                              : tone == Tone::Info ? palette.info
                                                   : palette.bad;
        pieces.append(QStringLiteral("<span style='color:%1; font-weight:600'>%2</span>")
                          .arg(color, escaped));
    }
    return pieces.join(QStringLiteral("&nbsp;&nbsp;"));
}

inline QString fieldRowHtml(const QString &field, const Palette &palette, bool dark)
{
    static const QRegularExpression labelled(
        QStringLiteral("^((?:[^\\w\\s]+\\s)?[A-Za-z][A-Za-z0-9 ]{0,23}):\\s+(.+)$"),
        QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch match = labelled.match(field);
    if (!match.hasMatch())
        return QStringLiteral("<tr><td colspan='2' style='padding-top:2px'>%1</td></tr>")
            .arg(chatformat::inlineHtml(field, {}, dark));
    return QStringLiteral(
               "<tr><td valign='top' style='color:%1; padding-top:2px; "
               "padding-right:14px; white-space:nowrap'>%2</td>"
               "<td valign='top' style='padding-top:2px'>%3</td></tr>")
        .arg(palette.muted, match.captured(1).toHtmlEscaped(),
             chatformat::inlineHtml(match.captured(2), {}, dark));
}

inline QString html(const QJsonArray &blocks, bool dark)
{
    const Palette palette = paletteFor(dark);
    QString out;
    for (const QJsonValue &value : blocks) {
        const QJsonObject block = value.toObject();
        const QString type = block.value(QStringLiteral("type")).toString();
        const QString text = block.value(QStringLiteral("text")).toString();
        if (type == QLatin1String("divider")) {
            out += QStringLiteral("<hr>");
            continue;
        }
        if (type == QLatin1String("context")) {
            out += QStringLiteral("<p style='margin-top:4px; margin-bottom:4px; "
                                  "color:%1'>%2</p>")
                       .arg(palette.muted, chatformat::toHtml(text, {}, dark));
            continue;
        }
        const QJsonArray fields = block.value(QStringLiteral("fields")).toArray();
        if (fields.isEmpty()) {
            out += QStringLiteral("<p style='margin-top:4px; margin-bottom:4px'>%1</p>")
                       .arg(chatformat::toHtml(text, {}, dark));
            continue;
        }
        QString card;
        if (!text.isEmpty())
            card += QStringLiteral("<p style='margin-top:0; margin-bottom:5px'>%1</p>")
                        .arg(headlineHtml(text, palette));
        QString rows;
        for (const QJsonValue &field : fields)
            rows += fieldRowHtml(field.toString(), palette, dark);
        card += QStringLiteral("<table cellspacing='0' cellpadding='0'>%1</table>")
                    .arg(rows);
        out += QStringLiteral(
                   "<table width='100%' cellspacing='0' cellpadding='0' "
                   "style='margin-top:3px; margin-bottom:6px; "
                   "border-collapse:collapse'><tr><td bgcolor='%2' "
                   "style='padding:9px 12px; border:1px solid %1'>%3</td>"
                   "</tr></table>")
                   .arg(palette.border, palette.card, card);
    }
    return out;
}

inline QString plainSummary(const QJsonArray &blocks)
{
    QStringList lines;
    for (const QJsonValue &value : blocks) {
        const QJsonObject block = value.toObject();
        const QString text = block.value(QStringLiteral("text")).toString();
        if (!text.isEmpty())
            lines << chatformat::toPlainText(text);
        for (const QJsonValue &field : block.value(QStringLiteral("fields")).toArray())
            lines << chatformat::toPlainText(field.toString());
    }
    return lines.join(QLatin1Char('\n'));
}

} // namespace forkmesh::chatblocks
