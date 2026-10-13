#include "AquariumOverlay.h"

#include "PetHostFeed.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFontInfo>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QRadioButton>
#include <QTabBar>
#include <QTextBlock>
#include <QTextDocumentFragment>
#include <QTextEdit>
#include <QTimer>
#include <QWindow>

#include <algorithm>

namespace forkmesh::aquarium {
namespace {

constexpr int kMaxLandmarks = 400;
constexpr double kShotEvery = 8;
constexpr double kRosterEvery = 1.5;
constexpr int kGrace = 400;
constexpr double kBarHold = 1.0;

QString plain(const QString &text)
{
    QString value = Qt::mightBeRichText(text) ? QTextDocumentFragment::fromHtml(text).toPlainText() : text;
    value.remove(QLatin1Char('&'));
    return value.simplified();
}

QWidget *titleBarAt(QWidget *window, const QPoint &pos)
{
    const QList<QWidget *> bars = window->findChildren<QWidget *>(QStringLiteral("windowChromeBar"));
    for (QWidget *bar : bars) {
        if (bar->isWindow() || !bar->isVisible())
            continue;
        bool inside = true;
        for (QWidget *clip = bar; inside && clip && clip != window; clip = clip->parentWidget())
            inside = clip->rect().contains(clip->mapFrom(window, pos));
        if (!inside)
            continue;
        const QPoint local = bar->mapFrom(window, pos);
        for (QWidget *hit = bar->childAt(local); hit && hit != bar; hit = hit->parentWidget()) {
            if (qobject_cast<QAbstractButton *>(hit) || (!qobject_cast<QLabel *>(hit) && hit->focusPolicy() != Qt::NoFocus))
                return nullptr;
        }
        return bar;
    }
    return nullptr;
}

QRect labelText(const QLabel *label, const QRect &whole, const QString &text)
{
    if (label->wordWrap() || text.isEmpty())
        return whole;
    const QFontMetrics metrics(label->font());
    const QRect inner = whole.marginsRemoved(label->contentsMargins());
    const int width = std::min(inner.width(), metrics.horizontalAdvance(text));
    const int height = std::min(inner.height(), metrics.height());
    int x = inner.left();
    if (label->alignment() & Qt::AlignHCenter)
        x = inner.center().x() - width / 2;
    else if (label->alignment() & Qt::AlignRight)
        x = inner.right() - width;
    int y = inner.center().y() - height / 2;
    if (label->alignment() & Qt::AlignTop)
        y = inner.top();
    else if (label->alignment() & Qt::AlignBottom)
        y = inner.bottom() - height;
    return QRect(x, y, width, height);
}

bool heading(const QWidget *widget)
{
    const QString name = widget->objectName().toLower();
    if (name.contains(QLatin1String("title")) || name.contains(QLatin1String("heading")))
        return true;
    const int base = QFontInfo(QApplication::font()).pixelSize();
    return QFontInfo(widget->font()).pixelSize() >= base * 1.2;
}

struct Scan {
    QWidget *window = nullptr;
    const QWidget *skip = nullptr;
    QList<Landmark> marks;

    void add(Spot kind, const QRect &rect, const QRect &clip, const QString &text)
    {
        const QRect visible = rect.intersected(clip);
        if (visible.width() < 6 || visible.height() < 6 || marks.size() >= kMaxLandmarks)
            return;
        marks.append({kind, QRectF(visible), text.left(120)});
    }

    QRect place(QWidget *widget) const
    {
        return QRect(widget->mapTo(window, QPoint(0, 0)), widget->size());
    }

    void rows(QAbstractItemView *view, const QRect &clip)
    {
        QWidget *port = view->viewport();
        const QRect area(port->mapTo(window, QPoint(0, 0)), port->size());
        const QRect visible = area.intersected(clip);
        int y = 1;
        for (int count = 0; count < 20 && y < port->height(); ++count) {
            QModelIndex index = view->indexAt(QPoint(4, y));
            if (!index.isValid())
                index = view->indexAt(QPoint(port->width() / 2, y));
            if (!index.isValid())
                break;
            const QRect cell = view->visualRect(index);
            if (cell.height() <= 0)
                break;
            QString text = index.data(Qt::DisplayRole).toString();
            for (int column = 0; text.simplified().isEmpty() && column < 4; ++column)
                text = index.siblingAtColumn(column).data(Qt::DisplayRole).toString();
            const QRect row(area.left(), area.top() + cell.top(), area.width(), cell.height());
            add(Spot::Row, row, visible, plain(text));
            y = cell.bottom() + 2;
        }
    }

    void visit(QWidget *parent, const QRect &clip)
    {
        for (QObject *object : parent->children()) {
            auto *widget = qobject_cast<QWidget *>(object);
            if (!widget || widget == skip || widget->isWindow() || !widget->isVisible() ||
                widget->objectName() == QLatin1String("genieSpider"))
                continue;
            if (marks.size() >= kMaxLandmarks)
                return;
            const QRect rect = place(widget);
            const QRect inside = rect.intersected(clip);
            if (inside.isEmpty())
                continue;
            if (auto *tabs = qobject_cast<QTabBar *>(widget)) {
                for (int i = 0; i < tabs->count(); ++i) {
                    if (tabs->isTabVisible(i))
                        add(Spot::Tab, tabs->tabRect(i).translated(rect.topLeft()), inside, plain(tabs->tabText(i)));
                }
                continue;
            }
            if (qobject_cast<QCheckBox *>(widget) || qobject_cast<QRadioButton *>(widget)) {
                add(Spot::Toggle, rect, inside, plain(static_cast<QAbstractButton *>(widget)->text()));
                continue;
            }
            if (auto *button = qobject_cast<QAbstractButton *>(widget)) {
                QString text = plain(button->text());
                if (text.isEmpty())
                    text = plain(button->toolTip()).left(40);
                add(text.isEmpty() ? Spot::Picture : Spot::Button, rect, inside, text);
                continue;
            }
            if (auto *label = qobject_cast<QLabel *>(widget)) {
                if (!label->pixmap().isNull()) {
                    add(Spot::Picture, rect, inside, QString());
                    continue;
                }
                const QString text = plain(label->text());
                if (!text.isEmpty())
                    add(heading(label) ? Spot::Heading : Spot::Label, labelText(label, rect, text), inside, text);
                continue;
            }
            if (auto *line = qobject_cast<QLineEdit *>(widget)) {
                add(Spot::Input, rect, inside, line->text().isEmpty() ? line->placeholderText() : line->text());
                continue;
            }
            if (auto *combo = qobject_cast<QComboBox *>(widget)) {
                add(combo->isEditable() ? Spot::Input : Spot::Button, rect, inside, plain(combo->currentText()));
                continue;
            }
            if (qobject_cast<QAbstractSpinBox *>(widget)) {
                add(Spot::Input, rect, inside, QString());
                continue;
            }
            if (qobject_cast<QProgressBar *>(widget) || qobject_cast<QAbstractSlider *>(widget)) {
                if (!qobject_cast<QAbstractSlider *>(widget) || widget->width() > widget->height())
                    add(Spot::Progress, rect, inside, QString());
                continue;
            }
            if (auto *edit = qobject_cast<QPlainTextEdit *>(widget)) {
                const QRect port(edit->viewport()->mapTo(window, QPoint(0, 0)), edit->viewport()->size());
                const QString first = edit->cursorForPosition(QPoint(4, 4)).block().text();
                const bool input = !edit->isReadOnly() && edit->height() < 200;
                add(input ? Spot::Input : Spot::Document, port, inside,
                    input && first.isEmpty() ? edit->placeholderText() : first);
                continue;
            }
            if (auto *edit = qobject_cast<QTextEdit *>(widget)) {
                const QRect port(edit->viewport()->mapTo(window, QPoint(0, 0)), edit->viewport()->size());
                const QString first = edit->cursorForPosition(QPoint(4, 4)).block().text();
                const bool input = !edit->isReadOnly() && edit->height() < 200;
                add(input ? Spot::Input : Spot::Document, port, inside,
                    input && first.isEmpty() ? edit->placeholderText() : first);
                continue;
            }
            if (auto *view = qobject_cast<QAbstractItemView *>(widget)) {
                if (!qobject_cast<QHeaderView *>(widget))
                    rows(view, inside);
                continue;
            }
            visit(widget, inside);
        }
    }
};

} // namespace

QList<Landmark> scanLandmarks(QWidget *window, const QWidget *skip)
{
    Scan scan;
    scan.window = window;
    scan.skip = skip;
    if (window)
        scan.visit(window, window->rect());
    return scan.marks;
}

QImage snapshotWindow(QWidget *window, const QWidget *skip)
{
    if (!window || window->size().isEmpty())
        return QImage();
    const qreal ratio = window->devicePixelRatioF();
    QImage image(window->size() * ratio, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(ratio);
    image.fill(window->palette().color(QPalette::Window));
    QPainter painter(&image);
    window->render(&painter, QPoint(), QRegion(), QWidget::DrawWindowBackground);
    for (QObject *object : window->children()) {
        auto *child = qobject_cast<QWidget *>(object);
        if (!child || child == skip || child->isWindow() || !child->isVisible())
            continue;
        child->render(&painter, child->pos(), QRegion(), QWidget::DrawWindowBackground | QWidget::DrawChildren);
    }
    return image;
}

Overlay::Overlay(QWidget *window)
    : QWidget(window), m_window(window)
{
    setObjectName(QStringLiteral("aquariumOverlay"));
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setFocusPolicy(Qt::NoFocus);
    hide();
    m_clock.start();
    m_timer = new QTimer(this);
    m_timer->setTimerType(Qt::PreciseTimer);
    m_timer->setInterval(42);
    connect(m_timer, &QTimer::timeout, this, [this] { tick(); });
    m_grace = new QTimer(this);
    m_grace->setSingleShot(true);
    m_grace->setInterval(kGrace);
    connect(m_grace, &QTimer::timeout, this, [this] {
        if (m_open && !m_preview && !barHeld())
            dismiss();
    });
    if (window)
        window->installEventFilter(this);
}

Overlay::~Overlay()
{
    if (m_window)
        m_window->removeEventFilter(this);
    qApp->removeEventFilter(this);
    submerge(false);
}

void Overlay::submerge(bool on)
{
    if (m_submerged == on)
        return;
    m_submerged = on;
    if (petfeed::hasHost() && petfeed::host().setSubmerged)
        petfeed::host().setSubmerged(on);
}

void Overlay::open(bool preview)
{
    if (!m_window)
        return;
    m_preview = preview;
    m_grace->stop();
    if (!m_open) {
        m_open = true;
        qApp->installEventFilter(this);
        m_fade = 0;
        setGeometry(m_window->rect());
        submerge(true);
        m_last = m_clock.elapsed() / 1000.0;
        m_nextShot = 0;
        m_nextRoster = 0;
        m_scene.setSize(size());
        m_scene.setPixelRatio(devicePixelRatioF());
        refreshShot();
        m_nextShot = m_last + kShotEvery;
        if (swimmers)
            m_scene.setSwimmers(swimmers(m_preview));
        m_nextRoster = m_last + kRosterEvery;
        syncPet();
    }
    m_direction = 1;
    show();
    raise();
    m_timer->start();
}

void Overlay::settle()
{
    m_preview = false;
    m_full = true;
}

void Overlay::activated()
{
    if (m_open && !m_preview && m_direction > 0 && !barHeld())
        m_grace->start();
}

bool Overlay::barHeld() const
{
    return m_clock.elapsed() / 1000.0 - m_barAt < kBarHold;
}

void Overlay::dismiss()
{
    if (!m_open)
        return;
    m_grace->stop();
    m_direction = -1;
    if (!m_timer->isActive())
        m_timer->start();
}

void Overlay::finish()
{
    if (!m_open)
        return;
    qApp->removeEventFilter(this);
    m_grace->stop();
    m_timer->stop();
    m_open = false;
    m_preview = false;
    m_direction = 0;
    m_fade = 0;
    hide();
    submerge(false);
}

void Overlay::refreshShot()
{
    if (!m_window)
        return;
    m_scene.setSize(size());
    m_scene.setPixelRatio(devicePixelRatioF());
    m_scene.setBackdrop(snapshotWindow(m_window, this));
    m_full = true;
    m_scene.setLandmarks(scanLandmarks(m_window, this));
}

void Overlay::syncPet()
{
    Pet pet;
    petfeed::Sprite sprite;
    if (petfeed::hasHost() && petfeed::host().sprite && petfeed::host().sprite(this, &sprite)) {
        pet.present = true;
        pet.frame = QRectF(sprite.frame);
        pet.body = QRectF(sprite.body);
        pet.image = sprite.image;
        pet.walking = sprite.walking;
        pet.airborne = sprite.airborne;
    }
    m_scene.setPet(pet);
}

void Overlay::advance(double seconds)
{
    for (double done = 0; done < seconds; done += 1.0 / 30)
        step(1.0 / 30);
}

void Overlay::step(double dt)
{
    const double now = m_clock.elapsed() / 1000.0;
    if (now >= m_nextRoster && swimmers) {
        m_nextRoster = now + kRosterEvery;
        m_scene.setSwimmers(swimmers(m_preview));
    }
    if (now >= m_nextShot && m_fade >= 1) {
        m_nextShot = now + kShotEvery;
        refreshShot();
    }
    syncPet();
    m_scene.setFade(m_fade);
    m_scene.step(dt);
    const QList<PetCue> cues = m_scene.takeCues();
    if (petfeed::hasHost()) {
        const petfeed::Host &host = petfeed::host();
        for (const PetCue &cue : cues) {
            switch (cue.cue) {
            case Cue::Look:
                if (host.lookAt)
                    host.lookAt(this, cue.at);
                break;
            case Cue::Walk:
                if (host.walkTo)
                    host.walkTo(this, cue.at.x());
                break;
            case Cue::Hop:
                if (host.hop)
                    host.hop(cue.at.x(), cue.at.y());
                break;
            default:
                if (host.react)
                    host.react(petfeed::Reaction(int(cue.cue)));
                break;
            }
        }
    }
    const QList<Note> notes = m_scene.takeNotes();
    if (noted) {
        for (const Note &note : notes)
            noted(note);
    }
}

void Overlay::tick()
{
    const double now = m_clock.elapsed() / 1000.0;
    const double dt = std::clamp(now - m_last, 0.0, 0.1);
    m_last = now;
    const QWindow *handle = m_window ? m_window->windowHandle() : nullptr;
    if (!m_window || m_window->isMinimized() || !m_window->isVisible() || (handle && !handle->isExposed())) {
        m_timer->setInterval(500);
        return;
    }
    m_timer->setInterval(42);
    m_fade = std::clamp(m_fade + m_direction * dt / (m_direction > 0 ? 0.9 : 0.25), 0.0, 1.0);
    if (m_direction < 0 && m_fade <= 0) {
        finish();
        return;
    }
    step(dt);
    const QObjectList &siblings = m_window->children();
    for (auto it = siblings.crbegin(); it != siblings.crend(); ++it) {
        auto *widget = qobject_cast<QWidget *>(*it);
        if (!widget || widget->isWindow() || !widget->isVisible())
            continue;
        if (widget != this)
            raise();
        break;
    }
    const QRegion dirty = m_scene.dirtyRegion();
    if (m_fade < 1 || m_full) {
        m_full = m_fade < 1;
        update();
    } else {
        update(dirty);
    }
}

void Overlay::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    painter.setClipRegion(event->region());
    m_scene.paint(painter);
    if (m_preview) {
        QFont font = painter.font();
        font.setPixelSize(12);
        painter.setFont(font);
        const QString hint = QStringLiteral("Click or press Esc to leave the aquarium, drag the top bar to move the window");
        const QRectF box(width() / 2.0 - 250, 12, 500, 24);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(4, 24, 44, 170));
        painter.drawRoundedRect(box, 12, 12);
        painter.setPen(QColor(230, 250, 255));
        painter.drawText(box, Qt::AlignCenter, hint);
    }
}

bool Overlay::eventFilter(QObject *watched, QEvent *event)
{
    if (m_open && watched->isWindowType()) {
        switch (event->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick: {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            if (m_window && watched == m_window->windowHandle() && mouse->button() == Qt::LeftButton &&
                titleBarAt(m_window, mouse->position().toPoint())) {
                m_barAt = m_clock.elapsed() / 1000.0;
                m_grace->stop();
                break;
            }
            const int id = m_window && watched == m_window->windowHandle() && mouse->button() == Qt::LeftButton
                               ? m_scene.fishAt(mapFrom(m_window, mouse->position().toPoint()))
                               : 0;
            finish();
            if (id && picked) {
                QTimer::singleShot(0, this, [this, id] {
                    if (picked)
                        picked(id);
                });
                return true;
            }
            break;
        }
        case QEvent::KeyPress:
            finish();
            return static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape;
        case QEvent::Wheel:
        case QEvent::TouchBegin:
            finish();
            break;
        default:
            break;
        }
    }
    if (watched == m_window && event->type() == QEvent::Resize && m_open) {
        setGeometry(m_window->rect());
        m_nextShot = 0;
        m_scene.setSize(size());
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace forkmesh::aquarium
