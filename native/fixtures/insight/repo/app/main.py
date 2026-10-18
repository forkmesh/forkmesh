import os
import sys
from src.util import strings
from src.util.helpers import shout


def main(argv):
    name = argv[1] if len(argv) > 1 else os.environ.get("USER", "")
    while name and name.endswith("!"):
        name = name[:-1]
    print(shout(strings.slug(name)))


if __name__ == "__main__":
    main(sys.argv)
