#pragma once

#include <QJsonObject>
#include <QRectF>
#include <QString>
#include <QUrl>
#include <QVector>

#include <array>
#include <functional>
#include <memory>
#include <optional>

class QWidget;

namespace forkmesh::djstudio {

struct DjDeckSound {
    double gain = 1;
    double trim = 1;
    std::array<double, 3> eq{0, 0, 0};
    double filter = 0;
    std::array<double, 3> fx{0, 0, 0};
    double beat = 0.5;
    bool keylock = false;
    bool operator==(const DjDeckSound &) const = default;
};

struct DjVideoSlot {
    QRectF rect;
    double opacity = 0;
    int z = 0;
    bool operator==(const DjVideoSlot &) const = default;
};

class DjPlayer
{
public:
    static constexpr int deckCount = 4;

    DjPlayer(const QUrl &origin, QWidget *parent);
    ~DjPlayer();

    static bool available();

    QWidget *view() const;
    bool ready() const;
    int scriptsSent() const;

    std::function<void(const QString &, const QJsonObject &)> onEvent;
    std::function<void()> onReload;

    void load(int deck, const QString &id, double start, double rate, bool play);
    void eject(int deck);
    void play(int deck);
    void pause(int deck);
    void seek(int deck, double seconds);
    void setRate(int deck, double rate);
    void setSound(int deck, const DjDeckSound &sound);
    void scratch(int deck, std::optional<double> target);
    void brake(int deck);
    void backspin(int deck);
    void pad(const QString &name);
    void setSamplerLevel(double level);
    void layout(const QVector<DjVideoSlot> &slots);
    void reset();

private:
    struct Private;
    std::unique_ptr<Private> d;
};

}
