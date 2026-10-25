#pragma once

#include <QByteArray>
#include <QImage>
#include <QList>
#include <QSize>
#include <QString>
#include <QStringList>

namespace forkmesh::facechanger {

struct LoopbackCamera {
    QString path;
    QString name;
};

QString cameraLabel();
QSize cameraFrameSize();
bool cameraOutputSupported();
QList<LoopbackCamera> loopbackCameras(
    const QString &sysRoot = QStringLiteral("/sys/class/video4linux"),
    const QString &devRoot = QStringLiteral("/dev"));
int preferredCamera(const QList<LoopbackCamera> &cameras);
QStringList cameraSetupCommand();
QString cameraSetupError(int exitCode, const QString &output);
QImage fitFrame(const QImage &image, const QSize &size);
QByteArray yuyvFrame(const QImage &image, const QSize &size);

class CameraOutput
{
public:
    CameraOutput() = default;
    CameraOutput(const CameraOutput &) = delete;
    CameraOutput &operator=(const CameraOutput &) = delete;
    ~CameraOutput();

    bool open(const QString &path, const QSize &size, QString *error);
    bool write(const QByteArray &frame);
    void close();
    bool isOpen() const { return m_fd >= 0; }
    QString path() const { return m_path; }
    QSize size() const { return m_size; }

private:
    int m_fd = -1;
    QString m_path;
    QSize m_size;
};

}
