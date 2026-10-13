// What the interface's event filter hears: every kind of control, driven with
// synthetic events through UiSounds::cueForEvent, which plays nothing. The cue
// table itself (what each cue sounds like) is checked by the sonifier suite.

#include "UiSounds.h"

#include <QActionGroup>
#include <QApplication>
#include <QButtonGroup>
#include <QComboBox>
#include <QDialog>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollBar>
#include <QShowEvent>
#include <QSlider>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabBar>
#include <QTableWidget>
#include <QVBoxLayout>

#include <iostream>
#include <optional>

using namespace forkmesh::sonify;

static int failures = 0;

static void check(bool ok, const QString &what)
{
    if (ok)
        return;
    std::cerr << "FAIL: " << qPrintable(what) << "\n";
    ++failures;
}

static QString named(std::optional<Cue> cue)
{
    return cue ? cueName(*cue) : QStringLiteral("nothing");
}

static void expect(std::optional<Cue> got, std::optional<Cue> want,
                   const QString &what)
{
    check(got == want, QStringLiteral("%1 (got %2, want %3)")
                           .arg(what, named(got), named(want)));
}

static std::optional<Cue> release(QWidget *widget, QPoint pos,
                                  Qt::MouseButton button = Qt::LeftButton)
{
    QMouseEvent event(QEvent::MouseButtonRelease, QPointF(pos),
                      QPointF(widget->mapToGlobal(pos)), button, Qt::NoButton,
                      Qt::NoModifier);
    return UiSounds::cueForEvent(widget, &event);
}

static std::optional<Cue> releaseCentre(QWidget *widget)
{
    return release(widget, widget->rect().center());
}

static std::optional<Cue> key(QWidget *widget, int code, bool press = true,
                              bool autoRepeat = false)
{
    QKeyEvent event(press ? QEvent::KeyPress : QEvent::KeyRelease, code,
                    Qt::NoModifier, QString(), autoRepeat);
    return UiSounds::cueForEvent(widget, &event);
}

static std::optional<Cue> shown(QWidget *widget, bool show = true)
{
    QEvent event(show ? QEvent::Show : QEvent::Hide);
    return UiSounds::cueForEvent(widget, &event);
}

static void testDefault()
{
    check(!UiSounds::defaultEnabled(),
          QStringLiteral("no sound by default offscreen and in test mode"));
    check(!UiSounds::enabled(),
          QStringLiteral("so the tests never reach the speakers"));
}

static void testButtons()
{
    QWidget window;
    auto *layout = new QVBoxLayout(&window);
    auto *plain = new QPushButton(QStringLiteral("Save"));
    auto *remove = new QPushButton(QStringLiteral("Delete"));
    auto *send = new QPushButton(QStringLiteral("Send"));
    auto *toggle = new QPushButton(QStringLiteral("Wrap"));
    toggle->setCheckable(true);
    auto *railA = new QPushButton(QStringLiteral("Repos"));
    auto *railB = new QPushButton(QStringLiteral("Agents"));
    auto *rail = new QButtonGroup(&window);
    for (QPushButton *button : {railA, railB}) {
        button->setCheckable(true);
        rail->addButton(button);
    }
    railA->setChecked(true);
    auto *radio = new QRadioButton(QStringLiteral("Dark"));
    auto *disabled = new QPushButton(QStringLiteral("Nope"));
    disabled->setEnabled(false);
    for (QWidget *w : {static_cast<QWidget *>(plain), static_cast<QWidget *>(remove),
                       static_cast<QWidget *>(send), static_cast<QWidget *>(toggle),
                       static_cast<QWidget *>(railA), static_cast<QWidget *>(railB),
                       static_cast<QWidget *>(radio), static_cast<QWidget *>(disabled)})
        layout->addWidget(w);
    window.show();

    expect(releaseCentre(plain), Cue::Click, QStringLiteral("a button clicks"));
    expect(release(plain, plain->rect().center(), Qt::RightButton), std::nullopt,
           QStringLiteral("a right click on a button is not a press"));
    expect(release(plain, QPoint(-5, -5)), std::nullopt,
           QStringLiteral("a release that wandered off cancels"));
    expect(releaseCentre(disabled), std::nullopt,
           QStringLiteral("a disabled button is silent"));
    expect(releaseCentre(remove), Cue::Delete,
           QStringLiteral("a Delete button sounds like a delete"));
    expect(releaseCentre(send), Cue::Send,
           QStringLiteral("a Send button sounds like a send"));
    expect(releaseCentre(toggle), Cue::ToggleOn,
           QStringLiteral("an unchecked switch sounds as it turns on"));
    toggle->setChecked(true);
    expect(releaseCentre(toggle), Cue::ToggleOff,
           QStringLiteral("a checked switch sounds as it turns off"));
    expect(releaseCentre(railB), Cue::Navigate,
           QStringLiteral("one of an exclusive set navigates, not toggles"));
    expect(releaseCentre(radio), Cue::Select,
           QStringLiteral("a radio button selects"));

    // The keyboard sounds like the mouse.
    expect(key(plain, Qt::Key_Return), Cue::Click,
           QStringLiteral("Return on a focused button clicks"));
    expect(key(plain, Qt::Key_Return, true, true), std::nullopt,
           QStringLiteral("auto-repeat is silent"));
    expect(key(plain, Qt::Key_Space), std::nullopt,
           QStringLiteral("Space clicks on release, not press"));
    expect(key(plain, Qt::Key_Space, false), std::nullopt,
           QStringLiteral("a Space release with no press is nothing"));
    plain->setDown(true);
    expect(key(plain, Qt::Key_Space, false), Cue::Click,
           QStringLiteral("Space released on a pressed button clicks"));
    plain->setDown(false);
    expect(key(plain, Qt::Key_A), std::nullopt,
           QStringLiteral("other keys on a button are nothing"));
}

static void testControls()
{
    QWidget window;
    window.resize(480, 640);
    auto *layout = new QVBoxLayout(&window);
    auto *tabs = new QTabBar;
    tabs->addTab(QStringLiteral("Code"));
    tabs->addTab(QStringLiteral("Issues"));
    auto *combo = new QComboBox;
    combo->addItems({QStringLiteral("One"), QStringLiteral("Two")});
    auto *slider = new QSlider(Qt::Horizontal);
    auto *scroll = new QScrollBar(Qt::Horizontal);
    auto *spin = new QSpinBox;
    auto *line = new QLineEdit;
    auto *list = new QListWidget;
    list->addItems({QStringLiteral("alpha"), QStringLiteral("beta")});
    auto *table = new QTableWidget(2, 2);
    auto *plainWidget = new QWidget;
    plainWidget->setMinimumHeight(20);
    auto *card = new QWidget;
    card->setMinimumHeight(40);
    card->setCursor(Qt::PointingHandCursor);
    auto *cardLabel = new QLabel(QStringLiteral("inside a card"), card);
    for (QWidget *w : {static_cast<QWidget *>(tabs), static_cast<QWidget *>(combo),
                       static_cast<QWidget *>(slider), static_cast<QWidget *>(scroll),
                       static_cast<QWidget *>(spin), static_cast<QWidget *>(line),
                       static_cast<QWidget *>(list), static_cast<QWidget *>(table),
                       plainWidget, card})
        layout->addWidget(w);
    window.show();
    QApplication::processEvents();

    expect(release(tabs, tabs->tabRect(1).center()), Cue::Navigate,
           QStringLiteral("a tab navigates"));
    tabs->setTabEnabled(1, false);
    expect(release(tabs, tabs->tabRect(1).center()), std::nullopt,
           QStringLiteral("a disabled tab is silent"));
    expect(releaseCentre(combo), Cue::Open, QStringLiteral("a combo opens"));
    combo->showPopup();
    QWidget *popup = combo->view()->window();
    check(popup != &window, QStringLiteral("the combo list is its own window"));
    expect(shown(popup), Cue::Open,
           QStringLiteral("a combo's list appearing opens"));
    expect(shown(popup, false), std::nullopt,
           QStringLiteral("a combo's list going away leaves the pick to speak"));
    expect(release(combo->view()->viewport(),
                   combo->view()->visualRect(combo->model()->index(1, 0)).center()),
           Cue::Select, QStringLiteral("picking a combo row selects"));
    combo->hidePopup();
    expect(releaseCentre(slider), Cue::Select,
           QStringLiteral("letting go of a slider picks a value"));
    expect(releaseCentre(scroll), std::nullopt,
           QStringLiteral("a scroll bar is looking, not doing"));
    expect(releaseCentre(spin), Cue::Click,
           QStringLiteral("a spin box's arrows click"));

    const QPoint row = list->visualItemRect(list->item(1)).center();
    expect(release(list->viewport(), row), Cue::Select,
           QStringLiteral("a list row selects"));
    expect(release(list->viewport(),
                   QPoint(4, list->viewport()->height() - 2)),
           std::nullopt, QStringLiteral("blank space under the rows is silent"));
    expect(release(list, row), std::nullopt,
           QStringLiteral("only the viewport carries a view's clicks"));
    QHeaderView *header = table->horizontalHeader();
    expect(release(header->viewport(),
                   QPoint(header->sectionViewportPosition(0) + 4,
                          header->height() / 2)),
           Cue::Select, QStringLiteral("a table header selects"));
    expect(key(list, Qt::Key_Down), Cue::Select,
           QStringLiteral("arrowing through a list selects"));
    expect(key(list, Qt::Key_Down, true, true), std::nullopt,
           QStringLiteral("a held arrow is one sound"));
    expect(key(list, Qt::Key_X), std::nullopt,
           QStringLiteral("typing into a list is not a selection"));

    expect(key(line, Qt::Key_Return), std::nullopt,
           QStringLiteral("Return in an empty field commits nothing"));
    line->setText(QStringLiteral("forkmesh"));
    expect(key(line, Qt::Key_Return), Cue::Click,
           QStringLiteral("Return in a field commits it"));
    expect(key(line, Qt::Key_A), std::nullopt,
           QStringLiteral("typing is silent"));

    expect(releaseCentre(plainWidget), std::nullopt,
           QStringLiteral("a plain widget is not clickable"));
    expect(releaseCentre(card), Cue::Click,
           QStringLiteral("a pointing-hand widget clicks"));
    expect(release(cardLabel, QPoint(2, 2)), Cue::Click,
           QStringLiteral("the label inside a clickable card clicks with it"));
}

static std::optional<Cue> typed(QWidget *widget, int code, const QString &text,
                                Qt::KeyboardModifiers modifiers = Qt::NoModifier,
                                bool autoRepeat = false)
{
    QKeyEvent event(QEvent::KeyPress, code, modifiers, text, autoRepeat);
    return UiSounds::cueForEvent(widget, &event);
}

static void testTyping()
{
    QPlainTextEdit prompt;
    QPlainTextEdit other;
    QLineEdit search;
    expect(typed(&prompt, Qt::Key_A, QStringLiteral("a")), std::nullopt,
           QStringLiteral("a field that did not ask to tick is silent"));
    prompt.setProperty(kTypingSoundProperty, true);
    search.setProperty(kTypingSoundProperty, true);

    expect(typed(&prompt, Qt::Key_A, QStringLiteral("a")), Cue::Keystroke,
           QStringLiteral("a letter in the prompt ticks"));
    expect(typed(&prompt, Qt::Key_A, QStringLiteral("A"), Qt::ShiftModifier),
           Cue::Keystroke, QStringLiteral("a capital ticks"));
    expect(typed(&prompt, Qt::Key_Space, QStringLiteral(" ")), Cue::Keystroke,
           QStringLiteral("a space ticks"));
    expect(typed(&prompt, Qt::Key_Backspace, QString()), Cue::Keystroke,
           QStringLiteral("Backspace ticks"));
    expect(typed(&prompt, Qt::Key_Delete, QString()), Cue::Keystroke,
           QStringLiteral("Delete ticks"));
    expect(typed(&search, Qt::Key_1, QStringLiteral("1")), Cue::Keystroke,
           QStringLiteral("a line edit can ask to tick too"));

    expect(typed(&prompt, Qt::Key_A, QStringLiteral("a"), Qt::NoModifier, true),
           std::nullopt, QStringLiteral("a held key is one tick"));
    expect(typed(&prompt, Qt::Key_V, QStringLiteral("\x16"), Qt::ControlModifier),
           std::nullopt, QStringLiteral("a shortcut is not typing"));
    expect(typed(&prompt, Qt::Key_Left, QString()), std::nullopt,
           QStringLiteral("moving the caret is not typing"));
    expect(typed(&prompt, Qt::Key_Shift, QString()), std::nullopt,
           QStringLiteral("a bare modifier is not typing"));
    expect(typed(&prompt, Qt::Key_Return, QStringLiteral("\r")), std::nullopt,
           QStringLiteral("Return sends; the send speaks for itself"));
    expect(typed(&other, Qt::Key_A, QStringLiteral("a")), std::nullopt,
           QStringLiteral("other editors stay silent"));

    prompt.setReadOnly(true);
    expect(typed(&prompt, Qt::Key_A, QStringLiteral("a")), std::nullopt,
           QStringLiteral("a read-only prompt types nothing"));
    prompt.setReadOnly(false);
    prompt.setEnabled(false);
    expect(typed(&prompt, Qt::Key_A, QStringLiteral("a")), std::nullopt,
           QStringLiteral("a disabled prompt is silent"));
}

static void testMenusAndWindows()
{
    QMenu menu;
    QAction *open = menu.addAction(QStringLiteral("Open"));
    QAction *remove = menu.addAction(QStringLiteral("Delete branch"));
    QAction *wrap = menu.addAction(QStringLiteral("Wrap lines"));
    wrap->setCheckable(true);
    QAction *off = menu.addAction(QStringLiteral("Unavailable"));
    off->setEnabled(false);
    QMenu *more = menu.addMenu(QStringLiteral("More"));
    more->addAction(QStringLiteral("Inside"));
    auto *group = new QActionGroup(&menu);
    QAction *choice = menu.addAction(QStringLiteral("Sort by name"));
    choice->setCheckable(true);
    group->addAction(choice);
    menu.resize(menu.sizeHint());

    auto at = [&menu](QAction *action) {
        return release(&menu, menu.actionGeometry(action).center());
    };
    expect(at(open), Cue::Click, QStringLiteral("a menu item clicks"));
    expect(at(remove), Cue::Delete,
           QStringLiteral("a Delete menu item sounds like a delete"));
    expect(at(wrap), Cue::ToggleOn,
           QStringLiteral("a checkable menu item toggles"));
    expect(at(off), std::nullopt, QStringLiteral("a disabled item is silent"));
    expect(at(more->menuAction()), std::nullopt,
           QStringLiteral("a submenu speaks when it opens"));
    expect(at(choice), Cue::Select,
           QStringLiteral("an item in an exclusive group selects"));
    expect(shown(&menu), Cue::Open, QStringLiteral("a menu opening"));
    expect(shown(&menu, false), Cue::Close, QStringLiteral("a menu closing"));

    QDialog dialog;
    expect(shown(&dialog), Cue::Open, QStringLiteral("a dialog opening"));
    expect(shown(&dialog, false), Cue::Close,
           QStringLiteral("a dialog closing"));
    QMessageBox critical(QMessageBox::Critical, QStringLiteral("t"),
                         QStringLiteral("broke"));
    expect(shown(&critical), Cue::Error,
           QStringLiteral("a critical message box sounds like an error"));
    QMessageBox warning(QMessageBox::Warning, QStringLiteral("t"),
                        QStringLiteral("careful"));
    expect(shown(&warning), Cue::Warn,
           QStringLiteral("a warning message box sounds like a warning"));
    QMessageBox question(QMessageBox::Question, QStringLiteral("t"),
                         QStringLiteral("sure?"));
    expect(shown(&question), Cue::Open,
           QStringLiteral("any other message box opens"));

    // Panels showing and hiding are not events worth a sound: startup would
    // otherwise play a chord.
    QWidget topLevel;
    auto *child = new QDialog(&topLevel);
    child->setWindowFlags(Qt::Widget); // the constructor insists on Qt::Dialog
    expect(shown(&topLevel), std::nullopt,
           QStringLiteral("a plain window appearing is silent"));
    expect(shown(child), std::nullopt,
           QStringLiteral("a dialog embedded as a panel is silent"));

    QObject notAWidget;
    QEvent event(QEvent::Show);
    expect(UiSounds::cueForEvent(&notAWidget, &event), std::nullopt,
           QStringLiteral("a plain object is never a cue"));
}

static void testQuietArea()
{
    QWidget chat;
    auto *layout = new QVBoxLayout(&chat);
    auto *send = new QPushButton(QStringLiteral("Send"));
    auto *composer = new QPlainTextEdit;
    layout->addWidget(send);
    layout->addWidget(composer);
    composer->setProperty(kTypingSoundProperty, true);
    chat.resize(240, 160);
    chat.show();
    expect(releaseCentre(send), Cue::Send,
           QStringLiteral("a button outside a quiet area sounds"));
    chat.setProperty(kQuietSoundProperty, true);
    expect(releaseCentre(send), std::nullopt,
           QStringLiteral("a button in a quiet area is silent"));
    expect(typed(composer, Qt::Key_A, QStringLiteral("a")), std::nullopt,
           QStringLiteral("typing in a quiet area is silent"));
    QMenu menu(send);
    expect(shown(&menu), std::nullopt,
           QStringLiteral("a menu opened from a quiet area is silent"));
}

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("FORKMESH_SONIFY_BACKEND", "none");
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("ForkMeshTests"));
    app.setApplicationName(QStringLiteral("forkmesh-ui-sounds-tests"));
    testDefault();
    testButtons();
    testControls();
    testTyping();
    testMenusAndWindows();
    testQuietArea();
    if (failures == 0)
        std::cout << "test_ui_sounds: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
