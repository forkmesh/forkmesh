import { add, describe } from './lib/math.js';
import { Button } from './components/Button';
import React from 'react';
import type { Props } from '@/types';

export function start(count: number): string {
    const total = add(count, 2);
    return total > 3 ? describe(total) : 'small';
}

export const render = () => React.createElement(Button, { label: start(1) });
