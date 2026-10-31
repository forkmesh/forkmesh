#pragma once

#include <QWidget>

#include <functional>

class QComboBox;
class QHBoxLayout;
class QLabel;
class QStackedWidget;

namespace forkmesh::djstudio {

class DjButton;
class DjFader;
class DjKnob;
class DjMeter;
class DjModel;

class DjMixerView : public QWidget
{
public:
    DjMixerView(DjModel *model, QWidget *video, QWidget *parent = nullptr);

    void refresh();
    void animate();
    void rebuildStrips();
    void showTab(int tab);

    std::function<void()> focusMenuRequested;

protected:
    void paintEvent(QPaintEvent *) override;

private:
    struct Strip {
        int deck = 0;
        QWidget *box = nullptr;
        DjKnob *gain = nullptr;
        DjKnob *eq[3] = {nullptr, nullptr, nullptr};
        DjKnob *filter = nullptr;
        DjMeter *meter = nullptr;
        DjFader *volume = nullptr;
    };

    QWidget *buildMixer();
    QWidget *buildVideo(QWidget *video);
    QWidget *buildSampler();
    QWidget *buildAutomix();
    Strip makeStrip(int deck, QWidget *parent);

    DjModel *m_model;
    QStackedWidget *m_pages = nullptr;
    QList<DjButton *> m_tabs;
    QHBoxLayout *m_stripRow = nullptr;
    QWidget *m_masterBox = nullptr;
    QList<Strip> m_strips;
    DjKnob *m_master = nullptr;
    DjKnob *m_sampler = nullptr;
    DjMeter *m_masterMeterL = nullptr;
    DjMeter *m_masterMeterR = nullptr;
    DjFader *m_crossfader = nullptr;
    DjButton *m_curve = nullptr;
    DjButton *m_automix = nullptr;
    DjButton *m_autoDj = nullptr;
    DjButton *m_mixNow = nullptr;
    DjButton *m_automixPage = nullptr;
    DjButton *m_autoDjPage = nullptr;
    DjButton *m_shuffle = nullptr;
    DjKnob *m_length = nullptr;
    QLabel *m_lengthLabel = nullptr;
    QLabel *m_focusLabel = nullptr;
    QLabel *m_nextLabel = nullptr;
    QList<DjButton *> m_videoModes;
    QList<DjButton *> m_sections;
    QList<DjButton *> m_drums;
    int m_stripCount = 0;
};

}
