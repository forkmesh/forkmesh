#pragma once

#include "NativePackageExtension.h"

#include <QPointer>

class QLabel;
class QSplitter;
class QTimer;
class QVBoxLayout;
class QGridLayout;

namespace forkmesh::djstudio {

class DjBrowserView;
class DjButton;
class DjDeckView;
class DjJamDialog;
class DjMixerView;
class DjModel;
class DjPeopleStrip;
class DjPlayer;
class DjReactionOverlay;
class DjScrollWave;
struct DjFocus;

class DjStudioFeature : public market::NativePackageWidget
{
public:
    DjStudioFeature(const market::NativePackageHost &host, QWidget *parent);
    ~DjStudioFeature() override;

    void refresh() override;
    void setHostSearchQuery(const QString &query) override;
    QList<market::NativePackageSearchHit> searchHits(const QString &query, int limit) const override;
    void activateSearchHit(const QString &id) override;
    void prepareSearch() override {}
    int navigationBadge() const override;
    QString railTitle() const override;
    QList<market::NativePackageRailTile> railTiles() const override;
    void activateRailStep(const QString &tile, const QString &step) override;

protected:
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *) override;

private:
    QWidget *buildTopBar();
    void layoutDecks();
    void fitDecks();
    void tick();
    void slowTick();
    void notice(const QString &text);
    void deckMenu(int deck, const QPoint &global);
    void fileMenu(int deck, const QPoint &global);
    void focusMenu(const QPoint &global);
    void settingsMenu(const QPoint &global);
    void reactionMenu(const QPoint &global);
    void openJam();
    void openSong(const QString &id);
    void openDiagnostics();
    void openHelp();
    void exportList(const DjFocus &focus);
    void importFile();
    void pasteSongs();
    bool shortcut(QKeyEvent *event, bool down);
    void railSoon();

    market::NativePackageHost m_host;
    DjPlayer *m_player = nullptr;
    DjModel *m_model = nullptr;
    QWidget *m_banner = nullptr;
    QWidget *m_waves = nullptr;
    QVBoxLayout *m_wavesLayout = nullptr;
    QList<DjScrollWave *> m_scrolls;
    QWidget *m_deckArea = nullptr;
    QGridLayout *m_deckGrid = nullptr;
    QList<DjDeckView *> m_decks;
    DjMixerView *m_mixer = nullptr;
    DjBrowserView *m_browser = nullptr;
    QSplitter *m_split = nullptr;
    DjReactionOverlay *m_overlay = nullptr;
    QLabel *m_notice = nullptr;
    QLabel *m_clock = nullptr;
    QLabel *m_engine = nullptr;
    DjButton *m_deckMode = nullptr;
    DjButton *m_automix = nullptr;
    DjButton *m_autoDj = nullptr;
    DjPeopleStrip *m_people = nullptr;
    QPointer<DjJamDialog> m_jamDialog;
    QTimer *m_frame = nullptr;
    QTimer *m_slow = nullptr;
    QTimer *m_noticeTimer = nullptr;
    QTimer *m_railTimer = nullptr;
    QString m_railState;
    QString m_query;
    bool m_fitted = false;
};

}
