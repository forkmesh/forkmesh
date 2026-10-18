import os


def shared_block(values):
    total = 0
    for value in values:
        if value > 10:
            total += value * 2
        elif value > 5:
            total += value
        else:
            total -= 1
    return total


def only_second():
    return os.sep
