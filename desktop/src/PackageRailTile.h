#pragma once

#include "NativePackageExtension.h"

#include <QAbstractButton>
#include <QColor>

namespace forkmesh::market {

QColor packageRailToneColor(NativePackageRailTone tone, bool dark);
QString packageRailTileToolTip(const NativePackageRailTile &tile);
QString packageRailStateKey(const NativePackageRailTile &tile);
int packageRailUrgency(const NativePackageRailTile &tile);
bool packageRailNeedsMoreAttention(const NativePackageRailTile &candidate,
                                   const NativePackageRailTile &current);
QString packageRailReminderTitle(const NativePackageRailTile &tile);
QString packageRailReminderBody(const NativePackageRailTile &tile);

class PackageRailTileButton final : public QAbstractButton
{
public:
    static constexpr int kSize = 54;

    PackageRailTileButton(const NativePackageRailTile &tile, bool dark,
                          QWidget *parent = nullptr);
    QSize sizeHint() const override;
    const NativePackageRailTile &tile() const { return m_tile; }

protected:
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    NativePackageRailTile m_tile;
    bool m_dark = false;
    bool m_hovered = false;
};

} // namespace forkmesh::market
