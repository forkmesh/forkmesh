#include "WindowFade.h"

#include <QApplication>
#include <QImage>
#include <QPalette>

#include <cstdio>

using forkmesh::ui::WindowFade;

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

QImage renderHost(QWidget &host)
{
    QImage image(host.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    host.render(&image);
    return image;
}

QWidget *addPanel(QWidget &host, const QColor &color, const QRect &geometry)
{
    auto *panel = new QWidget(&host);
    QPalette palette = panel->palette();
    palette.setColor(QPalette::Window, color);
    panel->setPalette(palette);
    panel->setAutoFillBackground(true);
    panel->setGeometry(geometry);
    panel->show();
    return panel;
}

}

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    check(!WindowFade::platformNeedsFade(),
          "offscreen keeps the native window opacity path");

    QWidget host;
    WindowFade::prepareHost(&host);
    check(host.testAttribute(Qt::WA_TranslucentBackground) &&
              !host.testAttribute(Qt::WA_NoSystemBackground),
          "the host gets an alpha surface but still paints its own background");
    host.resize(200, 120);
    addPanel(host, Qt::red, QRect(0, 0, 100, 120));
    auto *fade = new WindowFade(&host);
    host.show();
    QApplication::processEvents();

    check(fade->isVisible() && fade->geometry() == host.rect() &&
              fade->mask().boundingRect() == host.rect() &&
              !fade->mask().contains(QPoint(100, 60)),
          "at full opacity the fade covers only the window corners");
    QImage opaque = renderHost(host);
    check(qAlpha(opaque.pixel(10, 10)) == 255 &&
              qAlpha(opaque.pixel(150, 60)) == 255 &&
              qAlpha(opaque.pixel(10, 0)) == 255 &&
              qAlpha(opaque.pixel(0, 60)) == 255,
          "a full-opacity window renders opaque inside its rounded edge");
    check(qAlpha(opaque.pixel(0, 0)) == 0 && qAlpha(opaque.pixel(199, 0)) == 0 &&
              qAlpha(opaque.pixel(0, 119)) == 0 &&
              qAlpha(opaque.pixel(199, 119)) == 0,
          "the window's four corners are cut away");
    const int edge = qAlpha(opaque.pixel(2, 3));
    check(edge > 0 && edge < 255, "the rounded corner is antialiased");

    host.setWindowState(Qt::WindowMaximized);
    QApplication::processEvents();
    check(!fade->isVisible() && qAlpha(renderHost(host).pixel(0, 0)) == 255,
          "a maximized window keeps square corners");
    host.setWindowState(Qt::WindowNoState);
    host.resize(200, 120);
    QApplication::processEvents();
    check(fade->isVisible() && qAlpha(renderHost(host).pixel(0, 0)) == 0,
          "restoring the window rounds its corners again");

    fade->setOpacityPercent(50);
    QApplication::processEvents();
    QImage half = renderHost(host);
    check(fade->isVisible(), "lowering the opacity shows the fade");
    check(qAbs(qAlpha(half.pixel(10, 10)) - 127) <= 1 &&
              qAbs(qAlpha(half.pixel(150, 60)) - 127) <= 1 &&
              qAbs(qAlpha(half.pixel(199, 60)) - 127) <= 1,
          "every part of the window is scaled to the chosen alpha");
    check(qAlpha(half.pixel(0, 0)) == 0 && qAlpha(half.pixel(199, 119)) == 0,
          "a faded window keeps its rounded corners");
    check(qAbs(qRed(half.pixel(10, 10)) - 127) <= 1 &&
              qGreen(half.pixel(10, 10)) == 0,
          "child content keeps its colour under the fade");

    host.resize(300, 150);
    QApplication::processEvents();
    check(fade->geometry() == host.rect(), "the fade follows the window size");

    QWidget *late = addPanel(host, Qt::blue, QRect(0, 0, 300, 150));
    QApplication::processEvents();
    check(host.children().last() == fade,
          "a child added later is stacked under the fade");
    late->raise();
    check(host.children().last() == late, "raising a sibling moves it above");
    QApplication::processEvents();
    check(host.children().last() == fade,
          "the fade returns above a sibling that raised itself");
    QImage raised = renderHost(host);
    check(qAbs(qAlpha(raised.pixel(250, 140)) - 127) <= 1 &&
              qAbs(qBlue(raised.pixel(250, 140)) - 127) <= 1,
          "a raised sibling is faded too");

    fade->setOpacityPercent(30);
    QApplication::processEvents();
    check(qAbs(qAlpha(renderHost(host).pixel(50, 50)) - 76) <= 1,
          "the minimum setting lets most of the desktop through");

    fade->setOpacityPercent(100);
    QApplication::processEvents();
    QImage restored = renderHost(host);
    check(fade->isVisible() && !fade->mask().contains(QPoint(50, 50)) &&
              qAlpha(restored.pixel(50, 50)) == 255 &&
              qAlpha(restored.pixel(0, 0)) == 0,
          "returning to full opacity leaves only the corners cut");

    if (failures == 0)
        std::printf("window fade tests passed\n");
    return failures == 0 ? 0 : 1;
}
