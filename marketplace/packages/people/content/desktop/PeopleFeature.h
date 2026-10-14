#pragma once

#include "NativePackageExtension.h"
#include "PeopleModel.h"

#include <QHash>
#include <QPixmap>
#include <QPointer>

#include <functional>
#include <optional>

class QDialog;
class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;
class QVBoxLayout;

namespace forkmesh::people {

class CampfireView final : public QWidget
{
public:
    explicit CampfireView(QWidget *parent = nullptr);

    void setPeople(const QList<Section> &all, std::optional<Ring> focus, const QString &viewer);
    std::function<void(const Ranked &)> onPerson;
    std::function<void(Ring)> onRing;

    QSize sizeHint() const override;
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    struct Hit {
        QRectF rect;
        std::optional<Ranked> person;
        Ring ring = Ring::Embers;
        bool label = false;
    };

    QRectF square() const;
    double side() const;
    double seatSize() const;
    double labelWidth() const;
    void layoutHits();
    const Hit *hitAt(const QPointF &point) const;
    QPixmap avatar(const Member &member, int size);
    void paintFire(QPainter &painter, const QRectF &box);

    QList<Section> m_sections;
    std::optional<Ring> m_focus;
    QString m_viewer;
    QList<Seat> m_seats;
    QList<Hit> m_hits;
    QHash<QString, QPixmap> m_avatars;
    QTimer *m_flicker = nullptr;
    double m_phase = 0;
    int m_hover = -1;
};

class PeopleFeature final : public forkmesh::market::NativePackageWidget
{
public:
    explicit PeopleFeature(const forkmesh::market::NativePackageHost &host, QWidget *parent = nullptr);

    void refresh() override;
    QList<forkmesh::market::NativePackageSearchHit> searchHits(const QString &query, int limit) const override;
    void activateSearchHit(const QString &id) override;
    void setHostSearchQuery(const QString &query) override;

private:
    void render();
    void renderLegend(const QList<Section> &all);
    void renderSections(const QList<Section> &all);
    void toggleRing(Ring ring);
    void openPerson(const Ranked &person);
    QString viewer() const;

    forkmesh::market::NativePackageHost m_host;
    QList<Member> m_members;
    std::optional<Ring> m_focus;
    QString m_query;
    bool m_loading = false;
    bool m_loaded = false;
    QLabel *m_status = nullptr;
    QLineEdit *m_search = nullptr;
    CampfireView *m_fire = nullptr;
    QWidget *m_legend = nullptr;
    QVBoxLayout *m_sections = nullptr;
    QPointer<QDialog> m_dialog;
};

} // namespace forkmesh::people
