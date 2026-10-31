#include "ClaudeSessionImport.h"

#include "ClaudeSessionScan.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSaveFile>

namespace {

constexpr int kMaxToolResultChars = 16 * 1024;

double eventMs(const QJsonObject &o)
{
    const QDateTime when = QDateTime::fromString(
        o.value(QStringLiteral("timestamp")).toString(), Qt::ISODateWithMs);
    return when.isValid() ? double(when.toMSecsSinceEpoch()) : 0.0;
}

QString clip(const QString &text)
{
    if (text.size() <= kMaxToolResultChars)
        return text;
    return text.left(kMaxToolResultChars) +
           QStringLiteral("\n\xE2\x80\xA6 [%1 more characters not imported]")
               .arg(text.size() - kMaxToolResultChars);
}

QJsonValue clipToolContent(const QJsonValue &content)
{
    if (content.isString())
        return clip(content.toString());
    QJsonArray out;
    for (const QJsonValue &value : content.toArray()) {
        QJsonObject block = value.toObject();
        const QString type = block.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("text"))
            block.insert(QStringLiteral("text"),
                         clip(block.value(QStringLiteral("text")).toString()));
        else if (type == QLatin1String("image"))
            block = QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                {QStringLiteral("text"), QStringLiteral("[image]")}};
        out.append(block);
    }
    return out;
}

QString peerBody(const QString &text)
{
    static const QRegularExpression envelope(
        QStringLiteral("^<cross-session-message[^>]*>\\n([\\s\\S]*)\\n</cross-session-message>"));
    const QRegularExpressionMatch match = envelope.match(text.trimmed());
    if (!match.hasMatch())
        return text;
    QString body = match.captured(1).trimmed();
    if (body.startsWith(QLatin1String("Your user typed this in the ForkMesh"))) {
        const int split = body.indexOf(QLatin1String("\n\n"));
        body = split < 0 ? QString() : body.mid(split + 2).trimmed();
    }
    return body;
}

bool hiddenPrompt(const QString &text)
{
    const QString t = text.trimmed();
    return t.isEmpty() || t.startsWith(QLatin1Char('<')) ||
           t.startsWith(QLatin1String("Caveat:"));
}

} // namespace

namespace ClaudeSessionImport {

bool validSessionId(const QString &sessionId)
{
    static const QRegularExpression shape(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_-]{0,127}$"));
    return shape.match(sessionId).hasMatch();
}

ClaudeImportBatch convertLines(const QList<QJsonObject> &lines, const QString &sessionId)
{
    ClaudeImportBatch batch;
    for (const QJsonObject &o : lines) {
        if (o.value(QStringLiteral("isSidechain")).toBool() ||
            o.value(QStringLiteral("isMeta")).toBool())
            continue;
        const QString type = o.value(QStringLiteral("type")).toString();
        const double ms = eventMs(o);
        if (ms > batch.lastMs)
            batch.lastMs = qint64(ms);
        const QJsonObject message = o.value(QStringLiteral("message")).toObject();
        if (type == QLatin1String("user")) {
            const QJsonValue content = message.value(QStringLiteral("content"));
            QStringList texts;
            QJsonArray results;
            if (content.isString()) {
                texts << content.toString();
            } else {
                for (const QJsonValue &value : content.toArray()) {
                    QJsonObject block = value.toObject();
                    const QString blockType = block.value(QStringLiteral("type")).toString();
                    if (blockType == QLatin1String("text")) {
                        texts << block.value(QStringLiteral("text")).toString();
                    } else if (blockType == QLatin1String("image")) {
                        texts << QStringLiteral("[image]");
                    } else if (blockType == QLatin1String("tool_result")) {
                        block.insert(QStringLiteral("content"),
                                     clipToolContent(block.value(QStringLiteral("content"))));
                        results.append(block);
                    }
                }
            }
            const QString text = peerBody(texts.join(QStringLiteral("\n\n")).trimmed());
            if (!hiddenPrompt(text)) {
                QJsonObject ev{{QStringLiteral("type"), QStringLiteral("_local_user")},
                               {QStringLiteral("text"), text}};
                if (ms > 0)
                    ev.insert(QStringLiteral("_ts"), ms);
                batch.events.append(ev);
                batch.log += QStringLiteral("\n> ") + text + QLatin1Char('\n');
            }
            if (!results.isEmpty())
                batch.events.append(QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("user")},
                    {QStringLiteral("message"),
                     QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                 {QStringLiteral("content"), results}}},
                    {QStringLiteral("parent_tool_use_id"), QJsonValue::Null},
                    {QStringLiteral("session_id"), sessionId}});
        } else if (type == QLatin1String("assistant")) {
            QJsonObject msg = message;
            QJsonArray content;
            QString text;
            for (const QJsonValue &value : message.value(QStringLiteral("content")).toArray()) {
                QJsonObject block = value.toObject();
                const QString blockType = block.value(QStringLiteral("type")).toString();
                if (blockType == QLatin1String("redacted_thinking"))
                    continue;
                if (blockType == QLatin1String("thinking")) {
                    if (block.value(QStringLiteral("thinking")).toString().trimmed().isEmpty())
                        continue;
                    block.remove(QStringLiteral("signature"));
                }
                if (blockType == QLatin1String("text"))
                    text += block.value(QStringLiteral("text")).toString();
                content.append(block);
            }
            if (content.isEmpty())
                continue;
            msg.insert(QStringLiteral("content"), content);
            QJsonObject ev{{QStringLiteral("type"), QStringLiteral("assistant")},
                           {QStringLiteral("message"), msg},
                           {QStringLiteral("parent_tool_use_id"), QJsonValue::Null},
                           {QStringLiteral("session_id"), sessionId}};
            if (ms > 0)
                ev.insert(QStringLiteral("_ts"), ms);
            batch.events.append(ev);
            if (!text.trimmed().isEmpty())
                batch.log += QLatin1Char('\n') + text.trimmed() + QLatin1Char('\n');
        } else if (type == QLatin1String("system") &&
                   o.value(QStringLiteral("subtype")).toString() ==
                       QLatin1String("compact_boundary")) {
            batch.events.append(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("_local_notice")},
                {QStringLiteral("text"),
                 QStringLiteral("Claude Code compacted the conversation here.")}});
        }
    }
    return batch;
}

ClaudeImportBatch convert(const QString &path, qint64 fromOffset, const QString &sessionId)
{
    qint64 newOffset = fromOffset;
    const QList<QJsonObject> lines = ClaudeSessionScan::readEvents(path, fromOffset, &newOffset);
    ClaudeImportBatch batch = convertLines(lines, sessionId);
    batch.newOffset = newOffset;
    return batch;
}

QString stagedPath(const QString &configDir, const QString &workdir, const QString &sessionId)
{
    if (configDir.isEmpty() || workdir.isEmpty() || !validSessionId(sessionId))
        return QString();
    return QDir(configDir).filePath(QStringLiteral("projects/%1/%2.jsonl")
                                        .arg(ClaudeSessionScan::projectKey(
                                                 QDir(workdir).absolutePath()),
                                             sessionId));
}

bool stage(const QString &sourcePath, const QString &configDir, const QString &workdir,
           const QString &sessionId, QString *error)
{
    const QString target = stagedPath(configDir, workdir, sessionId);
    if (target.isEmpty()) {
        if (error)
            *error = QStringLiteral("no Claude config folder or session id to resume");
        return false;
    }
    const QFileInfo source(sourcePath);
    if (!source.isFile()) {
        if (error)
            *error = QStringLiteral("the Claude Code transcript %1 is gone").arg(sourcePath);
        return false;
    }
    const QFileInfo existing(target);
    if (existing.exists() &&
        existing.canonicalFilePath() == source.canonicalFilePath())
        return true;
    if (!QDir().mkpath(existing.absolutePath())) {
        if (error)
            *error = QStringLiteral("could not create %1").arg(existing.absolutePath());
        return false;
    }
    QFile in(sourcePath);
    if (!in.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("could not read %1").arg(sourcePath);
        return false;
    }
    QSaveFile out(target);
    if (!out.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("could not write %1").arg(target);
        return false;
    }
    while (!in.atEnd()) {
        const QByteArray chunk = in.read(1 << 20);
        if (chunk.isEmpty() || out.write(chunk) != chunk.size()) {
            out.cancelWriting();
            if (error)
                *error = QStringLiteral("could not copy %1").arg(sourcePath);
            return false;
        }
    }
    out.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (!out.commit()) {
        if (error)
            *error = QStringLiteral("could not write %1").arg(target);
        return false;
    }
    return true;
}

} // namespace ClaudeSessionImport
