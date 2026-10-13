#include "VoiceChangerFeature.h"

#include "VoiceAudio.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSlider>
#include <QStyle>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace forkmesh::voice {

namespace {

constexpr int kSampleSeconds = 8;
constexpr int kMaxCustomVoices = 24;

// Slider label and step per engine parameter; mix stays at 1 on calls.
struct SliderSpec {
    const char *name;
    const char *label;
    double step;
};
const SliderSpec kSliders[] = {
    {"targetPitchHz", "Pitch (Hz)", 1},
    {"semitones", "Shift (semitones)", 0.5},
    {"formant", "Vocal tract", 0.01},
    {"brightness", "Brightness", 0.05},
    {"breath", "Breath", 0.05},
    {"roughness", "Gravel", 0.05},
    {"whisper", "Whisper", 0.05},
    {"gainDb", "Level (dB)", 0.5},
};

const ParamBounds *bounds(const QString &name)
{
    for (const ParamBounds &b : paramBounds()) {
        if (name == QLatin1String(b.name))
            return &b;
    }
    return nullptr;
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
    // The theme paints labels; inside a card they sit on the card's colour.
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

QJsonObject paramsJson(const VoiceParams &params)
{
    QJsonObject object;
    for (const ParamBounds &b : paramBounds())
        object.insert(QString::fromLatin1(b.name), paramValue(params, b.name));
    return object;
}

VoiceParams paramsFromJson(const QJsonObject &object)
{
    VoiceParams params;
    for (const ParamBounds &b : paramBounds()) {
        const QJsonValue value = object.value(QString::fromLatin1(b.name));
        setParamValue(params, b.name, value.isDouble() ? value.toDouble() : b.fallback);
    }
    return params.clean();
}

QString presetKey(const char *id)
{
    return QStringLiteral("preset:") + QString::fromLatin1(id);
}

} // namespace

// --- page ----------------------------------------------------------------------

VoiceChangerFeature::VoiceChangerFeature(
    const forkmesh::market::NativePackageHost &host, QWidget *parent)
    : forkmesh::market::NativePackageWidget(parent),
      m_packageId(host.packageId.isEmpty() ? QStringLiteral("voice-changer")
                                           : host.packageId),
      m_request(host.request),
      m_searchIndexChanged(host.searchIndexChanged)
{
    setObjectName(QStringLiteral("voiceChangerSection"));
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
    auto *title = new QLabel(QStringLiteral("Voice Changer"));
    title->setObjectName(QStringLiteral("pageTitle"));
    heading->addWidget(title);
    heading->addWidget(mutedLabel(QStringLiteral(
        "Talk in another voice on any call or meeting app. "
        "Your voice is changed on this computer and never uploaded.")), 1);
    auto *refreshButton = new QPushButton(QStringLiteral("Refresh"));
    refreshButton->setObjectName(QStringLiteral("ghostButton"));
    heading->addWidget(refreshButton);
    layout->addLayout(heading);

    m_status = mutedLabel(QString());
    layout->addWidget(m_status);

    // --- live ---
    QVBoxLayout *liveBody = nullptr;
    layout->addWidget(card(QStringLiteral("Your microphone"), &liveBody));
    auto *startRow = new QHBoxLayout;
    m_startButton = new QPushButton;
    m_startButton->setObjectName(QStringLiteral("primaryButton"));
    startRow->addWidget(m_startButton);
    m_where = mutedLabel(QString());
    startRow->addWidget(m_where, 1);
    liveBody->addLayout(startRow);
    auto *devices = new QGridLayout;
    devices->addWidget(mutedLabel(QStringLiteral("Microphone")), 0, 0);
    m_input = new QComboBox;
    devices->addWidget(m_input, 0, 1);
    auto *rescan = new QPushButton(QStringLiteral("Rescan"));
    rescan->setObjectName(QStringLiteral("ghostButton"));
    devices->addWidget(rescan, 0, 2);
    m_outputLabel = mutedLabel(QStringLiteral("Send to"));
    devices->addWidget(m_outputLabel, 1, 0);
    m_output = new QComboBox;
    devices->addWidget(m_output, 1, 1, 1, 2);
    devices->setColumnStretch(1, 1);
    liveBody->addLayout(devices);
    m_hint = mutedLabel(QString());
    m_hint->setOpenExternalLinks(true);
    m_hint->setTextFormat(Qt::RichText);
    liveBody->addWidget(m_hint);
    m_monitor = new QCheckBox(QStringLiteral("Hear myself (use headphones)"));
    m_monitor->setChecked(setting(QStringLiteral("monitor")) == QLatin1String("true"));
    liveBody->addWidget(m_monitor);
    m_denoise = new QCheckBox(QStringLiteral("Reduce background noise (hiss, hum, fans)"));
    m_denoise->setChecked(setting(QStringLiteral("noiseReduction"), QStringLiteral("true")) ==
                          QLatin1String("true"));
    liveBody->addWidget(m_denoise);
    m_meter = new QProgressBar;
    m_meter->setRange(0, 100);
    m_meter->setTextVisible(false);
    m_meter->setMaximumHeight(8);
    liveBody->addWidget(m_meter);
    m_stats = mutedLabel(QString());
    liveBody->addWidget(m_stats);

    // --- voice ---
    QVBoxLayout *voiceBody = nullptr;
    layout->addWidget(card(QStringLiteral("Voice"), &voiceBody));
    auto *voiceRow = new QHBoxLayout;
    m_voice = new QComboBox;
    voiceRow->addWidget(m_voice, 1);
    auto *saveVoice = new QPushButton(QStringLiteral("Save as…"));
    saveVoice->setObjectName(QStringLiteral("ghostButton"));
    m_deleteVoice = new QPushButton(QStringLiteral("Delete"));
    m_deleteVoice->setObjectName(QStringLiteral("ghostButton"));
    voiceRow->addWidget(saveVoice);
    voiceRow->addWidget(m_deleteVoice);
    voiceBody->addLayout(voiceRow);
    auto *sliders = new QGridLayout;
    int row = 0;
    for (const SliderSpec &spec : kSliders) {
        const ParamBounds *b = bounds(QString::fromLatin1(spec.name));
        auto *slider = new QSlider(Qt::Horizontal);
        slider->setRange(int(std::lround(b->low / spec.step)),
                         int(std::lround(b->high / spec.step)));
        auto *value = mutedLabel(QString());
        value->setMinimumWidth(56);
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        sliders->addWidget(mutedLabel(QString::fromLatin1(spec.label)), row, 0);
        sliders->addWidget(slider, row, 1);
        sliders->addWidget(value, row, 2);
        const QString name = QString::fromLatin1(spec.name);
        const double step = spec.step;
        connect(slider, &QSlider::valueChanged, this, [this, name, step, value](int raw) {
            value->setText(QString::number(raw * step, 'g', 4));
            if (m_syncingSliders)
                return;
            setParamValue(m_params, name.toStdString(), raw * step);
            m_params = m_params.clean();
            m_params.mix = 1;
            // Any edit is a new voice until it is saved under a name.
            m_voice->setCurrentIndex(m_voice->findData(QStringLiteral("unsaved")));
            m_deleteVoice->setEnabled(false);
            pushParams();
        });
        m_sliders.append({name, slider});
        m_sliderValues.append(value);
        ++row;
    }
    sliders->setColumnStretch(1, 1);
    voiceBody->addLayout(sliders);

    // --- AI design ---
    QVBoxLayout *designBody = nullptr;
    layout->addWidget(card(QStringLiteral("Design a voice with AI"), &designBody));
    designBody->addWidget(mutedLabel(QStringLiteral(
        "Describe the person you want to sound like. ForkMesh AI turns it into voice "
        "settings you can fine-tune above. Only the words you type are sent.")));
    auto *designRow = new QHBoxLayout;
    m_prompt = new QLineEdit;
    m_prompt->setPlaceholderText(QStringLiteral("A calm, deep late-night radio host"));
    m_prompt->setMaxLength(400);
    m_designButton = new QPushButton(QStringLiteral("Design"));
    m_designButton->setObjectName(QStringLiteral("primaryButton"));
    designRow->addWidget(m_prompt, 1);
    designRow->addWidget(m_designButton);
    designBody->addLayout(designRow);
    m_designNote = mutedLabel(QString());
    designBody->addWidget(m_designNote);

    // --- match a sample ---
    QVBoxLayout *matchBody = nullptr;
    layout->addWidget(card(QStringLiteral("Match a voice sample"), &matchBody));
    matchBody->addWidget(mutedLabel(QStringLiteral(
        "Record %1 seconds of yourself reading aloud, then give a recording of the voice "
        "to match. The voice model learns its pitch and the shape of its vocal tract, "
        "on this computer.").arg(kSampleSeconds)));
    auto *matchRow = new QHBoxLayout;
    auto *recordMe = new QPushButton(QStringLiteral("Record me"));
    auto *recordThem = new QPushButton(QStringLiteral("Record the voice to match"));
    auto *loadThem = new QPushButton(QStringLiteral("Load a WAV…"));
    for (QPushButton *button : {recordMe, recordThem, loadThem}) {
        button->setObjectName(QStringLiteral("ghostButton"));
        matchRow->addWidget(button);
    }
    matchRow->addStretch();
    matchBody->addLayout(matchRow);
    m_sampleState = mutedLabel(QStringLiteral("No recordings yet."));
    matchBody->addWidget(m_sampleState);
    m_consent = new QCheckBox(QStringLiteral(
        "This is my own voice, or the person in the recording agreed to have their voice "
        "matched. I will not use it to deceive anyone."));
    m_consent->setStyleSheet(QStringLiteral("QCheckBox { spacing: 8px; }"));
    matchBody->addWidget(m_consent);
    m_learnButton = new QPushButton(QStringLiteral("Learn this voice"));
    m_learnButton->setObjectName(QStringLiteral("primaryButton"));
    auto *learnRow = new QHBoxLayout;
    learnRow->addWidget(m_learnButton);
    learnRow->addStretch();
    matchBody->addLayout(learnRow);

    layout->addWidget(mutedLabel(QStringLiteral(
        "Voice Changer is for play, privacy and performance. Pretending to be a real person "
        "to mislead or defraud anyone is not allowed and may be illegal where you live.")));
    layout->addStretch();

    // --- audio thread ---
    m_thread = new QThread(this);
    m_thread->setObjectName(QStringLiteral("forkmesh-voice-audio"));
    m_pipeline = new VoiceAudioPipeline;
    m_pipeline->moveToThread(m_thread);
    m_thread->start(QThread::TimeCriticalPriority);
    connect(m_pipeline, &VoiceAudioPipeline::devicesListed, this,
            [this](const QVariantMap &devices) { applyDevices(devices); });
    connect(m_pipeline, &VoiceAudioPipeline::started, this, [this](const QString &where) {
        m_running = true;
        m_where->setText(where);
        updateRunState();
    });
    connect(m_pipeline, &VoiceAudioPipeline::stopped, this, [this]() {
        m_running = false;
        m_meter->setValue(0);
        updateRunState();
    });
    connect(m_pipeline, &VoiceAudioPipeline::failed, this,
            [this](const QString &error) { setStatus(error, true); });
    connect(m_pipeline, &VoiceAudioPipeline::statsReady, this,
            [this](const QVariantMap &stats) { applyStats(stats); });
    connect(m_pipeline, &VoiceAudioPipeline::recorded, this,
            [this](const QString &tag, const QByteArray &bytes, int rate) {
        std::vector<float> samples(size_t(bytes.size() / 4));
        std::memcpy(samples.data(), bytes.constData(), samples.size() * 4);
        const double seconds = double(samples.size()) / std::max(1, rate);
        if (tag == QLatin1String("me")) {
            m_mine = std::move(samples);
            m_mineRate = rate;
        } else {
            m_theirs = std::move(samples);
            m_theirsRate = rate;
        }
        m_sampleState->setText(QStringLiteral("Your recording: %1 · voice to match: %2")
            .arg(m_mine.empty() ? QStringLiteral("none")
                                : QStringLiteral("%1 s").arg(double(m_mine.size()) / m_mineRate, 0, 'f', 1),
                 m_theirs.empty() ? QStringLiteral("none")
                                  : QStringLiteral("%1 s").arg(double(m_theirs.size()) / m_theirsRate, 0, 'f', 1)));
        setStatus(QStringLiteral("Recorded %1 s.").arg(seconds, 0, 'f', 1));
    });

    connect(refreshButton, &QPushButton::clicked, this, [this]() { refresh(); });
    connect(m_startButton, &QPushButton::clicked, this, [this]() { toggleRunning(); });
    connect(rescan, &QPushButton::clicked, this, [this]() {
        QMetaObject::invokeMethod(m_pipeline, &VoiceAudioPipeline::listDevices);
    });
    connect(m_monitor, &QCheckBox::toggled, this, [this](bool on) {
        setSetting(QStringLiteral("monitor"), on ? QStringLiteral("true") : QStringLiteral("false"));
        if (m_running)
            startPipeline();
    });
    connect(m_denoise, &QCheckBox::toggled, this, [this](bool on) {
        setSetting(QStringLiteral("noiseReduction"),
                   on ? QStringLiteral("true") : QStringLiteral("false"));
        QMetaObject::invokeMethod(m_pipeline, [pipeline = m_pipeline, on]() {
            pipeline->setNoiseReduction(on);
        });
    });
    connect(m_input, &QComboBox::activated, this, [this]() {
        setSetting(QStringLiteral("input"), m_input->currentData().toString());
        if (m_running)
            startPipeline();
    });
    connect(m_output, &QComboBox::activated, this, [this]() {
        setSetting(QStringLiteral("output"), m_output->currentData().toString());
        if (m_running)
            startPipeline();
    });
    connect(m_voice, &QComboBox::activated, this, [this]() {
        selectVoice(m_voice->currentData().toString());
    });
    connect(saveVoice, &QPushButton::clicked, this, [this]() { this->saveVoice(); });
    connect(m_deleteVoice, &QPushButton::clicked, this, [this]() { deleteVoice(); });
    connect(m_designButton, &QPushButton::clicked, this, [this]() { designVoice(); });
    connect(m_prompt, &QLineEdit::returnPressed, this, [this]() { designVoice(); });
    connect(recordMe, &QPushButton::clicked, this, [this]() { recordSample(QStringLiteral("me")); });
    connect(recordThem, &QPushButton::clicked, this, [this]() { recordSample(QStringLiteral("them")); });
    connect(loadThem, &QPushButton::clicked, this, [this]() { loadTargetWav(); });
    connect(m_learnButton, &QPushButton::clicked, this, [this]() { learn(); });

    loadVoices();
    QMetaObject::invokeMethod(m_pipeline, &VoiceAudioPipeline::listDevices);
    updateRunState();
}

VoiceChangerFeature::~VoiceChangerFeature()
{
    // Removing the package or closing the window takes the virtual microphone
    // down with it; nothing is left running.
    if (m_pipeline) {
        VoiceAudioPipeline *pipeline = m_pipeline;
        m_pipeline = nullptr;
        QMetaObject::invokeMethod(pipeline, [pipeline]() {
            pipeline->stop();
            delete pipeline;
        }, Qt::BlockingQueuedConnection);
    }
    m_thread->quit();
    m_thread->wait();
}

QString VoiceChangerFeature::setting(const QString &key, const QString &fallback) const
{
    // The host's package-data prefix: removing the package clears it.
    return QSettings().value(QStringLiteral("market/packages/%1/data/voice.%2")
                                 .arg(m_packageId, key), fallback).toString();
}

void VoiceChangerFeature::setSetting(const QString &key, const QVariant &value)
{
    QSettings().setValue(QStringLiteral("market/packages/%1/data/voice.%2")
                             .arg(m_packageId, key), value);
}

void VoiceChangerFeature::request(const QByteArray &method, const QString &suffix,
                                  const QJsonObject &body, ReplyHandler handler)
{
    if (!m_request) {
        handler(false, {}, QStringLiteral("Sign in to your ForkMesh account first."));
        return;
    }
    QPointer<VoiceChangerFeature> self(this);
    m_request(method, QStringLiteral("/api/voice-changer") + suffix, body,
              [self, handler = std::move(handler)](bool ok, const QJsonObject &payload,
                                                   const QString &error) {
        if (self)
            handler(ok, payload, error);
    });
}

void VoiceChangerFeature::setStatus(const QString &text, bool error)
{
    m_status->setText(text);
    m_status->setObjectName(error ? QStringLiteral("errorLabel") : QStringLiteral("mutedLabel"));
    m_status->style()->unpolish(m_status);
    m_status->style()->polish(m_status);
}

// --- ownership ------------------------------------------------------------------

void VoiceChangerFeature::refresh()
{
    // Nothing here is paid or account-scoped; the one question for the Worker
    // is whether AI voice design can be offered, and a desktop with no
    // ForkMesh account simply does without it.
    request("GET", QString(), {}, [this](bool ok, const QJsonObject &payload,
                                         const QString &error) {
        const bool ai = ok && payload.value(QStringLiteral("ai")).toBool();
        m_designButton->setEnabled(ai);
        m_prompt->setEnabled(ai);
        m_designNote->setText(ai ? QString()
            : QStringLiteral("Sign in to ForkMesh on this device to design voices with AI. "
                             "Everything else works without an account."));
        if (!ok && !error.isEmpty())
            setStatus(QString(), false);
    });
}

// --- running ----------------------------------------------------------------------

void VoiceChangerFeature::toggleRunning()
{
    if (m_running)
        stopPipeline();
    else
        startPipeline();
}

void VoiceChangerFeature::startPipeline()
{
    // Nothing is held back: every start offers the changed voice to call apps.
    m_live = true;
    QVariantMap params;
    const QJsonObject json = paramsJson(m_params);
    for (auto it = json.begin(); it != json.end(); ++it)
        params.insert(it.key(), it.value().toDouble());
    const QVariantMap config{
        {QStringLiteral("input"), m_input->currentData().toString()},
        {QStringLiteral("output"), m_output->currentData().toString()},
        {QStringLiteral("virtualMicrophone"), m_live},
        {QStringLiteral("monitor"), m_monitor->isChecked()},
        {QStringLiteral("noiseReduction"), m_denoise->isChecked()},
        {QStringLiteral("params"), params},
        {QStringLiteral("userPitchHz"), setting(QStringLiteral("userPitchHz")).toDouble()},
    };
    setStatus(QString());
    QMetaObject::invokeMethod(m_pipeline, [pipeline = m_pipeline, config]() {
        pipeline->start(config);
    });
}

void VoiceChangerFeature::stopPipeline()
{
    QMetaObject::invokeMethod(m_pipeline, &VoiceAudioPipeline::stop);
}

void VoiceChangerFeature::pushParams()
{
    QVariantMap params;
    const QJsonObject json = paramsJson(m_params);
    for (auto it = json.begin(); it != json.end(); ++it)
        params.insert(it.key(), it.value().toDouble());
    if (m_running) {
        QMetaObject::invokeMethod(m_pipeline, [pipeline = m_pipeline, params]() {
            pipeline->setParams(params);
        });
    }
}

void VoiceChangerFeature::applyDevices(const QVariantMap &devices)
{
    m_backend = devices.value(QStringLiteral("backend")).toString();
    m_virtualMicrophone = devices.value(QStringLiteral("virtualMicrophone")).toBool();
    const QString savedInput = setting(QStringLiteral("input"));
    const QString savedOutput = setting(QStringLiteral("output"));
    m_input->clear();
    m_input->addItem(QStringLiteral("System default"), QString());
    for (const QVariant &value : devices.value(QStringLiteral("inputs")).toList()) {
        const QVariantMap d = value.toMap();
        m_input->addItem(d.value(QStringLiteral("name")).toString(),
                         d.value(QStringLiteral("id")).toString());
    }
    m_input->setCurrentIndex(std::max(0, m_input->findData(savedInput)));
    m_output->clear();
    int cable = -1;
    for (const QVariant &value : devices.value(QStringLiteral("outputs")).toList()) {
        const QVariantMap d = value.toMap();
        m_output->addItem(d.value(QStringLiteral("name")).toString(),
                          d.value(QStringLiteral("id")).toString());
        if (cable < 0 && d.value(QStringLiteral("cable")).toBool())
            cable = m_output->count() - 1;
    }
    const int saved = m_output->findData(savedOutput);
    m_output->setCurrentIndex(saved >= 0 ? saved : std::max(0, cable));
    // Linux makes its own microphone; elsewhere the voice goes into a cable.
    m_output->setVisible(!m_virtualMicrophone);
    m_outputLabel->setVisible(!m_virtualMicrophone);
    if (m_backend.isEmpty()) {
        m_hint->setText(QStringLiteral(
            "No audio system was found. On Linux install PipeWire or PulseAudio; on Windows "
            "and macOS this ForkMesh build needs Qt Multimedia."));
    } else if (m_virtualMicrophone) {
        m_hint->setText(QStringLiteral(
            "While it runs, pick <b>%1</b> as the microphone in your call or meeting "
            "app. It disappears again when you stop.").arg(
                QString::fromLatin1(kVirtualMicLabel)));
    } else {
        m_hint->setText(QStringLiteral(
            "This system has no microphone an app can create, so install a virtual "
            "audio cable once (several free ones exist for Windows and macOS). Send "
            "the changed voice to the cable's input here, then pick the cable's "
            "microphone side in your call or meeting app.%1")
            .arg(cable < 0 ? QStringLiteral(" <b>No cable is installed yet.</b>") : QString()));
    }
    updateRunState();
}

void VoiceChangerFeature::applyStats(const QVariantMap &stats)
{
    // -60 dBFS .. 0 dBFS across the bar, like a mixer's meter.
    const double level = stats.value(QStringLiteral("level")).toDouble();
    const double db = level > 1e-6 ? 20 * std::log10(level) : -60;
    m_meter->setValue(std::clamp(int(std::lround((db + 60) * 100 / 60)), 0, 100));
    const int user = stats.value(QStringLiteral("userPitchHz")).toInt();
    const int now = stats.value(QStringLiteral("pitchHz")).toInt();
    m_stats->setText(QStringLiteral("Your pitch ≈ %1 Hz%2 · delay %3 ms")
        .arg(user)
        .arg(stats.value(QStringLiteral("voiced")).toBool()
                 ? QStringLiteral(" · now %1 Hz").arg(now) : QString())
        .arg(stats.value(QStringLiteral("latencyMs")).toInt()));
    // The learned pitch carries over to the next start, so a voice with a
    // target pitch sounds right from the first word.
    if (user > 50 && stats.value(QStringLiteral("voiced")).toBool())
        setSetting(QStringLiteral("userPitchHz"), QString::number(user));
}

void VoiceChangerFeature::updateRunState()
{
    if (m_running) {
        m_startButton->setText(QStringLiteral("Stop"));
    } else {
        m_startButton->setText(QStringLiteral("Start changing my voice"));
        m_where->setText(QStringLiteral("Stopped."));
    }
    m_startButton->setEnabled(!m_backend.isEmpty() || m_running);
}

// --- voices -------------------------------------------------------------------------

void VoiceChangerFeature::loadVoices()
{
    m_customVoices = QJsonDocument::fromJson(
        setting(QStringLiteral("customVoices")).toUtf8()).array();
    QString selected = setting(QStringLiteral("voiceId"), presetKey("deep-baritone"));
    rebuildVoiceList(selected);
    selectVoice(m_voice->currentData().toString());
}

void VoiceChangerFeature::rebuildVoiceList(const QString &selectId)
{
    m_voice->clear();
    for (const VoicePreset &preset : presets()) {
        if (std::strcmp(preset.id, "natural") != 0)
            m_voice->addItem(QString::fromLatin1(preset.name), presetKey(preset.id));
    }
    if (!m_customVoices.isEmpty())
        m_voice->insertSeparator(m_voice->count());
    for (const QJsonValue &value : m_customVoices) {
        const QJsonObject voice = value.toObject();
        m_voice->addItem(voice.value(QStringLiteral("name")).toString(),
                         QStringLiteral("custom:") + voice.value(QStringLiteral("id")).toString());
    }
    m_voice->addItem(QStringLiteral("Unsaved changes"), QStringLiteral("unsaved"));
    const int index = m_voice->findData(selectId);
    m_voice->setCurrentIndex(index >= 0 ? index : 0);
    // Saved voices are what the top search box lists from this page.
    if (m_searchIndexChanged)
        m_searchIndexChanged();
}

void VoiceChangerFeature::selectVoice(const QString &id)
{
    if (id.startsWith(QLatin1String("preset:"))) {
        for (const VoicePreset &preset : presets()) {
            if (id == presetKey(preset.id))
                useParams(preset.params, false);
        }
    } else if (id.startsWith(QLatin1String("custom:"))) {
        for (const QJsonValue &value : m_customVoices) {
            const QJsonObject voice = value.toObject();
            if (id.mid(7) == voice.value(QStringLiteral("id")).toString())
                useParams(paramsFromJson(voice.value(QStringLiteral("params")).toObject()), true);
        }
    }
    if (id != QLatin1String("unsaved"))
        setSetting(QStringLiteral("voiceId"), id);
    m_deleteVoice->setEnabled(id.startsWith(QLatin1String("custom:")));
}

void VoiceChangerFeature::useParams(const VoiceParams &params, bool)
{
    m_params = params.clean();
    m_params.mix = 1;
    m_syncingSliders = true;
    for (int i = 0; i < m_sliders.size(); ++i) {
        const double step = kSliders[i].step;
        const int raw = int(std::lround(
            paramValue(m_params, m_sliders[i].first.toStdString()) / step));
        m_sliders[i].second->setValue(raw);
        // setValue() is silent when the value does not change.
        m_sliderValues[i]->setText(QString::number(raw * step, 'g', 4));
    }
    m_syncingSliders = false;
    pushParams();
}

void VoiceChangerFeature::saveVoice()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("Save voice"),
        QStringLiteral("Name this voice"), QLineEdit::Normal,
        m_voice->currentData().toString() == QLatin1String("unsaved")
            ? QString() : m_voice->currentText(), &ok).simplified().left(40);
    if (!ok || name.isEmpty())
        return;
    QJsonArray kept;
    for (const QJsonValue &value : m_customVoices) {
        if (value.toObject().value(QStringLiteral("name")).toString() != name)
            kept.append(value);
    }
    const QString id = QString::number(QDateTime::currentMSecsSinceEpoch(), 36);
    kept.prepend(QJsonObject{{QStringLiteral("id"), id},
                             {QStringLiteral("name"), name},
                             {QStringLiteral("params"), paramsJson(m_params)}});
    while (kept.size() > kMaxCustomVoices)
        kept.removeLast();
    m_customVoices = kept;
    setSetting(QStringLiteral("customVoices"),
               QString::fromUtf8(QJsonDocument(m_customVoices).toJson(QJsonDocument::Compact)));
    rebuildVoiceList(QStringLiteral("custom:") + id);
    selectVoice(QStringLiteral("custom:") + id);
    setStatus(QStringLiteral("Saved \"%1\".").arg(name));
}

void VoiceChangerFeature::deleteVoice()
{
    const QString id = m_voice->currentData().toString();
    if (!id.startsWith(QLatin1String("custom:")))
        return;
    QJsonArray kept;
    for (const QJsonValue &value : m_customVoices) {
        if (value.toObject().value(QStringLiteral("id")).toString() != id.mid(7))
            kept.append(value);
    }
    m_customVoices = kept;
    setSetting(QStringLiteral("customVoices"),
               QString::fromUtf8(QJsonDocument(m_customVoices).toJson(QJsonDocument::Compact)));
    rebuildVoiceList(presetKey("deep-baritone"));
    selectVoice(m_voice->currentData().toString());
}

void VoiceChangerFeature::designVoice()
{
    const QString prompt = m_prompt->text().simplified();
    if (prompt.size() < 3) {
        setStatus(QStringLiteral("Describe the voice first."), true);
        return;
    }
    m_designButton->setEnabled(false);
    m_designNote->setText(QStringLiteral("Designing…"));
    request("POST", QStringLiteral("/design"), {{QStringLiteral("prompt"), prompt}},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        m_designButton->setEnabled(true);
        if (!ok) {
            m_designNote->setText(QString());
            setStatus(error, true);
            return;
        }
        const QJsonObject voice = payload.value(QStringLiteral("voice")).toObject();
        useParams(paramsFromJson(voice.value(QStringLiteral("params")).toObject()), true);
        m_voice->setCurrentIndex(m_voice->findData(QStringLiteral("unsaved")));
        const QString note = payload.value(QStringLiteral("note")).toString();
        m_designNote->setText(QStringLiteral("Designed \"%1\". Press Save as… to keep it.%2")
            .arg(voice.value(QStringLiteral("name")).toString(),
                 note.isEmpty() ? QString() : QLatin1Char(' ') + note));
    });
}

void VoiceChangerFeature::recordSample(const QString &tag)
{
    if (!m_running) {
        setStatus(QStringLiteral("Press Start (or Preview) first, then record."), true);
        return;
    }
    setStatus(tag == QLatin1String("me")
                  ? QStringLiteral("Recording you for %1 seconds: read anything aloud.").arg(kSampleSeconds)
                  : QStringLiteral("Recording the voice to match for %1 seconds.").arg(kSampleSeconds));
    QMetaObject::invokeMethod(m_pipeline, [pipeline = m_pipeline, tag]() {
        pipeline->record(kSampleSeconds, tag);
    });
}

void VoiceChangerFeature::loadTargetWav()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Voice to match"), QString(),
        QStringLiteral("WAV audio (*.wav)"));
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024 * 1024) {
        setStatus(QStringLiteral("Choose a WAV file under 64 MB."), true);
        return;
    }
    std::vector<float> samples;
    int rate = 0;
    QString error;
    if (!readWav(file.readAll(), &samples, &rate, &error)) {
        setStatus(error, true);
        return;
    }
    m_theirs = std::move(samples);
    m_theirsRate = rate;
    m_sampleState->setText(QStringLiteral("Voice to match: %1 s from %2")
        .arg(double(m_theirs.size()) / rate, 0, 'f', 1).arg(QFileInfo(path).fileName()));
}

void VoiceChangerFeature::learn()
{
    if (!m_consent->isChecked()) {
        setStatus(QStringLiteral("Confirm you have the right to match this voice."), true);
        return;
    }
    if (m_mine.empty() || m_theirs.empty()) {
        setStatus(QStringLiteral("Record yourself and give a voice to match first."), true);
        return;
    }
    const int rate = VoiceAudioPipeline::kSampleRate;
    const LearnedVoice fitted = learnVoice(resample(m_mine, m_mineRate, rate),
                                           resample(m_theirs, m_theirsRate, rate), rate);
    if (!fitted.ok) {
        setStatus(QStringLiteral("No clear speech was found in one of the recordings. "
                                 "Try a longer, cleaner one."), true);
        return;
    }
    useParams(fitted.params, true);
    m_voice->setCurrentIndex(m_voice->findData(QStringLiteral("unsaved")));
    m_sampleState->setText(QStringLiteral(
        "Learned: pitch %1 Hz (yours %2 Hz), vocal tract ×%3. Press Save as… to keep it.")
        .arg(int(fitted.params.targetPitchHz)).arg(fitted.speakerPitchHz)
        .arg(fitted.params.formant, 0, 'f', 2));
}

// --- top search ------------------------------------------------------------------------

QList<forkmesh::market::NativePackageSearchHit>
VoiceChangerFeature::searchHits(const QString &query, int limit) const
{
    QList<forkmesh::market::NativePackageSearchHit> hits;
    const QString needle = query.trimmed();
    if (needle.isEmpty())
        return hits;
    for (int i = 0; i < m_voice->count() && hits.size() < limit; ++i) {
        const QString id = m_voice->itemData(i).toString();
        const QString name = m_voice->itemText(i);
        if (id.isEmpty() || id == QLatin1String("unsaved") ||
            !name.contains(needle, Qt::CaseInsensitive))
            continue;
        hits.append({id, name, QStringLiteral("Voice Changer voice")});
    }
    return hits;
}

void VoiceChangerFeature::activateSearchHit(const QString &id)
{
    const int index = m_voice->findData(id);
    if (index < 0)
        return;
    m_voice->setCurrentIndex(index);
    selectVoice(id);
}

} // namespace forkmesh::voice

namespace {
// The desktop rail uses Octicons; "unmute" ships in desktop/resources/icons.qrc.
const forkmesh::market::NativePackageRegistration voiceChangerRegistration({
    QStringLiteral("builtin:voice-changer"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Voice Changer"),
    QStringLiteral("unmute"),
    QStringLiteral("/api/voice-changer"),
    QStringLiteral("forkmesh-voice-changer-request-v1"),
    [](const forkmesh::market::NativePackageHost &host, QWidget *parent) {
        return new forkmesh::voice::VoiceChangerFeature(host, parent);
    },
});
} // namespace
