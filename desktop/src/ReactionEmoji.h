#pragma once

#include "ReactionCatalog.h"

#include <QColor>
#include <QPixmap>
#include <QString>

// Hand-drawn art for the reaction vocabulary in ReactionCatalog.h. The drawings
// exist because emoji fonts are not installed everywhere ForkMesh runs, and a
// missing-glyph box makes a worse chip than no chip at all.
namespace reactions {

QString displayName(const QString &value);

bool hasEmoji(const QString &value);

QPixmap emojiPixmap(const QString &value, int size, qreal dpr);

QPixmap addGlyph(int size, qreal dpr, const QColor &color);

} // namespace reactions
