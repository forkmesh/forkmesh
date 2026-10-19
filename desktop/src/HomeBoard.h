#pragma once

#include "HomeLayout.h"

#include <QElapsedTimer>
#include <QHash>
#include <QPixmap>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <functional>

class QMenu;
class QTimer;

struct HomeTile {
    QString key;
    QString title;
    QString icon;
    QString detail;
    QString tooltip;
    int badge = 0;
    int pending = 0;
    bool urgent = false;
    bool live = false;
    bool alert = false;
    bool desktopOffline = false;

    bool operator==(const HomeTile &other) const
    {
        return key == other.key && title == other.title && icon == other.icon &&
               detail == other.detail && tooltip == other.tooltip &&
               badge == other.badge && pending == other.pending &&
               urgent == other.urgent && live == other.live &&
               alert == other.alert &&
               desktopOffline == other.desktopOffline;
    }
    bool operator!=(const HomeTile &other) const { return !(*this == other); }
};

struct HomeGroup {
    QString id;
    QString title;
    QVector<HomeTile> tiles;

    bool operator==(const HomeGroup &other) const
    {
        return id == other.id && title == other.title && tiles == other.tiles;
    }
    bool operator!=(const HomeGroup &other) const { return !(*this == other); }
};

class HomeBoard : public QWidget
{
public:
    static constexpr int kTileWidth = 132;
    static constexpr int kTileHeight = 132;
    static constexpr int kGap = 10;
    static constexpr int kPlate = 64;
    static constexpr int kGlyph = 36;
    static constexpr int kPullThreshold = 120;
    static constexpr int kPullDragPx = 56;
    static constexpr int kSwipeThreshold = 120;

    explicit HomeBoard(QWidget *parent = nullptr);

    bool setGroups(const QVector<HomeGroup> &groups);
    void setSavedPages(const HomeLayout::Pages &saved);
    void setFilter(const QString &text);
    void setPage(int page);
    void setIconProvider(
        std::function<QPixmap(const QString &, const QColor &, int)> provider)
    {
        m_iconProvider = std::move(provider);
    }
    void setOnActivate(std::function<void(const QString &)> handler)
    {
        m_onActivate = std::move(handler);
    }
    void setTileMenu(std::function<void(QMenu *, const QString &)> handler)
    {
        m_tileMenu = std::move(handler);
    }
    void setOnLayoutChanged(
        std::function<void(const HomeLayout::Pages &)> handler)
    {
        m_onLayoutChanged = std::move(handler);
    }
    void setOnPagesChanged(std::function<void(int, int)> handler)
    {
        m_onPagesChanged = std::move(handler);
    }
    void setOnPull(std::function<void()> handler)
    {
        m_onPull = std::move(handler);
    }
    void setPullGate(std::function<bool()> gate)
    {
        m_pullGate = std::move(gate);
    }
    void setOnType(std::function<void(const QString &)> handler)
    {
        m_onType = std::move(handler);
    }

    QMenu *contextMenuFor(const QPoint &pos);

    int page() const { return m_page; }
    int pageCount() const { return m_pages.size(); }
    HomeLayout::Pages pages() const { return m_pages; }
    bool customized() const { return !m_saved.isEmpty(); }
    int contentHeight() const { return m_contentHeight; }
    int visibleCount() const { return m_tiles.size(); }
    QStringList visibleKeys() const;
    QRect tileRect(const QString &key) const;
    QString firstVisibleKey() const;
    bool animating() const;
    bool dragging() const { return !m_dragKey.isEmpty(); }

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    bool event(QEvent *event) override;

private:
    struct PlacedTile {
        int group = 0;
        int tile = 0;
        QRect rect;
    };

    void relayout(int width);
    void syncAnimation();
    void applyLayout(const HomeLayout::Pages &saved);
    void notifyPages();
    void paintTile(QPainter &painter, const HomeTile &tile, const QString &group,
                   const QRect &rect, bool hovered) const;
    int tileAt(const QPoint &pos) const;
    int slotAt(const QPoint &pos) const;
    const HomeTile &tileOf(const PlacedTile &placed) const;
    bool matches(const HomeGroup &group, const HomeTile &tile) const;
    QStringList presentKeys() const;
    void endDrag(bool commit);

    QVector<HomeGroup> m_groups;
    QHash<QString, QPair<int, int>> m_index;
    HomeLayout::Pages m_saved;
    HomeLayout::Pages m_pages;
    QVector<PlacedTile> m_tiles;
    QString m_filter;
    int m_page = 0;
    int m_hover = -1;
    int m_contentHeight = 0;
    int m_columns = 1;
    int m_left = kGap;
    int m_phase = 0;
    int m_pull = 0;
    int m_swipe = 0;
    int m_notifiedCount = -1;
    int m_notifiedPage = -1;
    QElapsedTimer m_lastFlip;
    QPoint m_pressPos;
    int m_pressIndex = -1;
    bool m_pressed = false;
    bool m_pulled = false;
    QString m_dragKey;
    QPoint m_dragPos;
    QPoint m_dragOffset;
    int m_dragTarget = -1;
    QTimer *m_pulse = nullptr;
    std::function<QPixmap(const QString &, const QColor &, int)> m_iconProvider;
    std::function<void(const QString &)> m_onActivate;
    std::function<void(QMenu *, const QString &)> m_tileMenu;
    std::function<void(const HomeLayout::Pages &)> m_onLayoutChanged;
    std::function<void(int, int)> m_onPagesChanged;
    std::function<void()> m_onPull;
    std::function<bool()> m_pullGate;
    std::function<void(const QString &)> m_onType;
};

class HomePageDots : public QWidget
{
public:
    explicit HomePageDots(QWidget *parent = nullptr);

    void setPages(int count, int current);
    void setOnPick(std::function<void(int)> handler)
    {
        m_onPick = std::move(handler);
    }
    int count() const { return m_count; }
    int current() const { return m_current; }
    QRect dotRect(int index) const;
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    int m_count = 0;
    int m_current = 0;
    std::function<void(int)> m_onPick;
};
