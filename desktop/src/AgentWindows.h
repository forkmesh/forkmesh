#pragma once

#include <QHash>
#include <QJsonValue>
#include <QList>
#include <QJsonObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <functional>
#include <memory>

#include "AgentIntake.h"

class QWidget;

namespace forkmesh::agentwindows {

inline const QString MethodPrefix = QStringLiteral("windows.");
inline const QString ShowMethod = QStringLiteral("windows.show");
inline const QString ReadMethod = QStringLiteral("windows.read");
inline const QString CloseMethod = QStringLiteral("windows.close");
inline const QString BrowseMethod = QStringLiteral("windows.browse");
inline const QString CaptureMethod = QStringLiteral("windows.capture");
inline const QString ActMethod = QStringLiteral("windows.act");
inline const QString WidgetMethod = QStringLiteral("windows.widget");
inline const QString SuggestionKind = QStringLiteral("suggestion");
inline const QString WidgetKind = QStringLiteral("widget");

constexpr int kMaxTitleChars = 120;
constexpr int kMaxBodyChars = 40000;
constexpr int kMaxButtons = 6;
constexpr int kMaxButtonChars = 40;
constexpr int kMaxLabelChars = 200;
constexpr int kMaxInputChars = 20000;
constexpr int kMaxOpenPerSession = 8;
constexpr int kMaxRecordsPerSession = 32;
constexpr int kNewWindowLimit = 30;
constexpr qint64 kNewWindowWindowMs = 10 * 60 * 1000;
constexpr int kMinWidth = 240;
constexpr int kMaxWidth = 1400;
constexpr int kMinHeight = 160;
constexpr int kMaxHeight = 1000;
constexpr int kDefaultWidth = 520;
constexpr int kDefaultHeight = 380;
constexpr qint64 kMaxImageBytes = 16 * 1024 * 1024;
constexpr int kMaxUrlChars = 2048;
constexpr int kDefaultBrowserWidth = 900;
constexpr int kDefaultBrowserHeight = 640;
constexpr int kDefaultPageChars = 20000;
constexpr int kMaxPageChars = 100000;
constexpr int kMaxPageOffset = 50000000;
constexpr int kMaxPageLinks = 200;
constexpr int kMaxConsoleMessages = 200;
constexpr int kMaxSelectionChars = 5000;
constexpr int kCaptureTimeoutMs = 8000;
constexpr int kMaxShotWidth = 1280;
constexpr int kMaxWidgetHtmlChars = 50000;
constexpr int kMaxWidgetDataChars = 16000;
constexpr int kMaxWidgetEvents = 50;
constexpr int kMaxWidgetEventChars = 4000;
constexpr int kDefaultWidgetWidth = 420;
constexpr int kDefaultWidgetHeight = 320;
constexpr int kMaxSelectorChars = 500;
constexpr int kMaxActionValueChars = 5000;
constexpr int kMaxActionWaitMs = 8000;
constexpr int kActionSettleMs = 300;
constexpr int kActionPollMs = 250;

struct ShowRequest {
    QString windowId;
    QString title;
    QString body;
    QString format = QStringLiteral("markdown");
    QStringList buttons;
    QString inputLabel;
    bool inputMultiline = false;
    int width = 0;
    int height = 0;
};

struct BrowseRequest {
    QString windowId;
    QUrl url;
    QString title;
    int width = 0;
    int height = 0;
};

struct CaptureRequest {
    QString windowId;
    int maxChars = kDefaultPageChars;
    int offset = 0;
    bool links = false;
    bool screenshot = false;
};

struct WidgetRequest {
    QString windowId;
    QString title;
    QString html;
    QJsonValue data;
    bool hasData = false;
    int width = 0;
    int height = 0;
};

struct ActRequest {
    QString windowId;
    QString action;
    QUrl url;
    QString selector;
    QString target;
    QString value;
    bool submit = false;
    bool clear = true;
    int delta = 0;
    int waitMs = 0;
};

bool windowIdValid(const QString &id);
QString mintWindowId();
bool parseShowRequest(const QJsonObject &params, ShowRequest *out, QString *error);
bool parseBrowseRequest(const QJsonObject &params, BrowseRequest *out, QString *error);
bool parseCaptureRequest(const QJsonObject &params, CaptureRequest *out, QString *error);
bool parseWidgetRequest(const QJsonObject &params, WidgetRequest *out, QString *error);
bool parseActRequest(const QJsonObject &params, ActRequest *out, QString *error);
QString pageActionScript(const ActRequest &request);
QString widgetBridgeScript(const QJsonValue &data);
QString widgetDataScript(const QJsonValue &data);
QString widgetEventMarker();
bool widgetEventFromConsole(const QString &message, QJsonValue *event);
bool widgetRequestAllowed(const QUrl &url);
QString actionSummary(const ActRequest &request);
QString pageCaptureScript(const CaptureRequest &request);
QJsonObject pageCaptureReply(const QString &windowId, const QJsonObject &page,
                             const CaptureRequest &request);
bool browserUrlAllowed(const QUrl &url);
QUrl browserUrlFromInput(const QString &text);
bool embeddedBrowserAvailable();
QString actionFromLink(const QUrl &url);
bool localResourceAllowed(const QUrl &url);
QString windowTitle(const QString &agentLabel, const QString &title);

struct WindowState {
    int sessionId = 0;
    QString agentLabel;
    QString workdir;
    ShowRequest request;
    bool browser = false;
    QString kind;
    QJsonObject extra;
    QString widgetHtml;
    QJsonValue widgetData;
    QList<QJsonValue> events;
    int droppedEvents = 0;
    QUrl url;
    QString pageTitle;
    bool shared = true;
    qint64 lastReadMs = 0;
    QPointer<QWidget> widget;
    bool open = false;
    bool answered = false;
    QString button;
    QString input;
    qint64 createdMs = 0;
};

class Registry
{
public:
    explicit Registry(QWidget *host);
    ~Registry();

    QJsonObject handle(int sessionId, const QString &agentLabel,
                       const QString &workdir, const QString &method,
                       const QJsonObject &params);
    QJsonObject show(int sessionId, const QString &agentLabel,
                     const QString &workdir, const ShowRequest &request);
    QJsonObject read(int sessionId, const QString &windowId);
    QJsonObject close(int sessionId, const QString &windowId);
    QJsonObject browse(int sessionId, const QString &agentLabel,
                       const BrowseRequest &request);
    void setPageState(const QString &windowId, const QUrl &url, const QString &title);
    void capture(int sessionId, const QJsonObject &params,
                 std::function<void(const QJsonObject &)> reply);
    void act(int sessionId, const QJsonObject &params,
             std::function<void(const QJsonObject &)> reply);
    QJsonObject widget(int sessionId, const QString &agentLabel, const WidgetRequest &request);
    void addWidgetEvent(const QString &windowId, const QJsonValue &event);
    void setKind(const QString &windowId, const QString &kind);
    void setExtra(const QString &windowId, const QJsonObject &extra);
    void setAnswerHandler(std::function<void(const QString &windowId)> handler)
    {
        m_answerHandler = std::move(handler);
    }
    void setShared(const QString &windowId, bool shared);
    void setExternalOpener(std::function<bool(const QUrl &)> opener)
    {
        m_externalOpener = std::move(opener);
    }
    bool raise(const QString &windowId);
    void answer(const QString &windowId, const QString &button, const QString &input);
    void markClosed(const QString &windowId);
    int openCount(int sessionId) const;
    const WindowState *state(const QString &windowId) const;

private:
    QJsonObject stateReply(const QString &windowId, const WindowState &state) const;
    QString claimWindow(int sessionId, const QString &requested, QJsonObject *error);
    QWidget *frameFor(const QString &windowId, const WindowState &state);
    QWidget *buildWidget(const QString &windowId, const WindowState &state);
    QWidget *buildBrowser(const QString &windowId, const WindowState &state);
    QWidget *buildWidgetWindow(const QString &windowId, const WindowState &state);
    QObject *widgetProfile();
    void noteAgentUse(const QString &windowId, const QString &what);
    QObject *browserProfile();
    void place(QWidget *widget, const WindowState &state);
    void prune(int sessionId);

    QWidget *m_host = nullptr;
    std::unique_ptr<QObject> m_context;
    QObject *m_browserProfile = nullptr;
    QObject *m_widgetProfile = nullptr;
    std::function<void(const QString &)> m_answerHandler;
    std::function<bool(const QUrl &)> m_externalOpener;
    QHash<QString, WindowState> m_windows;
    forkmesh::agentintake::RateLimiter m_newWindows{kNewWindowLimit, kNewWindowWindowMs};
    int m_cascade = 0;
};

}
