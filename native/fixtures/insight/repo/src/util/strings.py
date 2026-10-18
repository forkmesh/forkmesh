"""Helpers for text.

Kept small on purpose.
"""
from . import helpers
from .helpers import Formatter
import re


def slug(text, separator="-"):
    """Lower-case a title."""
    text = text.strip().lower()
    if not text:
        return ""
    words = [w for w in re.split(r"\W+", text) if w]
    return separator.join(words)


def title(text):
    # Upper-case each word.
    parts = []
    for word in text.split():
        if word and word[0].isalpha() and len(word) > 1:
            parts.append(word[0].upper() + word[1:])
        elif word:
            parts.append(word)
    return " ".join(parts)


class Slugger:
    def __init__(self, separator):
        self.separator = separator

    def __call__(self, text):
        return slug(text, self.separator) if text else ""
