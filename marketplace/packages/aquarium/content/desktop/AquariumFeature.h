#pragma once

#include "AquariumScene.h"
#include "NativePackageExtension.h"

#include <QPointer>

class QCheckBox;
class QLabel;
class QListWidget;
class QSpinBox;
class QTimer;

namespace forkmesh::aquarium {

class Overlay;

class AquariumFeature final : public forkmesh::market::NativePackageWidget
{
public:
    explicit AquariumFeature(const forkmesh::market::NativePackageHost &host, QWidget *parent = nullptr);
    ~AquariumFeature() override;

    void refresh() override;

    static bool enabled();
    static int delaySeconds();
    static QList<Swimmer> roster(bool guests);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    Overlay *overlay();
    void stateChanged(Qt::ApplicationState state);
    void wait();
    void wake();
    void preview();
    void note(const Note &note);

    QPointer<Overlay> m_overlay;
    QTimer *m_wait = nullptr;
    QCheckBox *m_enabled = nullptr;
    QSpinBox *m_delay = nullptr;
    QLabel *m_tank = nullptr;
    QListWidget *m_fish = nullptr;
    QListWidget *m_lately = nullptr;
};

} // namespace forkmesh::aquarium
