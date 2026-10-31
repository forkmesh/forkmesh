#pragma once

#include "FaceCamera.h"
#include "NativePackageExtension.h"

#include <QByteArray>
#include <QFuture>
#include <QJsonArray>
#include <QJsonObject>
#include <QMutex>
#include <QString>
#include <QUrl>

#include <atomic>
#include <functional>
#include <memory>

class QComboBox;
class QLabel;
class QProcess;
class QPushButton;
class QTimer;
class QWebEnginePage;
class QWebEngineView;

namespace forkmesh::facechanger {

struct CameraFeed {
    QMutex mutex;
    CameraOutput output;
    std::atomic<bool> busy{false};
    std::atomic<int> frames{0};
};

class FaceChangerFeature final : public forkmesh::market::NativePackageWidget
{
public:
    explicit FaceChangerFeature(const forkmesh::market::NativePackageHost &host,
                                QWidget *parent = nullptr);
    ~FaceChangerFeature() override;

    void refresh() override;
    void prepareSearch() override {}

    static QUrl engineOrigin(const QString &hostUrl);

private:
    QString setting(const QString &key, const QString &fallback = QString()) const;
    void setSetting(const QString &key, const QString &value);
    void buildEngine();
    void run(const QString &script);
    void receive(const QJsonObject &message);
    void fillChoices(QComboBox *box, const QJsonArray &choices);
    void showLook(const QJsonObject &message);
    void toggleCamera();
    void takeFrame();
    void pushFrame(const QByteArray &jpeg);
    void openOutput();
    void closeOutput();
    void setUpCamera();
    void choosePhoto(bool face);
    void updateState();
    void setStatus(const QString &text, bool error = false);

    QString m_packageId;
    std::function<void(const QString &)> m_openWebPage;
    QUrl m_origin;
    QWebEngineView *m_view = nullptr;
    QWebEnginePage *m_page = nullptr;
    QTimer *m_tick = nullptr;
    QProcess *m_setup = nullptr;
    std::shared_ptr<CameraFeed> m_feed;
    QFuture<void> m_work;

    bool m_ready = false;
    bool m_starting = false;
    bool m_live = false;
    bool m_taking = false;
    bool m_customFace = false;
    bool m_customBackground = false;
    QString m_customFaceId;
    QString m_customBackgroundId;
    QString m_outputError;
    QString m_outputName;
    QString m_setupError;

    QLabel *m_status = nullptr;
    QPushButton *m_startButton = nullptr;
    QComboBox *m_camera = nullptr;
    QComboBox *m_face = nullptr;
    QComboBox *m_background = nullptr;
    QComboBox *m_outfit = nullptr;
    QLabel *m_output = nullptr;
    QLabel *m_command = nullptr;
    QPushButton *m_setupButton = nullptr;
    QPushButton *m_rescanButton = nullptr;
};

}
