#pragma once

#include <QJsonArray>
#include <QString>
#include <QUrl>
#include <QVariant>

#include <functional>

class QObject;
class QWidget;

namespace forkmesh::web {

inline const QString kModuleEntrypoint = QStringLiteral("core:web-engine");

enum class World { Main, Application };

struct ProfileOptions {
    QString storageName;
    QString storagePath;
    QString cachePath;
    bool persistentCookies = false;
    bool ungoogled = false;
    std::function<QString(const QString &defaultAgent)> userAgent;
    std::function<bool(const QUrl &url)> requestAllowed;
    std::function<QString()> downloadDirectory;
    std::function<QString(const QString &suggestedName)> downloadPath;
    std::function<void(const QString &path, bool ok)> downloadFinished;
};

struct PageOptions {
    std::function<bool(const QUrl &url, bool linkClicked, bool mainFrame)> acceptNavigation;
    std::function<bool(const QUrl &url)> requestAllowed;
    bool newWindowsInPlace = false;
    int consoleLimit = 0;
    std::function<void(const QString &message)> onConsole;
    bool restricted = false;
    QString documentScriptName;
    QString documentScript;
    bool clipboard = false;
    std::function<void(const QUrl &url)> onNewWindow;
    std::function<QString()> printPath;
    std::function<void(const QString &path, bool ok)> printFinished;
};

class View
{
public:
    virtual ~View();

    virtual QWidget *widget() = 0;
    virtual void load(const QUrl &url) = 0;
    virtual void setHtml(const QString &html, const QUrl &baseUrl) = 0;
    virtual void back() = 0;
    virtual void forward() = 0;
    virtual void reload() = 0;
    virtual bool canGoBack() const = 0;
    virtual bool canGoForward() const = 0;
    virtual QUrl url() const = 0;
    virtual QString title() const = 0;
    virtual QJsonArray consoleMessages() const = 0;
    virtual void runJavaScript(const QString &script, World world,
                               std::function<void(const QVariant &)> done) = 0;
    virtual void onUrlChanged(QObject *context, std::function<void(const QUrl &)> handler) = 0;
    virtual void onTitleChanged(QObject *context, std::function<void(const QString &)> handler) = 0;
    virtual void onLoadFinished(QObject *context, std::function<void(bool)> handler) = 0;
};

struct Engine {
    std::function<QObject *(const ProfileOptions &options, QObject *parent)> createProfile;
    std::function<View *(QObject *profile, const PageOptions &options, QWidget *parent)> createView;
    std::function<bool(QObject *profile, const QUrl &origin, const QString &name,
                       const QString &value, bool httpOnly)>
        setCookie;
};

bool registerEngine(const Engine &engine);
void setEngineModule(std::function<bool()> present, std::function<bool()> load);

bool built();
bool available();
bool loaded();

QObject *createProfile(const ProfileOptions &options, QObject *parent = nullptr);
View *createView(QObject *profile, const PageOptions &options, QWidget *parent);
View *viewOf(QWidget *widget);
bool setCookie(QObject *profile, const QUrl &origin, const QString &name, const QString &value,
               bool httpOnly);

}
