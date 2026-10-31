#include "../FaceCamera.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cstdio>
#include <cstdlib>

using namespace forkmesh::facechanger;

namespace {

int failures = 0;

void check(bool ok, const char *what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

void write(const QString &path, const QByteArray &text)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    file.open(QIODevice::WriteOnly);
    file.write(text);
}

bool near(int value, int expected)
{
    return std::abs(value - expected) <= 1;
}

void testColours()
{
    const QSize size(4, 2);
    QImage white(4, 2, QImage::Format_RGB32);
    white.fill(Qt::white);
    QByteArray frame = yuyvFrame(white, size);
    check(frame.size() == 4 * 2 * 2, "a YUYV frame has two bytes per pixel");
    check(near(uchar(frame[0]), 235) && near(uchar(frame[1]), 128) &&
              near(uchar(frame[2]), 235) && near(uchar(frame[3]), 128),
          "white is Y 235, U and V 128");

    QImage black(4, 2, QImage::Format_RGB32);
    black.fill(Qt::black);
    frame = yuyvFrame(black, size);
    check(uchar(frame[0]) == 16 && uchar(frame[1]) == 128 && uchar(frame[3]) == 128,
          "black is Y 16, U and V 128");

    QImage red(4, 2, QImage::Format_RGB32);
    red.fill(QColor(255, 0, 0));
    frame = yuyvFrame(red, size);
    check(near(uchar(frame[0]), 82) && near(uchar(frame[1]), 90) && near(uchar(frame[3]), 240),
          "red is Y 82, U 90, V 240");
}

void testFit()
{
    QImage tall(100, 200, QImage::Format_RGB32);
    tall.fill(Qt::white);
    const QImage fitted = fitFrame(tall, QSize(400, 200));
    check(fitted.size() == QSize(400, 200), "the frame takes the camera size");
    check(fitted.pixelColor(10, 100) == QColor(Qt::black), "a tall picture gets black bars");
    check(fitted.pixelColor(200, 100) == QColor(Qt::white), "the picture sits in the middle");
    check(yuyvFrame(QImage(), QSize(4, 2)).size() == 16, "no picture still sends a black frame");
    check(yuyvFrame(tall, QSize(5, 2)).size() == 4 * 2 * 2, "an odd width is rounded down");
}

void testDiscovery()
{
    QTemporaryDir root;
    const QString sys = root.filePath(QStringLiteral("sys"));
    const QString dev = root.filePath(QStringLiteral("dev"));
    write(sys + QStringLiteral("/video0/name"), "Integrated Camera\n");
    write(sys + QStringLiteral("/video10/name"), "ForkMesh Camera\n");
    write(sys + QStringLiteral("/video10/max_openers"), "10\n");
    write(sys + QStringLiteral("/video2/name"), "Dummy video device (0x0000)\n");
    write(sys + QStringLiteral("/video2/buffers"), "2\n");
    write(sys + QStringLiteral("/video3/name"), "Gone\n");
    write(sys + QStringLiteral("/video3/max_openers"), "10\n");
    for (const char *name : {"video0", "video2", "video10"})
        write(dev + QLatin1Char('/') + QLatin1String(name), QByteArray());
    const QList<LoopbackCamera> cameras = loopbackCameras(sys, dev);
    check(cameras.size() == 2, "only loopback devices with a device node are listed");
    check(cameras.size() == 2 && cameras.at(0).path == dev + QStringLiteral("/video2") &&
              cameras.at(1).path == dev + QStringLiteral("/video10"),
          "devices are in number order");
    check(preferredCamera(cameras) == 1, "the ForkMesh camera is preferred");
    check(preferredCamera(cameras.mid(0, 1)) == 0, "any loopback device works");
    check(preferredCamera({}) == -1, "no device, no camera");
    check(loopbackCameras(root.filePath(QStringLiteral("none")), dev).isEmpty(),
          "a missing sysfs folder lists nothing");
}

void testSetup()
{
    const QStringList command = cameraSetupCommand();
    check(command.value(0) == QLatin1String("pkexec") && command.value(1) == QLatin1String("modprobe") &&
              command.value(2) == QLatin1String("v4l2loopback"),
          "setup loads v4l2loopback through pkexec");
    check(command.contains(QStringLiteral("exclusive_caps=1")),
          "browsers only list the camera with exclusive_caps");
    check(command.contains(QStringLiteral("card_label=ForkMesh Camera")), "the camera is named");
    check(cameraSetupError(0, QString()).isEmpty(), "success has no error");
    check(cameraSetupError(126, QString()).contains(QLatin1String("nothing changed")),
          "a closed password prompt changes nothing");
    check(cameraSetupError(1, QStringLiteral("modprobe: FATAL: Module v4l2loopback not found"))
              .contains(QLatin1String("not installed")),
          "a missing driver says to install it");
    check(cameraSetupError(1, QStringLiteral("first\nlast line\n")).endsWith(QLatin1String("last line")),
          "other failures show the last line");
}

void testOutput(const QString &scratch)
{
    CameraOutput output;
    QString error;
    const QString plain = scratch + QStringLiteral("/plain");
    write(plain, QByteArray());
    check(!output.open(plain, QSize(640, 360), &error), "a plain file is not a camera");
    check(!error.isEmpty(), "a failed open says why");
    check(!output.isOpen() && !output.write(QByteArray(640 * 360 * 2, 0)),
          "a closed output writes nothing");
    check(!output.open(scratch + QStringLiteral("/missing"), QSize(640, 360), &error),
          "a missing device fails");
    check(cameraLabel() == QLatin1String("ForkMesh Camera") && cameraFrameSize() == QSize(1280, 720),
          "the camera is a 1280x720 ForkMesh Camera");
}

}

int main()
{
    QTemporaryDir scratch;
    testColours();
    testFit();
    testDiscovery();
    testSetup();
    testOutput(scratch.path());
    if (failures == 0)
        std::printf("face camera: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
