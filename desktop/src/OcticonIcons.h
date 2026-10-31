#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

class QPushButton;

namespace forkmesh::ui {

QIcon themedOcticon(const QString &name, const QColor &color, int size);
void setOcticon(QPushButton *button, const QString &name, int size = 16,
                qreal rotationDeg = 0.0);

}
