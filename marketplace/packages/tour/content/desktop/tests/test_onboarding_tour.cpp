#include "OnboardingTour.h"
#include "Theme.h"

#include <QApplication>
#include <QFrame>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QStyle>

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        qInfo("PASS: %s", message);
    else {
        qCritical("FAIL: %s", message);
        ++failures;
    }
}

QPushButton *visibleButtonWithText(QWidget &root, const QString &text)
{
    for (QPushButton *button : root.findChildren<QPushButton *>()) {
        if (button->isVisibleTo(&root) && button->text() == text)
            return button;
    }
    return nullptr;
}

} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    app.setStyleSheet(QString::fromLatin1(Theme::styleSheetForDark(true)));

    QWidget host;
    host.resize(1060, 700);
    QPushButton target(QStringLiteral("Agents"), &host);
    target.setGeometry(14, 108, 82, 46);
    target.show();

    forkmesh::ui::OnboardingTour tour(&host);
    int startRequests = 0;
    int welcomeDismissals = 0;
    int reveals = 0;
    int closes = 0;
    bool completed = false;
    QString closedOnStep;

    QVector<forkmesh::ui::OnboardingStep> steps;
    steps.append({QStringLiteral("agents"), QStringLiteral("AI WORKSPACE"),
                  QStringLiteral("Delegate work"),
                  QStringLiteral("Run an agent in an isolated worktree."),
                  {QStringLiteral("Pick a model"),
                   QStringLiteral("Review its diff")},
                  &target, [&reveals] { ++reveals; }});
    steps.append({QStringLiteral("search"), QStringLiteral("CTRL+K"),
                  QStringLiteral("Search everywhere"),
                  QStringLiteral("Find every workspace from one box."),
                  {QStringLiteral("Keyboard friendly")}, nullptr, {}});
    steps.append({QStringLiteral("ready"), QStringLiteral("YOU'RE READY"),
                  QStringLiteral("The mesh is yours"),
                  QStringLiteral("Start exploring."),
                  {QStringLiteral("Replay any time")}, &target, {}});

    tour.startTourRequested = [&] {
        ++startRequests;
        tour.startTour(steps);
    };
    tour.welcomeDismissed = [&] { ++welcomeDismissals; };
    tour.tourClosed = [&](bool done) {
        ++closes;
        completed = done;
        closedOnStep = tour.currentStepId();
    };

    host.show();
    tour.showWelcome(QStringLiteral("bold-otter"));
    QApplication::processEvents();

    auto *card = tour.findChild<QFrame *>(QStringLiteral("onboardingCard"));
    auto *title = tour.findChild<QLabel *>(QStringLiteral("onboardingTitle"));
    check(tour.view() == forkmesh::ui::OnboardingTour::View::Welcome &&
              tour.isVisibleTo(&host),
          "a welcome overlay opens above the real host window");
    check(title && title->text() == QStringLiteral("Your work has a new home.") &&
              tour.findChildren<QFrame *>(
                      QStringLiteral("onboardingFeatureCard")).size() == 3,
          "the welcome card introduces the three product pillars");
    check(card && tour.rect().contains(card->geometry()) &&
              tour.spotlightRect().isEmpty(),
          "the welcome card is centred inside the desktop without a spotlight");
    check(visibleButtonWithText(tour, QStringLiteral("Take the tour  →")) &&
              visibleButtonWithText(tour, QStringLiteral("Explore on my own")),
          "first install offers both the guided tour and a direct exit");

    visibleButtonWithText(tour, QStringLiteral("Take the tour  →"))->click();
    QApplication::processEvents();
    const QPoint targetCenter = tour.mapFromGlobal(
        target.mapToGlobal(target.rect().center()));
    check(startRequests == 1 &&
              tour.view() == forkmesh::ui::OnboardingTour::View::Tour &&
              tour.stepCount() == 3 && tour.currentStepId() == QLatin1String("agents"),
          "Take the tour switches the same overlay into its guided steps");
    check(reveals == 1 && tour.spotlightRect().contains(targetCenter),
          "a step reveals and spotlights the actual control it explains");
    check(card && !card->geometry().intersects(tour.spotlightRect()),
          "the callout is positioned away from the highlighted control");

    QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
    QApplication::sendEvent(&tour, &right);
    QApplication::processEvents();
    check(tour.currentStepId() == QLatin1String("search") &&
              tour.spotlightRect().isEmpty(),
          "Right Arrow advances and a targetless step becomes a centred explainer");

    host.resize(760, 540);
    QApplication::processEvents();
    check(tour.geometry() == host.rect() && card &&
              tour.rect().contains(card->geometry()),
          "the overlay and card stay inside a compact resized desktop");

    QKeyEvent left(QEvent::KeyPress, Qt::Key_Left, Qt::NoModifier);
    QApplication::sendEvent(&tour, &left);
    QApplication::sendEvent(&tour, &right);
    QApplication::sendEvent(&tour, &right);
    QApplication::processEvents();
    check(tour.currentStepId() == QLatin1String("ready") &&
              visibleButtonWithText(tour, QStringLiteral("Start exploring")) &&
              !visibleButtonWithText(tour, QStringLiteral("Skip tour")),
          "the final step has a clear completion action and no skip action");

    visibleButtonWithText(tour, QStringLiteral("Start exploring"))->click();
    check(closes == 1 && completed && closedOnStep == QLatin1String("ready") &&
              !tour.isVisible(),
          "finishing reports completion and preserves the closing step for persistence");

    tour.startTour(steps);
    QApplication::processEvents();
    check(visibleButtonWithText(tour, QStringLiteral("Skip tour")),
          "a replay restores the skip action after the completed final step");
    tour.dismiss();

    tour.showWelcome(QStringLiteral("bold-otter"));
    QApplication::processEvents();
    visibleButtonWithText(tour, QStringLiteral("Explore on my own"))->click();
    check(welcomeDismissals == 1 && !tour.isVisible(),
          "Explore on my own dismisses the one-time welcome explicitly");

    tour.startTour(steps);
    QApplication::processEvents();
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&tour, &escape);
    check(closes == 3 && !completed && closedOnStep == QLatin1String("agents"),
          "Escape skips a replayable tour without marking it complete");

    return failures == 0 ? 0 : 1;
}
