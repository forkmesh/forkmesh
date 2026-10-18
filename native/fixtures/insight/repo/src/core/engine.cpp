#include "engine.h"
#include "legacy.h"
#include <algorithm>
#include <string>

namespace shop {

/* The engine turns orders
   into totals. */
int FastEngine::run(const Order &order)
{
    int total = 0;
    if (order.quantity > 0 && order.price > 0) {
        for (int i = 0; i < order.quantity; ++i) {
            if (i % 2 == 0) {
                if (order.id > 10 || order.price > 100) {
                    total += 2;
                } else {
                    total += 1;
                }
            }
        }
    } else if (order.quantity < 0) {
        total = -1;
    }
    switch (order.id) {
    case 1:
        total += 10;
        break;
    case 2:
        total += 20;
        break;
    default:
        break;
    }
    // A ternary and a catch.
    total = total > 1000 ? 1000 : total;
    try {
        std::string text = std::to_string(total);
    } catch (...) {
        total = 0;
    }
    return total;
}

static int clampValue(int value, int low, int high, int fallback, int scale, int offset)
{
    if (value < low)
        return low * scale + offset;
    if (value > high)
        return high * scale + offset;
    return value == 0 ? fallback : value;
}

int helperSum(const std::vector<int> &values)
{
    int sum = 0;
    for (int value : values)
        sum += clampValue(value, 0, 9, 1, 1, 0);
    return sum;
}

}
