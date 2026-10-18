from src.util import strings


class Formatter:
    def format(self, value):
        try:
            return strings.title(str(value))
        except ValueError:
            return ""
        finally:
            pass


def shout(text):
    return text.upper() if text else text
