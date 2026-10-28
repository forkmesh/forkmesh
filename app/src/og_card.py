"""Social-preview (OpenGraph) card renderer for repo and referral pages.

Pure (builtin-only, no ``js``/``workers`` runtime, no ``env``/DB access): a
Mastodon/Slack/Twitter unfurl of a repo URL used to show only the ForkMesh
logo (issue adhoc #46); now it gets a 1200x630 PNG card drawn as a terminal
window - the repository shown by name, its description, its live counts and
the clone command - with the repository's or instance's mark in the corner.

Everything is rendered in-process with the stdlib (Pyodide workers have no
Pillow): pixels go into a flat RGB bytearray, text comes from an embedded
classic 5x7 bitmap font scaled up per use, the logo is a pre-scaled RGBA
blob baked in at authoring time (tools/embed_og_logo.py regenerates it from
www/public/assets/logo.png), and the PNG is assembled by hand with zlib. Drawing
sticks to bytearray row-slice writes so a card costs only a few thousand
Python-level ops - cheap enough for a worker request, and each colo renders
a given repo's card at most once per edge-cache TTL anyway. The repository
card's monospace text comes from glyph atlases baked in the same way
(tools/embed_og_mono_font.py, Geist Mono under the SIL OFL 1.1).

The referral and hackathon cards' colors mirror the dashboard's dark theme
variables in public/dashboard/repo.html.
"""

import base64
import json
import re
import struct
import unicodedata
import zlib
from datetime import datetime, timezone

CARD_W = 1200
CARD_H = 630
CARD_REVISION = "4"

_BG = (1, 4, 9)            # --background
_CARD = (13, 17, 23)       # --card
_BORDER = (48, 54, 61)     # --dashboard-border-rgb
_FG = (240, 246, 252)      # --foreground
_TEXT = (201, 209, 217)    # --dashboard-secondary-foreground-rgb
_MUTED = (139, 148, 158)   # --dashboard-muted-foreground-rgb
_ACCENT = (88, 166, 255)   # --dashboard-accent-rgb

# Classic 5x7 ASCII font (chars 32..126), 5 column bytes per glyph, bit 0 =
# top row. The old Adafruit/HD44780 table - public domain and legible even at
# small scales.
_FONT = bytes.fromhex(
    "0000000000"  # ' '
    "00005f0000"  # '!'
    "0007000700"  # '"'
    "147f147f14"  # '#'
    "242a7f2a12"  # '$'
    "2313086462"  # '%'
    "3649552250"  # '&'
    "0005030000"  # '''
    "001c224100"  # '('
    "0041221c00"  # ')'
    "082a1c2a08"  # '*'
    "08083e0808"  # '+'
    "0050300000"  # ','
    "0808080808"  # '-'
    "0060600000"  # '.'
    "2010080402"  # '/'
    "3e5149453e"  # '0'
    "00427f4000"  # '1'
    "4261514946"  # '2'
    "2141454b31"  # '3'
    "1814127f10"  # '4'
    "2745454539"  # '5'
    "3c4a494930"  # '6'
    "0171090503"  # '7'
    "3649494936"  # '8'
    "064949291e"  # '9'
    "0036360000"  # ':'
    "0056360000"  # ';'
    "0008142241"  # '<'
    "1414141414"  # '='
    "4122140800"  # '>'
    "0201510906"  # '?'
    "324979413e"  # '@'
    "7e1111117e"  # 'A'
    "7f49494936"  # 'B'
    "3e41414122"  # 'C'
    "7f4141221c"  # 'D'
    "7f49494941"  # 'E'
    "7f09090901"  # 'F'
    "3e4149497a"  # 'G'
    "7f0808087f"  # 'H'
    "00417f4100"  # 'I'
    "2040413f01"  # 'J'
    "7f08142241"  # 'K'
    "7f40404040"  # 'L'
    "7f020c027f"  # 'M'
    "7f0408107f"  # 'N'
    "3e4141413e"  # 'O'
    "7f09090906"  # 'P'
    "3e4151215e"  # 'Q'
    "7f09192946"  # 'R'
    "4649494931"  # 'S'
    "01017f0101"  # 'T'
    "3f4040403f"  # 'U'
    "1f2040201f"  # 'V'
    "3f4038403f"  # 'W'
    "6314081463"  # 'X'
    "0708700807"  # 'Y'
    "6151494543"  # 'Z'
    "007f414100"  # '['
    "0204081020"  # '\'
    "0041417f00"  # ']'
    "0402010204"  # '^'
    "4040404040"  # '_'
    "0001020400"  # '`'
    "2054545478"  # 'a'
    "7f48444438"  # 'b'
    "3844444420"  # 'c'
    "384444487f"  # 'd'
    "3854545418"  # 'e'
    "087e090102"  # 'f'
    "0c5252523e"  # 'g'
    "7f08040478"  # 'h'
    "00447d4000"  # 'i'
    "2040443d00"  # 'j'
    "7f10284400"  # 'k'
    "00417f4000"  # 'l'
    "7c04180478"  # 'm'
    "7c08040478"  # 'n'
    "3844444438"  # 'o'
    "7c14141408"  # 'p'
    "081414187c"  # 'q'
    "7c08040408"  # 'r'
    "4854545420"  # 's'
    "043f444020"  # 't'
    "3c4040207c"  # 'u'
    "1c2040201c"  # 'v'
    "3c4030403c"  # 'w'
    "4428102844"  # 'x'
    "0c5050503c"  # 'y'
    "4464544c44"  # 'z'
    "0008364100"  # '{'
    "00007f0000"  # '|'
    "0041360800"  # '}'
    "0201020402"  # '~'
)

# ForkMesh mark, pre-scaled to 98x112 raw RGBA (zlib+base64). Regenerate with
# tools/embed_og_logo.py whenever www/public/assets/logo.png changes.
_LOGO_W = 98
_LOGO_H = 112
_LOGO_B64 = (
    "eNrtXXd4VUUWR9dd/db1j1XB0AIhlNAhCT0xBAIohg7SsaxKCdVFIHRIEEIHI70EJaJIE3GVkgAW"
    "MDRpIlJDCEV6F0E4O7+bd+PNfTNz72t5hXe+7/wD793cd34zc/qZAgX85Cc/+QIR0WOMCzMuyvhx"
    "v0TyXf4FGc9gfIH+omuMFzEO9EvI5fJ/nPHbjC+SmG4yHsT4H36JuQSDSMZ7yDwdZdyCsV94zpF/"
    "VcZrGT/UC/qPP/6gb75ZT19+uY5u3bolwmMrMPRL0m75B1jO+wd6wT58+JC++up/FBZWk557rpDC"
    "5ctXotTUT+jBgwc8LIDhGsbBfsnapAP6Mr7OE+ipU6eoY8fOufLXc9Omzejnnw+J9sZdxqMZP+mX"
    "tBSDKMY/8QSIc2fcuPFUpEhxIQYqFypUmN57bxBdvnxZhMcxxi39usNK/iUZL+fpAJxBy5d/TpUq"
    "VTGUv56Dg8vS3Lnz6f79+yI8NjGu5Jc/Pc04gfEdnpB2795NTZq8bCDvgoZ41K0bQenpm0VYAKRk"
    "xs89gvIHt2F8mieY3367QHFxven5518QyjY8vCaNHZtAy5Z9Sp99tpwmTEiiyMj6Ujw6depCmZmZ"
    "IjwuW3yTxx4RDOoyTucJ4vbt2zR9+kwqVaq0UJYhIRVp8eIU+vPPP4l3hq1atZpCQ8OF3y9WLJDG"
    "jEmgK1euiPDYzfhlX9UdFh2wgqcDYGtiTVesKNYB0M8JCYl08+ZNQwcOfsUHHyRTyZLBwueVKlWG"
    "5syZS/fu3RM9Jo1xZR/TAYkiHbBr1y5q3Pgl6XnSrdvrdPJk3vMEeGRk7FB8uLVrv6Rt27bT1atX"
    "rc63vn37U8GCAcJn16lTj9LS0mS6Y5Y36w5LLLQr42y+DviNevaMk+qAiIgo2rr12zzfO3DgAHXp"
    "0o1eeKGI1ech71at2iqYaGnv3r2KXyHDGj7JyZMnZboDPs0TXoZBecZcAwU25MKFixSbUmZv4jN6"
    "e/OTT5Zx5c/jadNmcHWHzP4tVqwETZkylX7//XcRHohv1fYC+f/bEo/mHrpY21jjIjlgb/TqFafs"
    "FT2dP3+eChcuZtp3wLMOHjxo9Zzr16/T0KHDpXhWrx5GX3yxVsGOQwicfISchwfK/2+Me4ri0djv"
    "OONlcoOO2LVrt1D3fv/9Dzb7cfD/RPTrr0eoXbv20u83b95SOQcl8fVhjJ/yEB3QjjE3oIM1PHhw"
    "vHQd16kToaw9QXwuj01bvXq4aX+udOmy3H2lP6u2bNkqtROgc3r06EUnTgh1R5bF73jCTRhUYLxF"
    "pAOSk2dR8eIlhb8PchL5ASLCvoqObmi4D8LDa9G+fftNPxd4rF69humOqsJnBgQUpcTEcXTnzh3R"
    "YxAXq5PPOmCmSAdgfdWr96L03I6L60MXLlwgewi4rVnzBb3++ptWz27b9lUl9g3/wR66fv0GDRs2"
    "Qqo7qlULVf6+QHfgH5e6UndYdEAc40u8F8C+hT0pW6dNmjSl3bv3kLNIb/OePXvWKc89cuQIvfpq"
    "R+lviY1tQfv3C/ccklLDna072PMawnTn/cEbN24qcQKZDsB+h87EGsKZ9fXX39CAAf+ll15qqvhR"
    "UVHR1LlzV5o6dTrD84TbcVBp48aNVKtWXanuGDDgXbp4UZguP2nRn47KvwTj1aJ49MqVq5Q8mOg9"
    "ixYtTu+/Pz43Z5mRkUE1atSWrjP8NtiVklh1vuEAQtxj1qzZFBQkjnnh/+DvSGwN6NGKdsj/ScaD"
    "LbaZFR06dIj5rW0M/NMudOpUVu53fvnlMMMl0LTNGR8/zCNwUAlrvlev3tJ3btCgEf34Y4Yw7MU4"
    "ifEzJjFobtlP3JwkdCzyXaJ3adiwMaWlpVvpMaxxW2x/2CeSGBwXh8mTpb6wU2jnzl2Ga7Br19dk"
    "uVkY02/Kzir2f2/wzqAbN27Q6NFjpDqgcuVq9PnnK0R2BM2cmSy19599Vv+8qsJniXBQbdZNmzaR"
    "q2nTpjSqXbue9Hzt12+AzC4cIcAgEO6SPh69dGkq0wEVJTogkMaPn6D4WUZ+WKNGTThY8PNp0NuS"
    "WhghDiq3b9+Jjh496lIssF9nz56jxNBF74HYe3Lyh7y9DccpnINDUh4nMSuLmjaNNcxv4XNmCWdG"
    "UtJEKlu2vKmzSWtnmcEBcSH92TZ8+Ei6du2aS/G4dOmSkj+U/ZbIyCh2Vv2s/2oqB4cd2g9ER8dI"
    "nws7yOjcEAb1mS0EfQZ/GnFR2COIbRw4cJD69OlnJV+R36H/XFbWaWXt6f144J6S8pFNvrs9lJKy"
    "xHBd6XzMcxwcsrR+pdFaldjMDhMwwvrh+eHnzp03tJfOnDmrxIT0/x8V1YB27NjpsvfGujSq64Hd"
    "qPO/9Tjk5uyR1zKKpWENu5Jy8hSLqUyZkDzvEBgYpJzJWNtGditkrtdJqu91+nS2ks+DbfHxx0uV"
    "WAVsHKN4o4zwDKP1e/Cg1dlkBw55+e23uyu2rCvp2rXr7AycQCVKBFnFafXvw/MfIFfIp3btuqZ+"
    "U4UKlWns2ESrHKuMLly4SAMHvifNv7oSBzBs2dGjxyq2rSsJueZ+/eS5Zpkfh/01cOAg078LMXbE"
    "7WV09+5dmjFjJlsjpUw/11U4mKzzdRrt3buPY/+a86fhY8p+g96HGTRoiFAP6Ouc8wuHevUiqWrV"
    "UMO/07Ch1Ld3CkE3LFq02GYcZPkFUZ0IP6bT1vC7sNFiYho7HYdJk6YoOZBJkyYzu7CE4XvA/pTU"
    "+bol7g1/0xYc0FuhrXMeOXKUNKYDRu5i2LDhis+SmrrMJTiodObMGerevYfpOl+jOFF+4YB9FB8/"
    "VOqHqz4w6jVVPW+2zhk5C+QutPUlrsRBpW+//c6UHQLbJj093e04aGO/CQnjKDa2uRIXg20cGlqD"
    "OnToRPPnL8i1lczVOef4Z6hl01N+4aDGWJYs+Uj5PUbv26ZNO+W3eXo+DrUcb7zxH8N9U7p0OSV+"
    "KcpZ5ycO2tgR4s6y2gCV33rrHbtlBvsTtbC8nqDmzVsx/Z1id+z7ypWrSv+KkQ5QbSmjuJU7cFAJ"
    "8kVMweh3BAUFG9X5WtHRo8ekNQjaXL4t+w464NNPP5PGlfPWWr1sKrbmThxU+u6776XxeZXxGcTy"
    "jQg+k7x+ydpGQBzUTF7HqM5Zz8gHmyFPwEHVHVjzsvi8ts732LFjwmehdtjWPqAVK1ZK9u05wzpn"
    "vk6uYnoPewoO2vg84i+FCgUY5kJHjBjFjZEgLmfm3NYyL1aOfTV16jQKDCxpsy8MnjJlmunf7Wk4"
    "qIR3aNGileFvhe3Fq/NFjNfs+h0xYqTV30eds9l4n6gXxsxZ5+k4qLGZDRs2UkSEsb5FfRN8FC2h"
    "9gY50yJFinH7dZHDR91R3pjUXuZjdbBb/iqjXsMW8mQctPbn3LnzpH0QslwzbFPE/IDp+vXrFftI"
    "n89WbTdbdYCId+7c6XM4qIQYlBmbPSCgCA0fPsJUrtkWX8YsI5dnax7Ym3BQCbmwli1bm4pjwtYX"
    "yQSxuWrVwpwmf5URs7OVvBEHVXesW/eVqfh6TEwT+vDD2cq5BP9j/vyFpnCETWYrBuXKlWd77O4j"
    "g4NK6AVFjs+WPiwjRh6vf3/UBF+i7t172vRd1KbYQ96Og0rHj58wrOU3w4iravtS4EM0a9bCdH3n"
    "uXPnvBKH/fsPOLWu5ocfttErrzSzWf4vvlhfqfHn6RLoezO9Rb1797X7vd2NA/wtUe+tI7rDbE7Y"
    "bK4cdRbIO8jrjX7xahxkveiOEM6UpKRJXN0BHYB8my21I8ifiXyY1q3bOvSunoSDbDaDI4T1rv8b"
    "EydOtutZ6Anm4arNT/sKDmqtE2q0zMwqMSLU6umfj3oxewk5aH39vqM1QO7GAfFmWSwBNXPok7W3"
    "TtkVOIBGjRqT+6w5c+Y5vFY8wW6FjVO3bqRhvtpePegKHGBTtGvXQanPc0Z9oqf4D/hdOMeRp5TN"
    "NEStk7aXzl045PiQt6TzIFSb1xv9ONg4mDMmqwOF7kD/L/qA3YmDGUKNhjf706jbhV8k0x2w/ZH/"
    "NdId7sIhO/uMUifgC3GNPXt+UupgZboD+RzUknoaDrD30J/rS3mgBQsWKrVY8nm4g7l1su7AAXkN"
    "vK8v4aAS5kYOGRIvzQPB3503L+88XHfggN4htV/dV+OtqDmFHSufhxtJmzdvdhsOao2DL+OgxvIw"
    "iwBzT2R4oE4AfTj5iQN6blX7wtdxUAn18UazFHnsShy0szW8BQfUUTijBwUzuT0BB/TmaeOA3oID"
    "4nzI6TraL4c6S1fggBo1W2jixElWc2W8Ld7qSL+cq3DAeaevTxMR+v/1M6a8EQdtr3V2drZH4IDa"
    "VsxyP3z4sOFn0WvBm7PkzfkHzAvGHpfMfMw3HHL6JMKks0Vhv/F6fb0dB20/Gd7RSHe4GoecOr1o"
    "pQ+IR+jhEM0dcxcO48cnOQ2Hv35PDG3f/qNbcci52ymW28vVvn1Hh3BYsuRjp+OAPcyZH+QQDlrd"
    "cfr0abfhoPbsafcn/rYoLmwGh8zMU1S/fgN7cMjS5jlEcTfMqDDyDWzFQb2nBDOybt++4xYccmbk"
    "T8/9DOZhy+Yhigg154mJ7wvn/+hiyLy5Pz9p9ZNsvmdwcBmlZl5Um2QPDipXqVJdqVNyBw6ou1m/"
    "foNyHmC+kC04mLnTBfayzka5yJmDNVP7AcwGq1lTPmsV9XC4z8SZOGj75TAXKT9xAKNWH/drGc0H"
    "1dLhw79Ss2Ytpd8BPtD7OlotuDvjnj7ejpog2Juyed09e/bK0w/tDBzsYWfgYM6+aJSrR1G3JpsH"
    "jv/DzFTcO6E3ixlHC2ZW9ibO3FDU2SIfI6tZB1bqPVSoh/dlHLC24QfJ4pBYn++804OOHz8ueo0J"
    "BnN025NgljrqrxFzlr0jenSNZkd7Ow5GjJroffv2CVU54z5kYuY3+8yzjD+gnPuirGjz5i3KPCZH"
    "7zn0NRzQPyOpj8M/pjIuZsec7zDGGaL8Mu5oc2YPmrfiAB2Au+0k8RoUyzVwwt0zrzHmDibJmcXU"
    "00dxMN7T6AfWzlzSEQImAxj/3Yl3QDzDGA1K3GEumMHJm4Pmq/uhVq06Sm+eKKXIeA7jgi68F6UU"
    "41UkuJ8Ssa0KFSo5Za15Ig646wF2uWTGBu56qJqP9wRFMN4u8u0RF5T5Hd62H6ADhgwZKovp4LK6"
    "WHLD/bAW3dENaoL3Ys7u53cXDpgXJ5mdj4t7+5MH3HnJ3uFfjMeJdAfmIYhmrroDB7M2HmaibNwo"
    "vEsCNj2Gaz/vgfcqBjFeKdIdmAGJHhR34oD+XaPnwBfFDHGJDsDQxypecM9oNAnuWERPFmxteY96"
    "QavZwo7igNp/3HEg2wuIt7777kCl111AGGbehrzojnDKuespnjG3sQYxSjOzmGyxq3g4qLMijPpz"
    "c2LHwpl+GOowiUzeseSheBRmnEI599RayQj3eMvusXdkP5iZnRISUkGJrwvu6MD5imKnMj50L3t1"
    "xut4ugNnBuq5zc6LNMIhMzNTubNDNhsf8zIxI1vSf4RkQVQBHyX22+qj9FKcUxxn93wTzLDGXRGy"
    "+DywwaxAyXw3BKpbe5MOcPCu0n4i3YF5wkZ3tNmXv4mRzXWFDhgDvVbgESOL7lgs0h3Qr2FhNQzu"
    "bDDW4yEhFZX71wT1UjgncaCVLvCIE5NBDcbbRPbm9OkzpHl7cQ9qUaU3XdILjVhEowJ+0sdIOmvr"
    "qPLmZs8rutdsjKRTp66ynORlSw74Cb/khXg8jVJ3Etwzjtm4yNOL5F+zZp3c3i3BGbQA+Ua/pE3j"
    "UY7xMp7ugK2fM6cgLM8MvVmz5ijnmED+XxPnHkk/mcYj3GLLc3UHfIbly1fIZtjg/vIYvySdggW4"
    "o7b20wTBQYiDjeyXoNPx+KfFxpc1UNy31Jr4dYDr8Shria/f0+WENzAO9Uso3/F4xuJ71PKvfz/5"
    "yXfo/5R1re0="
)


def _logo_rgba():
    return zlib.decompress(base64.b64decode(_LOGO_B64))


LOGO_BOX = 112
MAX_PNG_PIXELS = 1024 * 1024
MAX_LOGO_PIXELS = 512 * 512
_PNG_CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}
_logo_memo = {}


def _unfilter(kind, line, prev, bpp):
    if kind == 0:
        return line
    if kind == 2:
        return bytearray((a + b) & 255 for a, b in zip(line, prev))
    out = bytearray(line)
    n = len(out)
    if kind == 1:
        for i in range(bpp, n):
            out[i] = (out[i] + out[i - bpp]) & 255
    elif kind == 3:
        for i in range(n):
            left = out[i - bpp] if i >= bpp else 0
            out[i] = (out[i] + ((left + prev[i]) >> 1)) & 255
    elif kind == 4:
        for i in range(n):
            a = out[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            p = a + b - c
            pa = abs(p - a)
            pb = abs(p - b)
            pc = abs(p - c)
            pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
            out[i] = (out[i] + pred) & 255
    else:
        raise ValueError("bad PNG filter")
    return out


def decode_png(data, max_pixels=MAX_PNG_PIXELS):
    data = bytes(data or b"")
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        return None
    pos = 8
    header = None
    palette = b""
    alpha = b""
    chunks = []
    while pos + 8 <= len(data):
        (size,) = struct.unpack(">I", data[pos:pos + 4])
        tag = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + size]
        pos += 12 + size
        if tag == b"IHDR" and size == 13:
            header = struct.unpack(">IIBBBBB", body)
        elif tag == b"PLTE":
            palette = body
        elif tag == b"tRNS":
            alpha = body
        elif tag == b"IDAT":
            chunks.append(body)
        elif tag == b"IEND":
            break
    if not header:
        return None
    width, height, depth, color, _, _, interlace = header
    channels = _PNG_CHANNELS.get(color)
    if (depth != 8 or interlace or not channels or width < 1 or height < 1
            or width * height > max_pixels):
        return None
    stride = width * channels
    try:
        raw = zlib.decompressobj().decompress(
            b"".join(chunks), height * (stride + 1))
        if len(raw) < height * (stride + 1):
            return None
        rows = []
        prev = bytearray(stride)
        for y in range(height):
            start = y * (stride + 1)
            prev = _unfilter(raw[start], raw[start + 1:start + 1 + stride],
                             prev, channels)
            rows.append(bytes(prev))
    except (zlib.error, ValueError, IndexError):
        return None
    pixels = b"".join(rows)
    if color == 6:
        return width, height, pixels
    out = bytearray(width * height * 4)
    if color == 2:
        out[0::4] = pixels[0::3]
        out[1::4] = pixels[1::3]
        out[2::4] = pixels[2::3]
        out[3::4] = b"\xff" * (width * height)
    elif color == 0:
        for k in range(3):
            out[k::4] = pixels
        out[3::4] = b"\xff" * (width * height)
    elif color == 4:
        for k in range(3):
            out[k::4] = pixels[0::2]
        out[3::4] = pixels[1::2]
    else:  # palette
        if not palette:
            return None
        table = []
        for index in range(256):
            rgb = palette[index * 3:index * 3 + 3]
            if len(rgb) < 3:
                rgb = b"\x00\x00\x00"
            table.append(rgb + bytes((alpha[index] if index < len(alpha)
                                      else 255,)))
        out = bytearray(b"".join(table[index] for index in pixels))
    return width, height, bytes(out)


def scale_logo(width, height, rgba, box=LOGO_BOX):
    factor = min(box / width, box / height, 1.0)
    out_w = max(1, int(round(width * factor)))
    out_h = max(1, int(round(height * factor)))
    if (out_w, out_h) == (width, height):
        return width, height, bytes(rgba)
    out = bytearray(out_w * out_h * 4)
    taps = (1 / 6, 1 / 2, 5 / 6)
    for oy in range(out_h):
        ys = [min(height - 1, int((oy + t) * height / out_h)) for t in taps]
        for ox in range(out_w):
            xs = [min(width - 1, int((ox + t) * width / out_w)) for t in taps]
            r = g = b = a = 0
            for sy in ys:
                base = sy * width
                for sx in xs:
                    i = (base + sx) * 4
                    alpha = rgba[i + 3]
                    r += rgba[i] * alpha
                    g += rgba[i + 1] * alpha
                    b += rgba[i + 2] * alpha
                    a += alpha
            o = (oy * out_w + ox) * 4
            if a:
                out[o] = r // a
                out[o + 1] = g // a
                out[o + 2] = b // a
                out[o + 3] = a // 9
    return out_w, out_h, bytes(out)


def card_logo(data):
    data = bytes(data or b"")
    key = zlib.crc32(data), len(data)
    if key in _logo_memo:
        return _logo_memo[key]
    decoded = decode_png(data, MAX_LOGO_PIXELS)
    logo = scale_logo(*decoded) if decoded else None
    if len(_logo_memo) > 16:
        _logo_memo.clear()
    _logo_memo[key] = logo
    return logo


class _Canvas:
    """Flat RGB byte canvas with just the ops the card needs."""

    def __init__(self, w, h, color):
        self.w = w
        self.h = h
        self.buf = bytearray(bytes(color) * (w * h))

    def fill_rect(self, x, y, w, h, color):
        x0 = max(0, x)
        y0 = max(0, y)
        x1 = min(self.w, x + w)
        y1 = min(self.h, y + h)
        if x1 <= x0 or y1 <= y0:
            return
        row = bytes(color) * (x1 - x0)
        stride = self.w * 3
        for yy in range(y0, y1):
            off = yy * stride + x0 * 3
            self.buf[off:off + len(row)] = row

    def frame_rect(self, x, y, w, h, thickness, color):
        self.fill_rect(x, y, w, thickness, color)
        self.fill_rect(x, y + h - thickness, w, thickness, color)
        self.fill_rect(x, y, thickness, h, color)
        self.fill_rect(x + w - thickness, y, thickness, h, color)

    def text(self, x, y, s, scale, color):
        """Draw s at (x, y) top-left, each font pixel a scale x scale block."""
        pen = x
        for ch in s:
            code = ord(ch)
            if not 32 <= code <= 126:
                code = 63  # '?'
            base = (code - 32) * 5
            for col in range(5):
                bits = _FONT[base + col]
                if not bits:
                    continue
                for row in range(7):
                    if bits & (1 << row):
                        self.fill_rect(pen + col * scale, y + row * scale,
                                       scale, scale, color)
            pen += 6 * scale
        return pen

    def blit_rgba(self, x, y, w, h, rgba):
        """Alpha-blend a raw RGBA blob onto the canvas."""
        stride = self.w * 3
        buf = self.buf
        for yy in range(h):
            ty = y + yy
            if ty < 0 or ty >= self.h:
                continue
            src = yy * w * 4
            dst = ty * stride + x * 3
            for xx in range(w):
                a = rgba[src + 3]
                if a == 255:
                    buf[dst:dst + 3] = rgba[src:src + 3]
                elif a:
                    inv = 255 - a
                    buf[dst] = (rgba[src] * a + buf[dst] * inv) // 255
                    buf[dst + 1] = (rgba[src + 1] * a + buf[dst + 1] * inv) // 255
                    buf[dst + 2] = (rgba[src + 2] * a + buf[dst + 2] * inv) // 255
                src += 4
                dst += 3

    def png(self):
        stride = self.w * 3
        raw = bytearray()
        for yy in range(self.h):
            raw.append(0)  # filter: None
            raw += self.buf[yy * stride:(yy + 1) * stride]
        out = bytearray(b"\x89PNG\r\n\x1a\n")
        for tag, body in (
                (b"IHDR", struct.pack(">IIBBBBB", self.w, self.h, 8, 2, 0, 0, 0)),
                (b"IDAT", zlib.compress(bytes(raw), 6)),
                (b"IEND", b"")):
            out += struct.pack(">I", len(body)) + tag + body
            out += struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)
        return bytes(out)


def text_width(s, scale):
    return max(0, len(s) * 6 * scale - scale)


def _fit(s, scale, max_w):
    """Truncate s with an ellipsis so it fits max_w at scale."""
    if text_width(s, scale) <= max_w:
        return s
    while s and text_width(s + "...", scale) > max_w:
        s = s[:-1]
    return s + "..."


def _wrap(s, scale, max_w, max_lines):
    words = s.split()
    lines = []
    cur = ""
    for word in words:
        cand = (cur + " " + word) if cur else word
        if cur and text_width(cand, scale) > max_w:
            lines.append(cur)
            cur = word
        else:
            cur = cand
    if cur:
        lines.append(cur)
    if len(lines) > max_lines:
        lines = lines[:max_lines]
        lines[-1] += "..."
    return [_fit(line, scale, max_w) for line in lines]


def format_count(value):
    """Catalog counts arrive as strings ('', '12'); render compactly."""
    try:
        n = int(str(value).strip() or 0)
    except (TypeError, ValueError):
        n = 0
    if n >= 1000000:
        text = "%.1fM" % (n / 1000000.0)
    elif n >= 1000:
        text = "%.1fk" % (n / 1000.0)
    else:
        return str(n)
    return text.replace(".0", "")


def format_exact(value):
    """Exact, grouped count - the referral card's whole point is the number,
    so 1,284 clicks must not collapse to the same '1.3k' as 1,285."""
    try:
        n = int(str(value).strip() or 0)
    except (TypeError, ValueError):
        n = 0
    return "{:,}".format(n)


def format_size(size_bytes):
    try:
        n = int(size_bytes or 0)
    except (TypeError, ValueError):
        n = 0
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if n < 1024 or unit == "TB":
            if unit == "B":
                return "%d %s" % (n, unit)
            return ("%.1f %s" % (n, unit)).replace(".0 ", " ")
        n /= 1024.0
    return "0 B"


def _ts_seconds(value):
    """Catalog updatedAt may be epoch ms, epoch s, or an ISO string."""
    text = str(value or "").strip()
    if not text:
        return None
    try:
        n = int(float(text))
        if n > 10 ** 11:  # milliseconds
            return n // 1000
        if n > 10 ** 8:
            return n
        return None
    except ValueError:
        pass
    try:
        stamp = datetime.fromisoformat(text.replace("Z", "+00:00"))
        if stamp.tzinfo is None:
            stamp = stamp.replace(tzinfo=timezone.utc)
        return int(stamp.timestamp())
    except ValueError:
        return None


def format_age(value, now_s):
    ts = _ts_seconds(value)
    if ts is None:
        return "-"
    delta = max(0, int(now_s) - ts)
    if delta < 60:
        return "now"
    if delta < 3600:
        return "%dm ago" % (delta // 60)
    if delta < 86400:
        return "%dh ago" % (delta // 3600)
    if delta < 30 * 86400:
        return "%dd ago" % (delta // 86400)
    if delta < 365 * 86400:
        return "%dmo ago" % (delta // (30 * 86400))
    return "%dy ago" % (delta // (365 * 86400))


MONO_GLYPHS = "".join(chr(c) for c in range(32, 127)) + "\u2014\u2026\u2588\u00b7"
_MONO_INDEX = {ch: index for index, ch in enumerate(MONO_GLYPHS)}

# --- generated by tools/embed_og_mono_font.py (do not edit) ---
_MONO_FONTS = {
    # GeistMono-Regular 20px
    "r20": (12, 27, 21, (
        "eNrtnXlclNX6wJ8BARdSccEFFHOlMLXUstK00iRzvWrX0kzJUjPLm5laaFipZXq1Tcw75koo"
        "4o4sriAqyiJugKyCEPvAMMDMMMv7/M7yLlPp/dnNcjDPH3y+M3Pe875z5jnnWc5zDgD3i92W"
        "rKzfx5mZCqemKJyUpPCJ4wqHhioc+MNNH6GRt3cjiQciDrwZ23EJw0d6Y5jED/tIHIo9vHE3"
        "52+xS1f8hvNi7PQgLuL8Onp2wKmch2G79jj0byqG/lWOsLqA85HDAPFBDBvULARXy0wqJALy"
        "Ug7QVh2LW9TnBfVSWuczDenfbH7p3hgifQc4Z6wHKF1BaanaEq/+EU+r2wJoxGYEKnJTsA3M"
        "tjZm1b8qAvghTXyCCIDzwZxLVoJD7cK73kFTpyp87JhEXd/Jz/3nAww/NtGvldeH4ATMm3o+"
        "aYW1wJX0CD5J63+MH5LrsBnlFsJRgNU4T26ndRGeyJba9NxWh9aYaU78VfPEiiq84iXd13Ut"
        "noBGpiT+DGnoCmctXRmfxNYwEs+2INzLQOeIQNRVFIbXmV6gV06Ot2D5/r6/eX7yyWT7EcLF"
        "+WXrXAAcEsKJZMRF4jKAtw2dYe8lFexOAfdKIm4xewBWFMPWTFJvY4WPR2rMIGE4ubR7NaLZ"
        "9wof7g8GrHx0QbWn1Kxn9aIvS/Lm8knhysd4NAFfIjhcGHQpDBzTiZy5ZG6Bss/Ip6cAPql0"
        "h4uH2PtdDLMBPqT1R0B4ggOA06qSG3PtfCS3+bGgJnE0Q9UZjNult7CfvROeBPDDJZT74FaA"
        "MbiAsnNZboOGp2s82AUzcP25qmd5O4/VYtaDHMcZNVfMRI48e7t0Md7o1Ca3qieswJ7/wmkA"
        "PtrctmG1zutwCKk41HxV2ALv4RbSV7AGLV7Q7Dpm7dqegGb8BKDVF6m1+vQfup2wDLL7CRHx"
        "5myqU7isVOFsGx19wUYXnzz5N9Aebkt8JWyWcLGZxHt0D0r4Kr4joerazy4SP4lfyY18gs/K"
        "vBtbynzKpNworkbhKGygWDb4kMzT8A2ZW5kilAs2CY/I7KE5rpJfjLEuUGq9bpn013eg7xI3"
        "mZtdjG8qv+hcvVupNQ9flrlhYaryBdbgAJmfw6Uyt8QQ5WpzjMK1cTI2wCiZH8b1Mr9BR7fU"
        "1aZWEvYRZCNSFV3eTuKF1lESvmKZetcFcpg70arDGTatIPbIPm0L9uKf6OeHksbbptNtZzBL"
        "tElwFsBgtfoM4hm1ejD9xDUrISGb63f4Ud+jRy2/oGnqHIDZ6bxNF/nP372cO3f7vFitVpeW"
        "kj+LyXtS/5/7/e3cL7+rNPT2bihxP8R+NvyYMh+WOysz8dcyNjX0uQd7ZIqalSmUN3Ax3HBf"
        "Tv7sUp9iDnMVRdhRUJTrh9hd5uREGb1xvszLBNkJgfRTMj6Gs2X+ytxa1qc3FGNlECpKdL1B"
        "tgcalCrhHF8cL/NWnTyqG1Ztld8ej75KIKisgX1164CIck0kt1tGmQ37I0wmakI002h7Em8b"
        "y8l3ns+6MsZKXGVi7bUFmIT/QtJHBYUAjfOjoPQGgP4SwHKTN6TWAhiuQhcjMSjT9KROMRws"
        "Ipq7itQ5gjPwNfLjYSTAAjSdIYbTEvwXMVsr6ds9tRpqsY4z1x3eZzCPZQ/0dJRGE1Ufgl/g"
        "Ro0W0dr7WmRXyZihhgmZ244qdWDsn8hrw8LCEvByWNhagERJpyfaew+OP15pKtjcmYcJ9UeC"
        "krDyIeoeX6Q+8Rw8CBCBj7KaRVqAN0X//0al4jFIEUdo+Z1Q6cPIeb4WE7lP8vBVLJzuwMWq"
        "RljjKsZ8KiwTpQZWUedYLCm4jyskNbHSpT5EO+9C14UXtZUXPmhCsMk5TAsOuY5JxCJ+F78g"
        "7zjsxlcBHhpCPyUj+EN5otolei2t1XsLCsU5tiv5slGyVdJi1DW94tl0skYrt8ysBtCUMBfI"
        "oYTMB9tZOMNhGRL3pMPPeC04LB9TqNfS9rscky5xUeP6MJhVkegv8Qcoc9+6DIldM7JGSLzd"
        "+OhAkaeQEShyV10wiOyUkPGAxKsNvUHk7kJxJCnnMCOyWz9ZxGRbcKDyDLdkOy0H+DfRUr5k"
        "ZKOIBf11l21c4j2gGL3rvyuqTWSadiKadXsPVeJqptmTiFJud5XK50MjmQofYeMZt8QTRE9x"
        "j7gFHiPeJT5M+UW6wjAZw4lh0/6qldjKDvuwODSsCj9iUjonqUZ7crT9C6HH97nGgr3MG/Au"
        "wTPbwo16oglVF6tpeManNIx2xrty7R8snb//2ZAeQO2PpOIMfVhIPsYR96GczZ+N9tDxbkIq"
        "e+BuIbraWM6vy6ohc2itSp5Ld+PTFL2EMwDD8UoH0sNHcTqzyXUR+0pxN62q8jtfW50wW2Xn"
        "PWir+/z5ZH6I6hFenE4VtJV4tflxWRity+VpIClTjieMwdeV6NcN2YTrxgSVF3/edawcsSoT"
        "XYWyVNgUz8jshRH1Yi58Pa66Om4a+63XYVloaClSV7SzcIloUNfLwoMAL+CX9NMv8QUy4uqS"
        "ybhrlGykIamXNcUhIcWaCUznJtFfIukpalQbU33d3HxTDeRFjIVp/C6WkwDVGfyWGTqAkkp2"
        "R5W2mFjV+BnR244raLi8fQ5e3x16HXPaUyP18yu1tVc+b2bvfei84ALtQ4qNz2H+nrBy/I7w"
        "ctxC5Kt5FF3tzDIwgfGgkUKjuPxWlkvs52pnPmaNNLr6rSP50oexjijrHMzauV+bXVRGZ7+1"
        "OcbCwLaG8/Idn6bz5BQe6/2Jeh87cBKLsF0g3epZJERvT8ZSbyaGW3ONeYEe9/3Ze85fttfv"
        "+HeJfbm+O0+kDqu0yLVK/5/MeGwEGYMO42Kxbmsv4BOnZoUUwV+HX7eXGnjqFJq2yd+3L7n0"
        "xEuSzvX8shLT5KhCk7lZ65QbO3jduwGrX60R8DL41vVt69he+3co8951lXk5Vq7qIKn0EcfQ"
        "/FN/6aPeW+swdpyD+KrdCg1zF1lp/zWKctVnmwlPUeWteukEuZitZfumYeWXYmRtXdbcJrJl"
        "41BfemjggRJD2sfMyPOz6qOCszHaCaBjXV5HYh/uoSafP19+exw3E3v7AxYj7Is/yiNti7wU"
        "6HEc14uD06tE7yfV2MIW5EW75rpy52HK0nsTb2/ZzByJ2OdmbMel+apsY/Zatk7cLQ+TgmKs"
        "KeSFS6p2GJ3CajbS9dJhrOYiUwvIiIQ2y9+cstQDx3fHia4ZiIVHoWTJq9juIxz7PLHwrqzz"
        "18CVWOIsroQL363NbiQQCdTPhZRV3ye3J16MO74CJfO/TG2CK2ECTnDD4QsrISO9d0L5+qUa"
        "59HYdTLikg/Q/Bo0M6+Arj5Er5AfEzbWUZfPnSmV1qkYtz3aNIF7kWtyDFnfdb3LnTgWcYzE"
        "m6uqJAFzKAvdU+oghaL93pAU8GqhXXtpyT7zAkAyt5x98HM60lm86yN8ipqBiymfx4iwsAi2"
        "rNdOzB8TiMKZyYPb0/AtgHDBnXIb4TC4GuNF/8noOgEDxBA6TrBrKWQBlJrkRS6Mk9XqvYVI"
        "Uwv60S4Bl6M01YgzTMKPZZ6LM0R2G1Oa+4DYDmKwu9TO9ngMby3XHy8clBkSrI1kPo2tJX7O"
        "nCFeu+20QPO+WJuWwp2P2v1YHnWyXH+Wx0M/wqq9+6toV0F/4XIbgLYp1sE0m/NxPh+GgKNW"
        "dGznvE2c2V1yI4/h9zdl2zqO2nQOQTHOsIfbgN3ousATeJH0dqtzArVtlqI2dH8FruJDJKZC"
        "f36qvfcg/a1rUld6gCg/P13F8mdAkpkxNZo2svxMp3EqkRuU/KySZekAtpV5A/aWOZDoA4n3"
        "YTuJHYuKHCR+jfY351G6yvb8GYKuYOVQ8dlqr63xqg99yMo8cRzRMlQZdwC3y0pIVW7H639p"
        "J1lenJbbtPfFadt1H8znz08Dx3SQyXVuh5XfQm5n6P/Szi3YrsuLx8urz/A8tPmo27u3jIWf"
        "+1hTiPZsdpyGVHbik0xvGi6AQ4WYe9iyDXS0yfjic+Ba2nGunMtph7bqKM2HG7BVgyrx2hhs"
        "ReZYamhDDyvh/kIamUtcT5oJw2Ks2R9cUvAjZRgVra3c33kltrLvGKwYZy5P/INsI8+JfElB"
        "bUy8c+3XJ5+3qkrxeX/vvpU7wbbPUM986s+VcNOfxp7RHzD+Gn2gifko4+OWB6jOHsNM/XDm"
        "I+KZoDNYxhwPj//kmwo3271eFsfLGhoT4DwDDziI/KwpsbFYp3tFXluxfosMrY94rdNJ81Cp"
        "nU2YI4YYybiOl9b0EnGbYyyO5FzlDJ10pW3k+05j3SbeNxTnytyy0OAjP9tw4ZKdp4DeKb15"
        "x7g0jJW1v7IZ7Nq0YaIQSfMxEKsciQmsY1yBfamFVkH5WM37RGnXHKMceWQ/saqjIhl/pFGp"
        "NIs5P42P9MKnODvXznmn1okzHAvZfRREXlJU7C/xM4iDJHYxGFwkhkmTQGb+cPbdhxvQSA2H"
        "TtSO3YC4UOGz1x1knkxHo8idLoYr/Ka1s8yNK7+SGdaWN5S5m/C6zBAVr/BonCCzQ+5JmWER"
        "KtzaaP8JoJPP6qrP8jz2tcRXDdUiHUbD8JI7gPsluq4XzpVDfwwDR4Po82YYHLtJtl8Idukv"
        "+ciB2E/m9di/G+6U6nRtYLzGOd3gCBF8naQvHQa+mEx+x1bJSLPsv8HK3bsrxQ2WU+NqauJe"
        "qxe+njF9dUs5bpOFeR6S/+XwKW6TfTFVZanil6XXydzenCLFcJ6/wH1GXr5WiW1+/+FDt+/r"
        "3ZzrkRwajh01iHKY1gGgwzUuh8xPeYrJoWjHZVE5FP2U3beQww34qI2sejYwZgC8Mx0gu9qJ"
        "yOETsNHaaQBN5fHFVM8+uClFGMLkUH/EiLhSkkMzyjkV8EC1TdrDhjovu5kPSdGdmaFivFO9"
        "KUyLwSDNRc3O0RA2Z5iIn9nwR0od00PitYe0FeNBTmOPGyC103hQgcZVvnYxPmfTzjSZh9Bc"
        "HZF9cI3M7rjDzsWw6WcptdozLIW/ZTpm7zpUIeY5rCUqpeV5OkcdQ3ENZTeNc/H13tHP0vya"
        "kvflXUz/zEMhaWknMVQ47NvraAmUl4yfOYs/QKNgPjRd8upcINvEzEQ4ge4wB6Np6HaAPo9M"
        "GJtRF7kjVjCyddKJxzV1uZsfqiehL8R+XmrJz1J73TVf77/xPR9zuAUnSJwAcMrsyOK35miA"
        "jdiX/4Y/EHcIo5sDNI9lmyk3YOmBA6UYyLTYmwl6fcKbdt6Fzyp2yJAGFbmccysaQBAyf64n"
        "nYteZmYjMdomkqFfx/ZVxNbRXXpHLG5EGVvYhsZ38BW67fRtyh3pVUHI16GTyx0dNRd4y8vw"
        "6YHSStNjuGIlSksl+Zcu35CeaT0qiwnDEYfbuxi+ijwOAAvwlUH4bwA/P6qeBz5I7DpXxCaw"
        "Czs5C6egF+IjECs4QXEWjEMcC9lFRMb08EFR8XzQx9O4QfPAM2fXu1E9+w0+HLV9R6QPTcJb"
        "gEOzApZlDqP3eQWnm6e+bvKjMcmBuBEHDiJ/nqYGeyo9TyMVyRTiZBX0KpVesNJDDQoxhWa5"
        "F9p5J/6EbDlqDAaR72JMI4NNdclINcNqujo/iadfulXkODmmV/C9u+/jbD9p749zzs+516WA"
        "wCSkCfC8TEV5+2mj/JyCPFG7+OPkN3maMLSpvqhyvKZjq6Ib8UWqoumO256WaDqIEyw+1Mdj"
        "SfFD7Tyc3bioqjnrpaqiRkQ/BvABN4fIQ05lU2K3VOY6sx7xp9txaKovOKRoXB/QXGMzG/wD"
        "Fy2WsxwSSsuSpRSkYcjOb+Alx0aQruX+72yvZYeklokf9gYPPx/EK9KnqiPK8v1iZnuz8qQ5"
        "Xjw/BNxyK+VN+HvxHxLOkU7DAehtTJRS3ppcq+oivb0VJypCqyymVgvb5f0paG/7U7zkpTbZ"
        "ounHeZaNpWPPfPqmz/9Xluk2B5x8InSUuTvaHJKQdEHhD7CHzB2EAOWD2HSF30YlCaG1eZXy"
        "QeQNJZv/dZs8z6YGReZgT6myd3GCjQJuqNuiVNpWJZvq8KIi6dCgbLe9yCErs+yf2fOflsbO"
        "3ZHDX5bnUTkdJNhoDBKxhTF4p0E8OuM9HPYCisJ7OdfBIe8Swyfo5roAZFmHGwUvusXkB7hf"
        "6kOxwxzaCza5RHNQK09LzfU2p9dsp8ETKWqA6fd/yD9SGn2coi/exII3TscxJeg8XqXzvB9u"
        "JIpjJdKEV99ldNnPQ0qPdXr0jUDcx7y1w3oUsnA/q5/47pOujoyTK2gLjzDOqSbTR/PjbHvW"
        "51gQdFiTlqfbTEzRRZl1Bd+3Gldo545KP5tdmf+Ffc/r89fz3MI44dK2eMx0Y1P7MvLWp3Rd"
        "tR/mUGF2KSqlOV1qdt0+9LTJB+v1q/ezWf1iVp+dKya2cxQvyO1PmHTRUBDofk8I4iM7i2ou"
        "vsvs6sE15sif8jCESKBzrv4J8nc/jqaJ08wC7xw5l2oc5eiFb2zyqW3fH8XnCa9KopBc8vVE"
        "xTrvpR4fDNWbI0j7fBmqT0hxXep79SS13d/GUPgtu6/P15/3ZdwiC+O3XRTiKH/HxHAZNTIc"
        "y4uo29wgh3BH3Muu3US4Fw9isTwH2/cdy4vl+mI7ASi1vzWZCCO7b2CB4eKkCXfbiLkzZejR"
        "iqoYnqww3Vp1cK+GGT3ta1LJMHO7aOxIIxEvsvA2tdiDkPlrITQCFi7p0Cj6vrL1bj6PT3QY"
        "RayjDvps0kLjaBZimGGtDt9ZJG7LeD5KUxX3cj3ooOY7SuU8ybV4TjrcB05W88CCtD7CbIkp"
        "anWxUdnLc7pcaefP5npepiXV/ry2CevnNVi6L7ImkvJTeIbM/F2u8/V0tvY3mfJJKwuHdePM"
        "PDVvyoHc5/Pj18a3JFUKWJtfYtm+iNpT/HecfL6m+OuOlL182ZpLL7oLoRceJjrI6Sccx+rn"
        "Bofm41E2UU46q9VfWuh8N7uHWRf/L/c8UqP5kR3vg9FlF3Zm8QOLkY7chonsgCHMVTFbky7s"
        "8Z3bo3Cm3I4vs4f/CNt56aV+QeaRUmD7F/YPLatltzvEZmN81zvMg1n7l5lnP+s352bYcwlc"
        "rLDxtyfnbbDJvmLnoVXbZGLl/smclyCjq4Gv0fur1ZtTxJNiaAJu4bcN64GNTc3hWHxPZFUw"
        "s+4Yr2SmIuMZeMpFrP+C+Wc38dpdVYL1GUmeC56syG3KufxheAW3cqYnj4Ww3VD8Xi2LNO3l"
        "ZxiJUfZ9KgYZL9XRvWS//qCQpnwWZJNLvhq7/iVMz+W4jCFsb0L9OavqNkR1anD1Jlhcee0Z"
        "wvGxV/GjgkOYRzhR1dpa5wOH0LMf1Z35RVQs+zDxuXaNGeh3iut9J47Fp0bG628sc2T8hTk6"
        "pIhtRx6LVU8TzyLX5EaZLaAso2GfsXQzH7VhxlBmxuYUHPvHuP6X0dGV2mh+3tEinh9IJ/Re"
        "lrTWAK2vmX2o2UgNNxhH5TEa2Z5xV3q20mXxlOeaS798fw3vmLHM9rOk0vzDVNoOab8iJKRC"
        "VBijY7TamNH3Qg/OwoffyzBk+LswDhVOB+XiYRVlywgA53Ca4TCLJ2f1EMIpv8Wuyy+hzBcG"
        "E02/fp8dYdyd17cMJ+0c4u2kWmN3XMcI1v6opdeNmexMp1k28YpbsV0XF3/ShXN9qK3rEI65"
        "O04LoZRfw0PEPRhhoRxl7cbVEOFSnvYxk7I5QZwOfvV+lJXpkB28nf1OAMNZOw6RmLMj1prC"
        "7OqGAZnGHP+RNja2722wnSoVRFObI0aqH4mNHYf7rgepuekzD2Nk02yekuZA+BkbHnj7zNo/"
        "xF32fngPncfrOOtClfbYEG6OY17wQYOVjlBX82UyvLtV5BO/tTWy011fnEcPloq3rnteOmCq"
        "7Q4jCmkrRCupyXMfRlrzyGePbxlBXy+lWRIdzemtmD6ifuLHqDuwLU5IY3tdxpzQ1mWubmH/"
        "PTQkoK1NmLTPbxlt4jYBAQHRuIH87Xfr+rfJn9jwOzaryz0t2Suk9uHVNNM9cX6L6C9soCb3"
        "H+G+LNYKWyh34usCCZQddOXuNOTDTPqlWBxyyhjK2HF5keHc4M/tfNvobZXLem6TGIh9spBP"
        "epOp8dFBYIf8HWKnPkfX0TzMOvbvP2bQ9CY//n8pmhkjiQYx8qG9x9yqlVk80nkczpwlWX7O"
        "FSejK6Tw1UarVV6xZvuA7oESgKMlW/sT9p9oZs6k526O6UhEycVkcoFN2BE0sfAE4uNwugLg"
        "eAnMKSt7G0pPUB+u+ZaIyM1u1MGejE9c/fSzKwPoSc4+ONsyeoxlNj5CxE0fg+09MMZIV6nO"
        "C4UAhUIil17yIx0UV7Lqd5nOD0kKRJrVu99KfJNnBJYS6F6a3qhhutZTlKvVX8j/y2Or1Swd"
        "VgyPKeeqOZ6tqTorRsf9cc4MMbDR13RCBREmml3cKK36QQBPLdUp3/Lzy/2UHLL6W+LqWAym"
        "meksDbK+wf2vufQ/x7ANtPssNNwcZiFzWhP9ERZaphmME3m+YiNdLE1H4WN5q+DpouMrUzAM"
        "54+SDq5xKEzYopMOtvs31my16X/Fht4ppTh1Ddgohx59sXZvU/vvQ2+bUGhbnrGhUfK93sVN"
        "Eo63HpYyJgYa4qVzu7w1WdIKcbvcUilU80ByjXRSmNMR8wipiUmYze8wgP5QYpliR50xbPl/"
        "pKjgAiWk5KyLl9fbuuP7NurYNvvvLZkH2Rzu/6lFunRZrG6O9G6ifo3colOPU9lKhtxL+LzM"
        "/bmg/uZevXliPCtuNmcbQ5RwWC09v3tI6V0Jib2l9AMkJt47/JcV//8sHyY/ABFCZdnEI1Gn"
        "rIq9b/MfLGyzAGfa8HgbJ83DopxTDzMrY5dJ7LlWL31Np+sxPaTc46F8HVrUVTfPOJzDz4YG"
        "rprFVD5/dbgQIT9/aci9kUBCHLaMmn+roCcdk6rMgl3CEtK9g+lpaZNgiRByI8eB5lqT/vii"
        "JvNJuF9+R5milsuUDUpe+v3/5POn6n3vFo29vRu28PamvvmiIYgDFvGzERS+QwXvlz9c7F2c"
        "6sMZ1/8HCk4qug=="
    )),
    # GeistMono-Regular 25px
    "r25": (15, 34, 26, (
        "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"
        "lg=="
    )),
    # GeistMono-SemiBold 25px
    "b25": (15, 34, 26, (
        "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"
    )),
    # GeistMono-Regular 35px
    "r35": (21, 47, 36, (
        "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"
    )),
    # GeistMono-SemiBold 35px
    "b35": (21, 47, 36, (
        "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"
    )),
}
# --- end of generated mono font ---

_TERM_BACKDROP = (13, 14, 17)
_TERM_BAR = (32, 34, 38)
_TERM_BORDER = (62, 65, 71)
_TERM_BODY = (8, 11, 10)
_TERM_TITLE = (156, 161, 168)
_TERM_TEXT = (212, 242, 220)       # phosphor white-green
_TERM_DIM = (112, 156, 126)        # labels, the command words
_TERM_DEFAULT_ACCENT = (74, 222, 128)
_TERM_DOTS = ((255, 95, 87), (254, 188, 46), (40, 200, 64))
TERM_MARGIN = 24                   # backdrop around the window
TERM_BAR_H = 50                    # title bar height
TERM_PAD = 44                      # body text inset
_TERM_FACE = "r35"
_TERM_BOLD = "b35"
_TERM_SCANLINE = 0.9               # every other body row, this bright

_mono_memo = {}
_blend_memo = {}
_TERM_ASCII = {
    "\u2018": "'", "\u2019": "'", "\u201a": "'", "\u201c": '"',
    "\u201d": '"', "\u201e": '"', "\u2013": "-", "\u2212": "-",
    "\u2010": "-", "\u2011": "-", "\u00a0": " ", "\u2022": "\u00b7",
}


def _mono(face):
    font = _mono_memo.get(face)
    if font is None:
        cell_w, cell_h, ascent, chunks = _MONO_FONTS[face]
        atlas = zlib.decompress(base64.b64decode("".join(chunks)))
        size = cell_w * cell_h
        glyphs = [
            tuple(atlas[index * size + row * cell_w:
                        index * size + (row + 1) * cell_w]
                  for row in range(cell_h))
            for index in range(len(MONO_GLYPHS))]
        font = _mono_memo[face] = (cell_w, cell_h, ascent, glyphs)
    return font


def terminal_text(value):
    out = []
    for ch in unicodedata.normalize("NFKD", str(value or "")):
        if ch in _MONO_INDEX:
            out.append(ch)
        elif unicodedata.combining(ch):
            continue
        elif ch in _TERM_ASCII:
            out.append(_TERM_ASCII[ch])
        elif ch.isspace():
            out.append(" ")
        else:
            out.append("?")
    return "".join(out)


def _blend_tables(fg, bg):
    key = (tuple(fg), tuple(bg))
    tables = _blend_memo.get(key)
    if tables is None:
        tables = tuple(
            bytes((bg[k] * (255 - a) + fg[k] * a + 127) // 255
                  for a in range(256))
            for k in range(3))
        if len(_blend_memo) > 64:
            _blend_memo.clear()
        _blend_memo[key] = tables
    return tables


def _mono_text(canvas, x, y, text, face, fg, bg):
    cell_w, cell_h, _, glyphs = _mono(face)
    fallback = _MONO_INDEX["?"]
    end = x + len(text) * cell_w
    picks = [glyphs[_MONO_INDEX.get(ch, fallback)] for ch in text.rstrip(" ")]
    x0 = max(0, x)
    x1 = min(canvas.w, x + len(picks) * cell_w)
    if x1 <= x0:
        return end
    red, green, blue = _blend_tables(fg, bg)
    stride = canvas.w * 3
    for row in range(cell_h):
        yy = y + row
        if not 0 <= yy < canvas.h:
            continue
        alpha = b"".join(glyph[row] for glyph in picks)[x0 - x:x1 - x]
        if not alpha.strip(b"\0"):
            continue
        out = bytearray(len(alpha) * 3)
        out[0::3] = alpha.translate(red)
        out[1::3] = alpha.translate(green)
        out[2::3] = alpha.translate(blue)
        off = yy * stride + x0 * 3
        canvas.buf[off:off + len(out)] = out
    return end


def _mix(a, b, t):
    return tuple(int(round(a[k] + (b[k] - a[k]) * t)) for k in range(3))


def _coverage(px, py, cx, cy, radius):
    if radius <= 0:
        return 0.0
    inside = 0
    limit = radius * radius
    for sy in (0.125, 0.375, 0.625, 0.875):
        dy = py + sy - cy
        for sx in (0.125, 0.375, 0.625, 0.875):
            dx = px + sx - cx
            if dx * dx + dy * dy <= limit:
                inside += 1
    return inside / 16.0


def _set_pixel(canvas, x, y, color):
    if 0 <= x < canvas.w and 0 <= y < canvas.h:
        off = (y * canvas.w + x) * 3
        canvas.buf[off:off + 3] = bytes(color)


def _disc(canvas, cx, cy, radius, color, bg):
    for py in range(int(cy - radius) - 1, int(cy + radius) + 2):
        for px in range(int(cx - radius) - 1, int(cx + radius) + 2):
            cover = _coverage(px, py, cx, cy, radius)
            if cover:
                _set_pixel(canvas, px, py, _mix(bg, color, cover))


def _round_corners(canvas, x, y, w, h, radius, top_fill, bottom_fill,
                   border, backdrop):
    corners = (
        (x, y, x + radius, y + radius, top_fill),
        (x + w - radius, y, x + w - radius, y + radius, top_fill),
        (x, y + h - radius, x + radius, y + h - radius, bottom_fill),
        (x + w - radius, y + h - radius, x + w - radius, y + h - radius,
         bottom_fill),
    )
    for left, top, cx, cy, fill in corners:
        for py in range(top, top + radius):
            outside = backdrop(py)
            for px in range(left, left + radius):
                outer = _coverage(px, py, cx, cy, radius)
                inner = _coverage(px, py, cx, cy, radius - 1)
                color = tuple(
                    int(round(outside[k] * (1 - outer)
                              + border[k] * (outer - inner)
                              + fill[k] * inner)) for k in range(3))
                _set_pixel(canvas, px, py, color)


def _dim_rows(canvas, x, w, rows, factor):
    table = bytes(int(v * factor) for v in range(256))
    stride = canvas.w * 3
    for yy in rows:
        off = yy * stride + x * 3
        canvas.buf[off:off + w * 3] = canvas.buf[off:off + w * 3].translate(
            table)


def parse_color(value):
    if isinstance(value, (tuple, list)) and len(value) == 3:
        try:
            return tuple(max(0, min(255, int(v))) for v in value)
        except (TypeError, ValueError):
            return None
    text = str(value or "").strip().lstrip("#")
    if len(text) == 6:
        try:
            return tuple(int(text[i:i + 2], 16) for i in (0, 2, 4))
        except ValueError:
            return None
    return None


def logo_accent(logo):
    if not logo:
        return None
    _, _, rgba = logo
    buckets = {}
    for i in range(0, len(rgba) - 3, 8):
        r, g, b, a = rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]
        high = max(r, g, b)
        if a < 192 or high < 80 or high - min(r, g, b) < 64:
            continue
        tally = buckets.setdefault((r >> 5, g >> 5, b >> 5), [0, 0, 0, 0])
        tally[0] += 1
        tally[1] += r
        tally[2] += g
        tally[3] += b
    if not buckets:
        return None
    n, r, g, b = max(buckets.values())
    if n < 8:
        return None
    return (r // n, g // n, b // n)


def _legible(color):
    for _ in range(8):
        if 0.299 * color[0] + 0.587 * color[1] + 0.114 * color[2] >= 105:
            break
        color = _mix(color, (255, 255, 255), 0.18)
    return color


def _count(info, key):
    try:
        return max(0, int(str(info.get(key) if info.get(key) is not None
                              else "").strip() or 0))
    except (TypeError, ValueError):
        return 0


_TERM_COUNTS = (
    ("commits", "commits"),
    ("contributors", "contributors"),
    ("issues", "issues"),
    ("pulls", "pulls"),
    ("stars", "stars"),
    ("branches", "branches"),
    ("followers", "followers"),
)


def repo_card_stats(info, now_s):
    pairs = []
    for label, key in _TERM_COUNTS:
        n = _count(info, key)
        if n:
            pairs.append((label, format_count(n)))
    mirrors = _count(info, "mirrors")
    if mirrors > 1:
        pairs.append(("mirrors", format_count(mirrors)))
    if _count(info, "sizeBytes"):
        pairs.append(("size", format_size(info.get("sizeBytes"))))
    age = format_age(info.get("updatedAt"), now_s)
    if age != "-":
        pairs.append(("updated", age))
    return pairs


ISSUES_OPEN_PATH = ".forkmesh/issues/open"
ROSTER_DIR = ".forkmesh/contributors"
ROSTER_PATH = ROSTER_DIR + "/contributors.json"
_ROSTER_MAX_CHARS = 2 * 1024 * 1024
_ROSTER_MAX_ROWS = 10000
_BOT_PATTERNS = (
    re.compile(r"\[bot\]$", re.I), re.compile(r"^github[\s_-]?actions?$", re.I),
    re.compile(r"^(?:dependabot|renovate)$", re.I),
    re.compile(r"(?:^|[\s_-])bot$", re.I),
    re.compile(r"^[a-z0-9][a-z0-9_-]*-actions$", re.I),
)


def open_issue_count(tree):
    if not isinstance(tree, dict) or not isinstance(tree.get("entries"), list):
        return None
    return sum(1 for entry in tree["entries"]
               if isinstance(entry, dict) and entry.get("type") == "tree"
               and re.fullmatch(r"[1-9][0-9]{0,8}", str(entry.get("name"))))


def branch_count(listing):
    if not isinstance(listing, dict) or not isinstance(
            listing.get("branches"), list):
        return None
    return len({str(item.get("name")) for item in listing["branches"]
                if isinstance(item, dict) and item.get("name")})


def has_roster(listing):
    entries = listing.get("entries") if isinstance(listing, dict) else None
    return any(isinstance(entry, dict) and entry.get("type") == "tree"
               and entry.get("name") == "contributors"
               for entry in entries or [])


def roster_count(text):
    if not isinstance(text, str) or not text or len(text) > _ROSTER_MAX_CHARS:
        return None
    try:
        data = json.loads(text)
    except ValueError:
        return None
    rows = data.get("contributors") if isinstance(data, dict) else None
    if not isinstance(rows, list) or not rows or len(rows) > _ROSTER_MAX_ROWS:
        return None
    named = sum(1 for row in rows if isinstance(row, dict) and " ".join(
        str(row.get("name") or row.get("login") or row.get("slug") or "")
        .split()))
    if not named:
        return None
    declared = data.get("count")
    if isinstance(declared, int) and not isinstance(declared, bool) \
            and 0 < declared <= 10000000:
        return declared
    return named


def _is_bot(row, platform=""):
    name = str(row.get("name") or "").strip()
    email = str(row.get("email") or "").strip().lower()
    if not name and not email:
        return False
    if any(pattern.search(name) for pattern in _BOT_PATTERNS) \
            or "[bot]@" in email:
        return True
    lowered = name.lower()
    platform = str(platform or "").strip().lower()
    if lowered in ("forkmesh actions", "forkmesh deploy") or (
            platform and lowered == platform + " actions"):
        return True
    return email == "actions@forkmesh.local"


def stats_contributor_count(stats, platform=""):
    if not isinstance(stats, dict):
        return None
    rows = [row for row in stats.get("contributors") or []
            if isinstance(row, dict) and (row.get("name") or row.get("email"))]
    people = [row for row in rows if not _is_bot(row, platform)]
    if not people:
        return None
    try:
        known = max(0, int(stats.get("contributorCount") or 0)) or len(rows)
    except (TypeError, ValueError):
        known = len(rows)
    return max(len(people), known - (len(rows) - len(people)))


def counts_token(counts):
    counts = counts or {}
    return "i%s.b%s.c%s" % tuple(
        "-" if counts.get(key) is None else int(counts[key])
        for key in ("issues", "branches", "contributors"))


def _stat_grid(pairs, max_cols):
    for ncols in (3, 2, 1):
        columns = [pairs[c::ncols] for c in range(ncols)]
        widths = [(max([len(k) for k, _ in col] or [0]),
                   max([len(v) for _, v in col] or [0])) for col in columns]
        total = sum(kw + 2 + vw for kw, vw in widths) + 3 * (ncols - 1)
        if total <= max_cols or ncols == 1:
            break
    rows = []
    for start in range(0, len(pairs), ncols):
        cells = []
        for c, (key, value) in enumerate(pairs[start:start + ncols]):
            kw, vw = widths[c]
            cells.append((key.ljust(kw + 2), value.ljust(vw + 3)))
        rows.append(cells)
    return rows


def _fit_cols(text, cols):
    if len(text) <= cols:
        return text
    return text[:max(0, cols - 1)] + "\u2026"


def _wrap_cols(text, cols, max_lines):
    lines = []
    cur = ""
    for word in text.split():
        cand = (cur + " " + word) if cur else word
        if cur and len(cand) > cols:
            lines.append(cur)
            cur = word
        else:
            cur = cand
    if cur:
        lines.append(cur)
    if len(lines) > max_lines:
        lines = lines[:max_lines]
        lines[-1] = _fit_cols(lines[-1].rstrip(".") + "\u2026", cols)
    return [_fit_cols(line, cols) for line in lines]


def _terminal_window(accent, title):
    top_wash = _mix(_TERM_BACKDROP, accent, 0.16)
    bottom_wash = _mix(_TERM_BACKDROP, accent, 0.05)

    def backdrop(yy):
        return _mix(top_wash, bottom_wash, yy / float(CARD_H - 1))

    canvas = _Canvas(CARD_W, CARD_H, _TERM_BACKDROP)
    for yy in range(CARD_H):
        canvas.fill_rect(0, yy, CARD_W, 1, backdrop(yy))

    wx = wy = TERM_MARGIN
    ww, wh = CARD_W - 2 * wx, CARD_H - 2 * wy
    bar_h = TERM_BAR_H
    canvas.fill_rect(wx, wy, ww, wh, _TERM_BODY)
    canvas.fill_rect(wx, wy, ww, bar_h, _TERM_BAR)
    canvas.fill_rect(wx, wy + bar_h, ww, 1, _TERM_BORDER)
    canvas.frame_rect(wx, wy, ww, wh, 1, _TERM_BORDER)
    _round_corners(canvas, wx, wy, ww, wh, 14, _TERM_BAR, _TERM_BODY,
                   _TERM_BORDER, backdrop)
    for index, color in enumerate(_TERM_DOTS):
        _disc(canvas, wx + 28 + index * 24, wy + bar_h / 2.0, 7, color,
              _TERM_BAR)

    tw, th, _, _ = _mono("r20")
    title = _fit_cols(title, (ww - 2 * 120) // tw)
    _mono_text(canvas, wx + (ww - len(title) * tw) // 2,
               wy + (bar_h - th) // 2, title, "r20", _TERM_TITLE, _TERM_BAR)
    return canvas, wx, wy, ww, wh


def render_repo_card(info, now_s, logo=None):
    """Render the 1200x630 OpenGraph PNG for a repo: a terminal window.

    info keys (all optional except owner/repo): owner, repo, description,
    host, stars, mirrors, issues, pulls, commits, branches, contributors,
    sizeBytes, updatedAt, followers, accent ('#rrggbb', the instance theme's
    accent) and ``path`` - the repository's public address on this site
    ("/name" on an instance with short addresses, else "/owner/name"), which
    the title bar, the command and the clone URL use. ``logo`` is a
    card_logo() result drawn in place of the ForkMesh mark; without a theme
    accent, the logo's own colour is the accent.
    """
    owner = str(info.get("owner") or "")
    repo = str(info.get("repo") or "")
    path = terminal_text(
        str(info.get("path") or "").strip("/") or "%s/%s" % (owner, repo))
    host = terminal_text(str(info.get("host") or "forkmesh.com"))
    if logo:
        mark = logo
    else:
        mark = (_LOGO_W, _LOGO_H, _logo_rgba())
    accent = _legible(parse_color(info.get("accent"))
                      or logo_accent(mark) or _TERM_DEFAULT_ACCENT)

    canvas, wx, wy, ww, wh = _terminal_window(accent, "%s \u2014 %s" % (
        host, path))
    bar_h = TERM_BAR_H

    cw, ch, _, _ = _mono(_TERM_FACE)
    line_h = 52
    pad = TERM_PAD
    left = wx + pad
    cols = (ww - 2 * pad) // cw
    body_top = wy + bar_h + 30

    mark_w, mark_h, mark_rgba = mark
    mark_x = wx + ww - pad - mark_w
    canvas.blit_rgba(mark_x, body_top + 4, mark_w, mark_h, mark_rgba)
    text_cols = (mark_x - 28 - left) // cw

    y = body_top
    x = _mono_text(canvas, left, y, "$ ", _TERM_BOLD, accent, _TERM_BODY)
    x = _mono_text(canvas, x, y, "forkmesh repo show ", _TERM_FACE, _TERM_DIM,
                   _TERM_BODY)
    _mono_text(canvas, x, y, _fit_cols(path, text_cols - 21), _TERM_BOLD,
               _TERM_TEXT, _TERM_BODY)

    description = terminal_text(" ".join(
        str(info.get("description") or "").split()))
    if not description:
        description = terminal_text(
            "A repository mirrored across the %s network." % (
                str(info.get("site") or "ForkMesh")))
    y += line_h + 8
    for line in _wrap_cols(description, text_cols, 3):
        _mono_text(canvas, left, y, line, _TERM_FACE, _TERM_TEXT, _TERM_BODY)
        y += line_h

    y = max(y + 16, body_top + 4 + mark_h + 20)
    clone_y = wy + wh - 34 - ch
    room = max(1, (clone_y - 16 - y) // line_h)
    for cells in _stat_grid(repo_card_stats(info, now_s), cols)[:room]:
        x = left
        for key, value in cells:
            x = _mono_text(canvas, x, y, key, _TERM_FACE, _TERM_DIM, _TERM_BODY)
            x = _mono_text(canvas, x, y, value, _TERM_BOLD, _TERM_TEXT, _TERM_BODY)
        y += line_h

    y = clone_y
    url = _fit_cols("https://%s/%s.git" % (host, path), cols - 13)
    x = _mono_text(canvas, left, y, "$ ", _TERM_BOLD, accent, _TERM_BODY)
    x = _mono_text(canvas, x, y, "git clone ", _TERM_FACE, _TERM_DIM, _TERM_BODY)
    x = _mono_text(canvas, x, y, url, _TERM_FACE, _TERM_TEXT, _TERM_BODY)
    canvas.fill_rect(x + 4, y + 4, cw - 2, ch - 8, accent)

    _dim_rows(canvas, wx + 1, ww - 2,
              range(wy + bar_h + 2, wy + wh - 14, 2), _TERM_SCANLINE)
    return canvas.png()


def render_referral_card(info, now_s):
    """Render the 1200x630 OpenGraph PNG for a /r/<name> share link.

    The counters are the live values read at request time, so a Mastodon (or
    Slack/Discord/Twitter) unfurl of the share link shows how far it has
    actually travelled instead of the generic signup-page preview.

    info keys: name (required), host, clicks, signups, lastTs.
    """
    name = str(info.get("name") or "")
    canvas = _Canvas(CARD_W, CARD_H, _BG)

    margin = 28
    canvas.fill_rect(margin, margin, CARD_W - 2 * margin, CARD_H - 2 * margin,
                     _CARD)
    canvas.frame_rect(margin, margin, CARD_W - 2 * margin, CARD_H - 2 * margin,
                      2, _BORDER)

    pad = 72
    logo_x = CARD_W - pad - _LOGO_W
    canvas.blit_rgba(logo_x, margin + 40, _LOGO_W, _LOGO_H, _logo_rgba())

    title = "@" + name
    title_max_w = logo_x - pad - 40
    scale = 6
    while scale > 3 and text_width(title, scale) > title_max_w:
        scale -= 1
    canvas.text(pad, 96, _fit(title, scale, title_max_w), scale, _FG)

    y = 96 + 7 * scale + 26
    for line in _wrap("invites you to ForkMesh - distributed Git hosting on a "
                      "mesh of desktop nodes.", 3, title_max_w, 2):
        canvas.text(pad, y, line, 3, _TEXT)
        y += 7 * 3 + 12

    canvas.fill_rect(pad, 268, CARD_W - 2 * pad, 2, _BORDER)

    # One wide row: the two counters this link has earned, plus how recently.
    cells = (
        ("CLICKS", format_exact(info.get("clicks"))),
        ("SIGNUPS", format_exact(info.get("signups"))),
        ("LAST ACTIVITY", format_age(info.get("lastTs"), now_s)),
    )
    grid_x = pad
    grid_w = CARD_W - 2 * pad
    col_w = grid_w // 3
    for index, (label, value) in enumerate(cells):
        cx = grid_x + index * col_w
        value_scale = 10
        while value_scale > 3 and text_width(value, value_scale) > col_w - 24:
            value_scale -= 1
        canvas.text(cx, 330, value, value_scale, _FG)
        canvas.text(cx, 330 + 7 * 10 + 16, label, 2, _MUTED)

    footer_y = CARD_H - margin - 48
    host = str(info.get("host") or "forkmesh.com")
    canvas.text(pad, footer_y,
                _fit("%s/r/%s" % (host, name), 3, grid_w - 360), 3, _ACCENT)
    right = "REFERRAL PROGRAM"
    canvas.text(CARD_W - pad - text_width(right, 2), footer_y + 4,
                right, 2, _MUTED)
    return canvas.png()


def render_hackathon_card(info):
    """Render the 1200x630 OpenGraph PNG for a hackathon share link.

    The counters are read live at request time, so an unfurl of an invite,
    rank or squad link shows the standing the invitation is actually built
    around rather than a generic campaign image. Same renderer, palette and
    cache rules as the repository and referral cards.

    info keys: kind, title, name, headline, cells (three label/value pairs),
    host, path.
    """
    canvas = _Canvas(CARD_W, CARD_H, _BG)

    margin = 28
    canvas.fill_rect(margin, margin, CARD_W - 2 * margin, CARD_H - 2 * margin,
                     _CARD)
    canvas.frame_rect(margin, margin, CARD_W - 2 * margin, CARD_H - 2 * margin,
                      2, _BORDER)

    pad = 72
    logo_x = CARD_W - pad - _LOGO_W
    canvas.blit_rgba(logo_x, margin + 40, _LOGO_W, _LOGO_H, _logo_rgba())

    # Campaign name above the headline, so the card reads as an invitation
    # to a specific event rather than to the product in general.
    kicker = str(info.get("title") or "ForkMesh Hackathon").upper()
    max_w = logo_x - pad - 40
    canvas.text(pad, 74, _fit(kicker, 2, max_w), 2, _ACCENT)

    headline = str(info.get("headline") or "")
    scale = 6
    while scale > 3 and text_width(headline, scale) > max_w:
        scale -= 1
    canvas.text(pad, 112, _fit(headline, scale, max_w), scale, _FG)

    y = 112 + 7 * scale + 24
    for line in _wrap(
            "Build something that outlives its host. Join the ForkMesh "
            "Hackathon and ship it on a mesh of independent nodes.",
            3, max_w, 2):
        canvas.text(pad, y, line, 3, _TEXT)
        y += 7 * 3 + 12

    canvas.fill_rect(pad, 300, CARD_W - 2 * pad, 2, _BORDER)

    cells = tuple(info.get("cells") or ())[:3]
    grid_w = CARD_W - 2 * pad
    col_w = grid_w // max(1, len(cells) or 1)
    for index, (label, value) in enumerate(cells):
        text = value if isinstance(value, str) else format_exact(value)
        cx = pad + index * col_w
        # Leave a real gutter rather than the last legible size: three
        # five-digit figures at scale 10 touch, and two counters that touch
        # read as one number.
        value_scale = 10
        while value_scale > 3 and text_width(text, value_scale) > col_w - 64:
            value_scale -= 1
        canvas.text(cx, 356, text, value_scale, _FG)
        canvas.text(cx, 356 + 7 * 10 + 16, str(label), 2, _MUTED)

    footer_y = CARD_H - margin - 48
    host = str(info.get("host") or "forkmesh.com")
    path = str(info.get("path") or "/hackathon")
    canvas.text(pad, footer_y,
                _fit(host + path, 3, grid_w - 360), 3, _ACCENT)
    right = "FORKMESH HACKATHON"
    canvas.text(CARD_W - pad - text_width(right, 2), footer_y + 4,
                right, 2, _MUTED)
    return canvas.png()


PULL_AVATAR = 48
PULL_REVIEW_AVATAR = 30
PULL_PEOPLE_SHOWN = 10
_PULL_GREEN = (63, 185, 80)
_PULL_RED = (248, 81, 73)
_PULL_ORANGE = (227, 160, 8)
_PULL_PURPLE = (163, 113, 247)
_PULL_GRAY = (139, 148, 158)
_PULL_TILE = (17, 22, 20)
_PULL_TRACK = (38, 44, 41)
_PULL_STATES = {
    "open": ("OPEN", _PULL_GREEN),
    "draft": ("DRAFT", _PULL_GRAY),
    "merged": ("MERGED", _PULL_PURPLE),
    "closed": ("CLOSED", _PULL_RED),
}
_PULL_TONES = {"green": _PULL_GREEN, "orange": _PULL_ORANGE, "red": _PULL_RED}
_PULL_VERDICTS = {
    "approved": ("approved", _PULL_GREEN),
    "changes_requested": ("requested changes", _PULL_RED),
    "commented": ("reviewed", _TERM_TITLE),
    "ai": ("AI review", None),
}
_PULL_INITIALS = (
    (88, 120, 196), (150, 96, 182), (196, 104, 92), (70, 150, 120),
    (180, 140, 60), (90, 140, 170), (170, 90, 130), (110, 130, 80),
)
_mask_memo = {}


def _get_pixel(canvas, x, y):
    off = (y * canvas.w + x) * 3
    return canvas.buf[off], canvas.buf[off + 1], canvas.buf[off + 2]


def _blend_pixel(canvas, x, y, color, alpha):
    if alpha <= 0 or not (0 <= x < canvas.w and 0 <= y < canvas.h):
        return
    off = (y * canvas.w + x) * 3
    if alpha >= 1:
        canvas.buf[off:off + 3] = bytes(color)
        return
    for k in range(3):
        canvas.buf[off + k] = int(canvas.buf[off + k] * (1 - alpha)
                                  + color[k] * alpha + 0.5)


def _disc_mask(diameter):
    key = ("disc", diameter)
    mask = _mask_memo.get(key)
    if mask is None:
        r = diameter / 2.0
        mask = _mask_memo[key] = [
            _coverage(px, py, r, r, r)
            for py in range(diameter) for px in range(diameter)]
    return mask


def _corner_mask(radius):
    key = ("corner", radius)
    mask = _mask_memo.get(key)
    if mask is None:
        mask = _mask_memo[key] = [
            _coverage(px, py, radius, radius, radius)
            for py in range(radius) for px in range(radius)]
    return mask


def _round_rect(canvas, x, y, w, h, radius, color):
    radius = max(0, min(radius, w // 2, h // 2))
    canvas.fill_rect(x, y + radius, w, h - 2 * radius, color)
    canvas.fill_rect(x + radius, y, w - 2 * radius, radius, color)
    canvas.fill_rect(x + radius, y + h - radius, w - 2 * radius, radius, color)
    mask = _corner_mask(radius)
    for py in range(radius):
        for px in range(radius):
            cover = mask[py * radius + px]
            for cx, cy in ((x + px, y + py),
                           (x + w - 1 - px, y + py),
                           (x + px, y + h - 1 - py),
                           (x + w - 1 - px, y + h - 1 - py)):
                _blend_pixel(canvas, cx, cy, color, cover)


def _initial_color(name):
    return _PULL_INITIALS[zlib.crc32(str(name or "").lower().encode(
        "utf-8")) % len(_PULL_INITIALS)]


def _avatar(canvas, x, y, diameter, rgba, name, ring=0):
    if ring:
        outer = diameter + 2 * ring
        mask = _disc_mask(outer)
        for py in range(outer):
            for px in range(outer):
                _blend_pixel(canvas, x - ring + px, y - ring + py, _TERM_BODY,
                             mask[py * outer + px])
    mask = _disc_mask(diameter)
    if rgba and len(rgba) == diameter * diameter * 4:
        for py in range(diameter):
            for px in range(diameter):
                i = (py * diameter + px) * 4
                _blend_pixel(canvas, x + px, y + py, rgba[i:i + 3],
                             mask[py * diameter + px] * rgba[i + 3] / 255.0)
        return
    color = _initial_color(name)
    for py in range(diameter):
        for px in range(diameter):
            _blend_pixel(canvas, x + px, y + py, color,
                         mask[py * diameter + px])
    face = "b25" if diameter >= 40 else "r20"
    cw, ch, _, _ = _mono(face)
    letter = (terminal_text(str(name or "?").lstrip("@")).strip()[:1]
              or "?").upper()
    _mono_text(canvas, x + (diameter - cw) // 2, y + (diameter - ch) // 2,
               letter, face, (255, 255, 255), color)


def _plural(n, word):
    return "%s %s%s" % (format_exact(n), word, "" if n == 1 else "s")


def _diff_squares(canvas, x, y, adds, dels, size=14, gap=3):
    total = adds + dels
    green = int(round(5.0 * adds / total)) if total else 0
    if adds and not green:
        green = 1
    if dels and green == 5:
        green = 4
    red = min(5 - green, int(round(5.0 * dels / total)) if total else 0)
    if dels and not red:
        red = 1
    for index in range(5):
        color = _PULL_GREEN if index < green else (
            _PULL_RED if index < green + red else _PULL_TRACK)
        canvas.fill_rect(x + index * (size + gap), y, size, size, color)
    return x + 5 * (size + gap) - gap


def _segments(canvas, x, y, parts, max_x, bg=_TERM_BODY):
    for text, face, color in parts:
        cw = _mono(face)[0]
        room = (max_x - x) // cw
        if room <= 0:
            break
        piece = _fit_cols(text, room)
        x = _mono_text(canvas, x, y, piece, face, color, bg)
        if piece != text:
            break
    return x


def pull_review_label(review):
    state = str((review or {}).get("state") or "")
    approvals = _count(review or {}, "approvals")
    blockers = _count(review or {}, "blockers")
    if state == "approved":
        return "approved by %s" % _plural(approvals, "reviewer"), _PULL_GREEN
    if state == "changes_requested":
        return "changes requested by %s" % format_exact(blockers), _PULL_RED
    if state == "review_required":
        return "review required", _PULL_GRAY
    return "", _PULL_GRAY


def _people_lookup(people):
    found = {}
    for person in people:
        for field in ("key", "name"):
            name = str(person.get(field) or "").lower()
            if name and name not in found:
                found[name] = person
    return found


def render_pull_card(info, now_s):
    host = terminal_text(str(info.get("host") or "forkmesh.com"))
    path = terminal_text(str(info.get("path") or "").strip("/"))
    number = _count(info, "number")
    accent = _legible(parse_color(info.get("accent")) or _TERM_DEFAULT_ACCENT)
    canvas, wx, wy, ww, wh = _terminal_window(
        accent, "%s — %s/pull/%d" % (host, path, number))

    left = wx + 40
    right = wx + ww - 40
    small_w, small_h, _, _ = _mono("r20")
    mid_w, mid_h, _, _ = _mono("r25")
    big_w, big_h, _, _ = _mono("b35")
    people = [p for p in info.get("people") or () if isinstance(p, dict)]
    lookup = _people_lookup(people)

    y = wy + TERM_BAR_H + 26
    state = str(info.get("state") or "open").lower()
    label, color = _PULL_STATES.get(state, (terminal_text(state.upper())[:10],
                                            _PULL_GRAY))
    pill_w = len(label) * mid_w + 28
    _round_rect(canvas, left, y, pill_w, 38, 10, color)
    _mono_text(canvas, left + 14, y + 2, label, "b25", _TERM_BODY, color)
    review_text, review_color = pull_review_label(info.get("review"))
    review_x = right
    if review_text:
        review_text = _fit_cols(review_text, 34)
        review_x = right - len(review_text) * mid_w
        _disc(canvas, review_x - 16, y + 19, 6, review_color, _TERM_BODY)
        _mono_text(canvas, review_x, y + 2, review_text, "b25", review_color,
                   _TERM_BODY)
        review_x -= 36
    head_bits = []
    commits = _count(info, "commits")
    if commits:
        head_bits.append(_plural(commits, "commit"))
    opened = format_age(info.get("ts"), now_s)
    if opened != "-":
        head_bits.append("opened " + opened)
    _segments(canvas, left + pill_w + 18, y + 2, [
        ("#%d" % number, "b25", _TERM_TEXT),
        ("".join("  ·  " + bit for bit in head_bits), "r25", _TERM_DIM),
    ], review_x)

    y += 38 + 16
    title = terminal_text(" ".join(str(info.get("title") or "").split())) \
        or "Pull request #%d" % number
    for line in _wrap_cols(title, (right - left) // big_w, 2):
        _mono_text(canvas, left, y, line, "b35", _TERM_TEXT, _TERM_BODY)
        y += big_h + 2

    y += 4
    author = terminal_text(str(info.get("author") or "unknown"))
    head = terminal_text(str(info.get("head") or ""))
    base = terminal_text(str(info.get("base") or ""))
    verb = {"merged": " merged ", "closed": " proposed merging "}.get(
        state, " wants to merge ")
    branch_cols = max(8, ((right - left) // mid_w - len(author) - 24) // 2)
    parts = [(author, "b25", _TERM_TEXT)]
    if head and base:
        parts += [(verb, "r25", _TERM_DIM),
                  (_fit_cols(head, branch_cols), "r25", _TERM_TEXT),
                  (" into ", "r25", _TERM_DIM),
                  (_fit_cols(base, branch_cols), "r25", _TERM_TEXT)]
    else:
        parts.append((" opened this pull request", "r25", _TERM_DIM))
    _segments(canvas, left, y, parts, right)
    y += mid_h + 14

    files = _count(info, "files")
    adds = _count(info, "additions")
    dels = _count(info, "deletions")
    comments = _count(info, "comments")
    commenters = _count(info, "commenters")
    stats = []
    x = left
    if files:
        x = _mono_text(canvas, x, y, "+" + format_exact(adds), "b25",
                       _PULL_GREEN, _TERM_BODY)
        x = _mono_text(canvas, x + mid_w, y, "-" + format_exact(dels), "b25",
                       _PULL_RED, _TERM_BODY)
        x = _diff_squares(canvas, x + mid_w, y + (mid_h - 14) // 2, adds, dels)
        stats.append(_plural(files, "file") + " changed")
    if comments:
        stats.append(_plural(comments, "comment") + (
            " from %s" % _plural(commenters, "person").replace(
                "persons", "people") if commenters else ""))
    elif state in ("open", "draft"):
        stats.append("no comments yet")
    _segments(canvas, x + (mid_w if files else 0), y, [
        ("  ·  ".join(stats), "r25", _TERM_TITLE)], right)
    y += mid_h + 14

    health = [m for m in info.get("health") or () if isinstance(m, dict)][:6]
    if health:
        gap = 12
        tile_w = (right - left - gap * (len(health) - 1)) // len(health)
        tile_h = 76
        for index, metric in enumerate(health):
            tx = left + index * (tile_w + gap)
            _round_rect(canvas, tx, y, tile_w, tile_h, 10, _PULL_TILE)
            score = max(0, min(100, _count(metric, "score")))
            tone = str(metric.get("tone") or "")
            shade = _PULL_TONES.get(tone, _PULL_GRAY)
            if metric.get("key") == "security" and tone == "green":
                shade = _PULL_GRAY
            name = _fit_cols(terminal_text(str(metric.get("label") or "")),
                             (tile_w - 24) // small_w)
            _mono_text(canvas, tx + 12, y + 6, name, "r20", _TERM_DIM,
                       _PULL_TILE)
            sx = _mono_text(canvas, tx + 12, y + 30, str(score), "b25", shade,
                            _PULL_TILE)
            _mono_text(canvas, sx, y + 30 + mid_h - small_h - 2, "/100", "r20",
                       _TERM_DIM, _PULL_TILE)
            bar_w = tile_w - 24
            canvas.fill_rect(tx + 12, y + tile_h - 12, bar_w, 4, _PULL_TRACK)
            canvas.fill_rect(tx + 12, y + tile_h - 12,
                             max(2, bar_w * score // 100), 4, shade)
        y += tile_h + 14

    people_y = wy + wh - 22 - PULL_AVATAR
    reviews = [r for r in info.get("reviews") or () if isinstance(r, dict)]
    row_h = max(mid_h, PULL_REVIEW_AVATAR) + 4
    room = max(0, (people_y - 10 - y) // row_h)
    if not reviews and room and state in ("open", "draft"):
        _mono_text(canvas, left, y, "no reviews yet", "r25", _TERM_DIM,
                   _TERM_BODY)
    shown = reviews[:room]
    if len(reviews) > room >= 3:
        shown = reviews[:room - 1] + [
            {"name": "", "state": "more",
             "text": "and %s" % _plural(len(reviews) - room + 1,
                                        "more review")}]
    for review in shown:
        name = terminal_text(str(review.get("name") or ""))
        x = left
        if review.get("state") != "more":
            person = lookup.get(str(review.get("key") or "").lower()) \
                or lookup.get(str(review.get("name") or "").lower()) or {}
            avatar = (person.get("avatars") or {}).get(PULL_REVIEW_AVATAR)
            _avatar(canvas, x, y + (mid_h - PULL_REVIEW_AVATAR) // 2,
                    PULL_REVIEW_AVATAR, avatar, name)
            x += PULL_REVIEW_AVATAR + 12
            word, tint = _PULL_VERDICTS.get(
                str(review.get("state") or ""), ("commented", _TERM_TITLE))
            text = terminal_text(" ".join(str(review.get("text") or "").split()))
            _segments(canvas, x, y, [
                (name, "b25", _TERM_TEXT),
                (" " + word, "b25", tint or accent),
                ((": \"%s\"" % text) if text else "", "r25", _TERM_TITLE),
            ], right)
        else:
            _mono_text(canvas, x, y, review.get("text"), "r25", _TERM_DIM,
                       _TERM_BODY)
        y += row_h

    x = left + 3
    step = PULL_AVATAR - 8
    for person in people[:PULL_PEOPLE_SHOWN]:
        _avatar(canvas, x, people_y, PULL_AVATAR,
                (person.get("avatars") or {}).get(PULL_AVATAR),
                person.get("name"), ring=3)
        x += step
    x += 8 + 8
    extra = len(people) - PULL_PEOPLE_SHOWN
    if extra > 0:
        x = _mono_text(canvas, x, people_y + (PULL_AVATAR - mid_h) // 2,
                       "+%d" % extra, "b25", _TERM_TEXT, _TERM_BODY) + mid_w
    if people:
        names = ", ".join(terminal_text(str(p.get("name") or ""))
                          for p in people[:4])
        label = "%s involved" % _plural(len(people), "person").replace(
            "persons", "people")
        _segments(canvas, x, people_y + (PULL_AVATAR - mid_h) // 2, [
            (label, "b25", _TERM_TEXT), ("  " + names, "r25", _TERM_DIM)],
            right)

    _dim_rows(canvas, wx + 1, ww - 2,
              range(wy + TERM_BAR_H + 2, wy + wh - 14, 2), _TERM_SCANLINE)
    return canvas.png()
