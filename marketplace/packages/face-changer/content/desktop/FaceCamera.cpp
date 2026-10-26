#include "FaceCamera.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPainter>

#include <algorithm>
#include <cerrno>
#include <cstring>

#ifdef Q_OS_LINUX
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace forkmesh::facechanger {

namespace {

QString readLine(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.readLine(256)).trimmed();
}

int naturalIndex(const QString &entry)
{
    bool ok = false;
    const int index = entry.mid(5).toInt(&ok);
    return ok ? index : 1 << 20;
}

inline uchar clampByte(int value)
{
    return uchar(std::clamp(value, 0, 255));
}

}

QString cameraLabel()
{
    return QStringLiteral("ForkMesh Camera");
}

QSize cameraFrameSize()
{
    return {1280, 720};
}

bool cameraOutputSupported()
{
#ifdef Q_OS_LINUX
    return true;
#else
    return false;
#endif
}

QList<LoopbackCamera> loopbackCameras(const QString &sysRoot, const QString &devRoot)
{
    QStringList entries = QDir(sysRoot).entryList({QStringLiteral("video*")},
                                                  QDir::Dirs | QDir::NoDotAndDotDot);
    std::sort(entries.begin(), entries.end(), [](const QString &a, const QString &b) {
        return naturalIndex(a) < naturalIndex(b);
    });
    QList<LoopbackCamera> cameras;
    for (const QString &entry : entries) {
        const QString base = sysRoot + QLatin1Char('/') + entry;
        if (!QFileInfo::exists(base + QStringLiteral("/max_openers")) &&
            !QFileInfo::exists(base + QStringLiteral("/buffers")))
            continue;
        const QString path = devRoot + QLatin1Char('/') + entry;
        if (!QFileInfo::exists(path))
            continue;
        QString name = readLine(base + QStringLiteral("/name"));
        if (name.isEmpty())
            name = entry;
        cameras.append({path, name});
    }
    return cameras;
}

int preferredCamera(const QList<LoopbackCamera> &cameras)
{
    for (int i = 0; i < cameras.size(); ++i) {
        if (cameras.at(i).name.contains(QLatin1String("ForkMesh"), Qt::CaseInsensitive))
            return i;
    }
    return cameras.isEmpty() ? -1 : 0;
}

QStringList cameraSetupCommand()
{
    return {QStringLiteral("pkexec"),
            QStringLiteral("modprobe"),
            QStringLiteral("v4l2loopback"),
            QStringLiteral("devices=1"),
            QStringLiteral("exclusive_caps=1"),
            QStringLiteral("card_label=") + cameraLabel()};
}

QString cameraSetupError(int exitCode, const QString &output)
{
    if (exitCode == 0)
        return {};
    if (exitCode == 126)
        return QStringLiteral("The password prompt was closed, so nothing changed.");
    if (exitCode == 127 && !output.contains(QLatin1String("modprobe")))
        return QStringLiteral("This computer did not let ForkMesh ask for your password. "
                              "Run the command below in a terminal instead.");
    if (output.contains(QLatin1String("not found"), Qt::CaseInsensitive))
        return QStringLiteral("The v4l2loopback driver is not installed yet. Install it from "
                              "your system's packages (v4l2loopback-dkms on Debian, Ubuntu and "
                              "Arch, akmod-v4l2loopback on Fedora), then set up the camera again.");
    const QStringList lines = output.trimmed().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    return lines.isEmpty() ? QStringLiteral("Setting up the camera failed.")
                           : QStringLiteral("Setting up the camera failed: %1").arg(lines.last().trimmed());
}

QImage fitFrame(const QImage &image, const QSize &size)
{
    QImage canvas(size, QImage::Format_RGB32);
    canvas.fill(Qt::black);
    if (image.isNull() || size.isEmpty())
        return canvas;
    const QImage scaled = image.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QPainter painter(&canvas);
    painter.drawImage((size.width() - scaled.width()) / 2,
                      (size.height() - scaled.height()) / 2, scaled);
    painter.end();
    return canvas;
}

QByteArray yuyvFrame(const QImage &image, const QSize &size)
{
    const int width = size.width() & ~1;
    const int height = size.height();
    if (width <= 0 || height <= 0)
        return {};
    const QImage frame = fitFrame(image, QSize(width, height));
    QByteArray out(width * height * 2, Qt::Uninitialized);
    auto *dst = reinterpret_cast<uchar *>(out.data());
    for (int y = 0; y < height; ++y) {
        const auto *row = reinterpret_cast<const QRgb *>(frame.constScanLine(y));
        for (int x = 0; x < width; x += 2) {
            const QRgb a = row[x];
            const QRgb b = row[x + 1];
            const int r0 = qRed(a), g0 = qGreen(a), b0 = qBlue(a);
            const int r1 = qRed(b), g1 = qGreen(b), b1 = qBlue(b);
            const int r = (r0 + r1 + 1) / 2, g = (g0 + g1 + 1) / 2, bl = (b0 + b1 + 1) / 2;
            *dst++ = clampByte(((66 * r0 + 129 * g0 + 25 * b0 + 128) >> 8) + 16);
            *dst++ = clampByte(((-38 * r - 74 * g + 112 * bl + 128) >> 8) + 128);
            *dst++ = clampByte(((66 * r1 + 129 * g1 + 25 * b1 + 128) >> 8) + 16);
            *dst++ = clampByte(((112 * r - 94 * g - 18 * bl + 128) >> 8) + 128);
        }
    }
    return out;
}

CameraOutput::~CameraOutput()
{
    close();
}

bool CameraOutput::open(const QString &path, const QSize &size, QString *error)
{
    close();
    auto fail = [error](const QString &text) {
        if (error)
            *error = text;
        return false;
    };
#ifdef Q_OS_LINUX
    const int width = size.width() & ~1;
    const int height = size.height();
    if (width <= 0 || height <= 0)
        return fail(QStringLiteral("The camera picture has no size."));
    const int fd = ::open(QFile::encodeName(path).constData(), O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return fail(errno == EACCES
                        ? QStringLiteral("ForkMesh may not write to %1. Add yourself to the "
                                         "video group, then sign in again.").arg(path)
                        : QStringLiteral("%1 could not be opened: %2")
                              .arg(path, QString::fromLocal8Bit(std::strerror(errno))));
    v4l2_capability caps{};
    if (::ioctl(fd, VIDIOC_QUERYCAP, &caps) < 0) {
        ::close(fd);
        return fail(QStringLiteral("%1 is not a camera.").arg(path));
    }
    v4l2_format format{};
    format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    format.fmt.pix.width = __u32(width);
    format.fmt.pix.height = __u32(height);
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    format.fmt.pix.field = V4L2_FIELD_NONE;
    format.fmt.pix.bytesperline = __u32(width * 2);
    format.fmt.pix.sizeimage = __u32(width * height * 2);
    format.fmt.pix.colorspace = V4L2_COLORSPACE_SMPTE170M;
    if (::ioctl(fd, VIDIOC_S_FMT, &format) < 0) {
        const int code = errno;
        ::close(fd);
        return fail(code == EBUSY
                        ? QStringLiteral("Another app is already sending its picture to %1.").arg(path)
                        : QStringLiteral("%1 did not take the picture format: %2")
                              .arg(path, QString::fromLocal8Bit(std::strerror(code))));
    }
    v4l2_streamparm rate{};
    rate.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    rate.parm.output.timeperframe.numerator = 1;
    rate.parm.output.timeperframe.denominator = 30;
    ::ioctl(fd, VIDIOC_S_PARM, &rate);
    m_fd = fd;
    m_path = path;
    m_size = QSize(width, height);
    return true;
#else
    Q_UNUSED(path);
    Q_UNUSED(size);
    return fail(QStringLiteral("A camera other apps can pick is only made on Linux."));
#endif
}

bool CameraOutput::write(const QByteArray &frame)
{
#ifdef Q_OS_LINUX
    if (m_fd < 0 || frame.size() != m_size.width() * m_size.height() * 2)
        return false;
    qsizetype done = 0;
    while (done < frame.size()) {
        const ssize_t wrote = ::write(m_fd, frame.constData() + done, size_t(frame.size() - done));
        if (wrote < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        done += wrote;
    }
    return true;
#else
    Q_UNUSED(frame);
    return false;
#endif
}

void CameraOutput::close()
{
#ifdef Q_OS_LINUX
    if (m_fd >= 0)
        ::close(m_fd);
#endif
    m_fd = -1;
    m_path.clear();
    m_size = QSize();
}

}
