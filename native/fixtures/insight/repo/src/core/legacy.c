#include "legacy.h"
#include <stdio.h>

int legacy_add(int a, int b)
{
    while (a > 0) {
        a--;
        b++;
    }
    return b;
}
