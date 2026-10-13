#pragma once

// The "Sponsored" strip's widget tree, with no MainWindow and no Q_OBJECT so a
// standalone probe can render it under Theme.h's sheets. MainWindowAds.cpp
// owns behaviour (fetching, clicks, Hide); this only builds and fills.

#include "AdBannerModel.h"

class QFrame;
class QLabel;
class QPushButton;
class QWidget;

namespace forkmesh::ads {

struct AdBannerWidgets {
    QFrame *banner = nullptr;      // #adBanner, hidden until an ad is applied
    QLabel *caption = nullptr;     // always "Sponsored", never squeezed
    QLabel *sponsor = nullptr;     // "by <sponsor>"; hidden when the ad names none
    QLabel *headline = nullptr;
    QLabel *body = nullptr;        // hidden when the ad has no body
    QPushButton *cta = nullptr;    // opens the destination
    QPushButton *why = nullptr;    // "Why this ad?"
    QPushButton *advertise = nullptr;
    QPushButton *hide = nullptr;
};

AdBannerWidgets buildAdBanner(QWidget *parent);
// The labels and the call-to-action of a strip buildAdBanner() made, found
// again by object name so an owner only has to keep the frame. The three link
// buttons are left null: they are wired once, at build time.
AdBannerWidgets adBannerWidgets(QFrame *banner);

// Every label showing an API string is Qt::PlainText, so markup in an ad is
// shown as the characters it is.
void applyAdBannerContent(const AdBannerWidgets &widgets,
                          const AdBannerContent &content);

} // namespace forkmesh::ads
