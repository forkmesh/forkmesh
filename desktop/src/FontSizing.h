#pragma once

#include <QFont>
#include <QtGlobal>

namespace forkmesh {

inline QFont scaledFont(QFont font, qreal factor)
{
    if (font.pixelSize() > 0)
        font.setPixelSize(qMax(1, qRound(font.pixelSize() * factor)));
    else if (font.pointSizeF() > 0)
        font.setPointSizeF(qMax(1.0, font.pointSizeF() * factor));
    return font;
}

inline QFont resizedFont(QFont font, qreal deltaPoints)
{
    if (font.pixelSize() > 0)
        font.setPixelSize(qMax(9, qRound(font.pixelSize() + deltaPoints * 4.0 / 3.0)));
    else if (font.pointSizeF() > 0)
        font.setPointSizeF(qMax(7.0, font.pointSizeF() + deltaPoints));
    return font;
}

inline QFont fontSizedLike(QFont font, const QFont &like)
{
    if (like.pixelSize() > 0)
        font.setPixelSize(like.pixelSize());
    else if (like.pointSizeF() > 0)
        font.setPointSizeF(like.pointSizeF());
    return font;
}

}
