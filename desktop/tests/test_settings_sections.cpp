// Settings is one scrolling page (SettingsSectionsView). Driven offscreen: the
// list on the left jumps to a section and follows the one being read, even the
// last short section reaches the top, search narrows the page to the sections
// mentioning every word and marks each matching control, and a scroll area
// borrowed from another page grows to its content instead of nesting.
#include "SettingsSectionsView.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>

#include <cstdio>

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    std::fprintf(condition ? stdout : stderr, "%s: %s\n",
                 condition ? "PASS" : "FAIL", message);
    if (!condition)
        ++failures;
}

void settle(int milliseconds)
{
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < milliseconds)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

// A section body: its controls, then filler so the page is taller than the
// window and there is something to scroll.
QWidget *body(const QList<QWidget *> &controls, int filler)
{
    auto *widget = new QWidget;
    auto *column = new QVBoxLayout(widget);
    for (QWidget *control : controls)
        column->addWidget(control);
    if (filler > 0) {
        auto *space = new QWidget;
        space->setFixedHeight(filler);
        column->addWidget(space);
    }
    return widget;
}

QString litText(const SettingsSectionsView &view)
{
    const auto *label = qobject_cast<const QLabel *>(view.litHeading());
    return label && label->property("settingsLit").toBool() ? label->text() : QString();
}

} // namespace

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    SettingsSectionsView view;
    view.resize(900, 500);

    auto *avatarCheck = new QCheckBox(QStringLiteral("Show my avatar"));
    auto *profile = body({new QLabel(QStringLiteral("Display name")), avatarCheck}, 300);

    auto *themeCombo = new QComboBox;
    themeCombo->addItems({QStringLiteral("Dark"), QStringLiteral("Light")});
    auto *trayCheck = new QCheckBox(QStringLiteral("Show tray icon"));
    auto *legacyCheck = new QCheckBox(QStringLiteral("Legacy mode"));
    auto *general = body({new QLabel(QStringLiteral("Theme")), themeCombo, trayCheck,
                          legacyCheck},
                         300);
    legacyCheck->hide(); // hidden by its section, as a platform-only row is

    auto *pushToTalk = new QCheckBox(QStringLiteral("Push to talk"));
    pushToTalk->setToolTip(QStringLiteral("Hold the <b>key</b> to record"));
    auto *voice = body({new QLabel(QStringLiteral("Microphone")), pushToTalk}, 300);

    auto *storageList = new QListWidget;
    for (int i = 0; i < 40; ++i)
        storageList->addItem(QStringLiteral("folder %1").arg(i));
    auto *data = body({new QLabel(QStringLiteral("Storage map")), storageList}, 300);

    auto *security = body({new QLabel(QStringLiteral("Report a vulnerability"))}, 0);
    // A row far wider than the window, as a long unwrapped hint is.
    auto *wideRow = new QLabel(QString(400, QLatin1Char('w')));
    auto *wide = body({wideRow}, 0);

    view.addSection(profile, QStringLiteral("Profile"), QStringLiteral("person"));
    view.addSection(general, QStringLiteral("General"), QStringLiteral("gear"));
    view.addSection(voice, QStringLiteral("Voice"), QStringLiteral("mic"));
    view.addSection(data, QStringLiteral("Data"), QStringLiteral("package"));
    view.addSection(security, QStringLiteral("Security"), QStringLiteral("shield-check"));
    view.addSection(wide, QStringLiteral("Wide"));

    int changes = 0;
    int activated = 0;
    QList<int> revealed;
    QObject::connect(&view, &SettingsSectionsView::currentChanged, [&](int) { ++changes; });
    QObject::connect(&view, &SettingsSectionsView::sectionActivated,
                     [&](int) { ++activated; });
    QObject::connect(&view, &SettingsSectionsView::sectionRevealed,
                     [&](int index) { revealed << index; });

    view.show();
    settle(120);

    auto *scroll = view.findChild<QScrollArea *>(QStringLiteral("settingsSectionsScroll"));
    auto *nav = view.findChild<QListWidget *>(QStringLiteral("settingsNav"));
    auto *search = view.findChild<QLineEdit *>(QStringLiteral("settingsSearch"));
    auto *noMatches = view.findChild<QLabel *>(QStringLiteral("settingsNoMatches"));
    check(scroll && nav && search && noMatches, "the page has its list, search box and scroller");
    if (!scroll || !nav || !search || !noMatches)
        return 1;
    QScrollBar *bar = scroll->verticalScrollBar();
    QWidget *voiceFrame = voice->parentWidget();
    QWidget *dataFrame = data->parentWidget();
    QWidget *securityFrame = security->parentWidget();

    check(view.count() == 6 && view.tabText(2) == QStringLiteral("Voice") &&
              view.widget(2) == voice && view.sectionIconName(4) == QStringLiteral("shield-check"),
          "sections keep the tab vocabulary: count, tabText, widget");
    check(nav->count() == 6 && nav->item(3)->text() == QStringLiteral("Data"),
          "every section has an entry in the list on the left");
    check(view.currentIndex() == 0 && nav->currentRow() == 0 && revealed.contains(0),
          "the page opens at its first section, which is revealed");
    check(!revealed.contains(3), "a section far down the page is not revealed yet");

    check(litText(view) == QStringLiteral("Profile"),
          "the heading of the section lit in the list is lit on the page");
    QWidget *profileTitle = view.litHeading();

    view.setCurrentIndex(3);
    settle(60);
    check(view.currentIndex() == 3 && bar->value() == dataFrame->y() && nav->currentRow() == 3,
          "setCurrentIndex scrolls the section's heading to the top and lights its entry");
    check(litText(view) == QStringLiteral("Data") && profileTitle &&
              !profileTitle->property("settingsLit").toBool(),
          "jumping moves the lit heading on the page with the list");
    check(revealed.contains(3), "jumping to a section reveals it");
    check(storageList->height() >= storageList->sizeHint().height() - 1,
          "a list on the long page keeps its preferred height");
    check(voice->width() <= scroll->viewport()->width() &&
              wide->parentWidget()->width() <= scroll->viewport()->width(),
          "one very wide section does not widen the rest of the page");

    view.setCurrentIndex(4);
    settle(60);
    check(view.currentIndex() == 4 && bar->value() == securityFrame->y(),
          "the last, short section still scrolls to the top");

    const int activatedBeforeScroll = activated;
    bar->setValue(voiceFrame->y() + 5);
    settle(30);
    check(view.currentIndex() == 2 && nav->currentRow() == 2,
          "scrolling lights the section being read");
    check(activated == activatedBeforeScroll,
          "scrolling past headings is not a section the person chose");

    nav->setCurrentRow(1);
    settle(60);
    check(view.currentIndex() == 1 && bar->value() == general->parentWidget()->y() &&
              activated == activatedBeforeScroll + 1,
          "clicking an entry jumps to its section and counts as a choice");

    view.setSearchText(QStringLiteral("push talk"));
    settle(30);
    check(view.visibleSectionTitles() == QStringList{QStringLiteral("Voice")},
          "search keeps only the sections mentioning every word");
    check(nav->item(0)->isHidden() && !nav->item(2)->isHidden(),
          "the list hides the sections the search hid");
    check(view.searchHits().contains(pushToTalk), "the matching control is marked");

    view.setSearchText(QStringLiteral("hold the key"));
    settle(30);
    check(view.visibleSectionTitles() == QStringList{QStringLiteral("Voice")} &&
              view.searchHits().contains(pushToTalk),
          "a control's tooltip is searched, rich text included");

    view.setCurrentIndex(2);
    settle(60);
    const int activatedBeforeSearch = activated;
    view.setSearchText(QStringLiteral("light"));
    settle(60);
    check(view.visibleSectionTitles() == QStringList{QStringLiteral("General")} &&
              view.searchHits().contains(themeCombo),
          "a choice inside a drop-down is searched");
    check(view.currentIndex() == 1 && nav->currentRow() == 1 &&
              nav->item(1)->isSelected() && activated == activatedBeforeSearch,
          "narrowing the page lights the first section left, without counting "
          "the hidden entry as a jump");
    QImage marked(view.size(), QImage::Format_ARGB32);
    marked.fill(Qt::white);
    view.render(&marked);
    // The amber outline sits a few pixels outside the control's left edge.
    int amber = 0;
    for (int dx = -6; dx <= -1; ++dx) {
        const QColor edge = marked.pixelColor(
            themeCombo->mapTo(&view, QPoint(dx, themeCombo->height() / 2)));
        amber = qMax(amber, edge.red() - edge.blue());
    }
    check(amber > 60, "the match is outlined on the page, even when drawn off screen");

    view.setSearchText(QStringLiteral("SECURITY"));
    settle(30);
    check(view.visibleSectionTitles() == QStringList{QStringLiteral("Security")},
          "a section title matches, whatever the case");
    view.setSectionHidden(4, true);
    settle(30);
    check(view.isSectionHidden(4) && view.visibleSectionTitles().isEmpty() &&
              nav->item(4)->isHidden(),
          "a hidden section stays out of the search results");
    view.setSearchText(QString());
    settle(30);
    check(!view.visibleSectionTitles().contains(QStringLiteral("Security")) &&
              view.visibleSectionTitles().contains(QStringLiteral("Data")) &&
              nav->item(4)->isHidden(),
          "a hidden section stays off the page when the search clears");
    view.setSectionHidden(4, false);
    settle(30);
    check(view.visibleSectionTitles().contains(QStringLiteral("Security")) &&
              !nav->item(4)->isHidden(),
          "showing the section again puts it back on the page and the list");
    view.setSearchText(QStringLiteral("SECURITY"));
    settle(30);

    view.setSearchText(QStringLiteral("legacy"));
    settle(30);
    check(view.visibleSectionTitles().isEmpty() && noMatches->isVisibleTo(&view),
          "a control its section hides is not offered, and no match says so");

    view.setSearchText(QStringLiteral("show"));
    settle(30);
    const QList<QWidget *> hits = view.searchHits();
    check(hits == QList<QWidget *>{avatarCheck, trayCheck},
          "matches are listed in page order");
    view.nextSearchHit();
    const int first = view.currentHit();
    view.nextSearchHit();
    const int second = view.currentHit();
    view.nextSearchHit();
    check(first == 0 && second == 1 && view.currentHit() == 0,
          "Enter steps through the matches and wraps around");

    view.setSearchText(QStringLiteral("voice"));
    settle(30);
    view.setCurrentIndex(4);
    settle(60);
    check(view.searchText().isEmpty() && view.visibleSectionTitles().size() == 6 &&
              view.currentIndex() == 4,
          "jumping to a section the search hid clears the search");

    view.setSearchText(QStringLiteral("microphone"));
    settle(30);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(search, &escape);
    settle(30);
    check(view.searchText().isEmpty() && view.visibleSectionTitles().size() == 6 &&
              !noMatches->isVisibleTo(&view) && view.searchHits().isEmpty(),
          "Esc clears the search and brings every section back");

    view.focusSearch();
    check(search->hasFocus() || !view.isActiveWindow(),
          "Ctrl+F on the page puts the cursor in the search box");
    search->setText(QStringLiteral("tray"));
    settle(400);
    check(view.visibleSectionTitles() == QStringList{QStringLiteral("General")},
          "typing searches after a short pause");
    view.setSearchText(QString());

    auto *inner = new QWidget;
    auto *innerColumn = new QVBoxLayout(inner);
    for (int i = 0; i < 30; ++i)
        innerColumn->addWidget(new QLabel(QStringLiteral("profile row %1").arg(i)));
    auto *borrowed = new QScrollArea;
    borrowed->setWidgetResizable(true);
    borrowed->setWidget(inner);
    auto *host = new QWidget;
    auto *hostRow = new QHBoxLayout(host);
    hostRow->setContentsMargins(0, 0, 0, 0);
    hostRow->addWidget(borrowed, 1);
    view.addSection(host, QStringLiteral("Borrowed"));
    SettingsSectionsView::setScrollAreaFlattened(borrowed, true);
    settle(120);
    check(borrowed->height() >= inner->sizeHint().height() &&
              borrowed->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff,
          "a flattened scroll area grows to its content instead of scrolling inside the page");
    SettingsSectionsView::setScrollAreaFlattened(borrowed, false);
    settle(30);
    check(borrowed->verticalScrollBarPolicy() == Qt::ScrollBarAsNeeded &&
              borrowed->maximumHeight() == QWIDGETSIZE_MAX,
          "unflattened, it scrolls on its own again for its home page");

    // Opening Settings at one control (a ping kind's row, #347): the page
    // lands on its section and then brings the control itself into view.
    auto *deep = new QLabel(QStringLiteral("Deep control"));
    data->layout()->addWidget(deep);
    view.setCurrentIndex(0);
    settle(60);
    view.revealWidget(deep);
    settle(120);
    const int deepY = deep->mapTo(scroll->widget(), QPoint(0, 0)).y();
    check(view.currentIndex() == 3 && deepY >= bar->value() &&
              deepY + deep->height() <= bar->value() + scroll->viewport()->height(),
          "revealWidget scrolls a control deep in a section into view");

    int builds = 0;
    auto *lazyCheck = new QCheckBox(QStringLiteral("Rotate backup keys"));
    const int lazy = view.addLazySection(
        [&] {
            ++builds;
            return body({lazyCheck}, 0);
        },
        QStringLiteral("Backups"));
    QList<QPair<int, bool>> toggles;
    QObject::connect(&view, &SettingsSectionsView::expandedChanged,
                     [&](int index, bool expanded) { toggles << qMakePair(index, expanded); });
    settle(60);
    check(!view.isBuilt(lazy) && !view.isExpanded(lazy) && builds == 0 &&
              view.widget(lazy) == nullptr,
          "a lazy section starts collapsed and is not built");
    check(view.collapsedTitles() == QStringList{QStringLiteral("Backups")},
          "the collapsed sections are listed by title");

    view.setSearchText(QStringLiteral("rotate"));
    settle(30);
    check(builds == 1 && view.visibleSectionTitles() == QStringList{QStringLiteral("Backups")} &&
              view.searchHits().contains(lazyCheck) && lazyCheck->isVisibleTo(&view),
          "search builds and opens a collapsed section that matches");
    view.setSearchText(QString());
    settle(30);
    check(!view.isExpanded(lazy) && !lazyCheck->isVisibleTo(&view) && builds == 1,
          "clearing the search folds it away again without rebuilding it");

    auto *header = view.findChildren<QWidget *>(QStringLiteral("settingsSectionHeader")).value(2);
    QWidget *voiceBody = view.widget(2);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(40, 5), header->mapToGlobal(QPointF(40, 5)),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(40, 5),
                        header->mapToGlobal(QPointF(40, 5)), Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QApplication::sendEvent(header, &press);
    QApplication::sendEvent(header, &release);
    settle(60);
    check(!view.isExpanded(2) && !voiceBody->isVisibleTo(&view) &&
              voiceFrame->height() < 80 && toggles.contains(qMakePair(2, false)),
          "clicking a section's heading folds it down to the heading");

    revealed.clear();
    bar->setValue(voiceFrame->y());
    settle(60);
    check(!revealed.contains(2), "a folded section is not revealed by scrolling past it");

    view.setCurrentIndex(2);
    settle(60);
    check(view.isExpanded(2) && voiceBody->isVisibleTo(&view) && revealed.contains(2),
          "jumping to a folded section opens it");

    view.setCollapsedTitles({QStringLiteral("Voice")});
    settle(30);
    check(!view.isExpanded(2) && view.isExpanded(lazy) && view.isBuilt(lazy) && builds == 1 &&
              lazyCheck->isVisibleTo(&view),
          "restoring the collapsed list opens the rest");

    check(changes > 0, "the section being read is announced as it changes");

    SettingsSectionsView paged;
    paged.resize(1500, 520);
    const auto card = [](QWidget *parent, const QString &heading, int height) {
        auto *frame = new QWidget(parent);
        frame->setProperty("sectionCard", true);
        auto *column = new QVBoxLayout(frame);
        auto *label = new QLabel(heading);
        label->setObjectName(QStringLiteral("sectionLabel"));
        column->addWidget(label);
        auto *space = new QWidget;
        space->setFixedHeight(height);
        column->addWidget(space);
        parent->layout()->addWidget(frame);
        return frame;
    };
    auto *intro = body({}, 200);
    auto *keys = new QWidget;
    new QVBoxLayout(keys);
    card(keys, QStringLiteral("GENERATE SSH KEY"), 260);
    QWidget *publicCard = card(keys, QStringLiteral("PUBLIC KEY"), 260);
    QWidget *hiddenCard = card(keys, QStringLiteral("DISCORD"), 40);
    hiddenCard->hide();
    auto *nestedHolder = new QWidget;
    new QVBoxLayout(nestedHolder);
    card(nestedHolder, QStringLiteral("GIT"), 30);
    keys->layout()->addWidget(nestedHolder);
    QWidget *usageCard = card(keys, QStringLiteral("USAGE & SPEND"), 400);
    int pagedBuilds = 0;
    paged.addSection(intro, QStringLiteral("Intro"));
    paged.addSection(keys, QStringLiteral("Keys"));
    const int deferred = paged.addLazySection(
        [&] {
            ++pagedBuilds;
            return body({new QLabel(QStringLiteral("Late"))}, 100);
        },
        QStringLiteral("Late"));
    paged.addSection(body({}, 2400), QStringLiteral("Tall"));
    const int farLazy = paged.addLazySection(
        [&] {
            ++pagedBuilds;
            return body({new QLabel(QStringLiteral("Far"))}, 100);
        },
        QStringLiteral("Far"));
    paged.setCollapsedTitles({});
    paged.show();
    settle(120);

    auto *pagedNav = paged.findChild<QListWidget *>(QStringLiteral("settingsNav"));
    auto *pagedScroll =
        paged.findChild<QScrollArea *>(QStringLiteral("settingsSectionsScroll"));
    QScrollBar *pagedBar = pagedScroll->verticalScrollBar();
    check(pagedScroll->widget()->width() <= 920 &&
              pagedScroll->widget()->x() > 0,
          "the page is one centred column on a wide window");
    check(paged.isExpanded(farLazy) && !paged.isBuilt(farLazy) && pagedBuilds <= 1,
          "an open section far below the fold waits to be built");
    check(paged.subSectionTitles(1) ==
              QStringList({QStringLiteral("Generate SSH key"), QStringLiteral("Public key"),
                           QStringLiteral("Usage & spend")}),
          "a section's cards are its sub-sections, in sentence case, without "
          "hidden or nested cards");
    check(paged.subSectionTitles(0).isEmpty(), "a section without cards has none");

    const int closedHeight = pagedNav->visualItemRect(pagedNav->item(1)).height();
    paged.setCurrentIndex(1);
    settle(60);
    const int openHeight = pagedNav->visualItemRect(pagedNav->item(1)).height();
    check(openHeight >= closedHeight * 3 && paged.currentSubSection() == 0 &&
              pagedNav->currentRow() == 1,
          "the section being read opens its sub-sections in the list and lights the first");
    check(litText(paged) == QStringLiteral("GENERATE SSH KEY"),
          "the page lights the heading of the sub-section lit in the list");
    check(pagedNav->visualItemRect(pagedNav->item(0)).height() == closedHeight,
          "the other sections stay folded in the list");

    pagedBar->setValue(publicCard->mapTo(pagedScroll->widget(), QPoint(0, 0)).y() + 4);
    settle(30);
    check(paged.currentIndex() == 1 && paged.currentSubSection() == 1,
          "scrolling lights the sub-section being read");
    check(litText(paged) == QStringLiteral("PUBLIC KEY"),
          "scrolling moves the lit heading on the page to the sub-section being read");

    auto *sticky = paged.findChild<QWidget *>(QStringLiteral("settingsStickyHeader"));
    auto *stickyTitle = paged.findChild<QLabel *>(QStringLiteral("settingsStickyTitle"));
    auto *stickySub = paged.findChild<QLabel *>(QStringLiteral("settingsStickySub"));
    check(sticky && stickyTitle && stickySub && sticky->isVisible() && sticky->y() == 0 &&
              stickyTitle->text() == QStringLiteral("Keys") && stickySub->isVisible() &&
              stickySub->text() == QStringLiteral("PUBLIC KEY") &&
              stickySub->property("settingsLit").toBool(),
          "the heading and sub-heading being read stay pinned to the top of the page");

    const QList<QWidget *> pagedFrames =
        pagedScroll->widget()->findChildren<QWidget *>(QStringLiteral("settingsSection"));
    QWidget *keysFrame = pagedFrames.value(1);
    QWidget *lateFrame = pagedFrames.value(2);
    pagedBar->setValue(lateFrame->y() - 30);
    settle(30);
    check(paged.currentIndex() == 1 && sticky->isVisible() && sticky->y() < 0 &&
              sticky->y() + sticky->height() <= 30,
          "the next section's heading pushes the pinned heading up and out");

    pagedBar->setValue(publicCard->mapTo(pagedScroll->widget(), QPoint(0, 0)).y() + 4);
    settle(30);
    const QPoint titlePoint(sticky->width() / 2, stickyTitle->geometry().center().y());
    QMouseEvent stickyPress(QEvent::MouseButtonPress, QPointF(titlePoint),
                            sticky->mapToGlobal(QPointF(titlePoint)), Qt::LeftButton,
                            Qt::LeftButton, Qt::NoModifier);
    QMouseEvent stickyRelease(QEvent::MouseButtonRelease, QPointF(titlePoint),
                              sticky->mapToGlobal(QPointF(titlePoint)), Qt::LeftButton,
                              Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(sticky, &stickyPress);
    QApplication::sendEvent(sticky, &stickyRelease);
    settle(60);
    check(pagedBar->value() == keysFrame->y() && !sticky->isVisible(),
          "clicking the pinned heading goes back to the top of its section");

    const int activatedBeforeSub = activated;
    int pagedActivated = 0;
    QObject::connect(&paged, &SettingsSectionsView::sectionActivated,
                     [&](int) { ++pagedActivated; });
    const QRect row = pagedNav->visualItemRect(pagedNav->item(1));
    const QPoint usageLine(row.center().x(), row.bottom() - 3);
    QMouseEvent subPress(QEvent::MouseButtonPress, QPointF(usageLine),
                         pagedNav->viewport()->mapToGlobal(QPointF(usageLine)), Qt::LeftButton,
                         Qt::LeftButton, Qt::NoModifier);
    QMouseEvent subRelease(QEvent::MouseButtonRelease, QPointF(usageLine),
                           pagedNav->viewport()->mapToGlobal(QPointF(usageLine)),
                           Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(pagedNav->viewport(), &subPress);
    QApplication::sendEvent(pagedNav->viewport(), &subRelease);
    settle(60);
    const int usageY = usageCard->mapTo(pagedScroll->widget(), QPoint(0, 0)).y();
    const int pinnedBottom = sticky->isVisible() ? sticky->y() + sticky->height() : 0;
    check(paged.currentSubSection() == 2 && pagedBar->value() + pinnedBottom <= usageY &&
              pagedBar->value() + pinnedBottom >= usageY - 20 && pagedActivated == 1 &&
              activated == activatedBeforeSub,
          "clicking a sub-section in the list scrolls to its card and counts as a choice");

    pagedBar->setValue(pagedBar->maximum());
    settle(60);
    check(paged.isBuilt(deferred) && paged.isBuilt(farLazy) && pagedBuilds == 2,
          "scrolling towards an open section builds it before it is seen");
    check(pagedNav->visualItemRect(pagedNav->item(1)).height() == closedHeight,
          "leaving a section folds its sub-sections away in the list");
    std::printf(failures ? "%d FAILED\n" : "ALL PASSED\n", failures);
    return failures ? 1 : 0;
}
