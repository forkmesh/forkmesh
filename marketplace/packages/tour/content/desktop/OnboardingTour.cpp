#include "OnboardingTour.h"

#include <QApplication>
#include <QEvent>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QRadialGradient>
#include <QResizeEvent>
#include <QShowEvent>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>

namespace forkmesh::ui {

namespace {

constexpr int kOverlayMargin = 22;
constexpr int kTargetPadding = 7;
constexpr int kCardGap = 22;
constexpr int kCardWidth = 440;
constexpr int kWelcomeCardWidth = 720;

QLabel *featureCopy(const QString &eyebrow, const QString &title,
                    const QString &body, QWidget *parent)
{
    auto *copy = new QLabel(parent);
    copy->setObjectName(QStringLiteral("onboardingFeatureCopy"));
    copy->setTextFormat(Qt::RichText);
    copy->setWordWrap(true);
    copy->setText(
        QStringLiteral("<span class='featureEyebrow'>%1</span><br>"
                       "<b>%2</b><br><span class='featureBody'>%3</span>")
            .arg(eyebrow.toHtmlEscaped(), title.toHtmlEscaped(),
                 body.toHtmlEscaped()));
    return copy;
}

QFrame *featureCard(const QString &number, const QString &title,
                    const QString &body, QWidget *parent)
{
    auto *card = new QFrame(parent);
    card->setObjectName(QStringLiteral("onboardingFeatureCard"));
    card->setAttribute(Qt::WA_StyledBackground, true);
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(14, 13, 14, 14);
    layout->setSpacing(7);
    auto *badge = new QLabel(number, card);
    badge->setObjectName(QStringLiteral("onboardingFeatureBadge"));
    badge->setAlignment(Qt::AlignCenter);
    badge->setFixedSize(28, 28);
    layout->addWidget(badge, 0, Qt::AlignLeft);
    layout->addWidget(featureCopy(QStringLiteral("FORKMESH"), title, body, card));
    layout->addStretch(1);
    return card;
}

constexpr OnboardingTrack kTracks[] = {
    OnboardingTrack::Essentials,
    OnboardingTrack::Advanced,
    OnboardingTrack::Everything,
};

QString trackSummary(OnboardingTrack track)
{
    switch (track) {
    case OnboardingTrack::Essentials:
        return QStringLiteral("The rail, agents, the prompt, repositories, Pings and search.");
    case OnboardingTrack::Advanced:
        return QStringLiteral("Adds planning, review, automation, chat, files, the Market and settings.");
    case OnboardingTrack::Everything:
        return QStringLiteral("Every workspace, from the ledger and calendar to your nodes.");
    }
    return {};
}

} // namespace

QVector<OnboardingStep> OnboardingTour::stepsForTrack(const QVector<OnboardingStep> &steps,
                                                      OnboardingTrack track)
{
    QVector<OnboardingStep> out;
    out.reserve(steps.size());
    for (const OnboardingStep &step : steps) {
        if (static_cast<int>(step.track) <= static_cast<int>(track))
            out.append(step);
    }
    return out;
}

QString OnboardingTour::trackTitle(OnboardingTrack track)
{
    switch (track) {
    case OnboardingTrack::Essentials:
        return QStringLiteral("Essentials");
    case OnboardingTrack::Advanced:
        return QStringLiteral("Advanced");
    case OnboardingTrack::Everything:
        return QStringLiteral("The whole shebang");
    }
    return {};
}

OnboardingTour::OnboardingTour(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("onboardingOverlay"));
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAccessibleName(QStringLiteral("ForkMesh welcome and feature tour"));
    if (parent) {
        parent->installEventFilter(this);
        setGeometry(parent->rect());
    }
    buildCard();
    hide();
}

OnboardingTour::~OnboardingTour()
{
    stopWatchingTarget();
    if (parent())
        parent()->removeEventFilter(this);
}

void OnboardingTour::buildCard()
{
    m_card = new QFrame(this);
    m_card->setObjectName(QStringLiteral("onboardingCard"));
    m_card->setAttribute(Qt::WA_StyledBackground, true);
    m_card->setAccessibleName(QStringLiteral("Tour card"));

    auto *outer = new QVBoxLayout(m_card);
    outer->setContentsMargins(28, 24, 28, 24);
    outer->setSpacing(11);

    auto *top = new QHBoxLayout;
    top->setContentsMargins(0, 0, 0, 0);
    top->setSpacing(8);
    m_brand = new QLabel(QStringLiteral("FORKMESH"), m_card);
    m_brand->setObjectName(QStringLiteral("onboardingBrand"));
    top->addWidget(m_brand);
    top->addStretch(1);
    m_closeButton = new QPushButton(QStringLiteral("×"), m_card);
    m_closeButton->setObjectName(QStringLiteral("onboardingCloseButton"));
    m_closeButton->setAccessibleName(QStringLiteral("Close welcome or skip tour"));
    m_closeButton->setCursor(Qt::PointingHandCursor);
    m_closeButton->setFixedSize(30, 30);
    connect(m_closeButton, &QPushButton::clicked, this,
            &OnboardingTour::dismiss);
    top->addWidget(m_closeButton, 0, Qt::AlignTop);
    outer->addLayout(top);

    m_kicker = new QLabel(m_card);
    m_kicker->setObjectName(QStringLiteral("onboardingKicker"));
    outer->addWidget(m_kicker);
    m_title = new QLabel(m_card);
    m_title->setObjectName(QStringLiteral("onboardingTitle"));
    m_title->setWordWrap(true);
    m_title->setTextFormat(Qt::PlainText);
    m_title->setAccessibleName(QStringLiteral("Tour step title"));
    outer->addWidget(m_title);
    m_body = new QLabel(m_card);
    m_body->setObjectName(QStringLiteral("onboardingBody"));
    m_body->setWordWrap(true);
    m_body->setTextFormat(Qt::PlainText);
    outer->addWidget(m_body);

    m_welcomeFeatures = new QWidget(m_card);
    m_welcomeFeatures->setObjectName(QStringLiteral("onboardingWelcomeFeatures"));
    auto *featureRow = new QHBoxLayout(m_welcomeFeatures);
    featureRow->setContentsMargins(0, 5, 0, 3);
    featureRow->setSpacing(10);
    featureRow->addWidget(featureCard(
        QStringLiteral("01"), QStringLiteral("Build with agents"),
        QStringLiteral("Delegate coding work in isolated branches and follow it live."),
        m_welcomeFeatures), 1);
    featureRow->addWidget(featureCard(
        QStringLiteral("02"), QStringLiteral("Ship together"),
        QStringLiteral("Plan, review, automate, discuss, and release in one workspace."),
        m_welcomeFeatures), 1);
    featureRow->addWidget(featureCard(
        QStringLiteral("03"), QStringLiteral("Own your mesh"),
        QStringLiteral("Mirror locally, encrypt private work, and control your nodes."),
        m_welcomeFeatures), 1);
    outer->addWidget(m_welcomeFeatures);

    m_trackRow = new QWidget(m_card);
    m_trackRow->setObjectName(QStringLiteral("onboardingTrackRow"));
    auto *trackLayout = new QHBoxLayout(m_trackRow);
    trackLayout->setContentsMargins(0, 4, 0, 2);
    trackLayout->setSpacing(10);
    for (const OnboardingTrack track : kTracks) {
        auto *button = new QPushButton(m_trackRow);
        button->setObjectName(QStringLiteral("onboardingTrackButton"));
        button->setProperty("onboardingTrack", static_cast<int>(track));
        button->setCursor(Qt::PointingHandCursor);
        button->setMinimumHeight(78);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        button->setAccessibleName(
            QStringLiteral("Take the %1 tour").arg(trackTitle(track)));
        button->setAccessibleDescription(trackSummary(track));
        connect(button, &QPushButton::clicked, this,
                [this, track] { chooseTrack(track); });
        trackLayout->addWidget(button, 1);
        m_trackButtons.append(button);
    }
    m_trackSizes = {0, 0, 0};
    outer->addWidget(m_trackRow);

    m_details = new QLabel(m_card);
    m_details->setObjectName(QStringLiteral("onboardingDetails"));
    m_details->setWordWrap(true);
    m_details->setTextFormat(Qt::RichText);
    outer->addWidget(m_details);

    m_hint = new QLabel(m_card);
    m_hint->setObjectName(QStringLiteral("onboardingHint"));
    m_hint->setWordWrap(true);
    outer->addWidget(m_hint);

    auto *progressRow = new QHBoxLayout;
    progressRow->setContentsMargins(0, 3, 0, 0);
    progressRow->setSpacing(10);
    m_progress = new QProgressBar(m_card);
    m_progress->setObjectName(QStringLiteral("onboardingProgress"));
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(5);
    m_progress->setMinimum(0);
    progressRow->addWidget(m_progress, 1);
    m_progressLabel = new QLabel(m_card);
    m_progressLabel->setObjectName(QStringLiteral("onboardingProgressLabel"));
    progressRow->addWidget(m_progressLabel);
    outer->addLayout(progressRow);

    auto *buttons = new QHBoxLayout;
    buttons->setContentsMargins(0, 5, 0, 0);
    buttons->setSpacing(8);
    m_backButton = new QPushButton(QStringLiteral("Back"), m_card);
    m_backButton->setObjectName(QStringLiteral("ghostButton"));
    m_backButton->setAccessibleName(QStringLiteral("Previous tour step"));
    m_backButton->setCursor(Qt::PointingHandCursor);
    connect(m_backButton, &QPushButton::clicked, this,
            &OnboardingTour::previousStep);
    buttons->addWidget(m_backButton);
    buttons->addStretch(1);
    m_secondaryButton = new QPushButton(m_card);
    m_secondaryButton->setObjectName(QStringLiteral("ghostButton"));
    m_secondaryButton->setCursor(Qt::PointingHandCursor);
    buttons->addWidget(m_secondaryButton);
    m_primaryButton = new QPushButton(m_card);
    m_primaryButton->setObjectName(QStringLiteral("primaryButton"));
    m_primaryButton->setCursor(Qt::PointingHandCursor);
    m_primaryButton->setDefault(true);
    buttons->addWidget(m_primaryButton);
    outer->addLayout(buttons);

    connect(m_secondaryButton, &QPushButton::clicked, this, &OnboardingTour::dismiss);
    connect(m_primaryButton, &QPushButton::clicked, this, [this] {
        if (m_view == View::Tour)
            nextStep();
    });
    setTrackSizes({});

    auto *effect = new QGraphicsOpacityEffect(m_card);
    effect->setOpacity(1.0);
    m_card->setGraphicsEffect(effect);
    m_cardFade = new QPropertyAnimation(effect, "opacity", this);
    m_cardFade->setDuration(180);
    m_cardFade->setEasingCurve(QEasingCurve::OutCubic);
}

void OnboardingTour::showWelcome(const QString &nodeName)
{
    stopWatchingTarget();
    m_steps.clear();
    m_currentStep = -1;
    setView(View::Welcome);
    m_kicker->setText(QStringLiteral("WELCOME TO FORKMESH"));
    m_title->setText(QStringLiteral("Your work has a new home."));
    const QString cleanName = nodeName.trimmed();
    m_body->setText(
        cleanName.isEmpty()
            ? QStringLiteral(
                  "Build with coding agents, collaborate across repositories, "
                  "and keep your work mirrored on infrastructure you control.")
            : QStringLiteral(
                  "Your node, %1, is ready. Build with coding agents, collaborate "
                  "across repositories, and keep your work mirrored on "
                  "infrastructure you control.")
                  .arg(cleanName));
    m_welcomeFeatures->show();
    m_details->hide();
    m_hint->setText(QStringLiteral(
        "Pick how deep to go. The tour opens feature pages but does not create, "
        "publish, or change anything."));
    showTrackChoices(true);
    m_secondaryButton->setText(QStringLiteral("Explore on my own"));
    m_secondaryButton->setAccessibleName(QStringLiteral("Skip the tour"));
    m_closeButton->setAccessibleName(QStringLiteral("Explore on my own"));
    show();
    raise();
    setFocus(Qt::OtherFocusReason);
    positionCard();
    animateCard();
    update();
}

void OnboardingTour::showChooser()
{
    stopWatchingTarget();
    m_steps.clear();
    m_currentStep = -1;
    setView(View::Choose);
    m_kicker->setText(QStringLiteral("TOUR"));
    m_title->setText(QStringLiteral("Take the tour"));
    m_body->setText(QStringLiteral(
        "Each track spotlights the real controls. Essentials covers the everyday "
        "desktop, Advanced adds the repository workspace and collaboration, and "
        "the whole shebang visits every page."));
    m_welcomeFeatures->hide();
    m_details->hide();
    m_hint->setText(QStringLiteral(
        "The tour opens feature pages but does not create, publish, or change anything."));
    showTrackChoices(true);
    m_secondaryButton->setText(QStringLiteral("Not now"));
    m_secondaryButton->setAccessibleName(QStringLiteral("Close the tour"));
    m_closeButton->setAccessibleName(QStringLiteral("Close the tour"));
    show();
    raise();
    setFocus(Qt::OtherFocusReason);
    positionCard();
    animateCard();
    update();
}

void OnboardingTour::showTrackChoices(bool shown)
{
    m_trackRow->setVisible(shown);
    m_primaryButton->setVisible(!shown);
    m_progress->setVisible(!shown);
    m_progressLabel->setVisible(!shown);
    m_backButton->setVisible(!shown);
    m_secondaryButton->show();
    if (shown && !m_trackButtons.isEmpty())
        m_trackButtons.first()->setDefault(true);
    m_primaryButton->setDefault(!shown);
}

void OnboardingTour::setTrackSizes(const QVector<OnboardingStep> &steps)
{
    for (int i = 0; i < m_trackButtons.size(); ++i) {
        const OnboardingTrack track = kTracks[i];
        const int size = steps.isEmpty() ? 0 : stepsForTrack(steps, track).size();
        m_trackSizes[i] = size;
        const QString heading = size > 0
                                    ? QStringLiteral("%1  ·  %2 stops").arg(trackTitle(track)).arg(size)
                                    : trackTitle(track);
        m_trackButtons.at(i)->setText(heading + QLatin1Char('\n') + trackSummary(track));
    }
}

QStringList OnboardingTour::trackChoices() const
{
    QStringList out;
    if (!m_trackRow || m_trackRow->isHidden())
        return out;
    for (const OnboardingTrack track : kTracks)
        out.append(trackTitle(track));
    return out;
}

void OnboardingTour::chooseTrack(OnboardingTrack track)
{
    if (m_view != View::Welcome && m_view != View::Choose)
        return;
    if (startTourRequested)
        startTourRequested(track);
}

void OnboardingTour::startTour(QVector<OnboardingStep> steps)
{
    if (steps.isEmpty()) {
        closeOverlay(true, false);
        return;
    }
    stopWatchingTarget();
    m_steps = std::move(steps);
    m_currentStep = 0;
    setView(View::Tour);
    m_welcomeFeatures->hide();
    m_details->show();
    showTrackChoices(false);
    m_secondaryButton->setText(QStringLiteral("Skip tour"));
    m_secondaryButton->setAccessibleName(QStringLiteral("Skip the rest of the tour"));
    m_closeButton->setAccessibleName(QStringLiteral("Skip the rest of the tour"));
    show();
    raise();
    setFocus(Qt::OtherFocusReason);
    renderStep();
}

void OnboardingTour::setView(View view)
{
    m_view = view;
    setProperty("onboardingView",
                view == View::Welcome  ? QStringLiteral("welcome")
                : view == View::Choose ? QStringLiteral("choose")
                                       : QStringLiteral("tour"));
    style()->unpolish(this);
    style()->polish(this);
}

void OnboardingTour::nextStep()
{
    if (m_view != View::Tour || m_steps.isEmpty())
        return;
    if (m_currentStep >= m_steps.size() - 1) {
        closeOverlay(true, false);
        return;
    }
    ++m_currentStep;
    renderStep();
}

void OnboardingTour::previousStep()
{
    if (m_view != View::Tour || m_currentStep <= 0)
        return;
    --m_currentStep;
    renderStep();
}

void OnboardingTour::dismiss()
{
    if (m_view == View::Welcome) {
        closeOverlay(false, true);
    } else if (m_view == View::Choose) {
        stopWatchingTarget();
        hide();
        m_view = View::Hidden;
    } else if (m_view == View::Tour) {
        closeOverlay(false, false);
    }
}

void OnboardingTour::closeOverlay(bool completed, bool fromWelcome)
{
    stopWatchingTarget();
    hide();
    m_view = View::Hidden;
    if (fromWelcome) {
        if (welcomeDismissed)
            welcomeDismissed();
    } else if (tourClosed) {
        tourClosed(completed);
    }
    m_steps.clear();
    m_currentStep = -1;
}

QString OnboardingTour::currentStepId() const
{
    return m_currentStep >= 0 && m_currentStep < m_steps.size()
               ? m_steps.at(m_currentStep).id
               : QString();
}

QStringList OnboardingTour::stepIds() const
{
    QStringList ids;
    ids.reserve(m_steps.size());
    for (const OnboardingStep &step : m_steps)
        ids.append(step.id);
    return ids;
}

QString OnboardingTour::currentTitle() const
{
    return m_title ? m_title->text() : QString();
}

void OnboardingTour::renderDetails(const QStringList &details)
{
    QString html;
    for (const QString &detail : details) {
        if (!html.isEmpty())
            html += QStringLiteral("<br>");
        html += QStringLiteral(
                    "<span class='tourBullet'>✓</span>&nbsp;&nbsp;%1")
                    .arg(detail.toHtmlEscaped());
    }
    m_details->setText(html);
    m_details->setVisible(!html.isEmpty());
}

void OnboardingTour::renderStep()
{
    if (m_currentStep < 0 || m_currentStep >= m_steps.size())
        return;
    stopWatchingTarget();
    const OnboardingStep &step = m_steps.at(m_currentStep);
    if (step.reveal)
        step.reveal();
    m_kicker->setText(step.kicker.toUpper());
    m_title->setText(step.title);
    m_body->setText(step.body);
    renderDetails(step.details);
    m_hint->setText(
        m_currentStep == m_steps.size() - 1
            ? QStringLiteral("Replay this tour any time from Tour in the navigation rail.")
            : QStringLiteral("Use ← and → to move through the tour. Esc skips it."));
    m_progress->setRange(0, m_steps.size());
    m_progress->setValue(m_currentStep + 1);
    m_progressLabel->setText(
        QStringLiteral("%1 of %2").arg(m_currentStep + 1).arg(m_steps.size()));
    m_backButton->setEnabled(m_currentStep > 0);
    const bool last = m_currentStep == m_steps.size() - 1;
    m_primaryButton->setText(last ? QStringLiteral("Start exploring")
                                  : QStringLiteral("Next  →"));
    m_primaryButton->setAccessibleName(
        last ? QStringLiteral("Finish the tour")
             : QStringLiteral("Next tour step"));
    m_secondaryButton->setVisible(!last);
    watchTarget(step.target);
    raise();
    QTimer::singleShot(0, this, [this] {
        positionCard();
        update();
    });
    positionCard();
    animateCard();
    update();
}

void OnboardingTour::watchTarget(QWidget *target)
{
    m_watchedTarget = target;
    if (m_watchedTarget)
        m_watchedTarget->installEventFilter(this);
}

void OnboardingTour::stopWatchingTarget()
{
    if (m_watchedTarget)
        m_watchedTarget->removeEventFilter(this);
    m_watchedTarget.clear();
}

QRect OnboardingTour::mappedTargetRect() const
{
    QWidget *target = m_watchedTarget.data();
    if (!target || !target->isVisible() || !target->isVisibleTo(window()))
        return {};
    // The spotlight overlay and its target are usually siblings under the app
    // frame, not ancestors of one another. Mapping through global coordinates
    // handles both that normal case and nested page controls without Qt's
    // "parent must be in parent hierarchy" fallback.
    const QPoint topLeft =
        const_cast<OnboardingTour *>(this)->mapFromGlobal(
            target->mapToGlobal(QPoint(0, 0)));
    QRect mapped(topLeft, target->size());
    mapped = mapped.intersected(rect());
    if (mapped.width() < 2 || mapped.height() < 2)
        return {};
    mapped.adjust(-kTargetPadding, -kTargetPadding, kTargetPadding,
                  kTargetPadding);
    return mapped.intersected(rect().adjusted(4, 4, -4, -4));
}

QRect OnboardingTour::spotlightRect() const
{
    return mappedTargetRect();
}

void OnboardingTour::positionCard()
{
    if (!m_card || width() <= 0 || height() <= 0)
        return;

    const bool choosing = m_view == View::Welcome || m_view == View::Choose;
    const bool centred = choosing || mappedTargetRect().isEmpty() ||
                         (m_view == View::Tour &&
                          m_currentStep == m_steps.size() - 1);
    const int wantedWidth = choosing ? kWelcomeCardWidth : kCardWidth;
    const int cardWidth = qMax(300, qMin(wantedWidth, width() - 2 * kOverlayMargin));
    m_card->setFixedWidth(cardWidth);
    m_card->setMaximumHeight(qMax(240, height() - 2 * kOverlayMargin));
    if (m_card->layout())
        m_card->layout()->activate();
    const int cardHeight = qMin(m_card->sizeHint().height(),
                                qMax(240, height() - 2 * kOverlayMargin));
    m_card->resize(cardWidth, cardHeight);

    const QRect bounds = rect().adjusted(kOverlayMargin, kOverlayMargin,
                                         -kOverlayMargin, -kOverlayMargin);
    if (centred) {
        m_card->move(bounds.center().x() - cardWidth / 2,
                     bounds.center().y() - cardHeight / 2);
        return;
    }

    const QRect target = mappedTargetRect();
    QVector<QPoint> candidates{
        QPoint(target.right() + kCardGap,
               target.center().y() - cardHeight / 2),
        QPoint(target.left() - kCardGap - cardWidth,
               target.center().y() - cardHeight / 2),
        QPoint(target.center().x() - cardWidth / 2,
               target.bottom() + kCardGap),
        QPoint(target.center().x() - cardWidth / 2,
               target.top() - kCardGap - cardHeight),
    };

    int bestOverlap = std::numeric_limits<int>::max();
    QPoint best = candidates.first();
    for (QPoint candidate : std::as_const(candidates)) {
        candidate.setX(qBound(bounds.left(), candidate.x(),
                              bounds.right() - cardWidth + 1));
        candidate.setY(qBound(bounds.top(), candidate.y(),
                              bounds.bottom() - cardHeight + 1));
        const QRect cardRect(candidate, QSize(cardWidth, cardHeight));
        const QRect overlap = cardRect.intersected(target);
        const int area = overlap.width() * overlap.height();
        if (area < bestOverlap) {
            bestOverlap = area;
            best = candidate;
            if (area == 0)
                break;
        }
    }
    m_card->move(best);
}

void OnboardingTour::animateCard()
{
    if (!m_cardFade)
        return;
    m_cardFade->stop();
    m_cardFade->setStartValue(0.72);
    m_cardFade->setEndValue(1.0);
    m_cardFade->start();
}

bool OnboardingTour::eventFilter(QObject *watched, QEvent *event)
{
    if (!event)
        return QWidget::eventFilter(watched, event);
    if (watched == parent() && event->type() == QEvent::Resize) {
        if (auto *parentWidget = qobject_cast<QWidget *>(parent()))
            setGeometry(parentWidget->rect());
        positionCard();
        update();
    } else if (watched == m_watchedTarget &&
               (event->type() == QEvent::Move ||
                event->type() == QEvent::Resize ||
                event->type() == QEvent::Show ||
                event->type() == QEvent::Hide ||
                event->type() == QEvent::LayoutRequest)) {
        positionCard();
        update();
    }
    return QWidget::eventFilter(watched, event);
}

void OnboardingTour::keyPressEvent(QKeyEvent *event)
{
    if (!event)
        return;
    if (event->key() == Qt::Key_Escape) {
        dismiss();
        event->accept();
        return;
    }
    if (m_view == View::Tour &&
        (event->key() == Qt::Key_Right || event->key() == Qt::Key_Space ||
         event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
        nextStep();
        event->accept();
        return;
    }
    if (m_view == View::Tour && event->key() == Qt::Key_Left) {
        previousStep();
        event->accept();
        return;
    }
    if ((m_view == View::Welcome || m_view == View::Choose) &&
        (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
        chooseTrack(OnboardingTrack::Essentials);
        event->accept();
        return;
    }
    if ((m_view == View::Welcome || m_view == View::Choose) &&
        event->key() >= Qt::Key_1 && event->key() <= Qt::Key_3) {
        chooseTrack(kTracks[event->key() - Qt::Key_1]);
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void OnboardingTour::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const bool choosing = m_view == View::Welcome || m_view == View::Choose;
    const QColor shadow(1, 4, 9, choosing ? 218 : 188);
    QPainterPath shade;
    shade.setFillRule(Qt::OddEvenFill);
    shade.addRect(rect());
    const QRect target = mappedTargetRect();
    if (m_view == View::Tour && !target.isEmpty())
        shade.addRoundedRect(QRectF(target), 11, 11);
    painter.fillPath(shade, shadow);

    if (choosing) {
        // A quiet mesh behind the welcome card: deterministic and code-native,
        // so it is crisp at every scale and follows the active palette.
        const QColor accent = palette().color(QPalette::Highlight);
        const QVector<QPointF> points{
            {0.08, 0.18}, {0.20, 0.08}, {0.33, 0.20}, {0.48, 0.08},
            {0.67, 0.16}, {0.88, 0.08}, {0.94, 0.33}, {0.82, 0.48},
            {0.93, 0.72}, {0.75, 0.89}, {0.55, 0.80}, {0.34, 0.92},
            {0.16, 0.78}, {0.06, 0.56}, {0.17, 0.40},
        };
        painter.setPen(QPen(QColor(accent.red(), accent.green(), accent.blue(), 42),
                            1.25));
        for (int i = 1; i < points.size(); ++i) {
            const QPointF a(points.at(i - 1).x() * width(),
                            points.at(i - 1).y() * height());
            const QPointF b(points.at(i).x() * width(),
                            points.at(i).y() * height());
            painter.drawLine(a, b);
            if (i > 2 && i % 3 == 0) {
                const QPointF c(points.at(i - 3).x() * width(),
                                points.at(i - 3).y() * height());
                painter.drawLine(b, c);
            }
        }
        painter.setPen(Qt::NoPen);
        for (int i = 0; i < points.size(); ++i) {
            QColor node = accent;
            node.setAlpha(i % 3 == 0 ? 170 : 95);
            painter.setBrush(node);
            const QPointF at(points.at(i).x() * width(),
                             points.at(i).y() * height());
            painter.drawEllipse(at, i % 3 == 0 ? 4.5 : 3.0,
                                i % 3 == 0 ? 4.5 : 3.0);
        }
    } else if (!target.isEmpty()) {
        QColor accent = palette().color(QPalette::Highlight);
        accent.setAlpha(235);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(accent, 2.5));
        painter.drawRoundedRect(QRectF(target).adjusted(1.25, 1.25, -1.25, -1.25),
                                10, 10);
    }
}

void OnboardingTour::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    positionCard();
}

void OnboardingTour::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    raise();
    positionCard();
}

} // namespace forkmesh::ui
