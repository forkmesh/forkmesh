import math
from decimal import ROUND_HALF_UP, Decimal

LEVELS = ("low", "moderate", "high", "critical")

V3_METRICS = (
    ("AV", "NALP"), ("AC", "LH"), ("PR", "NLH"), ("UI", "NR"), ("S", "UC"),
    ("C", "HLN"), ("I", "HLN"), ("A", "HLN"),
)
V3_AV = {"N": 0.85, "A": 0.62, "L": 0.55, "P": 0.2}
V3_AC = {"L": 0.77, "H": 0.44}
V3_PR = {"N": (0.85, 0.85), "L": (0.62, 0.68), "H": (0.27, 0.5)}
V3_UI = {"N": 0.85, "R": 0.62}
V3_CIA = {"H": 0.56, "L": 0.22, "N": 0.0}

V4_METRICS = (
    ("AV", "NALP"), ("AC", "LH"), ("AT", "NP"), ("PR", "NLH"), ("UI", "NPA"),
    ("VC", "HLN"), ("VI", "HLN"), ("VA", "HLN"), ("SC", "HLN"), ("SI", "HLN"),
    ("SA", "HLN"),
)
V4_OPTIONAL = (
    ("E", "XAPU"), ("CR", "XHML"), ("IR", "XHML"), ("AR", "XHML"),
    ("MAV", "XNALP"), ("MAC", "XLH"), ("MAT", "XNP"), ("MPR", "XNLH"),
    ("MUI", "XNPA"), ("MVC", "XHLN"), ("MVI", "XHLN"), ("MVA", "XHLN"),
    ("MSC", "XHLN"), ("MSI", "XSHLN"), ("MSA", "XSHLN"), ("S", "XNP"),
    ("AU", "XNY"), ("R", "XAUI"), ("V", "XDC"), ("RE", "XLMH"),
)
V4_URGENCY = ("X", "Clear", "Green", "Amber", "Red")
V4_LOOKUP = dict(item.split(":") for item in (
    "000000:10 000001:9.9 000010:9.8 000011:9.5 000020:9.5 000021:9.2 "
    "000100:10 000101:9.6 000110:9.3 000111:8.7 000120:9.1 000121:8.1 "
    "000200:9.3 000201:9 000210:8.9 000211:8 000220:8.1 000221:6.8 "
    "001000:9.8 001001:9.5 001010:9.5 001011:9.2 001020:9 001021:8.4 "
    "001100:9.3 001101:9.2 001110:8.9 001111:8.1 001120:8.1 001121:6.5 "
    "001200:8.8 001201:8 001210:7.8 001211:7 001220:6.9 001221:4.8 "
    "002001:9.2 002011:8.2 002021:7.2 002101:7.9 002111:6.9 002121:5 "
    "002201:6.9 002211:5.5 002221:2.7 010000:9.9 010001:9.7 010010:9.5 "
    "010011:9.2 010020:9.2 010021:8.5 010100:9.5 010101:9.1 010110:9 "
    "010111:8.3 010120:8.4 010121:7.1 010200:9.2 010201:8.1 010210:8.2 "
    "010211:7.1 010220:7.2 010221:5.3 011000:9.5 011001:9.3 011010:9.2 "
    "011011:8.5 011020:8.5 011021:7.3 011100:9.2 011101:8.2 011110:8 "
    "011111:7.2 011120:7 011121:5.9 011200:8.4 011201:7 011210:7.1 "
    "011211:5.2 011220:5 011221:3 012001:8.6 012011:7.5 012021:5.2 "
    "012101:7.1 012111:5.2 012121:2.9 012201:6.3 012211:2.9 012221:1.7 "
    "100000:9.8 100001:9.5 100010:9.4 100011:8.7 100020:9.1 100021:8.1 "
    "100100:9.4 100101:8.9 100110:8.6 100111:7.4 100120:7.7 100121:6.4 "
    "100200:8.7 100201:7.5 100210:7.4 100211:6.3 100220:6.3 100221:4.9 "
    "101000:9.4 101001:8.9 101010:8.8 101011:7.7 101020:7.6 101021:6.7 "
    "101100:8.6 101101:7.6 101110:7.4 101111:5.8 101120:5.9 101121:5 "
    "101200:7.2 101201:5.7 101210:5.7 101211:5.2 101220:5.2 101221:2.5 "
    "102001:8.3 102011:7 102021:5.4 102101:6.5 102111:5.8 102121:2.6 "
    "102201:5.3 102211:2.1 102221:1.3 110000:9.5 110001:9 110010:8.8 "
    "110011:7.6 110020:7.6 110021:7 110100:9 110101:7.7 110110:7.5 "
    "110111:6.2 110120:6.1 110121:5.3 110200:7.7 110201:6.6 110210:6.8 "
    "110211:5.9 110220:5.2 110221:3 111000:8.9 111001:7.8 111010:7.6 "
    "111011:6.7 111020:6.2 111021:5.8 111100:7.4 111101:5.9 111110:5.7 "
    "111111:5.7 111120:4.7 111121:2.3 111200:6.1 111201:5.2 111210:5.7 "
    "111211:2.9 111220:2.4 111221:1.6 112001:7.1 112011:5.9 112021:3 "
    "112101:5.8 112111:2.6 112121:1.5 112201:2.3 112211:1.3 112221:0.6 "
    "200000:9.3 200001:8.7 200010:8.6 200011:7.2 200020:7.5 200021:5.8 "
    "200100:8.6 200101:7.4 200110:7.4 200111:6.1 200120:5.6 200121:3.4 "
    "200200:7 200201:5.4 200210:5.2 200211:4 200220:4 200221:2.2 201000:8.5 "
    "201001:7.5 201010:7.4 201011:5.5 201020:6.2 201021:5.1 201100:7.2 "
    "201101:5.7 201110:5.5 201111:4.1 201120:4.6 201121:1.9 201200:5.3 "
    "201201:3.6 201210:3.4 201211:1.9 201220:1.9 201221:0.8 202001:6.4 "
    "202011:5.1 202021:2 202101:4.7 202111:2.1 202121:1.1 202201:2.4 "
    "202211:0.9 202221:0.4 210000:8.8 210001:7.5 210010:7.3 210011:5.3 "
    "210020:6 210021:5 210100:7.3 210101:5.5 210110:5.9 210111:4 210120:4.1 "
    "210121:2 210200:5.4 210201:4.3 210210:4.5 210211:2.2 210220:2 "
    "210221:1.1 211000:7.5 211001:5.5 211010:5.8 211011:4.5 211020:4 "
    "211021:2.1 211100:6.1 211101:5.1 211110:4.8 211111:1.8 211120:2 "
    "211121:0.9 211200:4.6 211201:1.8 211210:1.7 211211:0.7 211220:0.8 "
    "211221:0.2 212001:5.3 212011:2.4 212021:1.4 212101:2.4 212111:1.2 "
    "212121:0.5 212201:1 212211:0.3 212221:0.1 "
).split())
V4_MAX_COMPOSED = {
    1: {"0": ["AV:N/PR:N/UI:N/"],
        "1": ["AV:A/PR:N/UI:N/", "AV:N/PR:L/UI:N/", "AV:N/PR:N/UI:P/"],
        "2": ["AV:P/PR:N/UI:N/", "AV:A/PR:L/UI:P/"]},
    2: {"0": ["AC:L/AT:N/"], "1": ["AC:H/AT:N/", "AC:L/AT:P/"]},
    3: {"0": {"0": ["VC:H/VI:H/VA:H/CR:H/IR:H/AR:H/"],
              "1": ["VC:H/VI:H/VA:L/CR:M/IR:M/AR:H/",
                    "VC:H/VI:H/VA:H/CR:M/IR:M/AR:M/"]},
        "1": {"0": ["VC:L/VI:H/VA:H/CR:H/IR:H/AR:H/",
                    "VC:H/VI:L/VA:H/CR:H/IR:H/AR:H/"],
              "1": ["VC:L/VI:H/VA:L/CR:H/IR:M/AR:H/",
                    "VC:L/VI:H/VA:H/CR:H/IR:M/AR:M/",
                    "VC:H/VI:L/VA:H/CR:M/IR:H/AR:M/",
                    "VC:H/VI:L/VA:L/CR:M/IR:H/AR:H/",
                    "VC:L/VI:L/VA:H/CR:H/IR:H/AR:M/"]},
        "2": {"1": ["VC:L/VI:L/VA:L/CR:H/IR:H/AR:H/"]}},
    4: {"0": ["SC:H/SI:S/SA:S/"], "1": ["SC:H/SI:H/SA:H/"],
        "2": ["SC:L/SI:L/SA:L/"]},
    5: {"0": ["E:A/"], "1": ["E:P/"], "2": ["E:U/"]},
}
V4_MAX_SEVERITY = {
    "eq1": {0: 1, 1: 4, 2: 5}, "eq2": {0: 1, 1: 2},
    "eq3eq6": {0: {0: 7, 1: 6}, 1: {0: 8, 1: 8}, 2: {1: 10}},
    "eq4": {0: 6, 1: 5, 2: 4},
}
V4_LEVELS = {
    "AV": {"N": 0.0, "A": 0.1, "L": 0.2, "P": 0.3},
    "PR": {"N": 0.0, "L": 0.1, "H": 0.2},
    "UI": {"N": 0.0, "P": 0.1, "A": 0.2},
    "AC": {"L": 0.0, "H": 0.1},
    "AT": {"N": 0.0, "P": 0.1},
    "VC": {"H": 0.0, "L": 0.1, "N": 0.2},
    "VI": {"H": 0.0, "L": 0.1, "N": 0.2},
    "VA": {"H": 0.0, "L": 0.1, "N": 0.2},
    "SC": {"H": 0.1, "L": 0.2, "N": 0.3},
    "SI": {"S": 0.0, "H": 0.1, "L": 0.2, "N": 0.3},
    "SA": {"S": 0.0, "H": 0.1, "L": 0.2, "N": 0.3},
    "CR": {"H": 0.0, "M": 0.1, "L": 0.2},
    "IR": {"H": 0.0, "M": 0.1, "L": 0.2},
    "AR": {"H": 0.0, "M": 0.1, "L": 0.2},
}


def level_for_score(score):
    if score >= 9.0:
        return "critical"
    if score >= 7.0:
        return "high"
    if score >= 4.0:
        return "moderate"
    if score > 0:
        return "low"
    return "none"


def _pairs(text, prefix):
    if not isinstance(text, str) or not text.startswith(prefix):
        return None
    body = text[len(prefix):]
    if not body or len(text) > 400:
        return None
    pairs = {}
    order = []
    for part in body.split("/"):
        key, sep, value = part.partition(":")
        if not sep or not key or not value or key in pairs:
            return None
        pairs[key] = value
        order.append(key)
    return pairs, order


def _v3_roundup(value):
    whole = int(round(value * 100000))
    if whole % 10000 == 0:
        return whole / 100000.0
    return (math.floor(whole / 10000) + 1) / 10.0


V3_OPTIONAL = (
    ("E", "XUPFH"), ("RL", "XOTWU"), ("RC", "XURC"),
    ("CR", "XLMH"), ("IR", "XLMH"), ("AR", "XLMH"),
    ("MAV", "XNALP"), ("MAC", "XLH"), ("MPR", "XNLH"), ("MUI", "XNR"),
    ("MS", "XUC"), ("MC", "XNLH"), ("MI", "XNLH"), ("MA", "XNLH"),
)


def score_v3(vector):
    parsed = _pairs(vector, "CVSS:3.1/") or _pairs(vector, "CVSS:3.0/")
    if not parsed:
        return None
    metrics, order = parsed
    names = [name for name, _ in V3_METRICS]
    if order[:len(names)] != names:
        return None
    optional = dict(V3_OPTIONAL)
    for name in order[len(names):]:
        if name not in optional or metrics[name] not in tuple(optional[name]):
            return None
    for name, allowed in V3_METRICS:
        if metrics[name] not in tuple(allowed):
            return None
    changed = metrics["S"] == "C"
    iss = 1 - ((1 - V3_CIA[metrics["C"]]) * (1 - V3_CIA[metrics["I"]])
               * (1 - V3_CIA[metrics["A"]]))
    if changed:
        impact = 7.52 * (iss - 0.029) - 3.25 * (iss - 0.02) ** 15
    else:
        impact = 6.42 * iss
    exploitability = (8.22 * V3_AV[metrics["AV"]] * V3_AC[metrics["AC"]]
                      * V3_PR[metrics["PR"]][1 if changed else 0]
                      * V3_UI[metrics["UI"]])
    if impact <= 0:
        return 0.0
    if changed:
        return _v3_roundup(min(1.08 * (impact + exploitability), 10))
    return _v3_roundup(min(impact + exploitability, 10))


def _v4_metrics(vector):
    parsed = _pairs(vector, "CVSS:4.0/")
    if not parsed:
        return None
    metrics, order = parsed
    known = [name for name, _ in V4_METRICS + V4_OPTIONAL] + ["U"]
    if any(name not in known for name in order):
        return None
    positions = [known.index(name) for name in order]
    if positions != sorted(positions):
        return None
    for name, allowed in V4_METRICS:
        if metrics.get(name, "") not in tuple(allowed):
            return None
    for name, allowed in V4_OPTIONAL:
        if name in metrics and metrics[name] not in tuple(allowed):
            return None
    if "U" in metrics and metrics["U"] not in V4_URGENCY:
        return None
    return metrics


def _v4_value(metrics, name):
    selected = metrics.get(name, "X")
    if name == "E" and selected == "X":
        return "A"
    if name in ("CR", "IR", "AR") and selected == "X":
        return "H"
    modified = metrics.get("M" + name, "X")
    if modified != "X":
        return modified
    return selected


def _v4_macro(m):
    av, pr, ui = m("AV"), m("PR"), m("UI")
    if av == "N" and pr == "N" and ui == "N":
        eq1 = "0"
    elif (av == "N" or pr == "N" or ui == "N") and av != "P":
        eq1 = "1"
    else:
        eq1 = "2"
    eq2 = "0" if m("AC") == "L" and m("AT") == "N" else "1"
    vc, vi, va = m("VC"), m("VI"), m("VA")
    if vc == "H" and vi == "H":
        eq3 = "0"
    elif "H" in (vc, vi, va):
        eq3 = "1"
    else:
        eq3 = "2"
    if m("MSI") == "S" or m("MSA") == "S":
        eq4 = "0"
    elif "H" in (m("SC"), m("SI"), m("SA")):
        eq4 = "1"
    else:
        eq4 = "2"
    eq5 = {"A": "0", "P": "1", "U": "2"}[m("E")]
    high = ((m("CR") == "H" and vc == "H") or (m("IR") == "H" and vi == "H")
            or (m("AR") == "H" and va == "H"))
    eq6 = "0" if high else "1"
    return eq1 + eq2 + eq3 + eq4 + eq5 + eq6


def _v4_lookup(macro):
    value = V4_LOOKUP.get(macro)
    return float(value) if value is not None else float("nan")


def _v4_shift(macro, *steps):
    digits = [int(digit) for digit in macro]
    for index in steps:
        digits[index] += 1
    return "".join(str(digit) for digit in digits)


def _v4_extract(name, vector):
    start = vector.index(name + ":") + len(name) + 1
    return vector[start:].split("/", 1)[0]


def score_v4(vector):
    metrics = _v4_metrics(vector)
    if metrics is None:
        return None

    def m(name):
        return _v4_value(metrics, name)

    if all(m(name) == "N" for name in ("VC", "VI", "VA", "SC", "SI", "SA")):
        return 0.0
    macro = _v4_macro(m)
    value = _v4_lookup(macro)
    eq = [int(digit) for digit in macro]
    lower_eq1 = _v4_lookup(_v4_shift(macro, 0))
    lower_eq2 = _v4_lookup(_v4_shift(macro, 1))
    if eq[2] == 0 and eq[5] == 0:
        lower_eq3eq6 = max(_v4_lookup(_v4_shift(macro, 5)),
                           _v4_lookup(_v4_shift(macro, 2)))
    elif eq[2] == 1 and eq[5] == 0:
        lower_eq3eq6 = _v4_lookup(_v4_shift(macro, 5))
    elif eq[5] == 1 and eq[2] in (0, 1):
        lower_eq3eq6 = _v4_lookup(_v4_shift(macro, 2))
    else:
        lower_eq3eq6 = _v4_lookup(_v4_shift(macro, 2, 5))
    lower_eq4 = _v4_lookup(_v4_shift(macro, 3))
    lower_eq5 = _v4_lookup(_v4_shift(macro, 4))
    maxes = []
    for a in V4_MAX_COMPOSED[1][macro[0]]:
        for b in V4_MAX_COMPOSED[2][macro[1]]:
            for c in V4_MAX_COMPOSED[3][macro[2]][macro[5]]:
                for d in V4_MAX_COMPOSED[4][macro[3]]:
                    for e in V4_MAX_COMPOSED[5][macro[4]]:
                        maxes.append(a + b + c + d + e)
    distance = {}
    for candidate in maxes:
        distance = {
            name: V4_LEVELS[name][m(name)]
            - V4_LEVELS[name][_v4_extract(name, candidate)]
            for name in V4_LEVELS
        }
        if all(value_ >= 0 for value_ in distance.values()):
            break
    current = {
        "eq1": distance["AV"] + distance["PR"] + distance["UI"],
        "eq2": distance["AC"] + distance["AT"],
        "eq3eq6": sum(distance[name] for name in (
            "VC", "VI", "VA", "CR", "IR", "AR")),
        "eq4": distance["SC"] + distance["SI"] + distance["SA"],
    }
    step = 0.1
    max_severity = {
        "eq1": V4_MAX_SEVERITY["eq1"][eq[0]] * step,
        "eq2": V4_MAX_SEVERITY["eq2"][eq[1]] * step,
        "eq3eq6": V4_MAX_SEVERITY["eq3eq6"][eq[2]][eq[5]] * step,
        "eq4": V4_MAX_SEVERITY["eq4"][eq[3]] * step,
    }
    available = {
        "eq1": value - lower_eq1, "eq2": value - lower_eq2,
        "eq3eq6": value - lower_eq3eq6, "eq4": value - lower_eq4,
        "eq5": value - lower_eq5,
    }
    existing = 0
    normalized = 0.0
    for key in ("eq1", "eq2", "eq3eq6", "eq4", "eq5"):
        if available[key] >= 0:
            existing += 1
            if key != "eq5":
                normalized += available[key] * (
                    current[key] / max_severity[key])
    if existing:
        value -= normalized / existing
    value = min(10.0, max(0.0, value))
    return float(Decimal(value + 1e-6).quantize(
        Decimal("0.1"), rounding=ROUND_HALF_UP))


def score(vector):
    text = str(vector or "").strip()
    if text.startswith("CVSS:4.0/"):
        return score_v4(text), "CVSS_V4"
    if text.startswith(("CVSS:3.1/", "CVSS:3.0/")):
        return score_v3(text), "CVSS_V3"
    return None, ""
