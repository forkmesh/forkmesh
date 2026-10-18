#pragma once
#include "types.h"
#include <vector>

namespace shop {

class Engine {
public:
    virtual ~Engine() = default;
    virtual int run(const Order &order) = 0;
    virtual void stop() = 0;
};

class FastEngine : public Engine {
public:
    int run(const Order &order) override;
    void stop() override {}
};

}
