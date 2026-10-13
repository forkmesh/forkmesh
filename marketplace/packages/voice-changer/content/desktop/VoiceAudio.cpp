#include "VoiceAudio.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cstring>
#include <vector>

#ifdef FORKMESH_HAVE_MULTIMEDIA
#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QAudioSource>
#include <QMediaDevices>
#endif

#if defined(Q_OS_UNIX)
#include <cerrno>
#include <csignal>
#include <ctime>
#include <fcntl.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif
#if defined(Q_OS_LINUX)
#include <csignal>
#include <sys/prctl.h>
#endif

namespace forkmesh::voice {

namespace {

// Where a player's queue cannot be measured, drop blocks beyond this much
// instead of letting the call hear the voice ever later.
constexpr qint64 kMaxQueuedBytes = VoiceAudioPipeline::kSampleRate * 4 / 4;
constexpr int kToolTimeoutMs = 3000;
// How far ahead of a player the changed voice is kept. Measured on PipeWire,
// a player fed exactly as fast as the microphone runs dry and plays a
// 256-sample gap every cycle (loud crackle); the microphone's clock also
// drifts from the player's (0.3% on a desktop), which drained the fixed 50 ms
// cushion this used to write once within seconds. RateMatcher now holds the
// queue here instead, measured before every write.
constexpr double kQueueTargetMs = 10;
// The quantum each helper stream asks the sound server for. At 10 ms both
// pw-cat directions dropped 256-sample blocks on a desktop even when fed from
// a file (70-80 gaps in 9 s: the helpers are not real-time threads); 20 ms
// still dropped some, 30 ms and up none.
const QString kStreamLatency = QStringLiteral("40ms");
const QString kStreamLatencyMs = QStringLiteral("40");

#if defined(Q_OS_UNIX)
// A pipe for a player's stdin whose depth this side can measure (FIONREAD);
// QProcess keeps its own pipe out of reach. Both ends close on exec; the
// child's copy on fd 0 does not. Writes never block the audio thread.
bool makePipe(int fds[2])
{
#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
    if (::pipe2(fds, O_CLOEXEC) != 0)
        return false;
#else
    if (::pipe(fds) != 0)
        return false;
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
#endif
    ::fcntl(fds[1], F_SETFL, ::fcntl(fds[1], F_GETFL) | O_NONBLOCK);
#if defined(F_SETNOSIGPIPE)
    ::fcntl(fds[1], F_SETNOSIGPIPE, 1);
#endif
    return true;
}

// Writes what fits without blocking. A player that died must not take the app
// down with SIGPIPE (nothing ignores it process-wide), so the signal is held
// off on this thread for the write and a pending one is swallowed; the death
// itself arrives through QProcess::finished().
void writeNonBlocking(int fd, const char *data, size_t bytes)
{
    sigset_t pipeSignal;
    sigset_t previous;
    sigemptyset(&pipeSignal);
    sigaddset(&pipeSignal, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &pipeSignal, &previous);
    bool broken = false;
    while (bytes > 0) {
        const ssize_t wrote = ::write(fd, data, bytes);
        if (wrote < 0 && errno == EINTR)
            continue;
        if (wrote <= 0) {
            broken = wrote < 0 && errno == EPIPE;
            break;
        }
        data += wrote;
        bytes -= size_t(wrote);
    }
#if !defined(Q_OS_DARWIN)
    if (broken) {
        const timespec now{0, 0};
        sigtimedwait(&pipeSignal, nullptr, &now);
    }
#else
    Q_UNUSED(broken);
#endif
    pthread_sigmask(SIG_SETMASK, &previous, nullptr);
}
#endif

bool have(const char *program)
{
    return !QStandardPaths::findExecutable(QString::fromLatin1(program)).isEmpty();
}

QByteArray runTool(const QString &program, const QStringList &arguments,
                   bool *ok = nullptr)
{
    QProcess process;
    process.start(program, arguments);
    const bool finished = process.waitForStarted(kToolTimeoutMs) &&
                          process.waitForFinished(kToolTimeoutMs);
    if (!finished)
        process.kill();
    if (ok)
        *ok = finished && process.exitStatus() == QProcess::NormalExit &&
              process.exitCode() == 0;
    return process.readAllStandardOutput();
}

QVariantMap device(const QString &id, const QString &name, bool cable = false)
{
    return {{QStringLiteral("id"), id},
            {QStringLiteral("name"), name},
            {QStringLiteral("cable"), cable}};
}

bool ours(const QString &name)
{
    return name.startsWith(QLatin1String("forkmesh_voice"));
}

QVariantMap statsMap(const VoiceStats &s)
{
    return {{QStringLiteral("userPitchHz"), s.userPitchHz},
            {QStringLiteral("pitchHz"), s.pitchHz},
            {QStringLiteral("voiced"), s.voiced},
            {QStringLiteral("ratio"), s.ratio},
            {QStringLiteral("level"), s.level},
            {QStringLiteral("latencyMs"), s.latencyMs}};
}

VoiceParams paramsFrom(const QVariantMap &map)
{
    VoiceParams params;
    for (const ParamBounds &b : paramBounds()) {
        const QString key = QString::fromLatin1(b.name);
        setParamValue(params, b.name,
                      map.contains(key) ? map.value(key).toDouble() : b.fallback);
    }
    return params.clean();
}

} // namespace

AudioBackend detectBackend()
{
    const QString forced = QProcessEnvironment::systemEnvironment()
        .value(QStringLiteral("FORKMESH_VOICE_BACKEND")).toLower();
    if (forced == QLatin1String("pipewire")) return AudioBackend::PipeWire;
    if (forced == QLatin1String("pulseaudio")) return AudioBackend::PulseAudio;
    if (forced == QLatin1String("none")) return AudioBackend::None;
#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
    if (forced != QLatin1String("qt")) {
        if (have("pw-cat") && have("pw-loopback") && have("pw-dump"))
            return AudioBackend::PipeWire;
        if (have("parec") && have("pacat") && have("pactl"))
            return AudioBackend::PulseAudio;
    }
#endif
#ifdef FORKMESH_HAVE_MULTIMEDIA
    return AudioBackend::QtMultimedia;
#else
    return AudioBackend::None;
#endif
}

QString backendName(AudioBackend backend)
{
    switch (backend) {
    case AudioBackend::PipeWire: return QStringLiteral("PipeWire");
    case AudioBackend::PulseAudio: return QStringLiteral("PulseAudio");
    case AudioBackend::QtMultimedia: return QStringLiteral("system audio");
    case AudioBackend::None: break;
    }
    return QString();
}

bool backendMakesVirtualMicrophone(AudioBackend backend)
{
    return backend == AudioBackend::PipeWire || backend == AudioBackend::PulseAudio;
}

bool looksLikeVirtualCable(const QString &description)
{
    // Cable drivers differ per system and per vendor, so match what they all
    // call themselves rather than naming products: the pick is only a default
    // the person can change.
    static const QRegularExpression cable(
        QStringLiteral("virtual|cable|loopback"),
        QRegularExpression::CaseInsensitiveOption);
    return cable.match(description).hasMatch();
}

QVariantList parsePipeWireDevices(const QByteArray &pwDumpJson, bool inputs)
{
    QVariantList list;
    const QString wanted = inputs ? QStringLiteral("Audio/Source")
                                  : QStringLiteral("Audio/Sink");
    for (const QJsonValue &value : QJsonDocument::fromJson(pwDumpJson).array()) {
        const QJsonObject props = value.toObject()
            .value(QStringLiteral("info")).toObject()
            .value(QStringLiteral("props")).toObject();
        if (props.value(QStringLiteral("media.class")).toString() != wanted)
            continue;
        const QString name = props.value(QStringLiteral("node.name")).toString();
        if (name.isEmpty() || ours(name))
            continue;
        QString label = props.value(QStringLiteral("node.description")).toString();
        if (label.isEmpty())
            label = props.value(QStringLiteral("node.nick")).toString();
        if (label.isEmpty())
            label = name;
        list.append(device(name, label, !inputs && looksLikeVirtualCable(label)));
    }
    return list;
}

QVariantList parsePulseDevices(const QByteArray &pactlShort, bool inputs)
{
    // "index<TAB>name<TAB>driver<TAB>spec<TAB>state"
    QVariantList list;
    for (const QByteArray &line : pactlShort.split('\n')) {
        const QList<QByteArray> fields = line.split('\t');
        if (fields.size() < 2)
            continue;
        const QString name = QString::fromUtf8(fields.at(1)).trimmed();
        if (name.isEmpty() || ours(name) ||
            (inputs && name.endsWith(QLatin1String(".monitor"))))
            continue;
        list.append(device(name, name, !inputs && looksLikeVirtualCable(name)));
    }
    return list;
}

#ifdef FORKMESH_HAVE_MULTIMEDIA
struct VoiceAudioPipeline::QtStreams {
    QAudioSource *source = nullptr;
    QIODevice *in = nullptr;
    QAudioFormat inFormat;
    QAudioSink *sink = nullptr;
    QIODevice *out = nullptr;
    QAudioFormat outFormat;
    QAudioSink *monitorSink = nullptr;
    QIODevice *monitorOut = nullptr;
    QAudioFormat monitorFormat;
};

namespace {

QAudioDevice findDevice(const QList<QAudioDevice> &devices, const QString &id,
                        const QAudioDevice &fallback)
{
    for (const QAudioDevice &candidate : devices) {
        if (QString::fromUtf8(candidate.id()) == id)
            return candidate;
    }
    return fallback;
}

QAudioFormat formatFor(const QAudioDevice &device, int rate)
{
    QAudioFormat format;
    format.setSampleRate(rate);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Float);
    if (device.isFormatSupported(format))
        return format;
    format = device.preferredFormat();
    return format;
}

// Mono float at the device's rate (RateMatcher converts) -> its own sample
// format and channels.
QByteArray encode(const float *samples, int frames, const QAudioFormat &format)
{
    const int channels = std::max(1, format.channelCount());
    const int bytes = format.bytesPerSample();
    QByteArray out(frames * channels * bytes, Qt::Uninitialized);
    char *cursor = out.data();
    for (int i = 0; i < frames; ++i) {
        const double value = std::clamp(double(samples[i]), -1.0, 1.0);
        for (int c = 0; c < channels; ++c) {
            switch (format.sampleFormat()) {
            case QAudioFormat::Float: {
                const float v = float(value);
                std::memcpy(cursor, &v, 4);
                break;
            }
            case QAudioFormat::Int32: {
                const qint32 v = qint32(value * 2147483647.0);
                std::memcpy(cursor, &v, 4);
                break;
            }
            case QAudioFormat::Int16: {
                const qint16 v = qint16(value * 32767.0);
                std::memcpy(cursor, &v, 2);
                break;
            }
            default: {
                const quint8 v = quint8((value + 1) * 127.5);
                std::memcpy(cursor, &v, 1);
                break;
            }
            }
            cursor += bytes;
        }
    }
    return out;
}

// The device's frames -> mono float.
std::vector<float> decode(const QByteArray &raw, const QAudioFormat &format)
{
    const int channels = std::max(1, format.channelCount());
    const int bytes = std::max(1, format.bytesPerSample());
    const int frames = int(raw.size()) / (channels * bytes);
    std::vector<float> out(frames);
    const char *cursor = raw.constData();
    for (int i = 0; i < frames; ++i) {
        double sum = 0;
        for (int c = 0; c < channels; ++c) {
            switch (format.sampleFormat()) {
            case QAudioFormat::Float: {
                float v; std::memcpy(&v, cursor, 4); sum += v; break;
            }
            case QAudioFormat::Int32: {
                qint32 v; std::memcpy(&v, cursor, 4); sum += v / 2147483648.0; break;
            }
            case QAudioFormat::Int16: {
                qint16 v; std::memcpy(&v, cursor, 2); sum += v / 32768.0; break;
            }
            default: {
                quint8 v; std::memcpy(&v, cursor, 1); sum += v / 127.5 - 1; break;
            }
            }
            cursor += bytes;
        }
        out[i] = float(sum / channels);
    }
    return out;
}

} // namespace
#endif

VoiceAudioPipeline::VoiceAudioPipeline(QObject *parent) : QObject(parent)
{
}

VoiceAudioPipeline::~VoiceAudioPipeline()
{
    m_stopping = true;
    stop();
}

void VoiceAudioPipeline::listDevices()
{
    const AudioBackend backend = detectBackend();
    QVariantList inputs;
    QVariantList outputs;
    if (backend == AudioBackend::PipeWire) {
        const QByteArray dump = runTool(QStringLiteral("pw-dump"), {});
        inputs = parsePipeWireDevices(dump, true);
        outputs = parsePipeWireDevices(dump, false);
    } else if (backend == AudioBackend::PulseAudio) {
        inputs = parsePulseDevices(runTool(QStringLiteral("pactl"),
            {QStringLiteral("list"), QStringLiteral("short"), QStringLiteral("sources")}), true);
        outputs = parsePulseDevices(runTool(QStringLiteral("pactl"),
            {QStringLiteral("list"), QStringLiteral("short"), QStringLiteral("sinks")}), false);
    }
#ifdef FORKMESH_HAVE_MULTIMEDIA
    else if (backend == AudioBackend::QtMultimedia) {
        for (const QAudioDevice &d : QMediaDevices::audioInputs()) {
            if (!looksLikeVirtualCable(d.description()))
                inputs.append(device(QString::fromUtf8(d.id()), d.description()));
        }
        for (const QAudioDevice &d : QMediaDevices::audioOutputs())
            outputs.append(device(QString::fromUtf8(d.id()), d.description(),
                                  looksLikeVirtualCable(d.description())));
    }
#endif
    emit devicesListed({
        {QStringLiteral("backend"), backendName(backend)},
        {QStringLiteral("virtualMicrophone"), backendMakesVirtualMicrophone(backend)},
        {QStringLiteral("inputs"), inputs},
        {QStringLiteral("outputs"), outputs},
    });
}

QProcess *VoiceAudioPipeline::spawn(const QString &program,
                                    const QStringList &arguments, int stdinFd)
{
    auto *process = new QProcess(this);
#if defined(Q_OS_UNIX)
    process->setChildProcessModifier([stdinFd] {
#if defined(Q_OS_LINUX)
        // Take the helper down with this thread if the app dies mid-call, so
        // no orphaned stream keeps a virtual microphone alive.
        ::prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
        // Runs after QProcess has set up fd 0, so this pipe replaces it.
        if (stdinFd >= 0)
            ::dup2(stdinFd, STDIN_FILENO);
    });
#else
    Q_UNUSED(stdinFd);
#endif
    process->setProgram(program);
    process->setArguments(arguments);
    connect(process, &QProcess::finished, this, [this, process]() {
        if (m_stopping || !m_running)
            return;
        const QString detail = QString::fromUtf8(
            process->readAllStandardError()).trimmed().left(300);
        emit failed(QStringLiteral("%1 stopped%2").arg(
            process->program(), detail.isEmpty() ? QString() : QStringLiteral(": ") + detail));
        stop();
    });
    process->start();
    return process;
}

bool VoiceAudioPipeline::startVirtualMicrophone(QString *error)
{
    const QString sink = QString::fromLatin1(kVirtualSinkName);
    if (m_backend == AudioBackend::PipeWire) {
        m_loopback = spawn(QStringLiteral("pw-loopback"), {
            QStringLiteral("--name"), QStringLiteral("forkmesh-voice"),
            QStringLiteral("--channels"), QStringLiteral("1"),
            QStringLiteral("--capture-props=media.class=Audio/Sink node.name=%1 "
                           "node.description=\"%2 (output)\" audio.position=[MONO]")
                .arg(sink, QString::fromLatin1(kVirtualMicLabel)),
            QStringLiteral("--playback-props=media.class=Audio/Source node.name=%1 "
                           "node.description=\"%2\" audio.position=[MONO]")
                .arg(QString::fromLatin1(kVirtualMicName),
                     QString::fromLatin1(kVirtualMicLabel)),
        });
        if (!m_loopback->waitForStarted(kToolTimeoutMs)) {
            *error = QStringLiteral("pw-loopback could not start.");
            return false;
        }
        // pw-cat --target by name silently falls back to the default device
        // when the name is not there yet, which would play the changed voice
        // out of the speakers. Wait until the sink is really registered.
        for (int attempt = 0; attempt < 30; ++attempt) {
            const QByteArray dump = runTool(QStringLiteral("pw-dump"), {});
            if (dump.contains(QByteArray("\"") + kVirtualSinkName + "\""))
                return true;
            QThread::msleep(100);
        }
        *error = QStringLiteral("The ForkMesh Voice microphone did not appear in PipeWire.");
        return false;
    }
    // PulseAudio: clear modules a crashed run left behind, then load ours.
    const QByteArray modules = runTool(QStringLiteral("pactl"),
        {QStringLiteral("list"), QStringLiteral("short"), QStringLiteral("modules")});
    for (const QByteArray &line : modules.split('\n')) {
        if (line.contains("forkmesh_voice"))
            runTool(QStringLiteral("pactl"), {QStringLiteral("unload-module"),
                                              QString::fromUtf8(line.split('\t').value(0))});
    }
    bool ok = false;
    const QByteArray nullSink = runTool(QStringLiteral("pactl"), {
        QStringLiteral("load-module"), QStringLiteral("module-null-sink"),
        QStringLiteral("sink_name=%1").arg(sink),
        QStringLiteral("sink_properties=device.description=ForkMesh_Voice_Output"),
        QStringLiteral("rate=%1").arg(kSampleRate), QStringLiteral("channels=1")}, &ok);
    if (!ok) {
        *error = QStringLiteral("PulseAudio refused the ForkMesh Voice output.");
        return false;
    }
    m_pulseModules << QString::fromUtf8(nullSink).trimmed();
    const QByteArray remap = runTool(QStringLiteral("pactl"), {
        QStringLiteral("load-module"), QStringLiteral("module-remap-source"),
        QStringLiteral("master=%1.monitor").arg(sink),
        QStringLiteral("source_name=%1").arg(QString::fromLatin1(kVirtualMicName)),
        QStringLiteral("source_properties=device.description=ForkMesh_Voice")}, &ok);
    if (!ok) {
        *error = QStringLiteral("PulseAudio refused the ForkMesh Voice microphone.");
        return false;
    }
    m_pulseModules << QString::fromUtf8(remap).trimmed();
    return true;
}

void VoiceAudioPipeline::stopVirtualMicrophone()
{
    if (m_loopback) {
        m_loopback->terminate();
        if (!m_loopback->waitForFinished(1000))
            m_loopback->kill();
        m_loopback->deleteLater();
    }
    for (auto it = m_pulseModules.crbegin(); it != m_pulseModules.crend(); ++it) {
        if (!it->isEmpty())
            runTool(QStringLiteral("pactl"), {QStringLiteral("unload-module"), *it});
    }
    m_pulseModules.clear();
}

bool VoiceAudioPipeline::startProcessStreams(const QVariantMap &config,
                                             QString *error)
{
    const QString input = config.value(QStringLiteral("input")).toString();
    const bool virtualMic = config.value(QStringLiteral("virtualMicrophone")).toBool();
    const bool monitor = config.value(QStringLiteral("monitor")).toBool();
    const QString rate = QString::number(kSampleRate);
    const bool pipewire = m_backend == AudioBackend::PipeWire;
    auto capture = [&]() {
        QStringList args = pipewire
            ? QStringList{QStringLiteral("--record"), QStringLiteral("--raw"),
                          QStringLiteral("--format"), QStringLiteral("f32"),
                          QStringLiteral("--rate"), rate,
                          QStringLiteral("--channels"), QStringLiteral("1"),
                          QStringLiteral("--latency"), kStreamLatency}
            : QStringList{QStringLiteral("--raw"), QStringLiteral("--format=float32le"),
                          QStringLiteral("--rate=") + rate, QStringLiteral("--channels=1"),
                          QStringLiteral("--latency-msec=") + kStreamLatencyMs};
        if (!input.isEmpty() && !ours(input))
            args << (pipewire ? QStringList{QStringLiteral("--target"), input}
                              : QStringList{QStringLiteral("--device=") + input});
        if (pipewire)
            args << QStringLiteral("-");
        return spawn(pipewire ? QStringLiteral("pw-cat") : QStringLiteral("parec"), args);
    };
    auto playback = [&](const QString &target, Playback &slot) {
        QStringList args = pipewire
            ? QStringList{QStringLiteral("--playback"), QStringLiteral("--raw"),
                          QStringLiteral("--format"), QStringLiteral("f32"),
                          QStringLiteral("--rate"), rate,
                          QStringLiteral("--channels"), QStringLiteral("1"),
                          QStringLiteral("--latency"), kStreamLatency}
            : QStringList{QStringLiteral("--playback"), QStringLiteral("--raw"),
                          QStringLiteral("--format=float32le"),
                          QStringLiteral("--rate=") + rate, QStringLiteral("--channels=1"),
                          QStringLiteral("--latency-msec=") + kStreamLatencyMs};
        if (!target.isEmpty())
            args << (pipewire ? QStringList{QStringLiteral("--target"), target}
                              : QStringList{QStringLiteral("--device=") + target});
        if (pipewire)
            args << QStringLiteral("-");
        int fds[2] = {-1, -1};
#if defined(Q_OS_UNIX)
        if (!makePipe(fds))
            fds[0] = fds[1] = -1;
#endif
        slot.process = spawn(pipewire ? QStringLiteral("pw-cat") : QStringLiteral("pacat"),
                             args, fds[0]);
        slot.process->setStandardOutputFile(QProcess::nullDevice());
#if defined(Q_OS_UNIX)
        // The child has its copy now (QProcess forks inside start()).
        if (fds[0] >= 0)
            ::close(fds[0]);
#endif
        slot.fd = fds[1];
        slot.matcher = std::make_unique<RateMatcher>(kSampleRate, kSampleRate,
                                                     kQueueTargetMs);
    };
    if (virtualMic)
        playback(QString::fromLatin1(kVirtualSinkName), m_output);
    if (monitor)
        playback(QString(), m_monitor);
    m_capture = capture();
    connect(m_capture, &QProcess::readyReadStandardOutput, this, [this]() {
        if (m_capture)
            consume(m_capture->readAllStandardOutput());
    });
    for (QProcess *process : {m_capture.data(), m_output.process.data(),
                              m_monitor.process.data()}) {
        if (process && !process->waitForStarted(kToolTimeoutMs)) {
            *error = QStringLiteral("%1 could not start.").arg(process->program());
            return false;
        }
    }
    return true;
}

bool VoiceAudioPipeline::startQtStreams(const QVariantMap &config, QString *error)
{
#ifdef FORKMESH_HAVE_MULTIMEDIA
    m_qt = std::make_unique<QtStreams>();
    const QAudioDevice input = findDevice(QMediaDevices::audioInputs(),
        config.value(QStringLiteral("input")).toString(),
        QMediaDevices::defaultAudioInput());
    if (input.isNull()) {
        *error = QStringLiteral("No microphone was found.");
        return false;
    }
    m_qt->inFormat = formatFor(input, kSampleRate);
    m_rate = m_qt->inFormat.sampleRate();
    m_qt->source = new QAudioSource(input, m_qt->inFormat, this);
    m_qt->source->setBufferSize(m_qt->inFormat.bytesForDuration(40000));
    const QString outputId = config.value(QStringLiteral("output")).toString();
    if (config.value(QStringLiteral("virtualMicrophone")).toBool()) {
        const QAudioDevice cable = findDevice(QMediaDevices::audioOutputs(), outputId,
                                              QAudioDevice());
        if (cable.isNull()) {
            *error = QStringLiteral(
                "Pick the virtual cable to send the changed voice into.");
            return false;
        }
        m_qt->outFormat = formatFor(cable, m_rate);
        m_qt->sink = new QAudioSink(cable, m_qt->outFormat, this);
        // Room well past the matcher's ceiling, so a write is never cut short.
        m_qt->sink->setBufferSize(m_qt->outFormat.bytesForDuration(300000));
        m_qt->out = m_qt->sink->start();
        m_output.matcher = std::make_unique<RateMatcher>(
            m_rate, m_qt->outFormat.sampleRate(), kQueueTargetMs);
    }
    if (config.value(QStringLiteral("monitor")).toBool()) {
        const QAudioDevice speakers = QMediaDevices::defaultAudioOutput();
        m_qt->monitorFormat = formatFor(speakers, m_rate);
        m_qt->monitorSink = new QAudioSink(speakers, m_qt->monitorFormat, this);
        m_qt->monitorSink->setBufferSize(m_qt->monitorFormat.bytesForDuration(300000));
        m_qt->monitorOut = m_qt->monitorSink->start();
        m_monitor.matcher = std::make_unique<RateMatcher>(
            m_rate, m_qt->monitorFormat.sampleRate(), kQueueTargetMs);
    }
    m_qt->in = m_qt->source->start();
    if (!m_qt->in) {
        *error = QStringLiteral("The microphone could not be opened.");
        return false;
    }
    connect(m_qt->in, &QIODevice::readyRead, this, [this]() {
        if (!m_qt || !m_qt->in)
            return;
        const std::vector<float> mono = decode(m_qt->in->readAll(), m_qt->inFormat);
        consume(QByteArray(reinterpret_cast<const char *>(mono.data()),
                           qsizetype(mono.size() * sizeof(float))));
    });
    return true;
#else
    Q_UNUSED(config);
    *error = QStringLiteral("This build of ForkMesh has no audio support (Qt Multimedia).");
    return false;
#endif
}

void VoiceAudioPipeline::start(const QVariantMap &config)
{
    if (m_running)
        stop();
    m_stopping = false;
    m_backend = detectBackend();
    m_rate = kSampleRate;
    QString error;
    if (m_backend == AudioBackend::None) {
        emit failed(QStringLiteral(
            "No audio system was found. On Linux, install PipeWire (pw-cat) or "
            "PulseAudio (pactl, parec, pacat)."));
        return;
    }
    const bool virtualMic = config.value(QStringLiteral("virtualMicrophone")).toBool();
    m_running = true;
    bool ok = true;
    if (virtualMic && backendMakesVirtualMicrophone(m_backend))
        ok = startVirtualMicrophone(&error);
    if (ok) {
        ok = m_backend == AudioBackend::QtMultimedia
            ? startQtStreams(config, &error)
            : startProcessStreams(config, &error);
    }
    if (!ok) {
        emit failed(error);
        stop();
        return;
    }
    m_engine = std::make_unique<VoiceEngine>(
        double(m_rate), config.value(QStringLiteral("userPitchHz")).toDouble());
    m_engine->setParams(paramsFrom(config.value(QStringLiteral("params")).toMap()));
    m_engine->setBypass(config.value(QStringLiteral("bypass")).toBool());
    m_engine->setNoiseReduction(
        config.value(QStringLiteral("noiseReduction"), true).toBool() ? 1 : 0);
    if (!m_statsTimer) {
        m_statsTimer = new QTimer(this);
        m_statsTimer->setInterval(250);
        connect(m_statsTimer, &QTimer::timeout, this, &VoiceAudioPipeline::emitStats);
    }
    m_statsTimer->start();
    QString where;
    if (!virtualMic)
        where = QStringLiteral("Preview: you hear the changed voice; calls do not.");
    else if (backendMakesVirtualMicrophone(m_backend))
        where = QStringLiteral("Live on the \"%1\" microphone.").arg(
            QString::fromLatin1(kVirtualMicLabel));
    else
        where = QStringLiteral("Live on the virtual cable.");
    emit started(where);
}

void VoiceAudioPipeline::stop()
{
    const bool wasRunning = m_running;
    m_stopping = true;
    m_running = false;
    if (m_statsTimer)
        m_statsTimer->stop();
    if (QProcess *process = m_capture.data()) {
        process->disconnect(this);
        process->closeWriteChannel();
        process->terminate();
        if (!process->waitForFinished(800))
            process->kill();
        process->deleteLater();
    }
    m_capture = nullptr;
    closePlayback(m_output);
    closePlayback(m_monitor);
#ifdef FORKMESH_HAVE_MULTIMEDIA
    if (m_qt) {
        if (m_qt->source) m_qt->source->stop();
        if (m_qt->sink) m_qt->sink->stop();
        if (m_qt->monitorSink) m_qt->monitorSink->stop();
        delete m_qt->source;
        delete m_qt->sink;
        delete m_qt->monitorSink;
        m_qt.reset();
    }
#endif
    stopVirtualMicrophone();
    m_pending.clear();
    if (!m_recordTag.isEmpty())
        finishRecording();
    m_engine.reset();
    if (wasRunning)
        emit stopped();
}

void VoiceAudioPipeline::closePlayback(Playback &playback)
{
#if defined(Q_OS_UNIX)
    // End of input: the player drains what it has and exits.
    if (playback.fd >= 0)
        ::close(playback.fd);
#endif
    playback.fd = -1;
    if (QProcess *process = playback.process.data()) {
        process->disconnect(this);
        process->closeWriteChannel();
        process->terminate();
        if (!process->waitForFinished(800))
            process->kill();
        process->deleteLater();
    }
    playback.process = nullptr;
    playback.matcher.reset();
}

void VoiceAudioPipeline::setParams(const QVariantMap &params)
{
    if (m_engine)
        m_engine->setParams(paramsFrom(params));
}

void VoiceAudioPipeline::setBypass(bool bypass)
{
    if (m_engine)
        m_engine->setBypass(bypass);
}

void VoiceAudioPipeline::setNoiseReduction(bool on)
{
    if (m_engine)
        m_engine->setNoiseReduction(on ? 1 : 0);
}

void VoiceAudioPipeline::record(int seconds, const QString &tag)
{
    if (!m_running) {
        emit failed(QStringLiteral("Start the voice changer to record."));
        return;
    }
    m_recordTag = tag;
    m_recording.clear();
    m_recordBytesWanted = qint64(std::clamp(seconds, 1, 30)) * m_rate * 4;
}

void VoiceAudioPipeline::finishRecording()
{
    const QString tag = m_recordTag;
    m_recordTag.clear();
    emit recorded(tag, m_recording, m_rate);
    m_recording.clear();
}

void VoiceAudioPipeline::consume(const QByteArray &raw)
{
    if (!m_engine)
        return;
    m_pending.append(raw);
    const int count = int(m_pending.size() / 4);
    if (!count)
        return;
    std::vector<float> samples(count);
    std::memcpy(samples.data(), m_pending.constData(), size_t(count) * 4);
    m_pending.remove(0, qsizetype(count) * 4);
    if (!m_recordTag.isEmpty()) {
        const qint64 take = std::min<qint64>(m_recordBytesWanted - m_recording.size(),
                                             qint64(count) * 4);
        m_recording.append(reinterpret_cast<const char *>(samples.data()), take);
        if (m_recording.size() >= m_recordBytesWanted)
            finishRecording();
    }
    m_engine->process(samples.data(), samples.data(), count);
    writeOut(samples.data(), count);
}

void VoiceAudioPipeline::writeOut(const float *samples, int count)
{
    for (Playback *playback : {&m_output, &m_monitor}) {
        QProcess *process = playback->process.data();
        if (!process || !playback->matcher || process->state() != QProcess::Running)
            continue;
#if defined(Q_OS_UNIX)
        if (playback->fd >= 0) {
            int queuedBytes = 0;
            const int queued = ::ioctl(playback->fd, FIONREAD, &queuedBytes) == 0
                ? queuedBytes / 4 : -1;
            playback->matcher->process(samples, count, queued, m_matched);
            writeNonBlocking(playback->fd, reinterpret_cast<const char *>(m_matched.data()),
                             m_matched.size() * sizeof(float));
            continue;
        }
#endif
        if (process->bytesToWrite() >= kMaxQueuedBytes)
            continue;
        playback->matcher->process(samples, count, -1, m_matched);
        process->write(reinterpret_cast<const char *>(m_matched.data()),
                       qint64(m_matched.size() * sizeof(float)));
    }
#ifdef FORKMESH_HAVE_MULTIMEDIA
    if (m_qt) {
        auto play = [&](QAudioSink *sink, QIODevice *device, const QAudioFormat &format,
                        RateMatcher *matcher) {
            if (!sink || !device || !matcher)
                return;
            const int frameBytes = std::max(1, format.bytesPerFrame());
            const int queued = int((sink->bufferSize() - sink->bytesFree()) / frameBytes);
            matcher->process(samples, count, std::max(0, queued), m_matched);
            device->write(encode(m_matched.data(), int(m_matched.size()), format));
        };
        play(m_qt->sink, m_qt->out, m_qt->outFormat, m_output.matcher.get());
        play(m_qt->monitorSink, m_qt->monitorOut, m_qt->monitorFormat,
             m_monitor.matcher.get());
    }
#endif
}

void VoiceAudioPipeline::emitStats()
{
    if (m_engine)
        emit statsReady(statsMap(m_engine->stats()));
}

// --- WAV input -----------------------------------------------------------------

bool readWav(const QByteArray &bytes, std::vector<float> *samples,
             int *sampleRate, QString *error)
{
    auto u16 = [&](qsizetype at) {
        return quint16(quint8(bytes[at])) | quint16(quint8(bytes[at + 1])) << 8;
    };
    auto u32 = [&](qsizetype at) {
        return quint32(u16(at)) | quint32(u16(at + 2)) << 16;
    };
    if (bytes.size() < 44 || !bytes.startsWith("RIFF") || bytes.mid(8, 4) != "WAVE") {
        *error = QStringLiteral("That is not a WAV file.");
        return false;
    }
    int format = 0, channels = 0, bits = 0;
    qsizetype data = -1, dataSize = 0;
    for (qsizetype at = 12; at + 8 <= bytes.size();) {
        const QByteArray id = bytes.mid(at, 4);
        const qsizetype size = qsizetype(u32(at + 4));
        if (id == "fmt " && at + 24 <= bytes.size()) {
            format = u16(at + 8);
            channels = u16(at + 10);
            *sampleRate = int(u32(at + 12));
            bits = u16(at + 22);
            if (format == 0xFFFE && at + 34 <= bytes.size())
                format = u16(at + 32); // WAVE_FORMAT_EXTENSIBLE sub-format
        } else if (id == "data") {
            data = at + 8;
            dataSize = std::min(size, bytes.size() - data);
            break;
        }
        at += 8 + size + (size & 1);
    }
    const bool pcm = format == 1 && (bits == 16 || bits == 24 || bits == 32);
    const bool flt = format == 3 && bits == 32;
    if (data < 0 || channels < 1 || *sampleRate < 8000 || (!pcm && !flt)) {
        *error = QStringLiteral("Use a 16/24/32-bit PCM or 32-bit float WAV file.");
        return false;
    }
    const int width = bits / 8;
    const qsizetype frames = dataSize / (width * channels);
    samples->assign(size_t(frames), 0.f);
    const char *base = bytes.constData() + data;
    for (qsizetype i = 0; i < frames; ++i) {
        double sum = 0;
        for (int c = 0; c < channels; ++c) {
            const char *p = base + (i * channels + c) * width;
            if (flt) {
                float v;
                std::memcpy(&v, p, 4);
                sum += v;
            } else if (bits == 16) {
                qint16 v;
                std::memcpy(&v, p, 2);
                sum += v / 32768.0;
            } else if (bits == 24) {
                const qint32 v = (qint32(quint8(p[0])) | qint32(quint8(p[1])) << 8 |
                                  qint32(qint8(p[2])) << 16);
                sum += v / 8388608.0;
            } else {
                qint32 v;
                std::memcpy(&v, p, 4);
                sum += v / 2147483648.0;
            }
        }
        (*samples)[size_t(i)] = float(sum / channels);
    }
    return true;
}

std::vector<float> resample(const std::vector<float> &samples, int from, int to)
{
    if (from == to || samples.empty() || from <= 0 || to <= 0)
        return samples;
    const size_t count = size_t(double(samples.size()) * to / from);
    std::vector<float> out(count);
    for (size_t i = 0; i < count; ++i) {
        const double position = double(i) * from / to;
        const size_t low = std::min(samples.size() - 1, size_t(position));
        const size_t high = std::min(samples.size() - 1, low + 1);
        out[i] = float(samples[low] + (samples[high] - samples[low]) * (position - double(low)));
    }
    return out;
}

} // namespace forkmesh::voice
