#pragma once

// The Voice Changer's audio path: the real microphone in, VoiceEngine in the
// middle, and out to a microphone other apps can pick.
//
// A call or meeting app picks a microphone from the system's list, so the
// changed voice has to become one:
//
// * Linux: the package creates a virtual microphone called "ForkMesh Voice"
//   while it runs (PipeWire: one pw-loopback process; PulseAudio: a null sink
//   plus a remapped source through pactl) and streams raw audio through
//   pw-cat or parec/pacat. Nothing to install on a normal desktop.
// * Windows and macOS have no user-space virtual microphone, so the changed
//   voice is played through Qt Multimedia into a virtual audio cable the user
//   installs once, and the call app picks the cable's microphone side.
//
// The pipeline lives on its own thread (VoiceChangerFeature owns it), so a
// busy window never makes the voice stutter. Every call into it is queued.

#include "VoiceEngine.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <memory>
#include <vector>

class QProcess;
class QTimer;
class QIODevice;

namespace forkmesh::voice {

// How audio reaches the system on this machine, decided once per start.
enum class AudioBackend { None, PipeWire, PulseAudio, QtMultimedia };

AudioBackend detectBackend();
QString backendName(AudioBackend backend);
// True when the backend makes its own virtual microphone (Linux).
bool backendMakesVirtualMicrophone(AudioBackend backend);

// Names the package gives its virtual devices; never used as the input.
inline constexpr const char *kVirtualSinkName = "forkmesh_voice_sink";
inline constexpr const char *kVirtualMicName = "forkmesh_voice_mic";
inline constexpr const char *kVirtualMicLabel = "ForkMesh Voice";

// An output that looks like a virtual audio cable: somewhere the changed
// voice can be played on systems without a virtual microphone of our own.
bool looksLikeVirtualCable(const QString &description);

// The parsers behind device listing, exposed for the tests.
QVariantList parsePipeWireDevices(const QByteArray &pwDumpJson, bool inputs);
QVariantList parsePulseDevices(const QByteArray &pactlShort, bool inputs);

// Reads a PCM or float WAV file into mono floats (the "voice to match").
bool readWav(const QByteArray &bytes, std::vector<float> *samples,
             int *sampleRate, QString *error);
// Linear resampling, enough to line a recording up with the engine's rate.
std::vector<float> resample(const std::vector<float> &samples, int from, int to);

class VoiceAudioPipeline : public QObject
{
    Q_OBJECT
public:
    explicit VoiceAudioPipeline(QObject *parent = nullptr);
    ~VoiceAudioPipeline() override;

    static constexpr int kSampleRate = 48000;

public slots:
    // {inputs: [{id, name}], outputs: [{id, name, cable}], backend, virtualMicrophone}
    void listDevices();
    // config: input, output, virtualMicrophone (bool), monitor (bool),
    // bypass (bool), noiseReduction (bool, default on), params (QVariantMap
    // of VoiceParams), userPitchHz
    void start(const QVariantMap &config);
    void stop();
    void setParams(const QVariantMap &params);
    void setBypass(bool bypass);
    void setNoiseReduction(bool on);
    // Hands back the next `seconds` of the raw (unchanged) microphone as
    // native float32 samples through recorded().
    void record(int seconds, const QString &tag);

signals:
    void devicesListed(const QVariantMap &devices);
    void started(const QString &description);
    void failed(const QString &error);
    void stopped();
    void statsReady(const QVariantMap &stats);
    void recorded(const QString &tag, const QByteArray &samples, int sampleRate);

private:
    bool startVirtualMicrophone(QString *error);
    void stopVirtualMicrophone();
    bool startProcessStreams(const QVariantMap &config, QString *error);
    bool startQtStreams(const QVariantMap &config, QString *error);
    // One stream the changed voice is played into, and the rate matcher that
    // keeps its queue steady (see RateMatcher).
    struct Playback {
        QPointer<QProcess> process;
        int fd = -1;  // our end of the player's stdin pipe, measured and written
        std::unique_ptr<RateMatcher> matcher;
    };

    QProcess *spawn(const QString &program, const QStringList &arguments,
                    int stdinFd = -1);
    void closePlayback(Playback &playback);
    void consume(const QByteArray &raw);
    void emitStats();
    void writeOut(const float *samples, int count);
    void finishRecording();

    AudioBackend m_backend = AudioBackend::None;
    std::unique_ptr<VoiceEngine> m_engine;
    QByteArray m_pending;
    QPointer<QProcess> m_capture;
    Playback m_output;
    Playback m_monitor;
    std::vector<float> m_matched;
    QPointer<QProcess> m_loopback;
    QStringList m_pulseModules;
    QTimer *m_statsTimer = nullptr;
    bool m_running = false;
    bool m_stopping = false;
    QString m_recordTag;
    QByteArray m_recording;
    qint64 m_recordBytesWanted = 0;
    int m_rate = kSampleRate;
#ifdef FORKMESH_HAVE_MULTIMEDIA
    struct QtStreams;
    std::unique_ptr<QtStreams> m_qt;
#endif
};

} // namespace forkmesh::voice
