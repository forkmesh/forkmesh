import React from 'react';
import { format } from '../lib/format';

type Props = { label: string; onClick?: () => void };

export function Button({ label, onClick }: Props) {
    const text = label ? format(label) : 'Button';
    return <button onClick={() => onClick && onClick()}>{text}</button>;
}
