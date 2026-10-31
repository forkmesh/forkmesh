#pragma once

#include <QHash>
#include <QObject>
#include <QString>

#include <memory>
#include <vector>

class QTimer;

namespace forkmesh::poker {

class PokerMixer;

class PokerSound final : public QObject
{
public:
    explicit PokerSound(QObject *parent = nullptr);
    ~PokerSound() override;

    static bool available();
    void setMuted(bool muted);
    bool muted() const { return m_muted; }
    void play(const QString &name, float volume = 1.0f);
    void stop();

private:
    bool open();
    const std::vector<float> &samples(const QString &name);

    bool m_muted = false;
    bool m_failed = false;
    int m_rate = 44100;
    QHash<QString, std::shared_ptr<std::vector<float>>> m_cache;
    QTimer *m_idle = nullptr;
    struct Output;
    std::unique_ptr<Output> m_output;
};

}
