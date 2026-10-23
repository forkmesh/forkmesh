#include "FontSizing.h"

#include <QGuiApplication>

#include <cmath>
#include <cstdio>

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

bool near(qreal a, qreal b)
{
    return std::abs(a - b) < 0.01;
}

QFont pixelFont(int px)
{
    QFont font;
    font.setPixelSize(px);
    return font;
}

QFont pointFont(qreal pt)
{
    QFont font;
    font.setPointSizeF(pt);
    return font;
}

}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);

    const QFont px = pixelFont(12);
    check(px.pointSizeF() < 0, "a pixel-sized font reports no point size");

    const QFont scaledPx = forkmesh::scaledFont(px, 1.25);
    check(scaledPx.pixelSize() == 15, "scaledFont scales a pixel font in pixels");
    check(scaledPx.pointSizeF() < 0, "scaledFont keeps a pixel font in pixels");
    check(forkmesh::scaledFont(px, 0.85).pixelSize() == 10,
          "scaledFont shrinks a pixel font");
    check(forkmesh::scaledFont(pixelFont(1), 0.1).pixelSize() == 1,
          "scaledFont never reaches zero pixels");

    const QFont pt = pointFont(10.0);
    check(near(forkmesh::scaledFont(pt, 1.25).pointSizeF(), 12.5),
          "scaledFont scales a point font in points");
    check(forkmesh::scaledFont(pt, 1.25).pixelSize() < 0,
          "scaledFont keeps a point font in points");

    check(forkmesh::resizedFont(px, 3).pixelSize() == 16,
          "resizedFont adds points as pixels");
    check(forkmesh::resizedFont(px, -6).pixelSize() == 9,
          "resizedFont keeps a pixel font readable");
    check(near(forkmesh::resizedFont(pt, 4).pointSizeF(), 14.0),
          "resizedFont adds points to a point font");
    check(near(forkmesh::resizedFont(pt, -6).pointSizeF(), 7.0),
          "resizedFont keeps a point font readable");

    QFont mono;
    mono.setFamily(QStringLiteral("Monospace"));
    const QFont likePx = forkmesh::fontSizedLike(mono, px);
    check(likePx.pixelSize() == 12, "fontSizedLike copies a pixel size");
    check(likePx.family() == mono.family(), "fontSizedLike keeps the family");
    check(near(forkmesh::fontSizedLike(mono, pt).pointSizeF(), 10.0),
          "fontSizedLike copies a point size");

    if (failures == 0)
        std::printf("font sizing: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
