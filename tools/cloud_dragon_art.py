#!/usr/bin/env python3
import hashlib
import json
import re
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter
from scipy import ndimage as ni

ROOT = Path(__file__).resolve().parents[1]
ART = ROOT / "tools" / "art" / "cloud-dragon"
OUT = ROOT / "www" / "public" / "assets" / "cloud-dragon"
SPIDER = ROOT / "www" / "public" / "genie-spider.js"
STAMP_ONLY = "--stamp" in sys.argv
DEBUG = Path(sys.argv[1]) if len(sys.argv) > 1 and not STAMP_ONLY else None

RIG_SCALE = 0.34
PROP_DENSITY = 1.25
SHEET_TO_MASTER = 1215 / 165
SIZE = (1254, 1254)
DISPLAY = 1.254

ROWS = [(8, 186), (226, 404), (438, 606), (636, 790), (824, 981)]
COLUMNS = [
    [(29, 307), (309, 528), (549, 763), (785, 1025), (1061, 1247), (1316, 1499)],
    [(45, 235), (297, 512), (551, 773), (805, 1014), (1059, 1265), (1315, 1500)],
    [(27, 272), (306, 511), (544, 761), (805, 1006), (1051, 1264), (1307, 1510)],
    [(23, 260), (306, 534), (564, 740), (808, 998), (1050, 1269), (1313, 1502)],
    [(29, 247), (307, 521), (545, 756), (805, 990), (1076, 1247), (1312, 1503)],
]
NAMES = [
    "fire", "trail", "land", "terminal", "coffee", "book",
    "think", "magnify", "peek", "excited", "celebrate", "headphones",
    "fast", "bugfix", "deploy", "upload", "protect", "build",
    "share", "binoculars", "map", "travel", "sleep", "stretch",
    "bell", "thumbs", "five", "star", "globe", "heart",
]
PROPS = {
    "laptop": ("terminal", [(100, 148), (150, 150), (168, 105), (238, 100), (215, 158), (170, 166), (105, 163)], "solid"),
    "mug": ("coffee", (133, 92, 178, 132), "cover"),
    "book": ("book", (90, 98, 184, 160), "cover"),
    "cup": ("headphones", (50, 48, 98, 112), "solid"),
    "blocks": ("build", (118, 84, 206, 162), "solid"),
    "backpack": ("travel", (54, 50, 122, 134), "solid"),
    "star": ("star", (104, 58, 174, 132), "gold"),
    "heart": ("heart", (96, 70, 170, 128), "red"),
}

CLEAN_SCALE = 1536 / 1125
CLEAN = {
    "flame": ([(172, 40), (240, 40), (240, 112), (158, 112), (152, 84), (168, 68)], "hot"),
    "upcloud": ([(625, 385), (650, 372), (700, 372), (735, 392), (735, 436), (625, 436)], "cloud"),
    "arrow": ([(664, 390), (698, 390), (698, 422), (664, 422)], "green"),
    "bed": ([(742, 535), (760, 515), (800, 530), (880, 530), (905, 515), (928, 535), (928, 568), (742, 568)], "cloud"),
    "plane": ([(146, 455), (213, 455), (213, 500), (146, 500)], "keep"),
    "zee": ([(880, 445), (901, 445), (901, 467), (880, 467)], "keep"),
    "loveheart": ([(1065, 599), (1103, 599), (1103, 632), (1065, 632)], "redline"),
    "sparkle": ([(690, 598), (718, 598), (718, 628), (690, 628)], "keep"),
}
HIRES_SCALE = 0.5
HIRES_MASTER = 1.65

WING_LEFT = [(30, 600), (110, 480), (260, 415), (390, 425), (418, 462), (436, 488), (466, 520), (520, 556), (540, 566),
             (505, 592), (485, 612), (462, 632), (430, 650), (400, 668), (385, 690), (355, 700), (340, 722), (285, 725),
             (200, 700), (110, 700)]
WING_LEFT_ROOT = [(380, 420), (560, 540), (640, 640), (560, 740), (400, 700)]
WING_RIGHT = [(745, 590), (770, 560), (830, 500), (885, 515), (938, 600), (980, 720), (955, 722), (930, 690), (908, 760),
              (888, 748), (860, 690), (815, 730), (800, 722), (772, 640)]
WING_RIGHT_ROOT = [(690, 570), (780, 540), (830, 720), (740, 730)]
NECK = [(405, 420), (415, 445), (435, 470), (450, 490), (470, 510), (505, 530), (525, 555), (545, 557), (570, 545),
        (600, 540), (640, 535), (680, 530), (720, 530), (745, 525), (790, 515), (840, 490), (880, 440), (915, 395)]
HEAD = [(1010, 395), (1010, -5), (260, -5), (260, 420)]
NECK_PATCH = [(540, 470), (770, 470), (770, 545), (540, 545)]
EAR = [(296, 252), (340, 246), (400, 244), (470, 250), (480, 285), (458, 320), (434, 346), (402, 368), (376, 364),
       (345, 328)]
EAR_ROOT = [(440, 250), (530, 270), (530, 350), (440, 360)]
EYES = {"eyeNear": (787, 485, 66, 54), "eyeFar": (1065, 421, 27, 51)}

ARM_NEAR = [(700, 690), (806, 690), (824, 790), (810, 880), (800, 930), (720, 945), (690, 930), (664, 880), (648, 840), (660, 770)]
PAW_NEAR = [(690, 890), (810, 860), (850, 925), (866, 975), (866, 1012), (852, 1040), (796, 1050), (762, 1020), (742, 988), (704, 962)]
ARM_FAR = [(944, 735), (982, 760), (992, 850), (1006, 900), (950, 925), (936, 900), (944, 840), (938, 790)]
PAW_FAR = [(948, 880), (1006, 880), (1030, 950), (1022, 1002), (1002, 1036), (972, 1036), (950, 1028), (928, 985), (934, 930)]
THIGH_FAR = [(950, 925), (1024, 918), (1082, 958), (1090, 1025), (1078, 1068), (1052, 1104), (1012, 1122), (925, 1128), (868, 1138),
             (884, 1090), (908, 1040), (925, 990)]
FOOT_FAR = [(822, 1172), (860, 1148), (930, 1118), (1012, 1112), (1044, 1128), (1070, 1170), (1070, 1192), (1046, 1200),
            (1020, 1204), (980, 1208), (956, 1206), (900, 1198), (848, 1188)]
THIGH_NEAR = (592, 1030, 142, 152)
FOOT_NEAR = [(600, 1128), (680, 1103), (750, 1113), (800, 1148), (808, 1190), (762, 1208), (700, 1212), (620, 1206), (572, 1190), (560, 1150)]
STRAIGHT_ALIGN = (0.994, -1, 2)
STRAIGHT_GRADE = (0.965, 0.97, 0.955)
STRAIGHT_GREEN = (19, -4, 10)
TAIL_SHADOWED = [(395, 950), (640, 950), (760, 1060), (780, 1234), (395, 1234)]
TORSO = [(720, 640), (930, 640), (985, 780), (1036, 900), (1042, 1000), (970, 1085), (860, 1120), (640, 1120), (540, 1090),
         (466, 1040), (456, 960), (462, 880), (540, 800), (640, 700)]
TAIL = [(470, 878), (220, 880), (200, 1254), (620, 1254), (660, 1216), (712, 1180), (754, 1140), (772, 1082), (772, 1046),
        (745, 1058), (700, 1088), (650, 1088), (600, 1105), (520, 1086), (440, 1052), (452, 1000), (452, 930)]
TAIL_OUTER = 185
TAIL_INNER = 100
TAIL_SLICES = 28
TAIL_DENSITY = 0.42
TAIL_AXIS = [(475, 1042), (12, 1092)]
TAIL_SOURCE = [(0, 1100), (10, 1072), (60, 1035), (88, 1005), (95, 975), (140, 955), (200, 945), (270, 920), (340, 905), (345, 885),
               (420, 872), (455, 885), (480, 930), (476, 950), (470, 1000), (470, 1050), (480, 1100), (500, 1135), (300, 1130),
               (100, 1118)]
TAIL_CURL = [(530, 1050), (420, 1040), (330, 1065), (280, 1125), (310, 1185), (400, 1210), (500, 1212), (580, 1195)]
JOINTS = {
    "hip": [800, 1040], "neck": [800, 690], "axis": [860, 1236], "shoulderNear": [745, 735], "shoulderFar": [965, 770],
    "pawNear": [800, 975], "pawFar": [975, 985], "hipNear": [590, 930], "hipFar": [980, 980], "footNear": [690, 1170],
    "footFar": [948, 1166], "wingFar": [627, 752], "wingNear": [953, 752], "ear": [600, 390], "mouth": [982, 586],
}


def polygon(points, scale=1.0, feather=0.0):
    image = Image.new("L", SIZE, 0)
    ImageDraw.Draw(image).polygon([(x * scale, y * scale) for x, y in points], fill=255)
    if feather:
        image = image.filter(ImageFilter.GaussianBlur(feather))
    return np.asarray(image).astype(np.float32) / 255


def ellipse(cx, cy, rx, ry, feather=0.0):
    yy, xx = np.ogrid[:SIZE[1], :SIZE[0]]
    mask = (((xx - cx) / rx) ** 2 + ((yy - cy) / ry) ** 2 <= 1).astype(np.float32)
    return ni.gaussian_filter(mask, feather) if feather else mask


def fill_hole(rgb, hole):
    known = (~hole).astype(np.float32)
    result = rgb.astype(np.float32).copy()
    open_hole = hole.copy()
    for sigma in (3, 6, 12, 24, 48, 96):
        weight = ni.gaussian_filter(known, sigma)
        ready = open_hole & (weight > 0.08)
        for channel in range(3):
            mass = ni.gaussian_filter(rgb[..., channel].astype(np.float32) * known, sigma)
            result[..., channel][ready] = (mass[ready] / weight[ready])
        open_hole &= ~ready
    smooth = ni.gaussian_filter(result, (2, 2, 0))
    result[hole] = smooth[hole]
    grain = rgb.astype(np.float32) - ni.gaussian_filter(rgb.astype(np.float32), (3, 3, 0))
    shifted = np.roll(np.roll(grain, -120, axis=0), 260, axis=1)
    result[hole] += shifted[hole] * 0.3
    return np.clip(result, 0, 255)


def rig_sprite(rgb, mask):
    labels, count = ni.label(mask > 0.08, structure=np.ones((3, 3)))
    if count > 1:
        sizes = ni.sum(mask > 0.08, labels, range(1, count + 1))
        keep = np.isin(labels, [i + 1 for i, size in enumerate(sizes) if size >= max(sizes) * 0.15])
        mask = mask * ni.binary_dilation(keep, iterations=2)
    rows = np.where((mask > 0.01).any(1))[0]
    columns = np.where((mask > 0.01).any(0))[0]
    y0, y1 = int(rows[0]), int(rows[-1]) + 1
    x0, x1 = int(columns[0]), int(columns[-1]) + 1
    rgba = np.dstack([rgb, (mask * 255)]).astype(np.uint8)[y0:y1, x0:x1]
    image = Image.fromarray(rgba, "RGBA")
    size = (max(round((x1 - x0) * RIG_SCALE), 1), max(round((y1 - y0) * RIG_SCALE), 1))
    return {"image": image.resize(size, Image.LANCZOS), "offset": (x0, y0)}


def rig_frames(master):
    rgb = master[..., :3].astype(np.float32)
    alpha = master[..., 3].astype(np.float32) / 255
    solid = master[..., 3] > 200
    index = ni.distance_transform_edt(~solid, return_distances=False, return_indices=True)
    bled = rgb[index[0], index[1]]
    wing_left = polygon(WING_LEFT, DISPLAY, 1.6)
    wing_right = polygon(WING_RIGHT, DISPLAY, 1.6)
    head = polygon(NECK + HEAD, DISPLAY, 4)
    ear = polygon(EAR, DISPLAY, 1.6)
    limbs = {
        "armNear": polygon(ARM_NEAR, feather=5), "pawNear": polygon(PAW_NEAR, feather=5),
        "armFar": polygon(ARM_FAR, feather=5), "pawFar": polygon(PAW_FAR, feather=5),
        "thighFar": polygon(THIGH_FAR, feather=4), "footFar": polygon(FOOT_FAR, feather=3),
        "thighNear": ellipse(*THIGH_NEAR, feather=5),
    }
    removed = np.max(np.stack(list(limbs.values()) + [polygon(TAIL, feather=1), polygon(TAIL_SHADOWED, feather=2), ellipse(690, 1170, 135, 60)]), 0)
    hole = removed > 0.35
    green = (bled[..., 1] > bled[..., 0] + 2)
    filled = fill_hole(bled, hole | ni.binary_dilation(green, iterations=6))
    filled = np.where((hole | ni.binary_dilation(green, iterations=6))[..., None] & hole[..., None], filled, bled)
    torso = np.maximum(polygon(TORSO, feather=10), ellipse(745, 1050, 235, 125, feather=10))
    grain = ni.gaussian_filter(np.random.default_rng(7).random(SIZE), 2.2)
    torso = np.clip((torso + (grain - grain.mean()) * 2.4 - 0.5) / 0.2 + 0.5, 0, 1)
    torso = np.maximum(torso, ni.binary_erosion(ni.binary_fill_holes(torso > 0.5), iterations=14).astype(np.float32))
    filled[..., 1] = np.where(hole, np.minimum(filled[..., 1], filled[..., 0] * 0.95), filled[..., 1])
    filled[..., 2] = np.where(hole, np.minimum(filled[..., 2], filled[..., 0] * 0.84), filled[..., 2])
    cleared = np.clip(ni.maximum_filter(removed, 9) * 2, 0, 1)
    keep = alpha * (1 - cleared) + torso * cleared
    body = np.clip(keep, 0, 1) * (1 - wing_left) * (1 - wing_right) * np.maximum(1 - head, polygon(NECK_PATCH, DISPLAY, 12))
    core = ni.binary_opening(body > 0.3, iterations=8)
    body = body * ni.binary_dilation(core, iterations=10)
    frames = {
        "wingFar": rig_sprite(bled, np.maximum(wing_left, polygon(WING_LEFT_ROOT, DISPLAY, 3)) * alpha * (1 - head)),
        "wingNear": rig_sprite(bled, np.maximum(wing_right, polygon(WING_RIGHT_ROOT, DISPLAY, 3)) * alpha * (1 - head)),
        "body": rig_sprite(filled, body),
        "ear": rig_sprite(bled, np.maximum(ear, polygon(EAR_ROOT, DISPLAY, 3)) * alpha),
        "head": rig_sprite(bled, head * (1 - ear) * alpha),
    }
    straight, straight_alpha = straight_art()
    for name, mask in limbs.items():
        if name != "thighNear":
            frames[name] = rig_sprite(bled, mask * alpha)
    frames["thighNear"] = rig_sprite(straight, limbs["thighNear"] * straight_alpha)
    frames["footNear"] = rig_sprite(straight, polygon(FOOT_NEAR, feather=3) * straight_alpha)
    for name, (cx, cy, rx, ry) in EYES.items():
        frames[name] = rig_sprite(bled, ellipse(cx, cy, rx + 8, ry + 8) * alpha)
    strip, tail_data = tail_strip(straight, straight_alpha)
    frames["tailStrip"] = {"image": strip, "offset": (0, 0)}
    frames["__tail"] = tail_data
    return frames


def background_mask(pixels):
    high = pixels.max(2)
    low = pixels.min(2)
    chroma = high - low
    near = (chroma < 9) & (low > 205)
    labels, _ = ni.label(near)
    edge = np.concatenate([labels[0], labels[-1], labels[:, 0], labels[:, -1]])
    background = np.isin(labels, edge[edge > 0])
    white = (chroma < 8) & (low > 234)
    pockets, count = ni.label(white)
    for index in range(1, count + 1):
        pocket = pockets == index
        area = int(pocket.sum())
        if area < 10:
            continue
        ring = ni.binary_dilation(pocket, iterations=2) & ~pocket
        if area >= 40 or low[ring].mean() > 150:
            background |= pocket
    return ni.binary_opening(~background, iterations=1)


def prop_mask(rgb, kind):
    red, green, blue = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    chroma = rgb.max(2) - rgb.min(2)
    cream = (red > 190) & (green > 165) & (blue > 125) & (red >= green) & (green >= blue) & (chroma < 90)
    if kind == "solid":
        return ~cream
    if kind == "cover":
        return (rgb.max(2) < 125) | (green > red + 6)
    if kind == "steel":
        return (rgb.max(2) < 190) & (chroma < 34)
    if kind == "gold":
        return (red > 200) & (green > 150) & (blue < 130)
    if kind == "red":
        return (red > green + 45) & (red > 150)
    if kind == "hot":
        return (red > 200) & (red - blue > 110)
    if kind == "teal":
        return (blue > red + 8) | (green > red + 40)
    return np.ones_like(cream)


def cut_prop(rgb, mask):
    rows = np.where(mask.any(1))[0]
    columns = np.where(mask.any(0))[0]
    y0, y1 = max(int(rows[0]) - 3, 0), min(int(rows[-1]) + 4, mask.shape[0])
    x0, x1 = max(int(columns[0]) - 3, 0), min(int(columns[-1]) + 4, mask.shape[1])
    crop = mask[y0:y1, x0:x1]
    color = rgb[y0:y1, x0:x1]
    size = (round((x1 - x0) * PROP_DENSITY), round((y1 - y0) * PROP_DENSITY))
    field = np.asarray(Image.fromarray(ni.gaussian_filter(crop.astype(np.float32), 0.8)).resize(size, Image.BICUBIC))
    alpha = np.clip((field - 0.5) / 0.28, 0, 1)
    solid = ni.binary_erosion(crop, iterations=1)
    if not solid.any():
        solid = crop
    index = ni.distance_transform_edt(~solid, return_distances=False, return_indices=True)
    bled = Image.fromarray(color[index[0], index[1]].astype(np.uint8), "RGB").resize(size, Image.LANCZOS)
    bled = bled.filter(ImageFilter.UnsharpMask(radius=1.1, percent=70, threshold=2))
    return Image.merge("RGBA", (*bled.split(), Image.fromarray((alpha * 255).astype(np.uint8))))


def prop_frames(sheet):
    cells = {}
    index = 0
    for row, (top, bottom) in enumerate(ROWS):
        for left, right in COLUMNS[row]:
            cells[NAMES[index]] = (left - 3, top, right + 3, bottom)
            index += 1
    frames = {}
    for name, (pose, area, kind) in PROPS.items():
        x0, y0, x1, y1 = cells[pose]
        rgb = sheet[y0:y1, x0:x1]
        foreground = background_mask(rgb)
        rows = np.where(foreground.any(1))[0]
        columns = np.where(foreground.any(0))[0]
        oy, ox = int(rows[0]), int(columns[0])
        region = Image.new("L", (rgb.shape[1], rgb.shape[0]), 0)
        draw = ImageDraw.Draw(region)
        if len(area) == 4 and not isinstance(area[0], tuple):
            draw.rectangle([area[0] + ox, area[1] + oy, area[2] + ox, area[3] + oy], fill=255)
        else:
            draw.polygon([(px + ox, py + oy) for px, py in area], fill=255)
        mask = (np.asarray(region) > 0) & foreground & prop_mask(rgb, kind)
        mask = ni.binary_closing(mask, iterations=2) & (np.asarray(region) > 0)
        mask = ni.binary_fill_holes(mask)
        if kind == "cover":
            wide = np.pad(mask, 12)
            sealed = ni.binary_fill_holes(ni.binary_closing(wide, iterations=9))[12:-12, 12:-12]
            gaps = sealed & ~mask
            index = ni.distance_transform_edt(~mask, return_distances=False, return_indices=True)
            rgb = np.where(gaps[..., None], rgb[index[0], index[1]], rgb)
            mask = sealed
        if kind == "solid":
            high, low = rgb.max(2), rgb.min(2)
            cream = (rgb[..., 0] > 175) & (rgb[..., 1] > 150) & (low > 110) & (high - low < 95)
            edges, found = ni.label(mask & cream)
            rim = ni.binary_dilation(~mask, iterations=1)
            touching = np.unique(edges[rim & (edges > 0)])
            mask &= ~np.isin(edges, touching)
            mask = ni.binary_opening(mask, iterations=1)
        labels, count = ni.label(mask, structure=np.ones((3, 3)))
        sizes = ni.sum(mask, labels, range(1, count + 1))
        keep = np.isin(labels, [i + 1 for i, size in enumerate(sizes) if size >= max(sizes) * 0.12])
        frames[name] = cut_prop(rgb, keep)
    return frames


def clean_frames(sheet):
    frames = {}
    for name, (points, kind) in CLEAN.items():
        shape = Image.new("L", (sheet.shape[1], sheet.shape[0]), 0)
        ImageDraw.Draw(shape).polygon(points, fill=255)
        region = np.asarray(shape) > 0
        rows = np.where(region.any(1))[0]
        columns = np.where(region.any(0))[0]
        y0, y1, x0, x1 = int(rows[0]), int(rows[-1]) + 1, int(columns[0]), int(columns[-1]) + 1
        part = sheet[y0:y1, x0:x1].astype(np.float32)
        inside = region[y0:y1, x0:x1]
        alpha = part[..., 3] * inside
        rgb = part[..., :3].copy()
        red, green, blue = rgb[..., 0], rgb[..., 1], rgb[..., 2]
        if kind == "green":
            alpha = alpha * ni.binary_dilation(green - red > 40, iterations=1)
        if kind == "redline":
            line = ni.binary_dilation((red > green + 40) & (alpha > 60), iterations=1)
            alpha = alpha * largest_shape(ni.binary_fill_holes(line))
        if kind == "hot":
            hot = (red > 215) & ((blue < 95) | (green > 190)) & (alpha > 96)
            hot = ni.binary_closing(hot, iterations=2)
            labels, count = ni.label(hot)
            sizes = ni.sum(hot, labels, range(1, count + 1))
            alpha = alpha * ni.gaussian_filter((labels == int(np.argmax(sizes)) + 1).astype(np.float32), 0.6)
        if kind == "cloud":
            foreign = ni.binary_dilation((red - blue > 16) | (green - red > 12), iterations=2)
            white = (alpha > 96) & ~foreign
            labels, count = ni.label(ni.binary_opening(white, iterations=1))
            sizes = ni.sum(labels > 0, labels, range(1, count + 1))
            shape = np.pad(labels == int(np.argmax(sizes)) + 1, 16)
            shape = ni.binary_fill_holes(ni.binary_closing(shape, iterations=11))[16:-16, 16:-16]
            hole = shape & ~white
            solid = shape & ~hole
            index = ni.distance_transform_edt(~solid, return_distances=False, return_indices=True)
            patched = ni.gaussian_filter(rgb[index[0], index[1]], (2, 2, 0))
            rgb = np.where(hole[..., None], patched, rgb)
            soft = ni.gaussian_filter(shape.astype(np.float32), 0.7)
            alpha = np.where(hole, 255, alpha) * soft
        image = Image.fromarray(np.dstack([rgb, alpha]).astype(np.uint8), "RGBA")
        frames[name] = image.crop(image.getchannel("A").point(lambda value: 255 if value > 24 else 0).getbbox())
    return frames


def largest_shape(mask):
    labels, count = ni.label(mask)
    sizes = ni.sum(mask, labels, range(1, count + 1))
    return labels == int(np.argmax(sizes)) + 1


def source_polygon(points, shape):
    image = Image.new("L", (shape[1], shape[0]), 0)
    ImageDraw.Draw(image).polygon(points, fill=255)
    return np.asarray(image) > 0


def finish_hires(rgb, alpha):
    image = Image.fromarray(np.dstack([rgb, alpha]).clip(0, 255).astype(np.uint8), "RGBA")
    image = image.crop(image.getchannel("A").point(lambda value: 255 if value > 16 else 0).getbbox())
    return image.resize((round(image.width * HIRES_SCALE), round(image.height * HIRES_SCALE)), Image.LANCZOS)


def hires_frames():
    frames = {}
    source = np.asarray(Image.open(ART / "magnify.webp").convert("RGBA")).astype(np.float32)
    rgb, alpha = source[..., :3], source[..., 3]
    red, green, blue = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    region = source_polygon([(1085, 470), (1440, 470), (1440, 830), (1150, 830), (1040, 890), (985, 890), (985, 860), (1085, 740)], alpha.shape)
    glass = ((rgb.max(2) < 150) & (rgb.max(2) - rgb.min(2) < 40)) | (blue > red + 12) | (green > red + 20)
    shape = largest_shape(ni.binary_fill_holes(ni.binary_closing(glass & region & (alpha > 40), iterations=3)))
    frames["magnifier"] = finish_hires(rgb, alpha * ni.gaussian_filter(shape.astype(np.float32), 0.8))

    source = np.asarray(Image.open(ART / "binoculars.webp").convert("RGB")).astype(np.float32)
    red, green, blue = source[..., 0], source[..., 1], source[..., 2]
    region = source_polygon([(885, 395), (1460, 395), (1460, 670), (885, 670)], red.shape)
    lens = ((source.max(2) < 135) & (source.max(2) - source.min(2) < 45)) | (green > red + 25)
    shape = largest_shape(ni.binary_fill_holes(ni.binary_closing(lens & region, iterations=3)))
    shape = ni.binary_opening(shape, iterations=2)
    frames["binoculars"] = finish_hires(source, ni.gaussian_filter(shape.astype(np.float32), 0.9) * 255)

    source = np.asarray(Image.open(ART / "globe.webp").convert("RGBA")).astype(np.float32)
    rgb, alpha = source[..., :3], source[..., 3]
    red, green, blue = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    yy, xx = np.ogrid[:alpha.shape[0], :alpha.shape[1]]
    sphere = np.hypot(xx - 865, yy - 740) < 282
    rings = source_polygon([(365, 690), (600, 690), (660, 715), (660, 925), (365, 925)], alpha.shape) | source_polygon([(1040, 480), (1330, 480), (1330, 925), (1040, 925)], alpha.shape)
    fur = ni.binary_dilation((red - blue > 18) & (red > green), iterations=3)
    covered = source_polygon([(570, 800), (660, 770), (730, 720), (760, 640), (830, 545), (1030, 525), (1030, 440), (570, 440)], alpha.shape)
    hole = sphere & (fur | covered)
    rows, columns = np.mgrid[:alpha.shape[0], :alpha.shape[1]]
    turned = rgb[np.clip(2 * 740 - rows, 0, alpha.shape[0] - 1), np.clip(2 * 865 - columns, 0, alpha.shape[1] - 1)]
    rgb = np.where(hole[..., None], turned, rgb)
    alpha = np.where(hole, 255, alpha) * ni.gaussian_filter((sphere | (rings & ~fur)).astype(np.float32), 1)
    frames["globe"] = finish_hires(rgb, alpha)

    source = np.asarray(Image.open(ART / "shield.webp").convert("RGBA")).astype(np.float32)
    rgb, alpha = source[..., :3], source[..., 3]
    red, green, blue = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    region = source_polygon([(880, 450), (1160, 330), (1400, 445), (1398, 620), (1330, 770), (1170, 952), (1150, 952), (915, 728), (885, 600)], alpha.shape)
    metal = ((red > 205) & (blue < 70) & (green > 130)) | ((green > red + 18) & (green > blue + 8))
    shape = largest_shape(ni.binary_fill_holes(ni.binary_closing(metal & region & (alpha > 96), iterations=4)))
    shape = ni.binary_opening(shape, iterations=5)
    top, bottom, inner = 552, 742, 1003
    left_at = lambda row: int(np.where(shape[row])[0][0])
    upper, lower = left_at(top), left_at(bottom)
    panel_at = lambda row: int(np.where(((green - red) > 18)[row] & shape[row])[0][0])
    panel_upper, panel_lower = panel_at(top), panel_at(bottom)
    paw = np.zeros(alpha.shape, np.float32)
    paw[top + 8:bottom - 8, 860:inner + 3] = 1
    paw = ni.gaussian_filter(paw, 3)
    mended = shape.copy()
    patched = rgb.copy()
    for row in range(top, bottom):
        target = upper + (lower - upper) * (row - top) / (bottom - top)
        right = int(np.where(shape[row])[0][-1])
        source_columns = np.clip((right + target - np.arange(alpha.shape[1])).astype(int), 0, alpha.shape[1] - 1)
        patched[row] = rgb[row, source_columns]
        mended[row, :inner] = shape[row, source_columns][:inner]
        edge = round(panel_upper + (panel_lower - panel_upper) * (row - top) / (bottom - top))
        ramp = np.clip((np.arange(inner + 6 - edge) - 2) / 16, 0, 1)[:, None] * 0.5 + 0.5
        patched[row, edge:inner + 6] = rgb[row, inner + 6] * ramp
    rgb = rgb * (1 - paw[..., None]) + patched * paw[..., None]
    shape = ni.gaussian_filter(mended.astype(np.float32), 3) > 0.5
    solid = ni.binary_erosion(shape, iterations=3)
    index = ni.distance_transform_edt(~solid, return_distances=False, return_indices=True)
    rgb = np.where(solid[..., None], rgb, rgb[index[0], index[1]])
    frames["shield"] = finish_hires(rgb, ni.gaussian_filter(shape.astype(np.float32), 0.9) * 255)
    return frames


def straight_art():
    source = Image.open(ART / "straight-tail.webp").convert("RGBA")
    scale, dx, dy = STRAIGHT_ALIGN
    image = source.transform(source.size, Image.AFFINE, (1 / scale, 0, -dx / scale, 0, 1 / scale, -dy / scale), Image.BICUBIC)
    pixels = np.asarray(image).astype(np.float32)
    rgb, alpha = pixels[..., :3], pixels[..., 3] / 255
    solid = alpha > 0.8
    index = ni.distance_transform_edt(~solid, return_distances=False, return_indices=True)
    rgb = rgb[index[0], index[1]] * np.array(STRAIGHT_GRADE, np.float32)
    greenness = np.clip((rgb[..., 1] - rgb[..., 0]) / 50, 0, 1)[..., None]
    return np.clip(rgb + greenness * np.array(STRAIGHT_GREEN, np.float32), 0, 255), alpha


def tail_strip(rgb, alpha):
    region = polygon(TAIL_SOURCE, feather=2) * alpha
    start = np.array(TAIL_AXIS[0], np.float64)
    end = np.array(TAIL_AXIS[1], np.float64)
    total = float(np.hypot(*(end - start)))
    tangent = (end - start) / total
    normal = np.array([-tangent[1], tangent[0]])
    columns = round(total * TAIL_DENSITY)
    rows = round((TAIL_OUTER + TAIL_INNER) * TAIL_DENSITY)
    along = np.linspace(0, total, columns)
    offsets = np.linspace(TAIL_OUTER, -TAIL_INNER, rows)
    gridx = start[0] + along[None, :] * tangent[0] + offsets[:, None] * normal[0]
    gridy = start[1] + along[None, :] * tangent[1] + offsets[:, None] * normal[1]
    planes = [ni.map_coordinates(rgb[..., channel], [gridy, gridx], order=1, mode="nearest") for channel in range(3)]
    cover = ni.map_coordinates(region * 255, [gridy, gridx], order=1, mode="constant")
    labels, count = ni.label(cover > 40, structure=np.ones((3, 3)))
    sizes = ni.sum(cover > 40, labels, range(1, count + 1))
    cover = cover * ni.binary_dilation(labels == int(np.argmax(sizes)) + 1, iterations=2)
    planes.append(cover)
    strip = Image.fromarray(np.dstack(planes).clip(0, 255).astype(np.uint8), "RGBA")
    points = np.array(TAIL_CURL, np.float64)
    dense = []
    for index in range(len(points) - 1):
        p0 = points[max(index - 1, 0)]
        p1, p2 = points[index], points[index + 1]
        p3 = points[min(index + 2, len(points) - 1)]
        for u in np.linspace(0, 1, 40, endpoint=False):
            dense.append(0.5 * (2 * p1 + (p2 - p0) * u + (2 * p0 - 5 * p1 + 4 * p2 - p3) * u * u + (3 * p1 - p0 - 3 * p2 + p3) * u ** 3))
    dense.append(points[-1])
    dense = np.array(dense)
    lengths = np.concatenate([[0], np.cumsum(np.hypot(*np.diff(dense, axis=0).T))])
    samples = np.linspace(0, lengths[-1], TAIL_SLICES + 1)
    curve = np.dstack([np.interp(samples, lengths, dense[:, 0]), np.interp(samples, lengths, dense[:, 1])])[0]
    return strip, {"curve": [[round(float(x), 1), round(float(y), 1)] for x, y in curve], "outer": TAIL_OUTER, "inner": TAIL_INNER, "length": round(total, 1)}


def pack(images):
    order = sorted(images, key=lambda name: -images[name].height)
    width = 1400
    x = y = 0
    shelf = 0
    places = {}
    for name in order:
        image = images[name]
        if x + image.width + 2 > width:
            x = 0
            y += shelf + 2
            shelf = 0
        places[name] = (x, y)
        x += image.width + 2
        shelf = max(shelf, image.height)
    atlas = Image.new("RGBA", (width, y + shelf), (0, 0, 0, 0))
    for name, place in places.items():
        atlas.paste(images[name], place)
    return atlas, places


def stamp():
    digest = hashlib.sha256()
    for name in ("dragon.js", "atlas.json", "atlas.webp"):
        digest.update((OUT / name).read_bytes())
    version = digest.hexdigest()[:10]
    text = SPIDER.read_text(encoding="utf-8")
    SPIDER.write_text(
        re.sub(r'const DRAGON_VERSION = "[0-9a-f]+";', f'const DRAGON_VERSION = "{version}";', text),
        encoding="utf-8",
    )
    return version


def main():
    if STAMP_ONLY:
        print(stamp())
        return
    sheet = np.asarray(Image.open(ART / "poses.webp").convert("RGB")).astype(np.int32)
    master = np.asarray(Image.open(ART / "master.webp").convert("RGBA"))
    rig = rig_frames(master)
    tail_data = rig.pop("__tail")
    props = prop_frames(sheet)
    clean = clean_frames(np.asarray(Image.open(ART / "poses-clean.webp").convert("RGBA")))
    images = {name: frame["image"] for name, frame in rig.items()}
    images.update(props)
    images.update(clean)
    hires = hires_frames()
    images.update(hires)
    atlas, places = pack(images)
    OUT.mkdir(parents=True, exist_ok=True)
    rig_data = {}
    for name, frame in rig.items():
        x, y = places[name]
        image = frame["image"]
        ox, oy = frame["offset"]
        rig_data[name] = {"x": x, "y": y, "w": image.width, "h": image.height, "ox": ox, "oy": oy}
    prop_data = {}
    unit = SHEET_TO_MASTER / PROP_DENSITY
    for name, image in props.items():
        x, y = places[name]
        prop_data[name] = {"x": x, "y": y, "w": image.width, "h": image.height, "k": round(unit, 3)}
    for name, image in clean.items():
        x, y = places[name]
        prop_data[name] = {"x": x, "y": y, "w": image.width, "h": image.height, "k": round(SHEET_TO_MASTER * CLEAN_SCALE, 3)}
    for name, image in hires.items():
        x, y = places[name]
        prop_data[name] = {"x": x, "y": y, "w": image.width, "h": image.height, "k": round(HIRES_MASTER / HIRES_SCALE, 3)}
    meta = {
        "version": 2, "rig": rig_data, "props": prop_data, "rigScale": RIG_SCALE, "units": round(88 / 1215, 5),
        "joints": JOINTS, "tail": tail_data, "eyes": {name: list(value) for name, value in EYES.items()},
    }
    atlas.save(OUT / "atlas.webp", "WEBP", quality=84, alpha_quality=90, method=6)
    (OUT / "atlas.json").write_text(json.dumps(meta, separators=(",", ":"), sort_keys=True) + "\n", encoding="utf-8")
    if DEBUG:
        DEBUG.mkdir(parents=True, exist_ok=True)
        for name, image in images.items():
            image.save(DEBUG / f"{name}.png")
        atlas.save(DEBUG / "atlas.png")
    print(atlas.size, (OUT / "atlas.webp").stat().st_size, stamp())


if __name__ == "__main__":
    main()
