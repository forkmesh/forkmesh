#pragma once

#include <QImage>
#include <QPointF>
#include <QRect>

#include <functional>

class QWidget;

namespace forkmesh::petfeed {

enum class Reaction { Wave, Cheer, Nod, Eureka, Giggle, Surprise, Dizzy, Dance };

struct Sprite {
    QRect frame;
    QRect body;
    QImage image;
    bool walking = false;
    bool airborne = false;
};

struct Host {
    std::function<bool(QWidget *, Sprite *)> sprite;
    std::function<void(bool)> setSubmerged;
    std::function<void(Reaction)> react;
    std::function<void(QWidget *, double)> walkTo;
    std::function<void(double, double)> hop;
    std::function<void(QWidget *, const QPointF &)> lookAt;
};

void setHost(const void *owner, Host host);
void clearHost(const void *owner);
bool hasHost();
const Host &host();

} // namespace forkmesh::petfeed
