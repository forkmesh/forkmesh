#pragma once
#include "engine.h"

namespace shop {

struct Order {
    int id = 0;
    int quantity = 0;
    double price = 0;
};

enum class State { Open, Closed };

}
