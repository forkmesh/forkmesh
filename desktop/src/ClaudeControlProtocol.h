#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>

namespace forkmesh::claudecontrol {

inline const QString kAllowOnce = QStringLiteral("Allow once");
inline const QString kAllowAndRemember = QStringLiteral("Allow and remember");
inline const QString kDeny = QStringLiteral("Deny");
inline const QString kApproveAcceptEdits = QStringLiteral("Approve and accept edits");
inline const QString kApproveAskEdits = QStringLiteral("Approve and ask before edits");
inline const QString kKeepPlanning = QStringLiteral("Keep planning");
inline const QString kPromptKeyField = QStringLiteral("_forkmesh_prompt_key");

enum class PromptKind { None, Permission, Question, Plan, Elicitation, Dialog, Unanswerable };

struct Prompt {
    PromptKind kind = PromptKind::None;
    QString requestId;
    QString key;
    QString toolName;
    QJsonObject card;
};

struct Answer {
    bool valid = false;
    QJsonObject response;
    QString permissionMode;
    QString followUp;
};

struct ElicitationField {
    QString name;
    QString label;
    QString description;
    QString type;
    QStringList choices;
    QStringList choiceLabels;
    bool required = false;
    bool secret = false;
    QJsonValue defaultValue;
};

QStringList supportedDialogKinds();
QJsonObject initializeRequest(const QString &title);
QJsonObject request(const QString &requestId, const QJsonObject &body);
QJsonObject success(const QString &requestId, const QJsonObject &response = {});
QJsonObject failure(const QString &requestId, const QString &message);

bool isControlRequest(const QJsonObject &event);
bool isControlCancel(const QJsonObject &event);
QString requestIdOf(const QJsonObject &event);
QString subtypeOf(const QJsonObject &event);

Prompt promptFor(const QJsonObject &event);
QJsonObject cardEvent(const Prompt &prompt);
Answer answerFor(const QJsonObject &event, const QString &answer);

QList<ElicitationField> elicitationFields(const QJsonObject &request);
QJsonObject elicitationContent(const QList<ElicitationField> &fields,
                               const QMap<QString, QString> &values,
                               QString *error = nullptr);
QJsonObject elicitationResponse(const QString &action, const QJsonObject &content = {});

QString titleFromEvent(const QJsonObject &event);
QString cleanTitle(const QString &title);
QString permissionModeLabel(const QString &modeId);

} // namespace forkmesh::claudecontrol
