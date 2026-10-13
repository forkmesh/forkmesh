#include "ComponentHealthBoard.h"

#include <QApplication>
#include <QCoreApplication>
#include <QMouseEvent>
#include <QVector>

#include <cstdio>

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

HealthCard makeCard(const QString &id, const QString &title,
                    const QString &subtitle, int health)
{
    HealthCard card;
    card.id = id;
    card.title = title;
    card.subtitle = subtitle;
    card.loc = 1000;
    card.health = health;
    card.drillable = true;
    return card;
}

// A hidden widget never receives its QResizeEvent, so the board would keep
// laying out at the old width. The real page shows the board inside a scroll
// area, so the test shows it too and lets the event be delivered.
void resizeAndSettle(ComponentHealthBoard *board, int width, int height)
{
    board->resize(width, height);
    QCoreApplication::processEvents();
}

void click(ComponentHealthBoard *board, const QPoint &pos)
{
    QMouseEvent press(QEvent::MouseButtonPress, pos, board->mapToGlobal(pos),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(board, &press);
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    ComponentHealthBoard board;
    board.show();
    resizeAndSettle(&board, 600, 400); // ~4 columns at 132 + 8 gap

    QVector<HealthCard> cards;
    cards << makeCard(QStringLiteral("desktop/src/A.cpp"),
                      QStringLiteral("A.cpp"), QStringLiteral("Core"), 30)
          << makeCard(QStringLiteral("desktop/src/B.cpp"),
                      QStringLiteral("B.cpp"), QStringLiteral("UI · Chat"), 60)
          << makeCard(QStringLiteral("desktop/src/C.cpp"),
                      QStringLiteral("C.cpp"), QStringLiteral("Pulls"), 95);
    board.setCards(cards);

    check(board.visibleCount() == 3, "every card is visible with no filter");
    const int oneRow = board.contentHeight();
    check(oneRow > 0, "a populated board has height");

    // Narrower than two cards: one column, so the same cards stack taller.
    resizeAndSettle(&board, 160, 400);
    check(board.contentHeight() > oneRow,
          "a one-column board is taller than a four-column one");
    resizeAndSettle(&board, 600, 400);
    check(board.contentHeight() == oneRow,
          "returning to the original width restores the original height");

    // Filtering matches title, subtitle and id.
    board.setFilter(QStringLiteral("B.cpp"));
    check(board.visibleCount() == 1, "a title filter selects one card");
    board.setFilter(QStringLiteral("Pulls"));
    check(board.visibleCount() == 1, "a subtitle filter selects one card");
    board.setFilter(QStringLiteral("desktop/src/"));
    check(board.visibleCount() == 3, "a path filter matches every card by id");
    board.setFilter(QStringLiteral("nothing-matches-this"));
    check(board.visibleCount() == 0, "an unmatched filter hides every card");
    board.setFilter(QString());
    check(board.visibleCount() == 3, "clearing the filter restores every card");

    // Activation reports the card's id, and only for a real hit.
    QString activated;
    int activations = 0;
    board.setOnActivate([&](const QString &id) {
        activated = id;
        ++activations;
    });
    click(&board, QPoint(8 + 10, 8 + 10)); // inside the first card
    check(activations == 1, "clicking a card activates it once");
    check(activated == QLatin1String("desktop/src/A.cpp"),
          "activation carries the card's id, not its title");

    click(&board, QPoint(4, 4)); // in the gutter, between cards
    check(activations == 1, "clicking the gap activates nothing");

    // A filtered-out card cannot be activated by clicking where it used to be.
    board.setFilter(QStringLiteral("C.cpp"));
    activated.clear();
    click(&board, QPoint(8 + 10, 8 + 10));
    check(activated == QLatin1String("desktop/src/C.cpp"),
          "after filtering, the first slot activates the card now shown there");

    // An empty board must not crash when painted or clicked.
    board.setFilter(QString());
    board.setCards({});
    check(board.visibleCount() == 0, "an empty board shows no cards");
    click(&board, QPoint(20, 20));
    board.repaint();

    if (failures == 0)
        std::fprintf(stdout, "test_component_health_board: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
