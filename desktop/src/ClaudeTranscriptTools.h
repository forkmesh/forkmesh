#pragma once

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QVector>

namespace forkmesh::transcripttools {

bool isSubagentTool(const QString &name);
bool isMcpTool(const QString &name);
QString mcpServer(const QString &name);
QString mcpToolName(const QString &name);
bool isWindowTool(const QString &name);
bool isShowWindowTool(const QString &name);

QString displayName(const QString &name, const QJsonObject &input);
QString subtitle(const QString &name, const QJsonObject &input);
bool bodyHidden(const QString &name);
QString bodyMarkdown(const QString &name, const QJsonObject &input);
bool summarizeResult(const QString &name);
bool suppressResult(const QString &name);
bool isFileEditTool(const QString &name);

struct DiffLine {
    QChar kind;
    QString text;
};
QVector<DiffLine> lineDiff(const QString &oldText, const QString &newText);

struct EditStats {
    int added = 0;
    int removed = 0;
    bool known = false;
};
EditStats editStats(const QString &name, const QJsonObject &input);

QString toolResultText(const QJsonValue &content, int *imageCount = nullptr);
bool isServerToolResultBlock(const QString &blockType);
QString serverToolResultText(const QJsonObject &block, bool *isError = nullptr);

QString parentToolUseId(const QJsonObject &event);
QString systemNotice(const QJsonObject &event);
QString resultTokenSummary(const QJsonObject &resultEvent);

struct TaskNotification {
    QString toolUseId;
    QString status;
    QString summary;
    QString result;
    QString event;
};
QVector<TaskNotification> taskNotifications(const QString &text);
QString agentReport(const QString &resultText);
bool isAgentLaunchResult(const QString &resultText);
QString reportSummary(const QString &report, int limit = 420);
QString plainSummary(const QString &markdown, int limit = 160);

struct WindowResult {
    QString windowId;
    QString title;
    bool open = false;
    bool answered = false;
    QString button;
    QString input;
    bool browser = false;
    bool external = false;
    QString url;
    QString pageTitle;
    bool page = false;
    qint64 totalChars = 0;
    int consoleErrors = 0;
    bool screenshot = false;
    bool widget = false;
    int events = 0;
    QString action;
    QString suggestion;
    int sessionId = 0;
    QString message;
    bool valid = false;
};
WindowResult parseWindowResult(const QString &text);

QStringList commandPrograms(const QString &command);
QString commandIconKey(const QString &program);
QString commandIconLabel(const QString &key);
struct CommandIcon {
    QString key;
    QStringList programs;
};
QVector<CommandIcon> commandIcons(const QString &command);
QString windowResultSummary(const WindowResult &result);

int rawLogLineStart(const QString &raw, const QJsonObject &event);

class OutputTokenCounter
{
public:
    qint64 add(const QJsonObject &message);
    void clear() { m_seen.clear(); }

private:
    QHash<QString, qint64> m_seen;
};

}
