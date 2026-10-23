#include "UiKit.h"

#include "DesignTokens.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QButtonGroup>
#include <QChildEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFontMetrics>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QStyle>
#include <QStyleOption>
#include <QStyledItemDelegate>
#include <QSvgRenderer>
#include <QTableView>
#include <QTextDocument>
#include <QTimerEvent>
#include <QVBoxLayout>
#include <QVariantAnimation>

#include <algorithm>

namespace forkmesh::ui::kit {

namespace {

constexpr int kTrackWidth = 34;
constexpr int kTrackHeight = 20;
constexpr int kToggleSpacing = 10;

void repolish(QWidget *widget)
{
    if (!widget || !widget->style())
        return;
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

QString toneIcon(Tone tone)
{
    switch (tone) {
    case Tone::Success:
        return QStringLiteral("check-circle");
    case Tone::Warning:
    case Tone::Danger:
        return QStringLiteral("alert");
    case Tone::Accent:
    case Tone::Neutral:
        break;
    }
    return QStringLiteral("info");
}

Theme::Role toneRole(Tone tone)
{
    switch (tone) {
    case Tone::Accent:
        return Theme::Role::AccentFg;
    case Tone::Success:
        return Theme::Role::Success;
    case Tone::Warning:
        return Theme::Role::Warning;
    case Tone::Danger:
        return Theme::Role::Danger;
    case Tone::Neutral:
        break;
    }
    return Theme::Role::FgMuted;
}

}

QString toneName(Tone tone)
{
    switch (tone) {
    case Tone::Accent:
        return QStringLiteral("accent");
    case Tone::Success:
        return QStringLiteral("success");
    case Tone::Warning:
        return QStringLiteral("warning");
    case Tone::Danger:
        return QStringLiteral("danger");
    case Tone::Neutral:
        break;
    }
    return QStringLiteral("neutral");
}

QPixmap tintedIcon(const QString &octicon, const QColor &color, int px,
                   qreal devicePixelRatio)
{
    QString path = QStringLiteral(":/icons/octicons/%1.svg").arg(octicon);
    QSvgRenderer renderer(path);
    if (!renderer.isValid()) {
        path = QStringLiteral(":/icons/codicons/%1.svg").arg(octicon);
        renderer.load(path);
    }
    const qreal ratio = devicePixelRatio > 0.0 ? devicePixelRatio : 1.0;
    const int side = qMax(1, qRound(px * ratio));
    QPixmap pixmap(side, side);
    pixmap.fill(Qt::transparent);
    if (renderer.isValid()) {
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        renderer.render(&painter, QRectF(0, 0, side, side));
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(pixmap.rect(), color);
    }
    pixmap.setDevicePixelRatio(ratio);
    return pixmap;
}

Toggle::Toggle(QWidget *parent) : QCheckBox(parent)
{
    init();
}

Toggle::Toggle(const QString &text, QWidget *parent) : QCheckBox(text, parent)
{
    init();
}

void Toggle::init()
{
    setCursor(Qt::PointingHandCursor);
    setProperty("uiKitToggle", true);
    m_position = isChecked() ? 1.0 : 0.0;
    m_animation = new QVariantAnimation(this);
    m_animation->setDuration(130);
    m_animation->setEasingCurve(QEasingCurve::OutCubic);
    QObject::connect(m_animation, &QVariantAnimation::valueChanged, this,
                     [this](const QVariant &value) {
                         m_position = value.toReal();
                         update();
                     });
}

QSize Toggle::trackSize() const
{
    return QSize(kTrackWidth, kTrackHeight);
}

QSize Toggle::sizeHint() const
{
    const QSize track = trackSize();
    if (text().isEmpty())
        return track;
    const QFontMetrics metrics(font());
    return QSize(track.width() + kToggleSpacing +
                     metrics.horizontalAdvance(text()) + 2,
                 qMax(track.height(), metrics.height() + 4));
}

QSize Toggle::minimumSizeHint() const
{
    return sizeHint();
}

bool Toggle::hitButton(const QPoint &pos) const
{
    return rect().contains(pos);
}

void Toggle::animateTo(bool checked)
{
    const qreal target = checked ? 1.0 : 0.0;
    if (!m_animation || !isVisible()) {
        m_position = target;
        update();
        return;
    }
    m_animation->stop();
    m_animation->setStartValue(m_position);
    m_animation->setEndValue(target);
    m_animation->start();
}

void Toggle::checkStateSet()
{
    QCheckBox::checkStateSet();
    animateTo(isChecked());
}

void Toggle::nextCheckState()
{
    QCheckBox::nextCheckState();
    animateTo(isChecked());
}

void Toggle::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QSize track = trackSize();
    const QRectF trackRect(0.5, (height() - track.height()) / 2.0 + 0.5,
                           track.width() - 1.0, track.height() - 1.0);
    const qreal radius = trackRect.height() / 2.0;

    const QColor off = Theme::color(Theme::Role::BorderStrong);
    const QColor on = Theme::color(underMouse() ? Theme::Role::AccentHover
                                                : Theme::Role::Accent);
    QColor fill;
    fill.setRgbF(off.redF() + (on.redF() - off.redF()) * m_position,
                 off.greenF() + (on.greenF() - off.greenF()) * m_position,
                 off.blueF() + (on.blueF() - off.blueF()) * m_position);
    if (!isEnabled())
        fill.setAlphaF(0.45);
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill);
    painter.drawRoundedRect(trackRect, radius, radius);

    const qreal knob = trackRect.height() - 4.0;
    const qreal travel = trackRect.width() - knob - 4.0;
    const QRectF knobRect(trackRect.left() + 2.0 + travel * m_position,
                          trackRect.top() + 2.0, knob, knob);
    QColor knobColor(Qt::white);
    if (!isEnabled())
        knobColor.setAlphaF(0.7);
    painter.setBrush(knobColor);
    painter.drawEllipse(knobRect);

    if (hasFocus() && focusPolicy() != Qt::NoFocus &&
        property("uiKitKeyboardFocus").toBool()) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(Theme::color(Theme::Role::AccentFg), 1.5));
        painter.drawRoundedRect(trackRect.adjusted(-1.5, -1.5, 1.5, 1.5),
                                radius + 1.5, radius + 1.5);
    }

    if (!text().isEmpty()) {
        painter.setPen(Theme::color(isEnabled() ? Theme::Role::Fg
                                                : Theme::Role::FgSubtle));
        painter.setFont(font());
        const QRect textRect(track.width() + kToggleSpacing, 0,
                             width() - track.width() - kToggleSpacing, height());
        painter.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, text());
    }
}

Card::Card(QWidget *parent) : QFrame(parent)
{
    setObjectName(QStringLiteral("card"));
    setFrameShape(QFrame::NoFrame);
    setAttribute(Qt::WA_StyledBackground, true);
    m_body = new QVBoxLayout(this);
    m_body->setContentsMargins(1, 1, 1, 1);
    m_body->setSpacing(0);
}

QVBoxLayout *Card::body() const
{
    return m_body;
}

void Card::addRow(QWidget *row)
{
    if (!row)
        return;
    if (auto *settingsRow = dynamic_cast<SettingsRow *>(row))
        settingsRow->setFirstRow(m_rows == 0);
    m_body->addWidget(row);
    ++m_rows;
}

int Card::rowCount() const
{
    return m_rows;
}

SettingsRow::SettingsRow(const QString &title, const QString &description,
                         QWidget *control, QWidget *parent)
    : QFrame(parent), m_control(control)
{
    setObjectName(QStringLiteral("settingsRow"));
    setFrameShape(QFrame::NoFrame);
    setAttribute(Qt::WA_StyledBackground, true);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(16);

    auto *text = new QVBoxLayout;
    text->setContentsMargins(0, 0, 0, 0);
    text->setSpacing(3);
    m_title = new QLabel(title, this);
    m_title->setObjectName(QStringLiteral("settingsRowTitle"));
    text->addWidget(m_title);
    m_description = new QLabel(description, this);
    m_description->setObjectName(QStringLiteral("settingsRowDescription"));
    m_description->setWordWrap(true);
    m_description->setVisible(!description.isEmpty());
    text->addWidget(m_description);
    layout->addLayout(text, 1);

    if (m_control) {
        m_control->setParent(this);
        layout->addWidget(m_control, 0, Qt::AlignRight | Qt::AlignVCenter);
    }
}

QLabel *SettingsRow::titleLabel() const
{
    return m_title;
}

QLabel *SettingsRow::descriptionLabel() const
{
    return m_description;
}

QWidget *SettingsRow::control() const
{
    return m_control;
}

void SettingsRow::setFirstRow(bool first)
{
    setProperty("firstRow", first);
    repolish(this);
}

Badge::Badge(const QString &text, Tone tone, bool outline, QWidget *parent)
    : QLabel(text, parent)
{
    setObjectName(QStringLiteral("badge"));
    setAlignment(Qt::AlignCenter);
    setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);
    setTone(tone, outline);
}

void Badge::setTone(Tone tone, bool outline)
{
    setProperty("tone", toneName(tone));
    setProperty("outline", outline);
    repolish(this);
}

KeyHint::KeyHint(const QString &key, QWidget *parent) : QLabel(key, parent)
{
    setObjectName(QStringLiteral("keyHint"));
    setAlignment(Qt::AlignCenter);
    setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);
}

InlineNotice::InlineNotice(QWidget *parent) : QFrame(parent)
{
    setObjectName(QStringLiteral("inlineNotice"));
    setFrameShape(QFrame::NoFrame);
    setAttribute(Qt::WA_StyledBackground, true);
    m_layout = new QHBoxLayout(this);
    m_layout->setContentsMargins(10, 8, 10, 8);
    m_layout->setSpacing(8);
    m_icon = new QLabel(this);
    m_icon->setFixedSize(14, 14);
    m_layout->addWidget(m_icon, 0, Qt::AlignTop);
    m_text = new QLabel(this);
    m_text->setTextFormat(Qt::PlainText);
    m_text->setWordWrap(true);
    m_text->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_layout->addWidget(m_text, 1);
    hide();
}

QPushButton *InlineNotice::addAction(const QString &label, const QString &octicon)
{
    auto *button = new QPushButton(label, this);
    button->setObjectName(QStringLiteral("ghostButton"));
    button->setProperty("buttonSize", QStringLiteral("xs"));
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::NoFocus);
    if (!octicon.isEmpty()) {
        button->setIcon(QIcon(tintedIcon(octicon, Theme::color(Theme::Role::FgMuted),
                                         12, devicePixelRatioF())));
        button->setIconSize(QSize(12, 12));
    }
    if (!label.isEmpty())
        button->setAccessibleName(label);
    m_layout->addWidget(button, 0, Qt::AlignTop);
    m_actions.append(button);
    return button;
}

QPushButton *InlineNotice::addCloseButton()
{
    QPushButton *button = addAction(QString(), QStringLiteral("x"));
    button->setToolTip(QStringLiteral("Clear"));
    button->setAccessibleName(QStringLiteral("Clear notice"));
    button->setProperty("noticeAction", QStringLiteral("clear"));
    return button;
}

QList<QPushButton *> InlineNotice::actions() const
{
    return m_actions;
}

void InlineNotice::showMessage(const QString &text, Tone tone)
{
    m_tone = tone;
    m_text->setText(text);
    m_icon->setPixmap(tintedIcon(toneIcon(tone), Theme::color(toneRole(tone)),
                                 14, devicePixelRatioF()));
    setProperty("tone", toneName(tone));
    repolish(this);
    repolish(m_text);
    setVisible(!text.isEmpty());
}

void InlineNotice::clear()
{
    m_text->clear();
    hide();
}

QString InlineNotice::text() const
{
    return m_text->text();
}

Tone InlineNotice::tone() const
{
    return m_tone;
}

EmptyState::EmptyState(const QString &octicon, const QString &title,
                       const QString &body, QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("emptyState"));
    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(24, 32, 24, 32);
    m_layout->setSpacing(6);
    m_layout->addStretch(1);

    auto *icon = new QLabel(this);
    icon->setPixmap(tintedIcon(octicon, Theme::color(Theme::Role::FgSubtle), 24,
                               devicePixelRatioF()));
    icon->setAlignment(Qt::AlignCenter);
    m_layout->addWidget(icon, 0, Qt::AlignHCenter);
    m_layout->addSpacing(6);

    auto *titleLabel = new QLabel(title, this);
    titleLabel->setObjectName(QStringLiteral("emptyStateTitle"));
    titleLabel->setAlignment(Qt::AlignCenter);
    m_layout->addWidget(titleLabel, 0, Qt::AlignHCenter);

    auto *bodyLabel = new QLabel(body, this);
    bodyLabel->setObjectName(QStringLiteral("emptyStateBody"));
    bodyLabel->setAlignment(Qt::AlignCenter);
    bodyLabel->setWordWrap(true);
    bodyLabel->setVisible(!body.isEmpty());
    m_layout->addWidget(bodyLabel);
    m_layout->addStretch(1);
}

QPushButton *EmptyState::addAction(const QString &label)
{
    auto *button = new QPushButton(label, this);
    button->setProperty("buttonSize", QStringLiteral("sm"));
    const int tail = m_layout->count() - 1;
    m_layout->insertSpacing(tail, 8);
    m_layout->insertWidget(tail + 1, button, 0, Qt::AlignHCenter);
    return button;
}

SegmentedControl::SegmentedControl(const QStringList &labels, QWidget *parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("segmentedControl"));
    setFrameShape(QFrame::NoFrame);
    setAttribute(Qt::WA_StyledBackground, true);
    setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(3, 3, 3, 3);
    layout->setSpacing(2);
    m_group = new QButtonGroup(this);
    m_group->setExclusive(true);
    for (int index = 0; index < labels.size(); ++index) {
        auto *button = new QPushButton(labels.at(index), this);
        button->setObjectName(QStringLiteral("segmentedButton"));
        button->setCheckable(true);
        button->setCursor(Qt::PointingHandCursor);
        button->setChecked(index == 0);
        m_group->addButton(button, index);
        layout->addWidget(button);
    }
    QObject::connect(m_group, &QButtonGroup::idToggled, this,
                     [this](int id, bool checked) {
                         if (checked && m_changed)
                             m_changed(id);
                     });
}

int SegmentedControl::currentIndex() const
{
    return m_group->checkedId();
}

void SegmentedControl::setCurrentIndex(int index)
{
    if (QAbstractButton *button = m_group->button(index))
        button->setChecked(true);
}

void SegmentedControl::onChanged(std::function<void(int)> callback)
{
    m_changed = std::move(callback);
}

QList<QPushButton *> SegmentedControl::buttons() const
{
    QList<QPushButton *> result;
    for (QAbstractButton *button : m_group->buttons()) {
        if (auto *push = qobject_cast<QPushButton *>(button))
            result.append(push);
    }
    return result;
}

Spinner::Spinner(int px, QWidget *parent) : QWidget(parent), m_px(px)
{
    setFixedSize(px, px);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
}

void Spinner::start()
{
    if (m_timer == 0)
        m_timer = startTimer(60);
    show();
}

void Spinner::stop()
{
    if (m_timer != 0) {
        killTimer(m_timer);
        m_timer = 0;
    }
    hide();
}

bool Spinner::isSpinning() const
{
    return m_timer != 0;
}

QSize Spinner::sizeHint() const
{
    return QSize(m_px, m_px);
}

void Spinner::timerEvent(QTimerEvent *event)
{
    if (event->timerId() != m_timer) {
        QWidget::timerEvent(event);
        return;
    }
    m_angle = (m_angle + 30) % 360;
    update();
}

void Spinner::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const qreal stroke = qMax(1.5, m_px / 9.0);
    const QRectF arc(stroke, stroke, m_px - 2 * stroke, m_px - 2 * stroke);
    painter.setPen(QPen(Theme::color(Theme::Role::BorderStrong), stroke));
    painter.drawEllipse(arc);
    QPen pen(Theme::color(Theme::Role::AccentFg), stroke);
    pen.setCapStyle(Qt::RoundCap);
    painter.setPen(pen);
    painter.drawArc(arc, -m_angle * 16, 100 * 16);
}

QLabel *groupHeading(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("groupHeading"));
    return label;
}

QLabel *pageTitle(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("pageTitle"));
    return label;
}

QLabel *pageLede(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("pageLede"));
    label->setWordWrap(true);
    return label;
}

QWidget *labeledField(const QString &label, QWidget *field, const QString &hint,
                      QWidget *parent)
{
    auto *wrapper = new QWidget(parent);
    wrapper->setAttribute(Qt::WA_StyledBackground, false);
    wrapper->setStyleSheet(QStringLiteral("background: transparent;"));
    auto *layout = new QVBoxLayout(wrapper);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    auto *caption = new QLabel(label, wrapper);
    caption->setObjectName(QStringLiteral("fieldLabel"));
    layout->addWidget(caption);
    if (field) {
        field->setParent(wrapper);
        layout->addWidget(field);
    }
    if (!hint.isEmpty()) {
        auto *hintLabel = new QLabel(hint, wrapper);
        hintLabel->setObjectName(QStringLiteral("fieldHint"));
        hintLabel->setWordWrap(true);
        layout->addWidget(hintLabel);
    }
    return wrapper;
}

namespace {

const char kFloatingSurface[] = "floatingSurface";
const char kDataTable[] = "dataTable";
const char kHeaderAlignment[] = "nextHeaderAlignment";
const char kDataTableState[] = "nextDataTableState";
constexpr int kDataTableRowHeight = 38;

bool isNamedPopup(const QWidget *widget)
{
    if (!widget->isWindow())
        return false;
    const Qt::WindowType type = widget->windowType();
    if (type != Qt::Popup && type != Qt::ToolTip)
        return false;
    static const QSet<QString> names{
        QStringLiteral("agentMetaPopup"), QStringLiteral("reactionPicker"),
        QStringLiteral("slashActionsPopup"), QStringLiteral("globalSearchPopup"),
        QStringLiteral("mentionPopup")};
    return names.contains(widget->objectName());
}

bool isFloatingSurface(const QWidget *widget)
{
    return qobject_cast<const QMenu *>(widget) ||
           widget->inherits("QComboBoxPrivateContainer") || isNamedPopup(widget);
}

void setFloatingSurface(QWidget *widget, bool on)
{
    if (widget->testAttribute(Qt::WA_WState_Created) ||
        widget->testAttribute(Qt::WA_TranslucentBackground) == on)
        return;
    if (on && (widget->autoFillBackground() || !widget->styleSheet().isEmpty()))
        return;
    widget->setAttribute(Qt::WA_TranslucentBackground, on);
    widget->setAttribute(Qt::WA_NoSystemBackground, on);
    Qt::WindowFlags flags = widget->windowFlags();
    flags.setFlag(Qt::FramelessWindowHint, on);
    flags.setFlag(Qt::NoDropShadowWindowHint, on);
    widget->overrideWindowFlags(flags);
}

void repolishTree(QWidget *root)
{
    QList<QWidget *> widgets = root->findChildren<QWidget *>();
    widgets.prepend(root);
    for (QWidget *widget : std::as_const(widgets)) {
        if (widget->testAttribute(Qt::WA_WState_Polished))
            repolish(widget);
    }
}

void syncFloatingSurface(QWidget *widget, bool next)
{
    setFloatingSurface(widget, next);
    const QVariant state =
        next ? QVariant(widget->testAttribute(Qt::WA_TranslucentBackground))
             : QVariant();
    if (widget->property(kFloatingSurface) == state)
        return;
    widget->setProperty(kFloatingSurface, state);
    repolishTree(widget);
}

void syncHeaderAlignment(QTableView *table, bool next)
{
    QHeaderView *header = table->horizontalHeader();
    if (!header)
        return;
    const QVariant stored = table->property(kHeaderAlignment);
    if (next && !stored.isValid() &&
        header->defaultAlignment() == Qt::Alignment(Qt::AlignCenter)) {
        table->setProperty(kHeaderAlignment,
                           static_cast<int>(header->defaultAlignment()));
        header->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    } else if (!next && stored.isValid()) {
        header->setDefaultAlignment(Qt::Alignment(stored.toInt()));
        table->setProperty(kHeaderAlignment, QVariant());
    }
}

class DataTableRowDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt(option);
        const auto *view = qobject_cast<const QAbstractItemView *>(option.widget);
        if (view && view->selectionBehavior() == QAbstractItemView::SelectRows) {
            opt.state &= ~QStyle::State_MouseOver;
            QWidget *viewport = view->viewport();
            if (viewport && viewport->underMouse()) {
                const QModelIndex hovered =
                    view->indexAt(viewport->mapFromGlobal(QCursor::pos()));
                if (hovered.isValid() && hovered.row() == index.row() &&
                    hovered.parent() == index.parent())
                    opt.state |= QStyle::State_MouseOver;
            }
        }
        QStyledItemDelegate::paint(painter, opt, index);
    }
};

void syncDataTableRows(QTableView *table, bool next, QVariantMap *state)
{
    QAbstractItemDelegate *current = table->itemDelegate();
    if (next) {
        state->insert(QStringLiteral("elide"), static_cast<int>(table->textElideMode()));
        state->insert(QStringLiteral("hover"),
                      table->viewport()->testAttribute(Qt::WA_Hover));
        if (QHeaderView *horizontal = table->horizontalHeader())
            state->insert(QStringLiteral("stretchLast"), horizontal->stretchLastSection());
        table->setTextElideMode(Qt::ElideRight);
        table->viewport()->setAttribute(Qt::WA_Hover, true);
        if (QHeaderView *horizontal = table->horizontalHeader())
            horizontal->setStretchLastSection(true);
        if (current && qstrcmp(current->metaObject()->className(),
                               "QStyledItemDelegate") == 0) {
            state->insert(QStringLiteral("delegate"),
                          QVariant::fromValue<QObject *>(current));
            table->setItemDelegate(new DataTableRowDelegate(table));
        }
        return;
    }
    table->setTextElideMode(static_cast<Qt::TextElideMode>(
        state->value(QStringLiteral("elide"), static_cast<int>(Qt::ElideRight)).toInt()));
    table->viewport()->setAttribute(
        Qt::WA_Hover, state->value(QStringLiteral("hover")).toBool());
    if (QHeaderView *horizontal = table->horizontalHeader();
        horizontal && state->contains(QStringLiteral("stretchLast")))
        horizontal->setStretchLastSection(
            state->value(QStringLiteral("stretchLast")).toBool());
    auto *original = qobject_cast<QAbstractItemDelegate *>(
        state->value(QStringLiteral("delegate")).value<QObject *>());
    if (original && dynamic_cast<DataTableRowDelegate *>(current)) {
        table->setItemDelegate(original);
        current->deleteLater();
    }
}

void syncDataTable(QTableView *table, bool next)
{
    if (!table->property(kDataTable).toBool())
        return;
    const QVariant stored = table->property(kDataTableState);
    QHeaderView *horizontal = table->horizontalHeader();
    QHeaderView *vertical = table->verticalHeader();
    if (next && !stored.isValid()) {
        QVariantMap state;
        state.insert(QStringLiteral("grid"), table->showGrid());
        state.insert(QStringLiteral("alternating"), table->alternatingRowColors());
        state.insert(QStringLiteral("rowNumbers"), vertical && !vertical->isHidden());
        state.insert(QStringLiteral("rowHeight"),
                     vertical ? vertical->defaultSectionSize() : 0);
        state.insert(QStringLiteral("highlight"),
                     horizontal && horizontal->highlightSections());
        QVariantList fitted;
        if (horizontal) {
            for (int column = 0; column < horizontal->count(); ++column) {
                if (horizontal->sectionResizeMode(column) != QHeaderView::Interactive)
                    continue;
                horizontal->setSectionResizeMode(column, QHeaderView::ResizeToContents);
                fitted.append(column);
            }
            horizontal->setHighlightSections(false);
        }
        state.insert(QStringLiteral("fitted"), fitted);
        state.insert(QStringLiteral("wordWrap"), table->wordWrap());
        syncDataTableRows(table, true, &state);
        table->setProperty(kDataTableState, state);
        table->setWordWrap(false);
        table->setShowGrid(false);
        table->setAlternatingRowColors(false);
        if (vertical) {
            vertical->setVisible(false);
            vertical->setDefaultSectionSize(kDataTableRowHeight);
        }
    } else if (!next && stored.isValid()) {
        QVariantMap state = stored.toMap();
        syncDataTableRows(table, false, &state);
        table->setShowGrid(state.value(QStringLiteral("grid")).toBool());
        table->setWordWrap(state.value(QStringLiteral("wordWrap"), true).toBool());
        table->setAlternatingRowColors(
            state.value(QStringLiteral("alternating")).toBool());
        if (vertical) {
            vertical->setDefaultSectionSize(
                state.value(QStringLiteral("rowHeight")).toInt());
            vertical->setVisible(state.value(QStringLiteral("rowNumbers")).toBool());
        }
        if (horizontal) {
            horizontal->setHighlightSections(
                state.value(QStringLiteral("highlight")).toBool());
            const QVariantList fitted = state.value(QStringLiteral("fitted")).toList();
            for (const QVariant &column : fitted) {
                if (column.toInt() < horizontal->count())
                    horizontal->setSectionResizeMode(column.toInt(),
                                                     QHeaderView::Interactive);
            }
        }
        table->setProperty(kDataTableState, QVariant());
    }
}

void syncStockWidget(QWidget *widget, bool next)
{
    if (isFloatingSurface(widget)) {
        syncFloatingSurface(widget, next);
    } else if (auto *table = qobject_cast<QTableView *>(widget)) {
        syncHeaderAlignment(table, next);
        syncDataTable(table, next);
    }
}

const char kMessageCard[] = "nextMessageCard";
const char kMessageCardDressed[] = "nextMessageCardDressed";
const char kMessageHeading[] = "nextMessageHeading";

QHash<QWidget *, QPointer<QWidget>> &messageScrims()
{
    static QHash<QWidget *, QPointer<QWidget>> scrims;
    return scrims;
}

QMessageBox *messageCard(QObject *watched)
{
    if (!watched->isWidgetType() || !static_cast<QWidget *>(watched)->isWindow())
        return nullptr;
    auto *box = qobject_cast<QMessageBox *>(watched);
    return box && box->property(kMessageCard).toBool() ? box : nullptr;
}

void prepareMessageCard(QObject *watched)
{
    if (!watched->isWidgetType() || !static_cast<QWidget *>(watched)->isWindow())
        return;
    auto *box = qobject_cast<QMessageBox *>(watched);
    if (!box || box->testAttribute(Qt::WA_WState_Created) ||
        box->property(kMessageCard).isValid())
        return;
    box->setProperty(kMessageCard, true);
    box->setAttribute(Qt::WA_TranslucentBackground, true);
    box->setAttribute(Qt::WA_StyledBackground, true);
    Qt::WindowFlags flags = box->windowFlags();
    flags.setFlag(Qt::FramelessWindowHint, true);
#ifndef Q_OS_MACOS
    flags.setFlag(Qt::NoDropShadowWindowHint, true);
#endif
    box->overrideWindowFlags(flags);
}

QString messageGist(QString text)
{
    text = text.trimmed();
    while (!text.isEmpty() && QStringLiteral(".!?:\u2026").contains(text.back()))
        text.chop(1);
    return text.trimmed();
}

void dressMessageCard(QMessageBox *box)
{
    if (box->property(kMessageCardDressed).toBool())
        return;
    box->setProperty(kMessageCardDressed, true);
    const QString title = box->windowTitle().trimmed();
    if (!title.isEmpty() && box->informativeText().isEmpty() &&
        messageGist(title).compare(messageGist(box->text()), Qt::CaseInsensitive) != 0 &&
        title != QApplication::applicationDisplayName()) {
        const Qt::TextFormat format = box->textFormat();
        box->setInformativeText(box->text());
        if (format == Qt::RichText) {
            box->setText(title.toHtmlEscaped());
        } else {
            box->setTextFormat(Qt::PlainText);
            box->setText(title);
        }
        if (auto *info = box->findChild<QLabel *>(
                QStringLiteral("qt_msgbox_informativelabel")))
            info->setTextFormat(format);
    }
    box->setProperty(kMessageHeading, !box->informativeText().isEmpty());
}

void hideMessageScrim(QWidget *box)
{
    if (QPointer<QWidget> scrim = messageScrims().take(box))
        scrim->deleteLater();
}

void showMessageScrim(QMessageBox *box)
{
    QWidget *host = box->parentWidget() ? box->parentWidget()->window() : nullptr;
    if (!host || host == box || !host->isVisible() ||
        messageScrims().contains(box))
        return;
    auto *scrim = new QWidget(host);
    scrim->setObjectName(QStringLiteral("messageCardScrim"));
    scrim->setAttribute(Qt::WA_StyledBackground, true);
    scrim->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    scrim->setGeometry(host->rect());
    scrim->show();
    scrim->raise();
    messageScrims().insert(box, scrim);
    QObject::connect(box, &QObject::destroyed, scrim,
                     [box] { hideMessageScrim(box); });
}

void fitMessageScrims(QObject *watched)
{
    if (messageScrims().isEmpty() || !watched->isWidgetType())
        return;
    auto *host = static_cast<QWidget *>(watched);
    if (!host->isWindow())
        return;
    for (const QPointer<QWidget> &scrim : std::as_const(messageScrims())) {
        if (scrim && scrim->parentWidget() == host)
            scrim->setGeometry(host->rect());
    }
}

bool isMessageBlockText(const QString &text)
{
    if (text.contains(QLatin1Char('\n')))
        return true;
    if (!Qt::mightBeRichText(text))
        return false;
    static const QRegularExpression blocks(
        QStringLiteral("<(br|p|li|ul|ol|table|pre|div)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    return text.contains(blocks);
}

QList<QWidget *> messageCardRows(QMessageBox *box)
{
    QList<QWidget *> rows;
    auto *grid = qobject_cast<QGridLayout *>(box->layout());
    if (!grid)
        return rows;
    QList<std::pair<std::pair<int, int>, QWidget *>> cells;
    for (int index = 0; index < grid->count(); ++index) {
        QWidget *widget = grid->itemAt(index)->widget();
        if (!widget || widget->objectName() == QLatin1String("qt_msgboxex_icon_label"))
            continue;
        int row = 0, column = 0, rowSpan = 0, columnSpan = 0;
        grid->getItemPosition(index, &row, &column, &rowSpan, &columnSpan);
        cells.append({{row, column}, widget});
    }
    std::stable_sort(cells.begin(), cells.end(),
                     [](const auto &left, const auto &right) { return left.first < right.first; });
    for (const auto &cell : std::as_const(cells))
        rows.append(cell.second);
    return rows;
}

void layoutMessageCard(QMessageBox *box)
{
    if (!box->iconPixmap().isNull())
        box->setIconPixmap(QPixmap());
    if (auto *icon = box->findChild<QLabel *>(QStringLiteral("qt_msgboxex_icon_label")))
        icon->hide();
    box->setContentsMargins(0, 0, 0, 0);
    const QList<QWidget *> rows = messageCardRows(box);
    if (!rows.isEmpty()) {
        delete box->layout();
        auto *column = new QVBoxLayout(box);
        column->setSizeConstraint(QLayout::SetNoConstraint);
        column->setContentsMargins(24, 24, 24, 24);
        column->setSpacing(8);
        for (QWidget *row : rows) {
            if (qobject_cast<QDialogButtonBox *>(row))
                column->addSpacing(12);
            column->addWidget(row);
        }
    }
    auto *heading = box->findChild<QLabel *>(QStringLiteral("qt_msgbox_label"));
    auto *body = box->findChild<QLabel *>(QStringLiteral("qt_msgbox_informativelabel"));
    const bool centered = !(heading && isMessageBlockText(heading->text())) &&
                          !(body && isMessageBlockText(body->text()));
    const Qt::Alignment alignment =
        (centered ? Qt::AlignHCenter : Qt::AlignLeft) | Qt::AlignTop;
    if (heading)
        heading->setAlignment(alignment);
    if (body) {
        body->setContentsMargins(0, 0, 0, 0);
        body->setAlignment(alignment);
    }
    if (QCheckBox *check = box->checkBox(); check && box->layout())
        box->layout()->setAlignment(check, centered ? Qt::AlignHCenter : Qt::AlignLeft);
    if (auto *buttons = box->findChild<QDialogButtonBox *>(
            QStringLiteral("qt_msgbox_buttonbox"))) {
        buttons->setCenterButtons(true);
        const QList<QAbstractButton *> all = buttons->buttons();
        int width = 0;
        for (QAbstractButton *button : all) {
            button->setMinimumWidth(0);
            width = std::max(width, button->sizeHint().width());
        }
        for (QAbstractButton *button : all)
            button->setMinimumWidth(std::max(width, 96));
    }
    showMessageScrim(box);
}

void paintMessageCard(QMessageBox *box)
{
    QStyleOption option;
    option.initFrom(box);
    QPainter painter(box);
    box->style()->drawPrimitive(QStyle::PE_Widget, &option, &painter, box);
}

class StockPolish final : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::Polish:
            if (watched->isWidgetType())
                syncStockWidget(static_cast<QWidget *>(watched), Theme::isNext());
            if (QMessageBox *box = messageCard(watched))
                dressMessageCard(box);
            break;
        case QEvent::Show:
            if (QMessageBox *box = messageCard(watched))
                layoutMessageCard(box);
            break;
        case QEvent::Hide:
            if (messageCard(watched))
                hideMessageScrim(static_cast<QWidget *>(watched));
            break;
        case QEvent::Resize:
            fitMessageScrims(watched);
            break;
        case QEvent::Paint:
            if (QMessageBox *box = messageCard(watched))
                paintMessageCard(box);
            break;
        case QEvent::ChildAdded:
            if (Theme::isNext())
                prepareMessageCard(watched);
            if (Theme::isNext() && qobject_cast<QComboBox *>(watched)) {
                QObject *child = static_cast<QChildEvent *>(event)->child();
                if (child && child->isWidgetType()) {
                    auto *popup = static_cast<QWidget *>(child);
                    if (popup->windowType() == Qt::Popup)
                        setFloatingSurface(popup, true);
                }
            }
            break;
        case QEvent::MouseButtonPress:
        case QEvent::KeyPress:
            if (Theme::isNext()) {
                if (auto *combo = qobject_cast<QComboBox *>(watched))
                    prepareComboPopup(combo);
            }
            break;
        default:
            break;
        }
        return false;
    }
};

}

void installStockPolish()
{
    static bool installed = false;
    if (installed || !qApp)
        return;
    installed = true;
    qApp->installEventFilter(new StockPolish(qApp));
}

void syncStockWidgets()
{
    installStockPolish();
    const bool next = Theme::isNext();
    const QWidgetList widgets = QApplication::allWidgets();
    for (QWidget *widget : widgets) {
        if (widget->testAttribute(Qt::WA_WState_Polished))
            syncStockWidget(widget, next);
    }
}

void prepareComboPopup(QComboBox *combo)
{
    if (!combo || !Theme::isNext())
        return;
    QAbstractItemView *view = combo->view();
    if (!view)
        return;
    if (QWidget *container = view->parentWidget();
        container && isFloatingSurface(container))
        syncFloatingSurface(container, true);
    QAbstractItemDelegate *delegate = view->itemDelegate();
    if (!delegate)
        return;
    const char *name = delegate->metaObject()->className();
    if (qstrcmp(name, "QComboBoxDelegate") != 0 &&
        qstrcmp(name, "QComboMenuDelegate") != 0)
        return;
    combo->setItemDelegate(new QStyledItemDelegate(combo));
}

}
