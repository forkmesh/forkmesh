#pragma once

#include "NativePackageExtension.h"
#include "VoiceEngine.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QPointer>
#include <QString>
#include <QVariantMap>

#include <functional>
#include <vector>

class QCheckBox;
class QComboBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSlider;
class QThread;

namespace forkmesh::voice {

class VoiceAudioPipeline;

// The desktop Voice Changer: talk in another voice on any call or meeting
// app. The microphone is changed on this computer by
// VoiceEngine and offered back to the system as a microphone (see
// VoiceAudio.h); the call app just picks "ForkMesh Voice".
//
// The package is free and works with no account; the Worker
// (/api/voice-changer, signed with the package's request proof) only designs
// voices with AI for a signed-in desktop.
class VoiceChangerFeature final : public forkmesh::market::NativePackageWidget
{
public:
    explicit VoiceChangerFeature(const forkmesh::market::NativePackageHost &host,
                                 QWidget *parent = nullptr);
    ~VoiceChangerFeature() override;

    void refresh() override;
    // Voices are local, so the top search box needs no network warm-up.
    void prepareSearch() override {}
    QList<forkmesh::market::NativePackageSearchHit>
    searchHits(const QString &query, int limit) const override;
    void activateSearchHit(const QString &id) override;

private:
    using ReplyHandler =
        std::function<void(bool, const QJsonObject &, const QString &)>;

    void request(const QByteArray &method, const QString &suffix,
                 const QJsonObject &body, ReplyHandler handler);
    QString setting(const QString &key, const QString &fallback = QString()) const;
    void setSetting(const QString &key, const QVariant &value);

    void toggleRunning();
    void startPipeline();
    void stopPipeline();
    void applyDevices(const QVariantMap &devices);
    void applyStats(const QVariantMap &stats);
    void updateRunState();

    void loadVoices();
    void rebuildVoiceList(const QString &selectId);
    void selectVoice(const QString &id);
    void useParams(const VoiceParams &params, bool custom);
    void pushParams();
    void saveVoice();
    void deleteVoice();
    void designVoice();
    void recordSample(const QString &tag);
    void loadTargetWav();
    void learn();
    void setStatus(const QString &text, bool error = false);

    QString m_packageId;
    forkmesh::market::NativePackageRequest m_request;
    std::function<void()> m_searchIndexChanged;
    QThread *m_thread = nullptr;
    VoiceAudioPipeline *m_pipeline = nullptr;
    bool m_running = false;
    bool m_live = false;
    bool m_virtualMicrophone = false;
    QString m_backend;

    QLabel *m_status = nullptr;
    QPushButton *m_startButton = nullptr;
    QLabel *m_where = nullptr;
    QComboBox *m_input = nullptr;
    QComboBox *m_output = nullptr;
    QLabel *m_outputLabel = nullptr;
    QLabel *m_hint = nullptr;
    QCheckBox *m_monitor = nullptr;
    QCheckBox *m_denoise = nullptr;
    QProgressBar *m_meter = nullptr;
    QLabel *m_stats = nullptr;
    QComboBox *m_voice = nullptr;
    QPushButton *m_deleteVoice = nullptr;
    QList<QPair<QString, QSlider *>> m_sliders;
    QList<QLabel *> m_sliderValues;
    QLineEdit *m_prompt = nullptr;
    QPushButton *m_designButton = nullptr;
    QLabel *m_designNote = nullptr;
    QCheckBox *m_consent = nullptr;
    QPushButton *m_learnButton = nullptr;
    QLabel *m_sampleState = nullptr;

    VoiceParams m_params;
    QJsonArray m_customVoices;
    std::vector<float> m_mine;
    std::vector<float> m_theirs;
    int m_mineRate = 48000;
    int m_theirsRate = 48000;
    bool m_syncingSliders = false;
};

} // namespace forkmesh::voice
