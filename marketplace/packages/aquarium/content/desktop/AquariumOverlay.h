#pragma once

#include "AquariumScene.h"

#include <QElapsedTimer>
#include <QImage>
#include <QList>
#include <QPointer>
#include <QWidget>

#include <functional>

class QTimer;

namespace forkmesh::aquarium {

QList<Landmark> scanLandmarks(QWidget *window, const QWidget *skip);
QImage snapshotWindow(QWidget *window, const QWidget *skip);

class Overlay final : public QWidget
{
public:
    explicit Overlay(QWidget *window);
    ~Overlay() override;

    void open(bool preview);
    void dismiss();
    void settle();
    void activated();
    bool opened() const { return m_open; }
    bool previewing() const { return m_open && m_preview; }
    Scene &scene() { return m_scene; }
    void advance(double seconds);

    std::function<QList<Swimmer>(bool preview)> swimmers;
    std::function<void(const Note &)> noted;
    std::function<void(int fishId)> picked;

protected:
    void paintEvent(QPaintEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void tick();
    void finish();
    bool barHeld() const;
    void step(double dt);
    void refreshShot();
    void syncPet();
    void submerge(bool on);

    Scene m_scene;
    QPointer<QWidget> m_window;
    QTimer *m_timer = nullptr;
    QTimer *m_grace = nullptr;
    QElapsedTimer m_clock;
    double m_last = 0;
    double m_fade = 0;
    int m_direction = 0;
    bool m_open = false;
    bool m_preview = false;
    bool m_submerged = false;
    bool m_full = true;
    double m_nextShot = 0;
    double m_nextRoster = 0;
    double m_barAt = -10;
};

} // namespace forkmesh::aquarium
