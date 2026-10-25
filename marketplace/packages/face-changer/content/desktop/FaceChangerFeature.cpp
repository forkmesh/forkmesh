#include "FaceChangerFeature.h"

#include "AppPaths.h"
#include "FaceStudioPage.h"

#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QMimeDatabase>
#include <QMutexLocker>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#ifdef FORKMESH_HAVE_WEBENGINE
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineView>
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
#include <QWebEnginePermission>
#endif
#endif

namespace forkmesh::facechanger {

namespace {

constexpr int kFrameMs = 33;
constexpr qint64 kMaxPhotoBytes = 20 * 1024 * 1024;

QString quoted(const QString &text)
{
    return QString::fromUtf8(QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact))
        .mid(1)
        .chopped(1);
}

QLabel *mutedLabel(const QString &value)
{
    auto *label = new QLabel(value);
    label->setObjectName(QStringLiteral("mutedLabel"));
    label->setWordWrap(true);
    return label;
}

QFrame *card(const QString &title, QVBoxLayout **body)
{
    auto *frame = new QFrame;
    frame->setObjectName(QStringLiteral("insightsCard"));
    frame->setStyleSheet(QStringLiteral("QLabel { background: transparent; }"));
    auto *layout = new QVBoxLayout(frame);
    layout->setContentsMargins(14, 12, 14, 14);
    layout->setSpacing(8);
    auto *heading = new QLabel(title);
    heading->setStyleSheet(QStringLiteral("font-weight: 600; font-size: 14px;"));
    layout->addWidget(heading);
    *body = layout;
    return frame;
}

QString setupCommandText()
{
    QStringList parts = cameraSetupCommand();
    parts.replace(0, QStringLiteral("sudo"));
    for (QString &part : parts) {
        if (part.contains(QLatin1Char(' ')))
            part = part.section(QLatin1Char('='), 0, 0) + QStringLiteral("=\"") +
                   part.section(QLatin1Char('='), 1) + QLatin1Char('"');
    }
    return parts.join(QLatin1Char(' '));
}

#ifdef FORKMESH_HAVE_WEBENGINE
class FacePage : public QWebEnginePage
{
public:
    FacePage(QWebEngineProfile *profile, const QUrl &origin, QObject *parent)
        : QWebEnginePage(profile, parent), m_origin(origin)
    {
    }

    std::function<void(const QJsonObject &)> onMessage;

protected:
    void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel, const QString &message, int,
                                  const QString &) override
    {
        if (!onMessage || !message.startsWith(faceStudioMessagePrefix()))
            return;
        const QJsonObject object =
            QJsonDocument::fromJson(message.mid(faceStudioMessagePrefix().size()).toUtf8()).object();
        if (!object.isEmpty())
            onMessage(object);
    }

    bool acceptNavigationRequest(const QUrl &url, NavigationType type, bool isMainFrame) override
    {
        if (type == NavigationTypeLinkClicked)
            return false;
        if (!isMainFrame)
            return false;
        return url.scheme() == QLatin1String("data") || url.scheme() == QLatin1String("about") ||
               (url.scheme() == m_origin.scheme() && url.host() == m_origin.host() &&
                url.port() == m_origin.port());
    }

    QWebEnginePage *createWindow(WebWindowType) override { return nullptr; }

private:
    QUrl m_origin;
};
#endif

}

FaceChangerFeature::FaceChangerFeature(const forkmesh::market::NativePackageHost &host,
                                       QWidget *parent)
    : forkmesh::market::NativePackageWidget(parent),
      m_packageId(host.packageId.isEmpty() ? QStringLiteral("face-changer") : host.packageId),
      m_openWebPage(host.openWebPage),
      m_origin(engineOrigin(host.webPageUrl ? host.webPageUrl(QStringLiteral("/")) : QString())),
      m_feed(std::make_shared<CameraFeed>())
{
    setObjectName(QStringLiteral("faceChangerSection"));
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);
    auto *page = new QWidget;
    scroll->setWidget(page);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(20, 18, 20, 20);
    layout->setSpacing(12);

    auto *heading = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("Face Changer"));
    title->setObjectName(QStringLiteral("pageTitle"));
    heading->addWidget(title);
    heading->addWidget(mutedLabel(QStringLiteral(
        "Wear a stand-in face, a new outfit and another background on any call or meeting app. "
        "The picture is made on this computer and never uploaded.")), 1);
    auto *webButton = new QPushButton(QStringLiteral("Open in browser"));
    webButton->setObjectName(QStringLiteral("ghostButton"));
    webButton->setToolTip(QStringLiteral("Record clips and take snapshots on the web page"));
    webButton->setEnabled(bool(m_openWebPage));
    heading->addWidget(webButton);
    layout->addLayout(heading);

    m_status = mutedLabel(QString());
    layout->addWidget(m_status);

    m_tick = new QTimer(this);
    m_tick->setInterval(kFrameMs);
    m_tick->setTimerType(Qt::PreciseTimer);
    connect(m_tick, &QTimer::timeout, this, [this]() { takeFrame(); });

    QVBoxLayout *previewBody = nullptr;
    layout->addWidget(card(QStringLiteral("Your camera"), &previewBody));
    buildEngine();
    if (m_view)
        previewBody->addWidget(m_view, 1);
    auto *startRow = new QHBoxLayout;
    m_startButton = new QPushButton(QStringLiteral("Start camera"));
    m_startButton->setObjectName(QStringLiteral("primaryButton"));
    startRow->addWidget(m_startButton);
    startRow->addWidget(mutedLabel(QStringLiteral("Camera")));
    m_camera = new QComboBox;
    m_camera->setMinimumWidth(220);
    m_camera->addItem(QStringLiteral("Default camera"), QString());
    startRow->addWidget(m_camera, 1);
    previewBody->addLayout(startRow);

    QVBoxLayout *lookBody = nullptr;
    layout->addWidget(card(QStringLiteral("Look"), &lookBody));
    auto *grid = new QGridLayout;
    m_face = new QComboBox;
    m_outfit = new QComboBox;
    m_background = new QComboBox;
    auto *facePhoto = new QPushButton(QStringLiteral("Photo…"));
    auto *backgroundPhoto = new QPushButton(QStringLiteral("Photo…"));
    facePhoto->setObjectName(QStringLiteral("ghostButton"));
    backgroundPhoto->setObjectName(QStringLiteral("ghostButton"));
    facePhoto->setToolTip(QStringLiteral("Wear a face from a front-facing photo"));
    backgroundPhoto->setToolTip(QStringLiteral("Use your own photo as the background"));
    grid->addWidget(mutedLabel(QStringLiteral("Face")), 0, 0);
    grid->addWidget(m_face, 0, 1);
    grid->addWidget(facePhoto, 0, 2);
    grid->addWidget(mutedLabel(QStringLiteral("Outfit")), 1, 0);
    grid->addWidget(m_outfit, 1, 1, 1, 2);
    grid->addWidget(mutedLabel(QStringLiteral("Background")), 2, 0);
    grid->addWidget(m_background, 2, 1);
    grid->addWidget(backgroundPhoto, 2, 2);
    grid->setColumnStretch(1, 1);
    lookBody->addLayout(grid);
    lookBody->addWidget(mutedLabel(QStringLiteral(
        "The face, outfit and background are the ones your ForkMesh calls use.")));

    QVBoxLayout *outputBody = nullptr;
    layout->addWidget(card(QStringLiteral("Use it in a call or meeting"), &outputBody));
    m_output = mutedLabel(QString());
    m_output->setTextFormat(Qt::PlainText);
    outputBody->addWidget(m_output);
    m_command = new QLabel(setupCommandText());
    m_command->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_command->setStyleSheet(QStringLiteral("font-family: monospace;"));
    m_command->setWordWrap(true);
    outputBody->addWidget(m_command);
    auto *setupRow = new QHBoxLayout;
    m_setupButton = new QPushButton(QStringLiteral("Set up %1").arg(cameraLabel()));
    m_setupButton->setObjectName(QStringLiteral("primaryButton"));
    m_rescanButton = new QPushButton(QStringLiteral("Check again"));
    m_rescanButton->setObjectName(QStringLiteral("ghostButton"));
    setupRow->addWidget(m_setupButton);
    setupRow->addWidget(m_rescanButton);
    setupRow->addStretch();
    outputBody->addLayout(setupRow);
    outputBody->addWidget(mutedLabel(QStringLiteral(
        "To change your voice too, open Voice Changer: it adds a \"ForkMesh Voice\" "
        "microphone the same apps can pick.")));

    layout->addWidget(mutedLabel(QStringLiteral(
        "Face Changer is for play, privacy and performance. Pretending to be a real person to "
        "mislead or defraud anyone is not allowed and may be illegal where you live.")));

    connect(m_startButton, &QPushButton::clicked, this, [this]() { toggleCamera(); });
    connect(webButton, &QPushButton::clicked, this, [this]() {
        if (m_openWebPage)
            m_openWebPage(QStringLiteral("/dashboard/face-changer"));
    });
    connect(m_camera, QOverload<int>::of(&QComboBox::activated), this, [this](int) {
        const QString id = m_camera->currentData().toString();
        setSetting(QStringLiteral("camera"), id);
        if (m_live) {
            run(QStringLiteral("fmStop()"));
            m_starting = true;
            run(QStringLiteral("fmStart(%1)").arg(quoted(id)));
            updateState();
        }
    });
    connect(m_face, QOverload<int>::of(&QComboBox::activated), this, [this](int) {
        const QString id = m_face->currentData().toString();
        if (id == m_customFaceId && !m_customFace) {
            choosePhoto(true);
            return;
        }
        run(QStringLiteral("fmFace(%1)").arg(quoted(id)));
    });
    connect(m_background, QOverload<int>::of(&QComboBox::activated), this, [this](int) {
        const QString id = m_background->currentData().toString();
        if (id == m_customBackgroundId && !m_customBackground) {
            choosePhoto(false);
            return;
        }
        run(QStringLiteral("fmBackground(%1)").arg(quoted(id)));
    });
    connect(m_outfit, QOverload<int>::of(&QComboBox::activated), this, [this](int) {
        run(QStringLiteral("fmOutfit(%1)").arg(quoted(m_outfit->currentData().toString())));
    });
    connect(facePhoto, &QPushButton::clicked, this, [this]() { choosePhoto(true); });
    connect(backgroundPhoto, &QPushButton::clicked, this, [this]() { choosePhoto(false); });
    connect(m_setupButton, &QPushButton::clicked, this, [this]() { setUpCamera(); });
    connect(m_rescanButton, &QPushButton::clicked, this, [this]() { refresh(); });

    if (!m_view)
        setStatus(QStringLiteral("This build of ForkMesh has no web engine, so the face changer "
                                 "opens in your browser instead."));
    else
        setStatus(QStringLiteral("Loading the face changer…"));
    updateState();
}

FaceChangerFeature::~FaceChangerFeature()
{
    if (m_tick)
        m_tick->stop();
#ifdef FORKMESH_HAVE_WEBENGINE
    if (auto *page = static_cast<FacePage *>(m_page))
        page->onMessage = nullptr;
#endif
    m_work.waitForFinished();
    closeOutput();
    if (m_setup) {
        m_setup->disconnect();
        if (m_setup->state() != QProcess::NotRunning)
            m_setup->waitForFinished(100);
    }
}

QUrl FaceChangerFeature::engineOrigin(const QString &hostUrl)
{
    QUrl url(hostUrl);
    if (!url.isValid() || url.host().isEmpty() ||
        (url.scheme() != QLatin1String("https") && url.scheme() != QLatin1String("http")))
        url = QUrl(QStringLiteral("https://app.forkmesh.com/"));
    url.setUserInfo(QString());
    url.setPath(QStringLiteral("/"));
    url.setQuery(QString());
    url.setFragment(QString());
    return url;
}

void FaceChangerFeature::refresh()
{
    if (m_live)
        openOutput();
    updateState();
}

QString FaceChangerFeature::setting(const QString &key, const QString &fallback) const
{
    return QSettings()
        .value(QStringLiteral("market/packages/%1/data/face.%2").arg(m_packageId, key), fallback)
        .toString();
}

void FaceChangerFeature::setSetting(const QString &key, const QString &value)
{
    QSettings().setValue(QStringLiteral("market/packages/%1/data/face.%2").arg(m_packageId, key),
                         value);
}

void FaceChangerFeature::buildEngine()
{
#ifdef FORKMESH_HAVE_WEBENGINE
    m_view = new QWebEngineView;
    m_view->setMinimumHeight(320);
    m_view->setFocusPolicy(Qt::NoFocus);
    m_view->setContextMenuPolicy(Qt::NoContextMenu);
    auto *profile = new QWebEngineProfile(QStringLiteral("forkmesh-face-changer"), m_view);
    profile->setPersistentStoragePath(
        forkmesh::paths::pluginDataDir(m_packageId) + QStringLiteral("/web"));
    profile->setCachePath(forkmesh::paths::cacheRoot() + QStringLiteral("/face-changer"));
    QString agent = profile->httpUserAgent();
    agent.remove(QRegularExpression(QStringLiteral("\\s*QtWebEngine/\\S+")));
    profile->setHttpUserAgent(agent);
    auto *page = new FacePage(profile, m_origin, m_view);
    QWebEngineSettings *settings = page->settings();
    settings->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture, false);
    settings->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, false);
    settings->setAttribute(QWebEngineSettings::ShowScrollBars, false);
    settings->setAttribute(QWebEngineSettings::FocusOnNavigationEnabled, false);
    settings->setAttribute(QWebEngineSettings::PluginsEnabled, false);
    settings->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, false);
    settings->setAttribute(QWebEngineSettings::WebGLEnabled, true);
    page->setBackgroundColor(QColor(0x0b, 0x0d, 0x10));
    const QUrl origin = m_origin;
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    connect(page, &QWebEnginePage::permissionRequested, page,
            [origin](QWebEnginePermission permission) {
        const QUrl from = permission.origin();
        if (permission.permissionType() == QWebEnginePermission::PermissionType::MediaVideoCapture &&
            from.scheme() == origin.scheme() && from.host() == origin.host())
            permission.grant();
        else
            permission.deny();
    });
#else
    connect(page, &QWebEnginePage::featurePermissionRequested, page,
            [page, origin](const QUrl &from, QWebEnginePage::Feature feature) {
        const bool allow = feature == QWebEnginePage::MediaVideoCapture &&
                           from.scheme() == origin.scheme() && from.host() == origin.host();
        page->setFeaturePermission(from, feature,
                                   allow ? QWebEnginePage::PermissionGrantedByUser
                                         : QWebEnginePage::PermissionDeniedByUser);
    });
#endif
    page->onMessage = [this](const QJsonObject &message) { receive(message); };
    connect(page, &QWebEnginePage::renderProcessTerminated, this, [this, page]() {
        m_ready = false;
        m_starting = false;
        m_taking = false;
        if (m_live) {
            m_live = false;
            m_tick->stop();
            closeOutput();
        }
        setStatus(QStringLiteral("The face changer stopped unexpectedly. Start the camera again."),
                  true);
        page->setHtml(faceStudioPage(), m_origin);
        updateState();
    });
    m_view->setPage(page);
    m_page = page;
    page->setHtml(faceStudioPage(), m_origin);
#endif
}

void FaceChangerFeature::run(const QString &script)
{
#ifdef FORKMESH_HAVE_WEBENGINE
    if (m_page && m_ready)
        m_page->runJavaScript(script);
#else
    Q_UNUSED(script);
#endif
}

void FaceChangerFeature::fillChoices(QComboBox *box, const QJsonArray &choices)
{
    const QSignalBlocker block(box);
    box->clear();
    for (const QJsonValue &value : choices) {
        const QJsonArray pair = value.toArray();
        if (pair.size() == 2)
            box->addItem(pair.at(1).toString(), pair.at(0).toString());
    }
}

void FaceChangerFeature::showLook(const QJsonObject &message)
{
    m_customFace = message.value(QStringLiteral("customFace")).toBool();
    m_customBackground = message.value(QStringLiteral("customBackground")).toBool();
    const auto select = [](QComboBox *box, const QString &id) {
        const QSignalBlocker block(box);
        const int index = box->findData(id);
        box->setCurrentIndex(index < 0 ? 0 : index);
    };
    select(m_face, message.value(QStringLiteral("face")).toString());
    select(m_background, message.value(QStringLiteral("background")).toString());
    select(m_outfit, message.value(QStringLiteral("outfit")).toString());
}

void FaceChangerFeature::receive(const QJsonObject &message)
{
    const QString type = message.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("ready")) {
        m_ready = true;
        const QJsonObject support = message.value(QStringLiteral("support")).toObject();
        fillChoices(m_face, message.value(QStringLiteral("faces")).toArray());
        fillChoices(m_background, message.value(QStringLiteral("backgrounds")).toArray());
        fillChoices(m_outfit, message.value(QStringLiteral("outfits")).toArray());
        m_customFaceId = message.value(QStringLiteral("customFaceId")).toString();
        m_customBackgroundId = message.value(QStringLiteral("customBackgroundId")).toString();
        showLook(message);
        if (!support.value(QStringLiteral("media")).toBool())
            setStatus(QStringLiteral("The camera is not available to the face changer here."), true);
        else if (!support.value(QStringLiteral("faces")).toBool() &&
                 !support.value(QStringLiteral("backgrounds")).toBool())
            setStatus(QStringLiteral("The face changer needs WebGL, which is turned off here."), true);
        else
            setStatus(QStringLiteral("Ready. Start the camera to see yourself."));
    } else if (type == QLatin1String("failed")) {
        m_ready = false;
        setStatus(message.value(QStringLiteral("text")).toString(), true);
    } else if (type == QLatin1String("live")) {
        m_starting = false;
        m_live = true;
        const QString current = message.value(QStringLiteral("camera")).toString();
        {
            const QSignalBlocker block(m_camera);
            m_camera->clear();
            for (const QJsonValue &value : message.value(QStringLiteral("cameras")).toArray()) {
                const QJsonArray pair = value.toArray();
                if (pair.size() == 2)
                    m_camera->addItem(pair.at(1).toString(), pair.at(0).toString());
            }
            if (m_camera->count() == 0)
                m_camera->addItem(message.value(QStringLiteral("label")).toString(), current);
            const int index = m_camera->findData(current);
            m_camera->setCurrentIndex(index < 0 ? 0 : index);
        }
        openOutput();
        m_tick->start();
        setStatus(QStringLiteral("Live. The preview is only on this computer."));
    } else if (type == QLatin1String("stopped")) {
        m_live = false;
        m_tick->stop();
        closeOutput();
    } else if (type == QLatin1String("error")) {
        if (message.value(QStringLiteral("stopped")).toBool()) {
            m_starting = false;
            m_live = false;
            m_tick->stop();
            closeOutput();
        }
        setStatus(message.value(QStringLiteral("text")).toString(), true);
    } else if (type == QLatin1String("look")) {
        showLook(message);
    }
    updateState();
}

void FaceChangerFeature::toggleCamera()
{
    if (!m_ready)
        return;
    if (m_live || m_starting) {
        m_starting = false;
        run(QStringLiteral("fmStop()"));
        setStatus(QStringLiteral("Camera stopped."));
    } else {
        m_starting = true;
        setStatus(QStringLiteral("Starting the camera…"));
        run(QStringLiteral("fmStart(%1)").arg(quoted(setting(QStringLiteral("camera")))));
    }
    updateState();
}

void FaceChangerFeature::takeFrame()
{
#ifdef FORKMESH_HAVE_WEBENGINE
    if (!m_page || !m_live || m_taking)
        return;
    m_taking = true;
    bool sending = false;
    {
        QMutexLocker lock(&m_feed->mutex);
        sending = m_feed->output.isOpen();
    }
    QPointer<FaceChangerFeature> self(this);
    m_page->runJavaScript(QStringLiteral("fmTake(%1)").arg(sending ? 1 : 0),
                          [self](const QVariant &result) {
        if (!self)
            return;
        self->m_taking = false;
        static const QString prefix = QStringLiteral("data:image/jpeg;base64,");
        const QString text = result.toString();
        if (text.startsWith(prefix))
            self->pushFrame(QByteArray::fromBase64(text.mid(prefix.size()).toLatin1()));
    });
#endif
}

void FaceChangerFeature::pushFrame(const QByteArray &jpeg)
{
    if (jpeg.isEmpty() || m_feed->busy.exchange(true))
        return;
    m_work = QtConcurrent::run([feed = m_feed, jpeg]() {
        QSize size;
        {
            QMutexLocker lock(&feed->mutex);
            size = feed->output.size();
        }
        if (size.isValid()) {
            const QByteArray frame = yuyvFrame(QImage::fromData(jpeg, "JPEG"), size);
            QMutexLocker lock(&feed->mutex);
            if (feed->output.size() == size && feed->output.write(frame))
                feed->frames.fetch_add(1);
        }
        feed->busy.store(false);
    });
}

void FaceChangerFeature::openOutput()
{
    m_outputError.clear();
    if (!cameraOutputSupported())
        return;
    {
        QMutexLocker lock(&m_feed->mutex);
        if (m_feed->output.isOpen() && QFileInfo::exists(m_feed->output.path()))
            return;
        m_feed->output.close();
    }
    const QList<LoopbackCamera> cameras = loopbackCameras();
    const int index = preferredCamera(cameras);
    if (index < 0) {
        m_outputName.clear();
        return;
    }
    QString error;
    QMutexLocker lock(&m_feed->mutex);
    if (m_feed->output.open(cameras.at(index).path, cameraFrameSize(), &error))
        m_outputName = cameras.at(index).name;
    else
        m_outputError = error;
}

void FaceChangerFeature::closeOutput()
{
    m_work.waitForFinished();
    QMutexLocker lock(&m_feed->mutex);
    m_feed->output.close();
}

void FaceChangerFeature::setUpCamera()
{
    if (m_setup && m_setup->state() != QProcess::NotRunning)
        return;
    if (!m_setup) {
        m_setup = new QProcess(this);
        m_setup->setProcessChannelMode(QProcess::MergedChannels);
        connect(m_setup, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
            const QString output = QString::fromLocal8Bit(m_setup->readAll());
            m_setupError = cameraSetupError(status == QProcess::NormalExit ? code : 1, output);
            for (int delay : {300, 1200}) {
                QTimer::singleShot(delay, this, [this]() { refresh(); });
            }
            updateState();
        });
        connect(m_setup, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart)
                return;
            m_setupError = cameraSetupError(127, QString());
            updateState();
        });
    }
    m_setupError.clear();
    const QStringList command = cameraSetupCommand();
    m_setup->start(command.first(), command.mid(1));
    updateState();
}

void FaceChangerFeature::choosePhoto(bool face)
{
    if (!m_ready)
        return;
    const QString path = QFileDialog::getOpenFileName(
        this, face ? QStringLiteral("Choose a front-facing photo") : QStringLiteral("Choose a background photo"),
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation),
        QStringLiteral("Images (*.png *.jpg *.jpeg *.webp *.bmp *.gif)"));
    if (path.isEmpty()) {
        run(QStringLiteral("fmLook()"));
        return;
    }
    QFile file(path);
    if (QFileInfo(path).size() > kMaxPhotoBytes || !file.open(QIODevice::ReadOnly)) {
        setStatus(QStringLiteral("That photo is too large or could not be read."), true);
        return;
    }
    const QString mime = QMimeDatabase().mimeTypeForFile(path).name();
    const QString data = QStringLiteral("data:%1;base64,%2")
                             .arg(mime, QString::fromLatin1(file.readAll().toBase64()));
    setStatus(QStringLiteral("Reading your photo…"));
    run(QStringLiteral("%1(%2)").arg(face ? QStringLiteral("fmFacePhoto")
                                          : QStringLiteral("fmBackgroundPhoto"),
                                     quoted(data)));
}

void FaceChangerFeature::updateState()
{
    m_startButton->setEnabled(m_ready || m_live);
    m_startButton->setText(m_live || m_starting ? QStringLiteral("Stop camera")
                                                : QStringLiteral("Start camera"));
    m_camera->setEnabled(m_ready && !m_starting);
    for (QComboBox *box : {m_face, m_background, m_outfit})
        box->setEnabled(m_ready);

    const bool supported = cameraOutputSupported();
    const QList<LoopbackCamera> cameras = supported ? loopbackCameras() : QList<LoopbackCamera>{};
    const bool found = !cameras.isEmpty();
    bool sending = false;
    {
        QMutexLocker lock(&m_feed->mutex);
        sending = m_feed->output.isOpen();
    }
    const bool settingUp = m_setup && m_setup->state() != QProcess::NotRunning;
    QString text;
    if (!supported) {
        text = QStringLiteral(
            "On this computer other apps cannot pick a ForkMesh camera yet. Share the Face Changer "
            "window in your meeting app, or use the desktop app on Linux.");
    } else if (sending) {
        text = QStringLiteral("Your changed picture is going to \"%1\". In your call or meeting "
                              "app, choose it as the camera. It shows up while the camera here is on.")
                   .arg(m_outputName);
    } else if (!m_outputError.isEmpty()) {
        text = m_outputError;
    } else if (found) {
        text = QStringLiteral("\"%1\" is ready. Start the camera, then choose \"%1\" as the camera "
                              "in your call or meeting app.")
                   .arg(cameras.at(preferredCamera(cameras)).name);
    } else if (settingUp) {
        text = QStringLiteral("Setting up %1…").arg(cameraLabel());
    } else {
        text = QStringLiteral("Other apps see your changed picture through a \"%1\". Set it up once "
                              "after each restart; it asks for your password and needs the "
                              "v4l2loopback driver from your system's packages. Or run:")
                   .arg(cameraLabel());
    }
    if (!m_setupError.isEmpty() && !found)
        text += QStringLiteral("\n\n") + m_setupError;
    m_output->setText(text);
    m_command->setVisible(supported && !found);
    m_setupButton->setVisible(supported && !found);
    m_setupButton->setEnabled(!settingUp);
    m_rescanButton->setVisible(supported);
}

void FaceChangerFeature::setStatus(const QString &text, bool error)
{
    m_status->setText(text);
    m_status->setStyleSheet(error ? QStringLiteral("color: #e5484d;") : QString());
}

}

namespace {

const forkmesh::market::NativePackageRegistration faceChangerRegistration({
    QStringLiteral("builtin:face-changer"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Face Changer"),
    QStringLiteral("person"),
    QString(),
    QString(),
    [](const forkmesh::market::NativePackageHost &host, QWidget *parent) {
        return new forkmesh::facechanger::FaceChangerFeature(host, parent);
    },
});

}
