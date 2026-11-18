#!/usr/bin/env python3
# The Vita's animated backdrop (../backdrop.c) per theme, from the app's own Kotlin: each theme's page and blobs
# (theme/Theme.kt's ThemeSpec), its motif (theme/ThemeMotifs.kt), and the night versions DaisyBackdrop.kt works out at
# run time (NIGHT_PAGE themed by the theme's hue, nightGlow blobs; a dark page of its own deepened), worked out here with
# NightColors.kt's own HSL arithmetic. Writes vita/themes_gen.c; --check fails while it's stale.
#
# Also the Vita's own themes (VITA_THEMES: MILES, the user's), which the app doesn't have: their backdrop beside the app's,
# and their windows (3ds/core's pd_themes, generated from the app by 3ds/tools/gen_games.py) worked out from a tinted
# theme's (CHERRY's) the way the app tints: CHERRY's colours turned to the theme's hue, its greys to its wash. The Vita
# builds 3ds/core/pd_games_gen.c with its pd_themes renamed and links vita/themes_looks_gen.c's list (the app's and these).
#
#   python3 vita/tools/gen_themes.py [--check]
import math, os, re, sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
UI = os.path.join(REPO, 'app/src/main/kotlin/com/pokedaisy/app/companion/ui')
OUT = os.path.join(REPO, 'vita/themes_gen.c')
OUT_LOOKS = os.path.join(REPO, 'vita/themes_looks_gen.c')
GAMES_GEN = os.path.join(REPO, '3ds/core/pd_games_gen.c')
ICONS_GEN = os.path.join(REPO, '3ds/core/pd_icons_gen.c')

# The Vita's own themes. MILES: white windows with orange accents (the user's, 2026-11-18); its motif the PokeDaisy mark
# in orange.
VITA_THEMES = [
    dict(id=60, label='MILES', hue=28.0, wash=0.22, page=0xFFFDFA, blobs=[0xFFD1A6, 0xFFEBD8, 0xFFBF8A], motif='MILES',
         dark_page=False, value=0xE8650A, value_night=0xFF9A3C, game_wash=0xFFE6CC,
         # Its accents: the windows' borders, the tab chips, titles and the hint bar orange, their fills white. By the
         # look's colour tokens in order (pd_look: title[2], list[5], chip[2], fieldMove[2] layers, then titleFill ...
         # hintShadow, PD_NO_SHADOW not counted): {index: (light, dark)}; None keeps the tinted colour.
         accents={0: (0xD9661A, None), 1: (0xFFD8B6, 0x9C4A14), 2: (0x4A2A12, None), 3: (0xFFB574, 0xE07A28),
                  4: (0xF0842A, 0x7A3A12), 5: (0xFFEADB, None), 6: (0xFFFFFF, None), 7: (0xD9661A, None),
                  8: (0xFFD8B6, 0x9C4A14), 11: (0xFFFFFF, None), 12: (0x8A3D06, 0xFFE2CC), 13: (0xFFD9BC, None),
                  14: (0xF6F3F0, None), 16: (0x4D433B, None), 17: (0xD8CEC6, None), 28: (0x4A2A12, None),
                  29: (0xFFB574, 0xE07A28), 32: (0xFFFFFF, None), 33: (0xF6F3F0, None), 36: (0xEBD9CB, None),
                  41: (0xE8741A, 0xB4561A)}),
]

theme_kt = open(os.path.join(UI, 'theme/Theme.kt')).read()
motifs_kt = open(os.path.join(UI, 'theme/ThemeMotifs.kt')).read()
backdrop_kt = open(os.path.join(UI, 'DaisyBackdrop.kt')).read()


def colors(text):
    return [int(c[2:], 16) & 0xFFFFFF for c in re.findall(r'Color\((0x[0-9A-Fa-f]{8})\)', text)]


# --- NightColors.kt's arithmetic ---
def hsl(c):
    r, g, b = ((c >> 16) & 255) / 255, ((c >> 8) & 255) / 255, (c & 255) / 255
    mx, mn = max(r, g, b), min(r, g, b)
    l = (mx + mn) / 2
    d = mx - mn
    if d == 0:
        return 0.0, 0.0, l
    s = d / (1 - abs(2 * l - 1))
    if mx == r:
        h = ((g - b) / d) % 6
    elif mx == g:
        h = (b - r) / d + 2
    else:
        h = (r - g) / d + 4
    return h * 60, s, l


def rgb(h, s, l):
    c = (1 - abs(2 * l - 1)) * s
    x = c * (1 - abs((h / 60) % 2 - 1))
    m = l - c / 2
    k = min(max(int(h / 60), 0), 5)
    r, g, b = [(c, x, 0), (x, c, 0), (0, c, x), (0, x, c), (x, 0, c), (c, 0, x)][k]
    to = lambda v: int(round(min(max(v + m, 0), 1) * 255))
    return (to(r) << 16) | (to(g) << 8) | to(b)


def themed(c, hue, wash):
    if wash <= 0:
        return c
    _, s, l = hsl(c)
    if s > 0.5:
        return c
    l2 = 0.95 + (l - 0.95) * 0.4 if l > 0.95 else l
    edge = abs(2 * l2 - 1)
    reach = (1 - edge ** 3) * (0.55 + 0.6 * l2 if l2 < 0.5 else 1)
    return rgb(hue, max(s * 0.5, wash * reach), l2)


def night_glow(c):
    h, s, _ = hsl(c)
    return rgb(h, s * 0.6, 0.26)


def lerp(a, b, t):
    out = 0
    for sh in (16, 8, 0):
        x, y = (a >> sh) & 255, (b >> sh) & 255
        out |= int(round(x + (y - x) * t)) << sh
    return out


# --- DaisyBackdrop.kt's defaults ---
def val(name):
    m = re.search(r'private val ' + name + r' = (.*?)\n(?=\S|\n)', backdrop_kt, re.S)
    return m.group(1)

NIGHT_PAGE = colors(val('NIGHT_PAGE'))[0]
DAY_BLOBS = colors(val('BLOBS'))
NIGHT_BLOBS = colors(val('NIGHT_BLOBS'))

# --- the motifs ---
motifs = {}
for m in re.finditer(r'val (\w+) = ThemeMotif\(\s*listOf\((.*?)\),\s*mapOf\((.*?)\),\s*\)', motifs_kt, re.S):
    rows = re.findall(r'"([^"]*)"', m.group(2))
    palette = {k: int(v[2:], 16) for k, v in re.findall(r"'(.)' to Color\((0x[0-9A-Fa-f]{8})\)", m.group(3))}
    motifs[m.group(1)] = (rows, palette)

# The PokeDaisy mark, orange (MILES): PD_LOGO's rows with its reds turned orange.
icons = open(ICONS_GEN).read()
logo_rows = re.findall(r'"([^"]*)"', re.search(r'logoRows\[\] = \{(.*?)\};', icons, re.S).group(1))
logo_cols = [int(c, 16) for c in re.findall(r'0x([0-9A-Fa-f]{6})', re.search(r'logoColors\[\] = \{(.*?)\};', icons).group(1))]
logo_keys = re.search(r'PD_LOGO = \{ 32, 32, logoRows, "(\w+)"', icons).group(1)
ORANGE = {'R': 0xF5841F, 'D': 0xC0580A, 'H': 0xFFC487}
motifs['MILES'] = (logo_rows, {k: 0xFF000000 | ORANGE.get(k, c) for k, c in zip(logo_keys, logo_cols)})

# --- the themes ---
default_page = int(re.search(r'val page: Color = Color\((0x\w{8})\)', theme_kt).group(1)[2:], 16) & 0xFFFFFF
default_blobs = colors(re.search(r'val blobs: List<Color> = listOf\((.*?)\),?\n', theme_kt).group(1))
themes = []
for m in re.finditer(r'ThemeSpec\(\s*(\w+|\d+),(.*?)\)(?=,\s*\n\s*(?:ThemeSpec|\)))', theme_kt, re.S):
    ident, body = m.group(1), m.group(2)
    tid = 8 if ident == 'DAISY_THEME_ID' else int(ident)
    label = re.search(r'"([^"]+)"', body).group(1)
    hue = re.search(r'hue = ([\d.]+)f', body)
    wash = re.search(r'wash = ([\d.]+)f', body)
    page = re.search(r'page = Color\((0x\w{8})\)', body)
    blobs = re.search(r'blobs = listOf\(((?:\s*Color\(0x\w{8}\),?)+)\s*\)', body, re.S)
    motif = re.search(r'motif = ThemeMotifs\.(\w+)', body)
    dark_page = 'darkPage = true' in body
    t = dict(id=tid, label=label, hue=float(hue.group(1)) if hue else None, wash=float(wash.group(1)) if wash else 0.0,
             page=int(page.group(1)[2:], 16) & 0xFFFFFF if page else default_page,
             blobs=colors(blobs.group(1)) if blobs else default_blobs, motif=motif.group(1) if motif else None,
             dark_page=dark_page)
    plain = t['hue'] is None and not dark_page
    # DaisyBackdrop's page / colors at night.
    if dark_page:
        t['night_page'] = lerp(t['page'], NIGHT_PAGE, 0.4)
        t['night_blobs'] = t['blobs']
    elif plain:
        t['night_page'] = NIGHT_PAGE
        t['night_blobs'] = NIGHT_BLOBS
    else:
        t['night_page'] = themed(NIGHT_PAGE, t['hue'], t['wash'])
        t['night_blobs'] = [night_glow(c) for c in t['blobs']]
    if plain:
        t['blobs'] = DAY_BLOBS
    themes.append(t)
assert any(t['id'] == 8 for t in themes) and len(themes) >= 16, [t['id'] for t in themes]
for v in VITA_THEMES:
    t = dict(v)
    t['night_page'] = themed(NIGHT_PAGE, t['hue'], t['wash'])
    t['night_blobs'] = [night_glow(c) for c in t['blobs']]
    themes.append(t)
for t in themes:
    assert t['motif'] is None or t['motif'] in motifs, t['motif']

# --- C ---
lines = ['// Generated by vita/tools/gen_themes.py from the app\'s theme/Theme.kt, theme/ThemeMotifs.kt and DaisyBackdrop.kt.',
         '// Don\'t edit: change the Kotlin and run it again.', '#include "backdrop.h"', '']
names = sorted(motifs)
for n in names:
    rows, palette = motifs[n]
    lines.append(f'static const char* const MOTIF_{n}_ROWS[] = {{')
    lines += [f'    "{r}",' for r in rows]
    lines.append('};')
    lines.append(f'static const char MOTIF_{n}_KEYS[] = "{"".join(palette)}";')
    lines.append(f'static const uint32_t MOTIF_{n}_ARGB[] = {{ ' + ', '.join(f'0x{v:08X}' for v in palette.values()) + ' };')
    lines.append(f'static const struct bd_motif MOTIF_{n} = {{ {len(rows[0])}, {len(rows)}, MOTIF_{n}_ROWS, MOTIF_{n}_KEYS, MOTIF_{n}_ARGB }};')
    lines.append('')


def arr(cs):
    cs = (cs + [0] * 5)[:5]
    return '{ ' + ', '.join(f'0x{c:06X}' for c in cs) + ' }'


lines.append('const struct bd_theme bd_themes[] = {')
for t in themes:
    motif = f'&MOTIF_{t["motif"]}' if t['motif'] else 'NULL'
    lines.append(f'    {{ {t["id"]}, "{t["label"]}", 0x{t["page"]:06X}, {len(t["blobs"])}, {arr(t["blobs"])}, 0x{t["night_page"]:06X}, '
                 f'{len(t["night_blobs"])}, {arr(t["night_blobs"])}, {"true" if t["dark_page"] else "false"}, {motif} }},')
lines.append('};')
lines.append(f'const int bd_theme_count = {len(themes)};')
text = '\n'.join(lines) + '\n'

# --- the Vita's themes' windows (pd_themes) ---
games = open(GAMES_GEN).read()
block = re.search(r'const struct pd_theme pd_themes\[\] = \{\n(.*?)\n\};', games, re.S).group(1)
entries = re.split(r'\n(?=    \{ \d+, ")', block)
by_id = {int(re.match(r'    \{ (\d+),', e).group(1)): e for e in entries}
HEX = re.compile(r'0x([0-9A-F]{6})')


def turned(c, hue, scale):
    h, sat, l = hsl(c)
    return rgb(hue, min(1.0, sat * scale) if sat <= 0.5 else sat, l)


def look_of(v):
    daisy, cherry = by_id[8], by_id[20]
    dl, cl = daisy.split('\n')[1:], cherry.split('\n')[1:]
    out = []
    for dark, (dline, cline) in enumerate(zip(dl, cl)):
        dt, ct = HEX.findall(dline), HEX.findall(cline)
        assert len(dt) == len(ct)
        it = iter(enumerate(zip(dt, ct)))

        def swap(m):
            i, (d, c) = next(it)
            d, c = int(d, 16), int(c, 16)
            accent = v.get('accents', {}).get(i, (None, None))[dark]
            if accent is not None:
                n = accent
            elif d == c:
                n = d
            elif c == 0xC8143C:
                n = v['value']
            elif c == 0xFF6A88:
                n = v['value_night']
            else:
                n = turned(c, v['hue'], v['wash'] / 0.5)
            return f'0x{n:06X}'
        out.append(HEX.sub(swap, cline))
    head = f'    {{ {v["id"]}, {c_label(v["label"])}, 0x{v["game_wash"]:06X}, false, false,'
    return '\n'.join([head] + out)


def c_label(s):
    return '"' + s + '"'


# After the app's always-there themes, before the seasonal ones (whose header ends "seasonal = true").
first_seasonal = next(i for i, e in enumerate(entries) if e.split('\n')[0].rstrip().endswith('true,'))
looks = entries[:first_seasonal] + [look_of(v) for v in VITA_THEMES] + entries[first_seasonal:]
looks_text = '\n'.join([
    '// Generated by vita/tools/gen_themes.py: 3ds/core/pd_games_gen.c\'s themes (the app\'s) and the Vita\'s own (VITA_THEMES).',
    '// Don\'t edit: run it again. The Vita builds pd_games_gen.c with its own list renamed (pd_themes_3ds).',
    '#include "../3ds/core/pd_canvas.h"', '#include "../3ds/core/pd_style.h"', '',
    'const struct pd_theme pd_themes[] = {'] + looks + ['};', f'const int pd_theme_count = {len(looks)};', ''])
if '--check' in sys.argv and (not os.path.exists(OUT_LOOKS) or open(OUT_LOOKS).read() != looks_text):
    sys.exit('vita/themes_looks_gen.c is stale: run python3 vita/tools/gen_themes.py')
if '--check' not in sys.argv:
    open(OUT_LOOKS, 'w').write(looks_text)
if '--check' in sys.argv:
    if not os.path.exists(OUT) or open(OUT).read() != text:
        sys.exit('vita/themes_gen.c is stale: run python3 vita/tools/gen_themes.py')
else:
    open(OUT, 'w').write(text)
    print(f'{OUT}: {len(themes)} themes, {len(motifs)} motifs')
