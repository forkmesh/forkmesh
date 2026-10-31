#include "PokerSound.h"

#include "PokerSynth.h"

#include <QIODevice>
#include <QMutex>
#include <QMutexLocker>
#include <QTimer>

#include <algorithm>
#include <cstring>

#ifdef FORKMESH_HAVE_MULTIMEDIA
#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QMediaDevices>
#endif

namespace forkmesh::poker {

class PokerMixer final : public QIODevice
{
public:
    struct Voice {
        std::shared_ptr<std::vector<float>> samples;
        std::size_t position = 0;
        float volume = 1.0f;
    };

    PokerMixer(int channels, bool floatSamples, QObject *parent)
        : QIODevice(parent), m_channels(std::max(1, channels)), m_float(floatSamples)
    {
    }

    void add(const std::shared_ptr<std::vector<float>> &samples, float volume)
    {
        QMutexLocker lock(&m_mutex);
        if (m_voices.size() >= 12)
            m_voices.erase(m_voices.begin());
        m_voices.push_back({samples, 0, volume});
    }

    void clear()
    {
        QMutexLocker lock(&m_mutex);
        m_voices.clear();
    }

    bool idle()
    {
        QMutexLocker lock(&m_mutex);
        return m_voices.empty();
    }

    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return 4096 + QIODevice::bytesAvailable(); }

protected:
    qint64 readData(char *data, qint64 maxSize) override
    {
        const int sampleBytes = m_float ? int(sizeof(float)) : int(sizeof(qint16));
        const qint64 frameBytes = qint64(sampleBytes) * m_channels;
        const qint64 frames = maxSize / frameBytes;
        if (frames <= 0)
            return 0;
        std::vector<float> mix(std::size_t(frames), 0.0f);
        {
            QMutexLocker lock(&m_mutex);
            for (Voice &voice : m_voices) {
                const std::vector<float> &source = *voice.samples;
                const std::size_t left = source.size() - std::min(source.size(), voice.position);
                const std::size_t count = std::min<std::size_t>(left, std::size_t(frames));
                for (std::size_t index = 0; index < count; ++index)
                    mix[index] += source[voice.position + index] * voice.volume;
                voice.position += count;
            }
            m_voices.erase(std::remove_if(m_voices.begin(), m_voices.end(),
                                          [](const Voice &voice) { return voice.position >= voice.samples->size(); }),
                           m_voices.end());
        }
        for (float &sample : mix)
            sample = std::clamp(sample, -1.0f, 1.0f);
        const QByteArray bytes = m_float ? pcmFloat(mix, m_channels) : pcm16(mix, m_channels);
        std::memcpy(data, bytes.constData(), std::size_t(bytes.size()));
        return bytes.size();
    }

    qint64 writeData(const char *, qint64) override { return -1; }

private:
    QMutex m_mutex;
    std::vector<Voice> m_voices;
    int m_channels = 1;
    bool m_float = false;
};

struct PokerSound::Output {
#ifdef FORKMESH_HAVE_MULTIMEDIA
    QAudioSink *sink = nullptr;
#endif
    PokerMixer *mixer = nullptr;
};

PokerSound::PokerSound(QObject *parent) : QObject(parent)
{
    m_idle = new QTimer(this);
    m_idle->setSingleShot(true);
    connect(m_idle, &QTimer::timeout, this, [this] {
        if (m_output && m_output->mixer && !m_output->mixer->idle()) {
            m_idle->start(800);
            return;
        }
        stop();
    });
}

PokerSound::~PokerSound()
{
    stop();
}

bool PokerSound::available()
{
#ifdef FORKMESH_HAVE_MULTIMEDIA
    return true;
#else
    return false;
#endif
}

void PokerSound::setMuted(bool muted)
{
    m_muted = muted;
    if (muted)
        stop();
}

const std::vector<float> &PokerSound::samples(const QString &name)
{
    auto found = m_cache.find(name);
    if (found == m_cache.end())
        found = m_cache.insert(name, std::make_shared<std::vector<float>>(synthesize(name, m_rate)));
    return *found.value();
}

bool PokerSound::open()
{
#ifdef FORKMESH_HAVE_MULTIMEDIA
    if (m_output && m_output->sink)
        return true;
    if (m_failed)
        return false;
    const QAudioDevice device = QMediaDevices::defaultAudioOutput();
    if (device.isNull()) {
        m_failed = true;
        return false;
    }
    QAudioFormat format;
    format.setSampleRate(44100);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Int16);
    if (!device.isFormatSupported(format)) {
        format = device.preferredFormat();
        if (format.sampleFormat() != QAudioFormat::Float)
            format.setSampleFormat(QAudioFormat::Int16);
        if (!device.isFormatSupported(format)) {
            m_failed = true;
            return false;
        }
    }
    if (format.sampleRate() != m_rate) {
        m_rate = format.sampleRate();
        m_cache.clear();
    }
    m_output = std::make_unique<Output>();
    m_output->mixer = new PokerMixer(format.channelCount(), format.sampleFormat() == QAudioFormat::Float, this);
    m_output->mixer->open(QIODevice::ReadOnly);
    m_output->sink = new QAudioSink(device, format, this);
    m_output->sink->setBufferSize(format.bytesForDuration(60000));
    m_output->sink->start(m_output->mixer);
    if (m_output->sink->error() != QAudio::NoError) {
        stop();
        m_failed = true;
        return false;
    }
    return true;
#else
    return false;
#endif
}

void PokerSound::play(const QString &name, float volume)
{
    if (m_muted || name.isEmpty() || !available())
        return;
    if (!open())
        return;
    samples(name);
    m_output->mixer->add(m_cache.value(name), std::clamp(volume, 0.0f, 1.5f));
    m_idle->start(2500);
}

void PokerSound::stop()
{
    if (!m_output)
        return;
#ifdef FORKMESH_HAVE_MULTIMEDIA
    if (m_output->sink) {
        m_output->sink->stop();
        m_output->sink->deleteLater();
    }
#endif
    if (m_output->mixer) {
        m_output->mixer->clear();
        m_output->mixer->close();
        m_output->mixer->deleteLater();
    }
    m_output.reset();
}

}
