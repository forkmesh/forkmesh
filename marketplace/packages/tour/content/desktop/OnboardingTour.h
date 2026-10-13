#pragma once

#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <functional>

class QFrame;
class QLabel;
class QProgressBar;
class QPushButton;
class QPropertyAnimation;

namespace forkmesh::ui {

// One stop in the desktop tour. reveal() may select the page that gives the
// target context, but must not change user data; target is then spotlighted by
// the overlay. A null/hidden target deliberately produces a centred card, which
// is useful for role-gated and optional features.
struct OnboardingStep {
    QString id;
    QString kicker;
    QString title;
    QString body;
    QStringList details;
    QPointer<QWidget> target;
    std::function<void()> reveal;
};

// A child overlay rather than a modal dialog: the app remains visible beneath
// the tour, so every explanation stays anchored to the actual control it names.
// It intentionally has no Q_OBJECT macro; callbacks keep the reusable widget
// out of the app's hand-maintained moc list.
class OnboardingTour final : public QWidget
{
public:
    enum class View {
        Hidden,
        Welcome,
        Tour,
    };

    explicit OnboardingTour(QWidget *parent = nullptr);
    ~OnboardingTour() override;

    std::function<void()> startTourRequested;
    std::function<void()> welcomeDismissed;
    std::function<void(bool completed)> tourClosed;

    void showWelcome(const QString &nodeName);
    void startTour(QVector<OnboardingStep> steps);
    void nextStep();
    void previousStep();
    void dismiss();

    View view() const { return m_view; }
    int currentStep() const { return m_currentStep; }
    int stepCount() const { return m_steps.size(); }
    QStringList stepIds() const;
    QString currentStepId() const;
    QString currentTitle() const;
    QRect spotlightRect() const;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    void buildCard();
    void setView(View view);
    void renderStep();
    void renderDetails(const QStringList &details);
    void positionCard();
    void watchTarget(QWidget *target);
    void stopWatchingTarget();
    void animateCard();
    void closeOverlay(bool completed, bool fromWelcome);
    QRect mappedTargetRect() const;

    View m_view = View::Hidden;
    QVector<OnboardingStep> m_steps;
    int m_currentStep = -1;
    QPointer<QWidget> m_watchedTarget;

    QFrame *m_card = nullptr;
    QLabel *m_brand = nullptr;
    QLabel *m_kicker = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_body = nullptr;
    QWidget *m_welcomeFeatures = nullptr;
    QLabel *m_details = nullptr;
    QLabel *m_hint = nullptr;
    QLabel *m_progressLabel = nullptr;
    QProgressBar *m_progress = nullptr;
    QPushButton *m_backButton = nullptr;
    QPushButton *m_secondaryButton = nullptr;
    QPushButton *m_primaryButton = nullptr;
    QPushButton *m_closeButton = nullptr;
    QPropertyAnimation *m_cardFade = nullptr;
};

} // namespace forkmesh::ui
