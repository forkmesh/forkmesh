#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace forkmesh::teammates {

inline const QString kWidgetChecklist = QStringLiteral("checklist");
inline const QString kWidgetNote = QStringLiteral("note");
inline const QString kWidgetPrompt = QStringLiteral("prompt");
inline const QString kWidgetLinks = QStringLiteral("links");
inline const QString kWidgetGoal = QStringLiteral("goal");

inline const QString kWorkspaceFromYou = QStringLiteral("you");

struct WidgetItem {
    QString text;
    QString url;
    bool done = false;
};

struct Widget {
    QString id;
    QString kind;
    QString title;
    QString text;
    QList<WidgetItem> items;
    double value = 0.0;
    double target = 0.0;
    QString unit;
    QString createdBy;
    qint64 updatedAtMs = 0;
};

struct ChatEntry {
    QString from;
    QString text;
    qint64 atMs = 0;
    int sessionId = 0;
};

struct Workspace {
    QString notes;
    qint64 notesUpdatedAtMs = 0;
    QList<Widget> widgets;
    QList<ChatEntry> messages;
    QStringList handledProposals;
};

struct WidgetProposal {
    Widget widget;
    QString fingerprint;
    int sessionId = 0;
    bool updatesExisting = false;
};

constexpr int kWorkspaceMaxMessages = 200;
constexpr int kWorkspaceMaxWidgets = 40;
constexpr int kWorkspaceMaxItems = 60;
constexpr int kWorkspaceMaxNotes = 64 * 1024;
constexpr int kWorkspaceMaxHandled = 400;

QStringList widgetKinds();
QString widgetKindLabel(const QString &kind);
QString widgetKindIcon(const QString &kind);
QString newWidgetId();
Widget newWidget(const QString &kind, const QString &title = QString());
bool normalizeWidget(Widget *widget);
QString safeUrl(const QString &url);
double widgetProgress(const Widget &widget);
int openChecklistItems(const Widget &widget);

QJsonObject widgetToJson(const Widget &widget);
Widget widgetFromJson(const QJsonObject &object);
QJsonObject workspaceToJson(const Workspace &workspace);
Workspace workspaceFromJson(const QJsonObject &object);

void appendMessage(Workspace *workspace, ChatEntry entry);
bool upsertWidget(Workspace *workspace, Widget widget);
bool removeWidget(Workspace *workspace, const QString &widgetId);
void markProposalHandled(Workspace *workspace, const QString &fingerprint);

QList<Widget> widgetBlocksFromOutput(const QString &output);
QString proposalFingerprint(const Widget &widget);
QList<WidgetProposal> pendingProposals(const Workspace &workspace,
                                       const QString &output, int sessionId);

QString workspacePromptSection(const Workspace &workspace);
QString widgetContract();
QString widgetBuildAssignment(const QString &request);

class WorkspaceStore
{
public:
    explicit WorkspaceStore(QString root);

    QString root() const { return m_root; }
    QString pathFor(const QString &teammateId) const;
    Workspace load(const QString &teammateId) const;
    bool save(const QString &teammateId, const Workspace &workspace) const;

private:
    QString m_root;
};

} // namespace forkmesh::teammates
