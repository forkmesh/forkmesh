// The Voice Changer's audio path with no sound card: the device parsers, the
// WAV reader, and a whole run of VoiceAudioPipeline against stand-in audio
// tools (a fake pw-cat, pw-loopback and pw-dump that tests/test_voice_changer.py
// writes into a directory and puts first on PATH).
//
// Usage: test_voice_audio [<fake-tools-dir> <captured-output-file>]
// Exits non-zero on a failure and prints one JSON line of measurements.

#include "../VoiceAudio.h"

#include <QCoreApplication>
#include <QFile>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace forkmesh::voice;

namespace {

int failures = 0;

void check(bool ok, const char *what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

QByteArray wav16(const std::vector<float> &samples, int rate, int channels)
{
    QByteArray data;
    for (float s : samples) {
        for (int c = 0; c < channels; ++c) {
            const qint16 v = qint16(std::lround(s * 32767));
            data.append(reinterpret_cast<const char *>(&v), 2);
        }
    }
    auto le32 = [](quint32 v) { return QByteArray(reinterpret_cast<const char *>(&v), 4); };
    auto le16 = [](quint16 v) { return QByteArray(reinterpret_cast<const char *>(&v), 2); };
    QByteArray out = "RIFF" + le32(quint32(36 + data.size())) + "WAVE";
    out += "fmt " + le32(16) + le16(1) + le16(quint16(channels)) + le32(quint32(rate)) +
           le32(quint32(rate * channels * 2)) + le16(quint16(channels * 2)) + le16(16);
    out += "LIST" + le32(4) + "INFO"; // a chunk readers must skip
    out += "data" + le32(quint32(data.size())) + data;
    return out;
}

double medianPitch(const std::vector<float> &signal, double rate)
{
    const int step = 4;
    std::vector<double> dec(signal.size() / step);
    for (size_t i = 0; i < dec.size(); ++i) {
        double s = 0;
        for (int j = 0; j < step; ++j)
            s += signal[i * step + j];
        dec[i] = s / step;
    }
    PitchDetector detector(rate / step, 1024);
    std::vector<double> found;
    for (size_t s = 0; s + 1024 <= dec.size(); s += 256) {
        const double hz = detector.detect(dec.data() + s);
        if (hz > 0)
            found.push_back(hz);
    }
    if (found.empty())
        return 0;
    std::sort(found.begin(), found.end());
    return found[found.size() / 2];
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    // --- device parsers ---
    const QByteArray dump = R"([
      {"id": 40, "info": {"props": {"media.class": "Audio/Source", "node.name": "alsa_input.usb",
                                     "node.description": "USB Microphone"}}},
      {"id": 41, "info": {"props": {"media.class": "Audio/Source", "node.name": "forkmesh_voice_mic",
                                     "node.description": "ForkMesh Voice"}}},
      {"id": 42, "info": {"props": {"media.class": "Audio/Sink", "node.name": "alsa_output.hdmi",
                                     "node.description": "HDMI Audio"}}},
      {"id": 43, "info": {"props": {"media.class": "Audio/Sink", "node.name": "forkmesh_voice_sink"}}},
      {"id": 44, "info": {"props": {"media.class": "Video/Source", "node.name": "cam"}}}
    ])";
    const QVariantList pwInputs = parsePipeWireDevices(dump, true);
    const QVariantList pwOutputs = parsePipeWireDevices(dump, false);
    check(pwInputs.size() == 1 && pwInputs[0].toMap().value("id") == "alsa_input.usb" &&
          pwInputs[0].toMap().value("name") == "USB Microphone",
          "PipeWire inputs list real microphones only, never ForkMesh Voice");
    check(pwOutputs.size() == 1 && pwOutputs[0].toMap().value("id") == "alsa_output.hdmi",
          "PipeWire outputs skip the package's own sink");
    const QByteArray pactl =
        "1\talsa_input.pci.analog-stereo\tmodule-alsa-card.c\ts16le 2ch 44100Hz\tSUSPENDED\n"
        "2\talsa_output.pci.analog-stereo.monitor\tmodule-alsa-card.c\ts16le 2ch\tIDLE\n"
        "3\tforkmesh_voice_mic\tmodule-remap-source.c\tfloat32le 1ch 48000Hz\tRUNNING\n";
    const QVariantList pulseInputs = parsePulseDevices(pactl, true);
    check(pulseInputs.size() == 1 &&
          pulseInputs[0].toMap().value("id") == "alsa_input.pci.analog-stereo",
          "PulseAudio inputs skip monitors and ForkMesh Voice");
    check(looksLikeVirtualCable("Virtual Audio Cable (Input)") &&
          looksLikeVirtualCable("Loopback Device") &&
          !looksLikeVirtualCable("Built-in Speakers"),
          "cable outputs are recognised by what every vendor calls them");

    // --- WAV reader ---
    std::vector<float> tone(22050);
    for (size_t i = 0; i < tone.size(); ++i)
        tone[i] = float(0.5 * std::sin(2 * M_PI * 220 * double(i) / 22050));
    std::vector<float> read;
    int rate = 0;
    QString error;
    check(readWav(wav16(tone, 22050, 2), &read, &rate, &error) && rate == 22050 &&
          read.size() == tone.size() && std::abs(read[100] - tone[100]) < 1e-3,
          "a stereo 16-bit WAV reads back as mono floats");
    check(!readWav(QByteArray("not a wav file at all, but long enough to parse......"),
                   &read, &rate, &error) && !error.isEmpty(),
          "anything else is refused with a reason");
    check(resample(tone, 22050, 48000).size() == 48000, "resampling keeps the duration");

    // --- a live run against the stand-in tools ---
    double outputPitch = -1;
    qint64 outputBytes = -1;
    bool started = false;
    bool stoppedCleanly = false;
    if (argc >= 3) {
        const QString tools = QString::fromLocal8Bit(argv[1]);
        const QString captured = QString::fromLocal8Bit(argv[2]);
        qputenv("PATH", (tools + ":" + qgetenv("PATH")).toLocal8Bit());
        qputenv("FORKMESH_VOICE_BACKEND", "pipewire");
        QThread thread;
        thread.start();
        auto *pipeline = new VoiceAudioPipeline;
        pipeline->moveToThread(&thread);
        QObject::connect(pipeline, &VoiceAudioPipeline::started, &app,
                         [&](const QString &) { started = true; });
        QObject::connect(pipeline, &VoiceAudioPipeline::failed, &app, [](const QString &e) {
            std::fprintf(stderr, "pipeline: %s\n", qPrintable(e));
        });
        const QVariantMap config{
            {"input", "alsa_input.usb"},
            {"virtualMicrophone", true},
            {"monitor", false},
            {"params", QVariantMap{{"targetPitchHz", 220}, {"formant", 1.2}}},
            {"userPitchHz", 120},
        };
        QMetaObject::invokeMethod(pipeline, [pipeline, config]() { pipeline->start(config); });
        QTimer::singleShot(4000, &app, [&]() {
            QMetaObject::invokeMethod(pipeline, [pipeline]() {
                pipeline->stop();
                delete pipeline;
            }, Qt::BlockingQueuedConnection);
            thread.quit();
            thread.wait();
            app.quit();
        });
        app.exec();
        QFile file(captured);
        if (file.open(QIODevice::ReadOnly)) {
            const QByteArray bytes = file.readAll();
            outputBytes = bytes.size();
            std::vector<float> out(size_t(bytes.size() / 4));
            std::memcpy(out.data(), bytes.constData(), out.size() * 4);
            // Skip the first second: the engine's pitch glide settles there.
            if (out.size() > 48000)
                out.erase(out.begin(), out.begin() + 48000);
            outputPitch = medianPitch(out, 48000);
        }
        QFile log(tools + "/calls.log");
        const QByteArray calls = log.open(QIODevice::ReadOnly) ? log.readAll() : QByteArray();
        stoppedCleanly = calls.contains("loopback-stopped");
        check(started, "the pipeline starts");
        check(calls.contains("pw-loopback") && calls.contains("node.name=forkmesh_voice_sink") &&
              calls.contains("node.name=forkmesh_voice_mic") &&
              calls.contains("node.description=\"ForkMesh Voice\""),
              "a ForkMesh Voice microphone is created");
        check(calls.contains("--record") && calls.contains("--target alsa_input.usb"),
              "the chosen microphone is recorded");
        check(calls.contains("--playback") && calls.contains("--target forkmesh_voice_sink"),
              "the changed voice goes to the ForkMesh Voice sink");
        check(outputBytes > 48000 * 4, "changed audio reached the virtual microphone");
        check(std::abs(outputPitch - 220) < 0.07 * 220, "the voice reached its target pitch");
        check(stoppedCleanly, "stopping takes the virtual microphone down");
    }

    std::printf("{\"outputBytes\":%lld,\"outputPitch\":%.1f,\"started\":%s,"
                "\"stoppedCleanly\":%s,\"failures\":%d}\n",
                static_cast<long long>(outputBytes), outputPitch,
                started ? "true" : "false", stoppedCleanly ? "true" : "false", failures);
    return failures ? 1 : 0;
}
