#!/usr/bin/env python3
"""Build reversible PS5 UI overlays from the pinned, untouched game disc."""
import argparse
import ctypes
import hashlib
import json
import re
import sys
from pathlib import Path
from PIL import Image
from nfs_assets import (language, write_language, locale, write_locale, textures,
                        texture_image, replace_textures, jdlz_decode, jdlz_encode)

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT/'ps5/assets'
WHITE = '#f3f3f3'
if sys.platform == 'darwin' and Path('/opt/homebrew/lib/libcairo.2.dylib').exists():
    ctypes.CDLL('/opt/homebrew/lib/libcairo.2.dylib')
import cairosvg


def digest(data):
    return hashlib.sha256(data).hexdigest()


def svg(body, width=32, height=32):
    return f'<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="{width}" height="{height}" viewBox="0 0 {width} {height}">{body}</svg>'


def render(source, target):
    # SVG is the editable source for icons and diagram connector geometry.
    size = 256 if 'diagram' in source.name else 32
    render_size = size if size == 256 else size*4
    cairosvg.svg2png(url=str(source),write_to=str(target),output_width=render_size,output_height=render_size,unsafe=True)
    image = Image.open(target).convert('RGBA')
    if image.size != (size,size):
        image = image.resize((size,size),Image.Resampling.LANCZOS)
    image.save(target)
    return image


def icon(name):
    shape = ''
    stroke = f'fill="none" stroke="{WHITE}" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"'
    if name in ('XBOX_A','XBOX_B','XBOX_X','XBOX_Y'):
        shape += f'<circle cx="16" cy="16" r="13" fill="#111318" fill-opacity="0.85" stroke="{WHITE}" stroke-width="1.5"/>'
        geometry = {
            'XBOX_A':'<path d="M10 10L22 22M22 10L10 22"/>',
            'XBOX_B':'<circle cx="16" cy="16" r="8"/>',
            'XBOX_X':'<rect x="8.5" y="8.5" width="15" height="15"/>',
            'XBOX_Y':'<path d="M16 7L25 23H7Z"/>'}
        shape += f'<g {stroke}>{geometry[name]}</g>'
    elif name in ('XBOX_BLACK_BUTTON','XBOX_WHITE_BUTTON') or 'TRIGGER' in name:
        label = {'XBOX_BLACK_BUTTON':'R1','XBOX_WHITE_BUTTON':'L1'}.get(name)
        if label is None:
            label = 'L2' if 'LEFT_' in name else 'R2'
        shape += f'<rect x="1.5" y="6" width="29" height="20" rx="5" fill="#111318" fill-opacity="0.85" stroke="{WHITE}" stroke-width="1.6"/>'
        shape += f'<text x="16" y="21" text-anchor="middle" fill="{WHITE}" font-size="14">{label}</text>'
    elif name == 'XBOX_START':
        shape += f'<circle cx="16" cy="16" r="13" fill="#111318" fill-opacity="0.85" stroke="{WHITE}" stroke-width="1.5"/>'
        shape += f'<path d="M9 11H23M9 16H23M9 21H23" {stroke}/>'
    elif name == 'XBOX_BACK':
        shape += f'<rect x="2" y="7" width="28" height="18" rx="4" fill="#111318" fill-opacity="0.85" stroke="{WHITE}" stroke-width="1.8"/>'
        for x in range(7,27,5):
            for y in (12,17,21):
                shape += f'<circle cx="{x}" cy="{y}" r="0.7" fill="{WHITE}"/>'
    elif 'ANALOG' in name:
        label = 'L' if 'LEFTANALOG' in name else 'R'
        shape += f'<circle cx="16" cy="16" r="12" fill="#111318" fill-opacity="0.85" stroke="{WHITE}" stroke-width="1.7"/>'
        shape += f'<text x="16" y="22" text-anchor="middle" fill="{WHITE}" font-size="18">{label}</text>'
    elif 'DPAD' in name:
        shape += f'<path d="M11 3H21V11H29V21H21V29H11V21H3V11H11Z" fill="#111318" fill-opacity="0.85" stroke="{WHITE}" stroke-width="1.8"/>'
        directions = []
        if name != 'XBOX_DPAD':
            # The source uses identical two-way art for UP, DOWN and UP_DOWN,
            # and likewise for LEFT/RIGHT. Keep those aliases identical so
            # host texture fingerprints cannot choose conflicting artwork.
            directions = ['UP','DOWN'] if ('UP' in name or 'DOWN' in name) else ['LEFT','RIGHT']
        arrows = {'UP':'M16 4L12 9H20Z','DOWN':'M16 28L12 23H20Z','LEFT':'M4 16L9 12V20Z','RIGHT':'M28 16L23 12V20Z'}
        for direction in directions:
            shape += f'<path d="{arrows[direction]}" fill="{WHITE}"/>'
    else:
        raise ValueError('Unmapped icon '+name)
    return svg(shape)


def diagram():
    body = '<image x="25" y="60" width="206" height="138" xlink:href="dualsense-front.png"/>'
    # Outer endpoints match the Xbox diagram's existing FEng action labels.
    # Route leaders to DualSense controls using this port's actual input mapping.
    routes = [
        '1,38 62,38 62,65',                    # L2 / Xbox left trigger
        '1,87 83,87 83,130 93,130',            # left stick
        '1,117 112,117 112,88 128,88',         # touch pad / Xbox Back
        '1,146 38,146 38,101 63,101',           # d-pad
        '1,230 154,230 154,147 162,147 162,130', # right stick
        '255,6 217,6 217,87 193,87',           # Triangle / Xbox Y
        '255,42 192,42 192,65',                # R2 / Xbox right trigger
        '255,82 227,82 227,101 207,101',        # Circle / Xbox B
        '255,127 238,127 238,52 190,52 190,73', # R1 / Xbox Black
        '255,169 246,169 246,215 18,215 18,73 65,73', # L1 / Xbox White
        '255,211 193,211 193,115',             # Cross / Xbox A
        '255,252 178,252 178,101',             # Square / Xbox X
    ]
    for points in routes:
        body += f'<polyline points="{points}" fill="none" stroke="#aeb3b2" stroke-width="1"/>'
    for label,x in [('L2',62),('R2',192)]:
        body += f'<text x="{x}" y="57" text-anchor="middle" fill="white" font-size="9">{label}</text>'
    return svg(body,256,256)


ENGLISH = [
    'Your PS5 has insufficient space for saved data. Press Cross to continue without saving.',
    'Loading %1. Please do not close the game.',
    'Unable to load %1. Press Cross to continue.',
    'Your PS5 has insufficient space for saved data. Press Cross to continue without saving.',
    'Insufficient space in save storage %1. Press Cross to continue without saving.',
    'Are you sure you want to overwrite %1?',
    'Are you sure you want to overwrite %1 in save storage %2?',
    'Saving %1. Please do not turn off your PS5.',
    'Saving to save storage %1. Please do not close the game.',
    'Unable to save. Press Cross to continue.',
    'Are you sure you want to delete this %1?',
    'Deleting %1. Please do not turn off your PS5.',
    'Deleting %1 from save storage %2. Please do not close the game.',
    'Unable to delete. Press Cross to continue.',
    'Yes','No','PS5 console storage','Saved data',
    'Save storage %1 is unavailable. Load failed. Press Cross to continue.',
    'Save storage %1 is unavailable. Save failed. Press Cross to continue.',
    'Save storage %1 is unavailable. Delete failed. Press Cross to continue.',
]
FRENCH = [
    "Espace insuffisant pour les sauvegardes sur votre PS5. Appuyez sur Croix pour continuer sans sauvegarder.",
    'Chargement de %1. Ne fermez pas le jeu.',
    'Chargement de %1 impossible. Appuyez sur Croix pour continuer.',
    "Espace insuffisant pour les sauvegardes sur votre PS5. Appuyez sur Croix pour continuer sans sauvegarder.",
    'Espace insuffisant dans le stockage %1. Appuyez sur Croix pour continuer sans sauvegarder.',
    'Voulez-vous remplacer %1 ?',
    'Voulez-vous remplacer %1 dans le stockage %2 ?',
    "Sauvegarde de %1. N'eteignez pas votre PS5.",
    'Sauvegarde dans le stockage %1. Ne fermez pas le jeu.',
    'Sauvegarde impossible. Appuyez sur Croix pour continuer.',
    'Voulez-vous supprimer %1 ?',
    "Suppression de %1. N'eteignez pas votre PS5.",
    'Suppression de %1 du stockage %2. Ne fermez pas le jeu.',
    'Suppression impossible. Appuyez sur Croix pour continuer.',
    'Oui','Non','Stockage de la console PS5','Sauvegardes',
    'Stockage %1 indisponible. Chargement impossible. Appuyez sur Croix pour continuer.',
    'Stockage %1 indisponible. Sauvegarde impossible. Appuyez sur Croix pour continuer.',
    'Stockage %1 indisponible. Suppression impossible. Appuyez sur Croix pour continuer.',
]


def c_text(text):
    return 'u"'+''.join(c if ' ' <= c < '\x7f' and c not in '"\\' else f'\\u{ord(c):04x}' for c in text)+'"'


def recipe(changes):
    """The port's own strings as a table, so the app can apply them to a disc as it is."""
    tables = {'LANGUAGE_ENGLISH':'LANGUAGES/LANGUAGE_ENGLISH.bin','LOCALE_ENGLISH':'MEMCARD/XBOX_LOCALE_ENGLISH.loc',
              'LOCALE_FRENCH':'MEMCARD/XBOX_LOCALE_FRENCH.loc'}
    out = '/* Made by tools/build-ps5-ui.py. */\n'
    for table,path in tables.items():
        out += f'static const struct row {table}[]={{\n'
        for row in changes:
            if row['file'] == path:
                key = f"0x{row['hash']}" if 'hash' in row else row['index']
                out += f" {{{key},{c_text(row['after'])}}},\n"
        out += '};\n'
    (ROOT/'ps5/src/ui_text.inc').write_text(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game',type=Path,default=ROOT/'source/Need For Speed Underground')
    parser.add_argument('--out',type=Path,default=ROOT/'build/ps5-ui-overlay')
    args = parser.parse_args()
    source, out = args.game.resolve(), args.out.resolve()
    if out == source or source in out.parents:
        raise SystemExit('Overlay must be outside source game')
    out.mkdir(parents=True,exist_ok=True)
    previews = ROOT/'build/ui-audit/ps5'
    previews.mkdir(parents=True,exist_ok=True)
    icons_dir = ASSETS/'icons'
    icons_dir.mkdir(parents=True,exist_ok=True)
    report = {'title_id':'PPSA99025','changes':[],'files':{},'console_verified':False}
    def save(relative, data):
        original = (source/relative).read_bytes()
        path = out/relative
        path.parent.mkdir(parents=True,exist_ok=True)
        path.write_bytes(data)
        report['files'][relative] = {'source_sha256':digest(original),'sha256':digest(data),'size':len(data)}
    # Every icon, including alternate French labels, uses the actual PS5 mapping.
    path = 'GLOBAL/GLOBALB.BUN'
    data = (source/path).read_bytes()
    replacements = {}
    for row in textures(data):
        if row['name'].startswith('XBOX_'):
            native = icons_dir/(row['name']+'.svg')
            native.write_text(icon(row['name']))
            replacements[row['hash']] = render(native,previews/(row['name']+f"-{row['hash']:08x}.png"))
    patched = replace_textures(data,replacements)
    save(path,patched)
    compressed = (source/'GLOBAL/GlobalB.lzc').read_bytes()
    if jdlz_decode(compressed) != data:
        raise ValueError('GlobalB.lzc differs from GLOBALB.BUN')
    compressed_ui = jdlz_encode(patched)
    if len(compressed_ui) >= len(patched):
        raise ValueError('GlobalB must fit the in-place LZC loader')
    save('GLOBAL/GlobalB.lzc',compressed_ui)
    report['icon_count'] = len(replacements)
    native = ASSETS/'dualsense-diagram.svg'
    native.write_text(diagram())
    controller = render(native,previews/'dualsense-diagram.png')
    path = 'GLOBAL/DYNTEX.BIN'
    data = (source/path).read_bytes()
    replacements = {row['hash']:controller for row in textures(data) if row['name'].startswith('LOADING_CONTROLLER_XBOX')}
    save(path,replace_textures(data,replacements))
    report['controller_diagram_count'] = len(replacements)
    # Disc language strings are UTF-16, not Huffman-packed in this Xbox revision.
    labels = dict(language((source/'LANGUAGES/LANGUAGE_LABELS.bin').read_bytes()))
    rows = language((source/'LANGUAGES/LANGUAGE_ENGLISH.bin').read_bytes())
    updated = []
    for key,text in rows:
        name = labels.get(key,'')
        replacement = text
        if name in ('TITLE_SCREEN_START_PROMPT','TITLE_SCREEN_START_PROMPT_XBOX','TITLE_SCREEN_START_PROMPT_GC','PLAYER_1_PRESS_START','PLAYER_2_PRESS_START'):
            replacement = re.sub(r'\bSTART\b','OPTIONS',text)
        if name in ('XBOX_CONTROLLER_UNPLUGGED','PSX2_CONTROLLER_UNPLUGGED','GAMECUBE_CONTROLLER_UNPLUGGED','MU_OPTIONS_CONTROLLER_PLUGIN') or name.startswith(('WARNING_PLUGIN_PORT_','WARNING_USE_PORT_')):
            replacement = 'Reconnect your DualSense^wireless controller.^Press PS, then OPTIONS^to continue.'
        if replacement != text:
            report['changes'].append({'file':'LANGUAGES/LANGUAGE_ENGLISH.bin','label':name,'hash':f'{key:08X}','before':text,'after':replacement})
        updated.append((key,replacement))
    encoded = write_language(updated)
    if language(encoded) != updated:
        raise ValueError('Language roundtrip failed')
    save('LANGUAGES/LANGUAGE_ENGLISH.bin',encoded)
    for filename,strings in [('XBOX_LOCALE_ENGLISH.loc',ENGLISH),('XBOX_LOCALE_FRENCH.loc',FRENCH)]:
        path = 'MEMCARD/'+filename
        original = (source/path).read_bytes()
        for i,(before,after) in enumerate(zip(locale(original),strings)):
            if before != after:
                report['changes'].append({'file':path,'index':i,'before':before,'after':after})
        save(path,write_locale(original,strings))
    report['legacy_language_strings'] = [{'hash':f'{key:08X}','label':labels.get(key,''),'text':text} for key,text in updated if re.search(r'Xbox|memory.card|PlayStation.{0,2}2|Nintendo|GameCube',text,re.I)]
    report['remaining_internal_identifiers'] = 'Xbox texture names/hashes, XBE revision, MEMCARD filenames and runtime APIs are retained for binary compatibility. Legacy PS2/GameCube-specific language entries are recorded separately; their visibility on PS5 has not been verified. Original credits are unchanged.'
    # Ensure the source disc was never altered, including hard-linked staging files.
    for relative,details in report['files'].items():
        if digest((source/relative).read_bytes()) != details['source_sha256']:
            raise ValueError('Source game changed')
    recipe(report['changes'])
    (out/'manifest.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
    print(json.dumps({'files':len(report['files']),'icons':report['icon_count'],'diagrams':report['controller_diagram_count'],'text_changes':len(report['changes']),'out':str(out)},ensure_ascii=False))


if __name__ == '__main__':
    main()
