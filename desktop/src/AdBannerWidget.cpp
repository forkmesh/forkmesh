#include "AdBannerWidget.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace forkmesh::ads {

namespace {

QLabel *plainLabel(const char *objectName, QWidget *parent)
{
    auto *label = new QLabel(parent);
    label->setObjectName(QLatin1String(objectName));
    // Ad text is untrusted: never auto-detect rich text, never follow links.
    label->setTextFormat(Qt::PlainText);
    label->setOpenExternalLinks(false);
    label->setTextInteractionFlags(Qt::NoTextInteraction);
    return label;
}

QPushButton *flatButton(const char *objectName, const QString &text,
                        QWidget *parent)
{
    auto *button = new QPushButton(text, parent);
    button->setObjectName(QLatin1String(objectName));
    button->setCursor(Qt::PointingHandCursor);
    button->setFlat(true);
    button->setFocusPolicy(Qt::TabFocus);
    button->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    return button;
}

} // namespace

AdBannerWidgets buildAdBanner(QWidget *parent)
{
    AdBannerWidgets w;
    w.banner = new QFrame(parent);
    w.banner->setObjectName(QStringLiteral("adBanner"));
    w.banner->setFrameShape(QFrame::NoFrame);
    w.banner->setAccessibleName(QStringLiteral("Sponsored message"));
    // Preferred, not Maximum or Fixed: the wrapped body makes the strip
    // height-for-width, and a cap at sizeHint() clips the last line.
    w.banner->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);

    w.caption = plainLabel("adBannerCaption", w.banner);
    w.caption->setText(QStringLiteral("Sponsored"));
    w.caption->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    w.sponsor = plainLabel("adBannerSponsor", w.banner);
    w.headline = plainLabel("adBannerHeadline", w.banner);
    w.body = plainLabel("adBannerBody", w.banner);
    w.body->setWordWrap(true);
    w.body->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    w.body->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    // A QLabel's minimum is its whole text. Left alone, a 60-character
    // headline beside a 48-character sponsor would set the window's minimum
    // width; an explicit minimum lets a narrow window clip them instead. The
    // "Sponsored" caption keeps its full width so the label is never lost.
    w.sponsor->setMinimumWidth(1);
    w.sponsor->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    w.headline->setMinimumWidth(1);
    w.headline->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);

    w.cta = flatButton("adBannerCta", QStringLiteral("Learn more"), w.banner);
    w.cta->setFlat(false);
    w.why = flatButton("adBannerLink", QStringLiteral("Why this ad?"), w.banner);
    w.why->setToolTip(QStringLiteral(
        "How ForkMesh Ads works: text only, reviewed by a person, and chosen "
        "without knowing anything about you."));
    w.advertise = flatButton("adBannerLink", QStringLiteral("Advertise"), w.banner);
    w.advertise->setToolTip(QStringLiteral("Place your own sponsored message"));
    w.hide = flatButton("adBannerHide", QStringLiteral("Hide"), w.banner);
    w.hide->setToolTip(QStringLiteral(
        "Hide sponsored messages for 7 days. Settings > General turns them "
        "off for good."));

    auto *titleRow = new QHBoxLayout;
    titleRow->setContentsMargins(0, 0, 0, 0);
    titleRow->setSpacing(8);
    titleRow->addWidget(w.caption);
    titleRow->addWidget(w.sponsor);
    titleRow->addWidget(w.headline);
    titleRow->addStretch(1);

    auto *text = new QVBoxLayout;
    text->setContentsMargins(0, 0, 0, 0);
    text->setSpacing(1);
    text->addLayout(titleRow);
    text->addWidget(w.body);

    auto *row = new QHBoxLayout(w.banner);
    row->setContentsMargins(10, 4, 10, 4);
    row->setSpacing(8);
    row->addLayout(text, 1);
    row->addWidget(w.cta, 0, Qt::AlignVCenter);
    row->addWidget(w.why, 0, Qt::AlignVCenter);
    row->addWidget(w.advertise, 0, Qt::AlignVCenter);
    row->addWidget(w.hide, 0, Qt::AlignVCenter);

    w.banner->hide();
    return w;
}

AdBannerWidgets adBannerWidgets(QFrame *banner)
{
    AdBannerWidgets w;
    if (!banner)
        return w;
    w.banner = banner;
    w.caption = banner->findChild<QLabel *>(QStringLiteral("adBannerCaption"));
    w.sponsor = banner->findChild<QLabel *>(QStringLiteral("adBannerSponsor"));
    w.headline = banner->findChild<QLabel *>(QStringLiteral("adBannerHeadline"));
    w.body = banner->findChild<QLabel *>(QStringLiteral("adBannerBody"));
    w.cta = banner->findChild<QPushButton *>(QStringLiteral("adBannerCta"));
    return w;
}

void applyAdBannerContent(const AdBannerWidgets &w, const AdBannerContent &content)
{
    if (!w.banner || !w.sponsor || !w.headline || !w.body || !w.cta)
        return;
    w.sponsor->setText(QStringLiteral("by %1").arg(content.sponsor));
    w.sponsor->setVisible(!content.sponsor.isEmpty());
    w.headline->setText(content.headline);
    w.body->setText(content.body);
    w.body->setVisible(!content.body.isEmpty());
    // QPushButton text is always plain, but '&' would become a mnemonic.
    QString cta = content.cta;
    cta.replace(QLatin1Char('&'), QStringLiteral("&&"));
    w.cta->setText(cta);
    // Tooltips sniff for rich text, so this one is rich text on purpose with
    // the destination escaped into it.
    w.cta->setToolTip(QStringLiteral("<span>Opens %1 in your browser</span>")
                          .arg(content.url.toDisplayString().toHtmlEscaped()));
    w.cta->setAccessibleName(
        QStringLiteral("%1 (opens %2 in your browser)")
            .arg(content.cta, content.url.host()));
}

} // namespace forkmesh::ads
