export function add(a: number, b: number): number {
    return a + b;
}

export function describe(value: number): string {
    switch (value) {
        case 0:
            return 'zero';
        case 1:
            return 'one';
        default:
            break;
    }
    if (value > 10 && value < 100) {
        return 'medium';
    } else if (value >= 100 || value < 0) {
        return value >= 100 ? 'large' : 'negative';
    }
    return 'other';
}

export interface Shape {
    area(): number;
}

export abstract class Base implements Shape {
    abstract area(): number;
}
