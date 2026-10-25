#include "DjMixerView.h"
#include "DjModel.h"
#include "DjWidgets.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace forkmesh::djstudio {

namespace {

QLabel *caption(const QString &text, QWidget *parent, int pixels = 9, const QColor &color = theme::dim())
{
    auto *l = new QLabel(text, parent);
    l->setFont(theme::font(pixels, true));
    l->setAlignment(Qt::AlignCenter);
    QPalette p = l->palette();
    p.setColor(QPalette::WindowText, color);
    l->setPalette(p);
    return l;
}

DjButton *button(const QString &text, QWidget *parent, int height = 22)
{
    auto *b = new DjButton(text, parent);
    b->setFontSize(9);
    b->setFixedHeight(height);
    return b;
}

class AspectBox : public QWidget
{
public:
    explicit AspectBox(QWidget *child, QWidget *parent) : QWidget(parent), m_child(child)
    {
        child->setParent(this);
        m_shield = new QWidget(this);
        m_shield->setAttribute(Qt::WA_NoSystemBackground);
        m_shield->setToolTip(QStringLiteral("The decks' videos, faded with the crossfader"));
    }

protected:
    void resizeEvent(QResizeEvent *) override
    {
        const int w = width();
        const int h = std::min(height(), w * 9 / 16);
        const QRect area((width() - w) / 2, (height() - h) / 2, w, h);
        m_child->setGeometry(area);
        m_shield->setGeometry(area);
        m_shield->raise();
    }
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), Qt::black);
    }

private:
    QWidget *m_child;
    QWidget *m_shield;
};

}

DjMixerView::DjMixerView(DjModel *model, QWidget *video, QWidget *parent) : QWidget(parent), m_model(model)
{
    auto *column = new QVBoxLayout(this);
    column->setContentsMargins(4, 4, 4, 4);
    column->setSpacing(4);
    auto *tabs = new QHBoxLayout;
    tabs->setSpacing(2);
    const QStringList names{QStringLiteral("MIXER"), QStringLiteral("VIDEO"), QStringLiteral("SAMPLER"), QStringLiteral("AUTOMIX")};
    for (int i = 0; i < names.size(); ++i) {
        auto *tab = button(names.at(i), this, 20);
        tab->clicked = [this, i]() { showTab(i); };
        m_tabs.append(tab);
        tabs->addWidget(tab);
    }
    column->addLayout(tabs);
    m_pages = new QStackedWidget(this);
    m_pages->addWidget(buildMixer());
    m_pages->addWidget(buildVideo(video));
    m_pages->addWidget(buildSampler());
    m_pages->addWidget(buildAutomix());
    column->addWidget(m_pages, 1);

    auto *fader = new QHBoxLayout;
    fader->setSpacing(3);
    auto *left = button(QStringLiteral("◀"), this, 24);
    left->setFixedWidth(22);
    left->setToolTip(QStringLiteral("Slam the crossfader to the left decks"));
    left->clicked = [this]() { m_model->slam(DjSide::A); };
    m_crossfader = new DjFader(Qt::Horizontal, true, this);
    m_crossfader->setFixedHeight(30);
    m_crossfader->setColor(QColor(0xdd, 0xdd, 0xdd));
    m_crossfader->setToolTip(QStringLiteral("Crossfader. Double-click to centre it."));
    m_crossfader->changed = [this](double value) { m_model->setCrossfader(value); };
    auto *right = button(QStringLiteral("▶"), this, 24);
    right->setFixedWidth(22);
    right->setToolTip(QStringLiteral("Slam the crossfader to the right decks"));
    right->clicked = [this]() { m_model->slam(DjSide::B); };
    m_curve = button(QStringLiteral("SMOOTH"), this, 24);
    m_curve->setFixedWidth(52);
    m_curve->setToolTip(QStringLiteral("Crossfader curve: smooth (equal power), full (both decks loud in the middle) or cut (for scratching)"));
    m_curve->clicked = [this]() { m_model->setCrossfaderCurve((m_model->crossfaderCurve() + 1) % 3); };
    fader->addWidget(left);
    fader->addWidget(m_crossfader, 1);
    fader->addWidget(right);
    fader->addWidget(m_curve);
    column->addLayout(fader);
    showTab(0);
    refresh();
}

void DjMixerView::showTab(int tab)
{
    m_pages->setCurrentIndex(tab);
    for (int i = 0; i < m_tabs.size(); ++i)
        m_tabs.at(i)->setLit(i == tab);
}

DjMixerView::Strip DjMixerView::makeStrip(int deck, QWidget *parent)
{
    const QColor color = theme::deck(deck);
    Strip strip;
    strip.deck = deck;
    strip.box = new QWidget(parent);
    strip.box->setFixedWidth(48);
    auto *column = new QVBoxLayout(strip.box);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);
    column->addWidget(caption(QString::number(deck + 1), strip.box, 11, color));
    strip.gain = new DjKnob(QStringLiteral("GAIN"), true, strip.box);
    strip.gain->setColor(color);
    strip.gain->setFixedSize(36, 38);
    strip.gain->changed = [this, deck](double value) { m_model->setGain(deck, value * 12); };
    column->addWidget(strip.gain, 0, Qt::AlignHCenter);
    const QStringList bands{QStringLiteral("LOW"), QStringLiteral("MID"), QStringLiteral("HIGH")};
    for (int band = 2; band >= 0; --band) {
        auto *knob = new DjKnob(bands.at(band), true, strip.box);
        knob->setColor(color);
        knob->setFixedSize(36, 38);
        knob->setToolTip(QStringLiteral("%1 EQ. Double-click to centre, right-click to kill the band.").arg(bands.at(band)));
        knob->changed = [this, deck, band](double value) { m_model->setEq(deck, band, value); };
        knob->rightClicked = [this, deck, band]() { m_model->toggleKill(deck, band); };
        strip.eq[band] = knob;
        column->addWidget(knob, 0, Qt::AlignHCenter);
    }
    strip.filter = new DjKnob(QStringLiteral("FILTER"), true, strip.box);
    strip.filter->setColor(QColor(0xc0, 0x7c, 0xff));
    strip.filter->setFixedSize(36, 38);
    strip.filter->setToolTip(QStringLiteral("Filter: left is low-pass, right is high-pass"));
    strip.filter->changed = [this, deck](double value) { m_model->setFilter(deck, value); };
    column->addWidget(strip.filter, 0, Qt::AlignHCenter);
    auto *bottom = new QHBoxLayout;
    bottom->setSpacing(2);
    strip.meter = new DjMeter(Qt::Vertical, strip.box);
    strip.meter->level = [this, deck]() { return m_model->deck(deck).meter; };
    strip.meter->hold = [this, deck]() { return m_model->deck(deck).meterHold; };
    strip.volume = new DjFader(Qt::Vertical, false, strip.box);
    strip.volume->setColor(color);
    strip.volume->setDefault(1);
    strip.volume->setToolTip(QStringLiteral("Deck %1 volume").arg(deck + 1));
    strip.volume->changed = [this, deck](double value) { m_model->setLevel(deck, value); };
    bottom->addWidget(strip.meter);
    bottom->addWidget(strip.volume, 1);
    column->addLayout(bottom, 1);
    return strip;
}

QWidget *DjMixerView::buildMixer()
{
    auto *page = new QWidget(this);
    m_stripRow = new QHBoxLayout(page);
    m_stripRow->setContentsMargins(0, 0, 0, 0);
    m_stripRow->setSpacing(4);
    m_masterBox = new QWidget(page);
    m_masterBox->setFixedWidth(60);
    auto *master = new QVBoxLayout(m_masterBox);
    master->setContentsMargins(0, 0, 0, 0);
    master->setSpacing(1);
    master->addWidget(caption(QStringLiteral("MASTER"), m_masterBox, 9, theme::text()));
    m_master = new DjKnob(QStringLiteral("VOL"), false, m_masterBox);
    m_master->setFixedSize(38, 40);
    m_master->setDefault(0.8);
    m_master->changed = [this](double value) { m_model->setMaster(value * 1.25); };
    master->addWidget(m_master, 0, Qt::AlignHCenter);
    m_sampler = new DjKnob(QStringLiteral("SAMPLER"), false, m_masterBox);
    m_sampler->setFixedSize(38, 40);
    m_sampler->setDefault(0.8);
    m_sampler->setColor(QColor(0xff, 0x9f, 0x1c));
    m_sampler->changed = [this](double value) { m_model->setSamplerLevel(value); };
    master->addWidget(m_sampler, 0, Qt::AlignHCenter);
    m_automix = button(QStringLiteral("AUTOMIX"), m_masterBox);
    m_automix->setToolTip(QStringLiteral("AUTOMIX: play the queue (then your library) one song into the next. Right-click to choose what it plays."));
    m_automix->clicked = [this]() { m_model->toggleAutomix(); };
    m_automix->rightClicked = [this]() {
        if (focusMenuRequested)
            focusMenuRequested();
    };
    m_autoDj = button(QStringLiteral("AUTO DJ"), m_masterBox);
    m_autoDj->setToolTip(QStringLiteral("AUTO DJ: follow each song with a related song people liked, found on YouTube"));
    m_autoDj->clicked = [this]() { m_model->toggleAutoDj(); };
    m_mixNow = button(QStringLiteral("MIX NOW"), m_masterBox);
    m_mixNow->setToolTip(QStringLiteral("Crossfade into the other deck now"));
    m_mixNow->clicked = [this]() { m_model->mixNow(); };
    master->addWidget(m_automix);
    master->addWidget(m_autoDj);
    master->addWidget(m_mixNow);
    auto *meters = new QHBoxLayout;
    meters->setSpacing(3);
    meters->addStretch();
    m_masterMeterL = new DjMeter(Qt::Vertical, m_masterBox);
    m_masterMeterR = new DjMeter(Qt::Vertical, m_masterBox);
    auto sideLevel = [this](bool right, bool peak) {
        double level = 0;
        for (int i = 0; i < m_model->deckCount(); ++i) {
            const DjDeck &deck = m_model->deck(i);
            const bool onRight = deck.assign == 1;
            if (deck.assign != 2 && onRight != right)
                continue;
            level = std::max(level, peak ? deck.meterHold : deck.meter);
        }
        return level;
    };
    m_masterMeterL->level = [sideLevel]() { return sideLevel(false, false); };
    m_masterMeterL->hold = [sideLevel]() { return sideLevel(false, true); };
    m_masterMeterR->level = [sideLevel]() { return sideLevel(true, false); };
    m_masterMeterR->hold = [sideLevel]() { return sideLevel(true, true); };
    meters->addWidget(m_masterMeterL);
    meters->addWidget(m_masterMeterR);
    meters->addStretch();
    master->addLayout(meters, 1);
    rebuildStrips();
    return page;
}

void DjMixerView::rebuildStrips()
{
    const int count = m_model->deckCount();
    if (count == m_stripCount && !m_strips.isEmpty())
        return;
    m_stripCount = count;
    for (const Strip &strip : m_strips)
        strip.box->deleteLater();
    m_strips.clear();
    while (m_stripRow->count())
        m_stripRow->takeAt(0);
    QWidget *page = m_masterBox->parentWidget();
    const QList<int> order = count == 4 ? QList<int>{2, 0, -1, 1, 3} : QList<int>{0, -1, 1};
    m_stripRow->addStretch();
    for (int deck : order) {
        if (deck < 0) {
            m_stripRow->addWidget(m_masterBox);
            continue;
        }
        Strip strip = makeStrip(deck, page);
        m_strips.append(strip);
        m_stripRow->addWidget(strip.box);
    }
    m_stripRow->addStretch();
    refresh();
}

QWidget *DjMixerView::buildVideo(QWidget *video)
{
    auto *page = new QWidget(this);
    auto *column = new QVBoxLayout(page);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(4);
    column->addWidget(new AspectBox(video, page), 1);
    auto *modes = new QHBoxLayout;
    modes->setSpacing(2);
    const QList<QPair<QString, QString>> list{{QStringLiteral("mix"), QStringLiteral("MIX")},
                                              {QStringLiteral("split"), QStringLiteral("SPLIT")},
                                              {QStringLiteral("deck0"), QStringLiteral("1")},
                                              {QStringLiteral("deck1"), QStringLiteral("2")},
                                              {QStringLiteral("deck2"), QStringLiteral("3")},
                                              {QStringLiteral("deck3"), QStringLiteral("4")}};
    for (const auto &mode : list) {
        auto *b = button(mode.second, page);
        b->setToolTip(mode.first == QLatin1String("mix") ? QStringLiteral("Fade the videos with the crossfader")
                      : mode.first == QLatin1String("split") ? QStringLiteral("Show every deck side by side")
                                                              : QStringLiteral("Show only deck %1").arg(mode.second));
        const QString key = mode.first;
        b->clicked = [this, key]() { m_model->setVideoMode(key); };
        b->setProperty("mode", key);
        m_videoModes.append(b);
        modes->addWidget(b);
    }
    column->addLayout(modes);
    auto *note = caption(QStringLiteral("YouTube's player and its advertising stay under YouTube's control."), page, 9);
    note->setWordWrap(true);
    column->addWidget(note);
    return page;
}

QWidget *DjMixerView::buildSampler()
{
    auto *page = new QWidget(this);
    auto *column = new QVBoxLayout(page);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(4);
    column->addWidget(caption(QStringLiteral("DRUMS  (keys E R T Y U I O P)"), page));
    auto *drums = new QGridLayout;
    drums->setSpacing(3);
    const QList<DjDrumPad> pads = drumPads();
    const QString keys = QStringLiteral("ERTYUIOP");
    for (int i = 0; i < pads.size(); ++i) {
        auto *b = new DjButton(drumPadLabel(pads.at(i)).toUpper(), page);
        b->setFixedHeight(30);
        b->setFontSize(9);
        b->setColor(QColor(0xff, 0x9f, 0x1c));
        b->setToolTip(QStringLiteral("%1 (key %2)").arg(drumPadLabel(pads.at(i)), keys.at(i)));
        const QString key = drumPadKey(pads.at(i));
        b->pressed = [this, key]() { m_model->playPad(key); };
        m_drums.append(b);
        drums->addWidget(b, i / 4, i % 4);
    }
    column->addLayout(drums);
    column->addWidget(caption(QStringLiteral("SOUND EFFECTS"), page));
    auto *effects = new QGridLayout;
    effects->setSpacing(3);
    const auto &list = soundEffects();
    for (int i = 0; i < list.size(); ++i) {
        auto *b = new DjButton(list.at(i).name.toUpper(), page);
        b->setFixedHeight(26);
        b->setFontSize(9);
        b->setColor(QColor(0xc0, 0x7c, 0xff));
        const QString id = list.at(i).id;
        b->pressed = [this, id]() { m_model->playPad(id); };
        effects->addWidget(b, i / 2, i % 2);
    }
    column->addLayout(effects);
    column->addWidget(caption(QStringLiteral("SAMPLES  (record with SAMPLE on a deck)"), page));
    auto *samples = new QGridLayout;
    samples->setSpacing(3);
    for (int i = 0; i < studio::sectionCount; ++i) {
        auto *b = new DjButton(QStringLiteral("S%1").arg(i + 1), page);
        b->setFixedHeight(26);
        b->setFontSize(9);
        b->setFilled(true);
        b->clicked = [this, i]() { m_model->playSection(i); };
        b->rightClicked = [this, i]() { m_model->clearSection(i); };
        m_sections.append(b);
        samples->addWidget(b, 0, i);
    }
    column->addLayout(samples);
    column->addStretch();
    return page;
}

QWidget *DjMixerView::buildAutomix()
{
    auto *page = new QWidget(this);
    auto *column = new QVBoxLayout(page);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(5);
    m_automixPage = button(QStringLiteral("AUTOMIX"), page, 26);
    m_automixPage->clicked = [this]() { m_model->toggleAutomix(); };
    m_autoDjPage = button(QStringLiteral("AUTO DJ"), page, 26);
    m_autoDjPage->clicked = [this]() { m_model->toggleAutoDj(); };
    auto *row = new QHBoxLayout;
    row->addWidget(m_automixPage);
    row->addWidget(m_autoDjPage);
    column->addLayout(row);
    auto *lengthRow = new QHBoxLayout;
    m_length = new DjKnob(QStringLiteral("MIX LENGTH"), false, page);
    m_length->setFixedSize(60, 52);
    m_length->setDefault((studio::automixSeconds - 2) / 28);
    m_length->changed = [this](double value) { m_model->setAutomixLength(2 + value * 28); };
    m_lengthLabel = caption(QString(), page, 11, theme::text());
    lengthRow->addWidget(m_length);
    lengthRow->addWidget(m_lengthLabel, 1);
    column->addLayout(lengthRow);
    auto *focus = button(QStringLiteral("CHOOSE WHAT AUTOMIX PLAYS…"), page, 24);
    focus->clicked = [this]() {
        if (focusMenuRequested)
            focusMenuRequested();
    };
    column->addWidget(focus);
    m_focusLabel = caption(QString(), page, 10, theme::text());
    m_focusLabel->setWordWrap(true);
    column->addWidget(m_focusLabel);
    m_shuffle = button(QStringLiteral("SHUFFLE"), page, 22);
    m_shuffle->setToolTip(QStringLiteral("With a playlist or tag chosen, shuffle it or play it in order"));
    m_shuffle->clicked = [this]() { m_model->setShuffle(!m_model->crate().shuffle); };
    column->addWidget(m_shuffle);
    m_nextLabel = caption(QString(), page, 10, theme::dim());
    m_nextLabel->setWordWrap(true);
    column->addWidget(m_nextLabel);
    auto *now = button(QStringLiteral("MIX INTO THE OTHER DECK NOW"), page, 24);
    now->clicked = [this]() { m_model->mixNow(); };
    column->addWidget(now);
    column->addStretch();
    return page;
}

void DjMixerView::refresh()
{
    if (!m_crossfader || !m_nextLabel)
        return;
    rebuildStrips();
    for (Strip &strip : m_strips) {
        const DjDeck &deck = m_model->deck(strip.deck);
        strip.gain->setValue(deck.gainDb / 12);
        strip.gain->setCaption(QStringLiteral("%1 dB").arg(deck.gainDb, 0, 'f', 1));
        for (int band = 0; band < 3; ++band) {
            strip.eq[band]->setValue(deck.eq[band]);
            strip.eq[band]->setMarked(deck.kill[band]);
        }
        strip.filter->setValue(deck.filter);
        strip.volume->setValue(deck.level);
    }
    m_master->setValue(m_model->master() / 1.25);
    m_sampler->setValue(m_model->samplerLevel());
    m_crossfader->setValue(m_model->crossfader());
    static const QStringList curves{QStringLiteral("SMOOTH"), QStringLiteral("FULL"), QStringLiteral("CUT")};
    m_curve->setLabel(curves.at(m_model->crossfaderCurve()));
    for (DjButton *b : {m_automix, m_automixPage}) {
        b->setLit(m_model->automixOn());
        b->setBlink(m_model->automixing());
    }
    for (DjButton *b : {m_autoDj, m_autoDjPage}) {
        b->setLit(m_model->autoDjOn());
        b->setBlink(m_model->autoDjFinding());
    }
    m_mixNow->setLit(m_model->automixing());
    m_length->setValue((m_model->automixLength() - 2) / 28);
    m_lengthLabel->setText(QStringLiteral("Crossfades over %1 s").arg(m_model->automixLength(), 0, 'f', 0));
    const DjCrate &crate = m_model->crate();
    m_focusLabel->setText(QStringLiteral("With the queue empty, AUTOMIX plays: %1").arg(crate.label(crate.focus)));
    m_shuffle->setLit(crate.shuffle);
    m_shuffle->setEnabled(!crate.focus.isEverything());
    const auto next = m_model->autoDjNext();
    m_nextLabel->setText(m_model->autoDjOn() ? (m_model->autoDjFinding() ? QStringLiteral("AUTO DJ is finding the next song…")
                                                : next ? QStringLiteral("AUTO DJ's next pick: %1").arg(next->title)
                                                       : QStringLiteral("AUTO DJ has no pick yet."))
                                             : QStringLiteral("Queue: %1 songs").arg(m_model->shownQueue().size()));
    for (DjButton *b : m_videoModes)
        b->setLit(b->property("mode").toString() == m_model->videoMode());
    const auto &sections = m_model->sections();
    for (int i = 0; i < m_sections.size(); ++i) {
        const bool has = i < sections.size() && sections.at(i).has_value();
        m_sections.at(i)->setLit(has);
        if (has)
            m_sections.at(i)->setColor(theme::deck(sections.at(i)->deck));
        m_sections.at(i)->setToolTip(has ? QStringLiteral("%1 s from deck %2 at %3 (right-click to clear)")
                                               .arg(sections.at(i)->duration, 0, 'f', 1)
                                               .arg(sections.at(i)->deck + 1)
                                               .arg(studio::clock(sections.at(i)->start))
                                         : QStringLiteral("Empty sample slot"));
    }
}

void DjMixerView::animate()
{
    for (const Strip &strip : m_strips)
        strip.meter->update();
    m_masterMeterL->update();
    m_masterMeterR->update();
    if (m_model->automixing())
        m_crossfader->setValue(m_model->crossfader());
    if (m_model->automixing() || m_model->autoDjFinding()) {
        m_automix->update();
        m_autoDj->update();
    }
}

void DjMixerView::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), theme::panel());
    painter.setPen(theme::line());
    painter.drawRect(rect().adjusted(0, 0, -1, -1));
}

}
