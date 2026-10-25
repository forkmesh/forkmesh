#pragma once

#include <QWidget>

#include <functional>

class QLabel;
class QGridLayout;

namespace forkmesh::djstudio {

class DjButton;
class DjFader;
class DjKnob;
class DjModel;
class DjOverview;
class DjPlatter;

class DjDeckView : public QWidget
{
public:
    DjDeckView(DjModel *model, int deck, bool mirrored, QWidget *parent = nullptr);

    int deck() const { return m_deck; }
    void setCompact(bool compact);
    void refresh();
    void animate();

    std::function<void(int, const QPoint &)> menuRequested;
    std::function<void(int, const QPoint &)> fileRequested;
    std::function<void(int)> similarRequested;
    std::function<void(int)> loadRequested;

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *) override;
    void dropEvent(QDropEvent *event) override;
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    QWidget *buildInfo();
    QWidget *buildPlatter();
    QWidget *buildPitch();
    void refreshPads();
    void padPressed(int pad, bool down);

    DjModel *m_model;
    int m_deck;
    bool m_mirrored;
    bool m_compact = false;
    bool m_dropping = false;
    int m_padMode = 0;

    QLabel *m_art = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_artist = nullptr;
    QLabel *m_bpm = nullptr;
    QLabel *m_elapsed = nullptr;
    QLabel *m_remain = nullptr;
    QLabel *m_gain = nullptr;
    QLabel *m_key = nullptr;
    QLabel *m_pitchLabel = nullptr;
    QLabel *m_loopLabel = nullptr;
    QString m_artFor;
    DjOverview *m_overview = nullptr;
    DjPlatter *m_platter = nullptr;
    DjFader *m_pitch = nullptr;
    DjButton *m_tap = nullptr;
    DjButton *m_cue = nullptr;
    DjButton *m_play = nullptr;
    DjButton *m_stop = nullptr;
    DjButton *m_restart = nullptr;
    DjButton *m_sync = nullptr;
    DjButton *m_assign = nullptr;
    DjButton *m_range = nullptr;
    DjButton *m_reset = nullptr;
    DjButton *m_loop = nullptr;
    DjButton *m_loopIn = nullptr;
    DjButton *m_loopOut = nullptr;
    DjButton *m_repeat = nullptr;
    DjButton *m_keylock = nullptr;
    DjButton *m_sample = nullptr;
    DjButton *m_backspin = nullptr;
    DjButton *m_brake = nullptr;
    DjButton *m_padModeButton = nullptr;
    QList<DjButton *> m_fxButtons;
    QList<DjKnob *> m_fxKnobs;
    QList<DjButton *> m_pads;
    QWidget *m_padRow = nullptr;
    QWidget *m_fxRow = nullptr;
};

}
