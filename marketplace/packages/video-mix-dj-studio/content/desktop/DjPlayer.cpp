#include "DjPlayer.h"
#include "DjScripts.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#ifdef FORKMESH_HAVE_WEBENGINE
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineSettings>
#include <QWebEngineView>
#endif

#include <cmath>

namespace forkmesh::djstudio {

namespace {

QString number(double value)
{
    return std::isfinite(value) ? QString::number(value, 'f', 4) : QStringLiteral("0");
}

QString quoted(const QString &text)
{
    return QString::fromUtf8(QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact)).mid(1).chopped(1);
}

#ifdef FORKMESH_HAVE_WEBENGINE
class DjPage : public QWebEnginePage
{
public:
    DjPage(QWebEngineProfile *profile, const QUrl &origin, QObject *parent)
        : QWebEnginePage(profile, parent), m_origin(origin)
    {
    }

    std::function<void(const QString &)> onMessage;

protected:
    void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel, const QString &message, int,
                                  const QString &source) override
    {
        if (!message.startsWith(QLatin1String("fmdj:")) || source.contains(QLatin1String("youtube")))
            return;
        if (onMessage)
            onMessage(message.mid(5));
    }

    bool acceptNavigationRequest(const QUrl &url, NavigationType type, bool isMainFrame) override
    {
        const QString scheme = url.scheme();
        if (!isMainFrame)
            return scheme == QLatin1String("https") || scheme == QLatin1String("about") ||
                   scheme == QLatin1String("blob");
        if (type == NavigationTypeLinkClicked)
            return false;
        return scheme == QLatin1String("data") || scheme == QLatin1String("about") ||
               url.host() == m_origin.host();
    }

    QWebEnginePage *createWindow(WebWindowType) override { return nullptr; }

private:
    QUrl m_origin;
};
#endif

}

struct DjPlayer::Private {
    QUrl origin;
    QPointer<QWidget> holder;
    bool apiReady = false;
    int scripts = 0;
    QStringList queued;
    bool flushing = false;
    QHash<int, QString> pendingLoads;
    QHash<int, QString> lastLoads;
    QHash<int, DjDeckSound> sounds;
    QVector<DjVideoSlot> lastSlots;
    double samplerLevel = 0.8;
#ifdef FORKMESH_HAVE_WEBENGINE
    QWebEngineProfile *profile = nullptr;
    DjPage *page = nullptr;
    QWebEngineView *view = nullptr;
#endif
    DjPlayer *owner = nullptr;

    void run(const QString &script)
    {
#ifdef FORKMESH_HAVE_WEBENGINE
        if (!apiReady || !page)
            return;
        queued.append(script);
        if (flushing)
            return;
        flushing = true;
        QTimer::singleShot(0, holder, [this]() {
            flushing = false;
            if (queued.isEmpty() || !page)
                return;
            const QString batch = queued.join(QLatin1Char(';'));
            queued.clear();
            ++scripts;
            page->runJavaScript(batch);
        });
#else
        Q_UNUSED(script);
#endif
    }

    void loadPage()
    {
#ifdef FORKMESH_HAVE_WEBENGINE
        apiReady = false;
        queued.clear();
        sounds.clear();
        lastSlots.clear();
        page->setHtml(hostPage(), origin);
#endif
    }

    void receive(const QString &text)
    {
        const QJsonObject message = QJsonDocument::fromJson(text.toUtf8()).object();
        const QString type = message.value(QStringLiteral("type")).toString();
        if (type.isEmpty())
            return;
        if (type == QLatin1String("api")) {
            apiReady = true;
            const auto loads = pendingLoads;
            pendingLoads.clear();
            for (const QString &script : loads)
                run(script);
            run(QStringLiteral("fmLevel(%1)").arg(number(samplerLevel)));
        }
        if (owner->onEvent)
            owner->onEvent(type, message);
    }
};

DjPlayer::DjPlayer(const QUrl &origin, QWidget *parent) : d(std::make_unique<Private>())
{
    d->owner = this;
    d->origin = origin;
#ifdef FORKMESH_HAVE_WEBENGINE
    d->view = new QWebEngineView(parent);
    d->holder = d->view;
    d->profile = new QWebEngineProfile(d->view);
    QString agent = d->profile->httpUserAgent();
    agent.remove(QRegularExpression(QStringLiteral("\\s*QtWebEngine/\\S+")));
    d->profile->setHttpUserAgent(agent);
    d->page = new DjPage(d->profile, origin, d->view);
    QWebEngineSettings *settings = d->page->settings();
    settings->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture, false);
    settings->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, false);
    settings->setAttribute(QWebEngineSettings::ShowScrollBars, false);
    settings->setAttribute(QWebEngineSettings::FocusOnNavigationEnabled, false);
    settings->setAttribute(QWebEngineSettings::PluginsEnabled, false);
    settings->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, false);
    QWebEngineScript script;
    script.setName(QStringLiteral("fmdj-frame"));
    script.setSourceCode(frameScript());
    script.setInjectionPoint(QWebEngineScript::DocumentCreation);
    script.setRunsOnSubFrames(true);
    script.setWorldId(QWebEngineScript::MainWorld);
    d->page->scripts().insert(script);
    d->page->setBackgroundColor(Qt::black);
    d->page->onMessage = [this](const QString &text) { d->receive(text); };
    QObject::connect(d->page, &QWebEnginePage::renderProcessTerminated, d->view, [this]() {
        d->pendingLoads = d->lastLoads;
        d->loadPage();
        if (onReload)
            onReload();
    });
    d->view->setPage(d->page);
    d->view->setFocusPolicy(Qt::NoFocus);
    d->view->setContextMenuPolicy(Qt::NoContextMenu);
    d->loadPage();
#else
    auto *label = new QLabel(parent);
    label->setAlignment(Qt::AlignCenter);
    label->setWordWrap(true);
    label->setText(QStringLiteral("No video: this build of ForkMesh has no Qt WebEngine."));
    label->setStyleSheet(QStringLiteral("color:#8a93a3;background:#000;font-size:11px;"));
    d->holder = label;
#endif
}

DjPlayer::~DjPlayer() = default;

bool DjPlayer::available()
{
#ifdef FORKMESH_HAVE_WEBENGINE
    return true;
#else
    return false;
#endif
}

QWidget *DjPlayer::view() const { return d->holder; }

bool DjPlayer::ready() const { return d->apiReady; }

int DjPlayer::scriptsSent() const { return d->scripts; }

void DjPlayer::load(int deck, const QString &id, double start, double rate, bool play)
{
    const QString script = QStringLiteral("fmLoad(%1,%2,%3,%4,%5)")
                               .arg(deck)
                               .arg(quoted(id), number(start), number(rate),
                                    play ? QStringLiteral("1") : QStringLiteral("0"));
    d->lastLoads.insert(deck, script);
    d->sounds.remove(deck);
    if (d->apiReady)
        d->run(script);
    else
        d->pendingLoads.insert(deck, script);
}

void DjPlayer::eject(int deck)
{
    d->lastLoads.remove(deck);
    d->pendingLoads.remove(deck);
    d->sounds.remove(deck);
    d->run(QStringLiteral("fmEject(%1)").arg(deck));
}

void DjPlayer::play(int deck)
{
    if (d->lastLoads.contains(deck))
        d->lastLoads[deck].replace(QRegularExpression(QStringLiteral(",[01]\\)$")), QStringLiteral(",1)"));
    d->run(QStringLiteral("fmCall(%1,'playVideo')").arg(deck));
}

void DjPlayer::pause(int deck) { d->run(QStringLiteral("fmCall(%1,'pauseVideo')").arg(deck)); }

void DjPlayer::seek(int deck, double seconds)
{
    d->run(QStringLiteral("fmCall(%1,'seekTo',%2,true)").arg(deck).arg(number(std::max(0.0, seconds))));
}

void DjPlayer::setRate(int deck, double rate)
{
    d->run(QStringLiteral("fmSet(%1,{rate:%2})").arg(deck).arg(number(rate)));
}

void DjPlayer::setSound(int deck, const DjDeckSound &sound)
{
    const auto known = d->sounds.constFind(deck);
    if (known != d->sounds.constEnd() && *known == sound)
        return;
    d->sounds.insert(deck, sound);
    d->run(QStringLiteral("fmSet(%1,{gain:%2,trim:%3,eq:[%4,%5,%6],filter:%7,fx:[%8,%9,%10],beat:%11,keylock:%12})")
               .arg(deck)
               .arg(number(sound.gain), number(sound.trim), number(sound.eq[0]), number(sound.eq[1]),
                    number(sound.eq[2]), number(sound.filter), number(sound.fx[0]), number(sound.fx[1]),
                    number(sound.fx[2]))
               .arg(number(sound.beat), sound.keylock ? QStringLiteral("1") : QStringLiteral("0")));
}

void DjPlayer::scratch(int deck, std::optional<double> target)
{
    d->run(QStringLiteral("fmSet(%1,{scratch:%2})")
               .arg(deck)
               .arg(target ? number(*target) : QStringLiteral("null")));
}

void DjPlayer::brake(int deck)
{
    d->run(QStringLiteral("fmSet(%1,{brake:Date.now()})").arg(deck));
}

void DjPlayer::backspin(int deck)
{
    d->run(QStringLiteral("fmSet(%1,{spin:Date.now()})").arg(deck));
}

void DjPlayer::pad(const QString &name) { d->run(QStringLiteral("fmPad(%1)").arg(quoted(name))); }

void DjPlayer::setSamplerLevel(double level)
{
    d->samplerLevel = level;
    d->run(QStringLiteral("fmLevel(%1)").arg(number(level)));
}

void DjPlayer::layout(const QVector<DjVideoSlot> &slots)
{
    if (slots == d->lastSlots)
        return;
    d->lastSlots = slots;
    QStringList rows;
    for (const DjVideoSlot &slot : slots) {
        rows.append(QStringLiteral("[%1,%2,%3,%4,%5,%6]")
                        .arg(number(slot.rect.x()), number(slot.rect.y()), number(slot.rect.width()),
                             number(slot.rect.height()), number(slot.opacity))
                        .arg(slot.z));
    }
    d->run(QStringLiteral("fmLayout([%1])").arg(rows.join(QLatin1Char(','))));
}

void DjPlayer::reset()
{
    d->pendingLoads.clear();
    d->lastLoads.clear();
    d->loadPage();
}

}
