#!/usr/bin/env python3
"""Build lossless host texture replacements while preserving Xbox UV geometry."""
import argparse
import ctypes
import hashlib
import io
import json
import re
import struct
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path
from PIL import Image
from nfs_assets import textures,texture_payload,texture_image
import font_hd
import font_strokes
import numpy as np

ROOT=Path(__file__).resolve().parent.parent
if sys.platform=='darwin' and Path('/opt/homebrew/lib/libcairo.2.dylib').exists():
    ctypes.CDLL('/opt/homebrew/lib/libcairo.2.dylib')
import cairosvg
font_hd.cairosvg=cairosvg
LIBERATION=ROOT/'ps5/assets/fonts/liberation'
# Arial cap height is 1467/2048 em. Conduit 14 px is redrawn from the same
# face rasterised at 21 px; other atlases have no larger source.
FONT_SOURCES={'FONT_ARIAL':'liberation','FONT_ARIAL12':'liberation','CONDUITMDITC_TT14I':'CONDUITMDITC_TT21I'}
STROKES=(0,.125,.25)


def fnv(data):
    value=0xcbf29ce484222325
    for byte in data:
        value=((value^byte)*0x100000001b3)&0xffffffffffffffff
    return value


def sampled(data):
    length=len(data)
    value=2166136261^length
    step=max(4,length//256)&~3
    for i in range(0,length-3,step):
        value=((value^struct.unpack_from('<I',data,i)[0])*16777619)&0xffffffff
    return ((value^struct.unpack_from('<I',data,length-4)[0])*16777619)&0xffffffff


def contour_upscale(image,target,work):
    """Smooth existing atlas/shape contours, without changing glyph metrics."""
    width,height=image.size
    layers=[]
    for level,opacity in [(48,.25),(112,1/3),(176,.5),(240,1.)]:
        bitmap=work.with_suffix(f'.{level}.pbm')
        vector=work.with_suffix(f'.{level}.svg')
        image.getchannel('A').point(lambda a:0 if a>=level else 255).convert('1').save(bitmap)
        subprocess.run(['potrace',str(bitmap),'-s','-o',str(vector),'--turdsize','0','--alphamax','0.75','--opttolerance','0.12'],check=True)
        root=ET.fromstring(vector.read_text())
        children=''.join(ET.tostring(child,encoding='unicode') for child in root)
        layers.append(f'<g opacity="{opacity}">{children}</g>')
    native=f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">'+''.join(layers)+'</svg>'
    work.with_suffix('.svg').write_text(native)
    raw=cairosvg.svg2png(bytestring=native.encode(),output_width=target[0],output_height=target[1])
    alpha=Image.open(io.BytesIO(raw)).convert('RGBA').getchannel('A')
    out=image.resize(target,Image.Resampling.LANCZOS)
    out.putalpha(alpha)
    return out


def own_font(name,alpha,glyphs,scale):
    """An atlas with no pixel of the disc: outlines or strokes in its FNTX cells."""
    if FONT_SOURCES.get(name)=='liberation':
        sources=[(f'Liberation Sans {style} (OFL)',font_hd.OutlineFont(LIBERATION/f'LiberationSans-{style}.ttf')) for style in ('Regular','Bold')]
        sizes=font_hd.sizes_around(font_hd.cap_height(glyphs)*2048/1467)
        return font_hd.upscale(alpha,glyphs,scale,sources,sizes,STROKES,trace=False)
    hd=np.zeros((alpha.height*scale,alpha.width*scale))
    table={**font_strokes.LATIN,**font_strokes.SIGNS}
    _,drawn=font_strokes.redraw(hd,name,{g['ch']:g for g in glyphs},np.asarray(alpha,float)/255,table,scale)
    return hd,{'method':'strokes','stroke_glyphs':len(drawn)}


def font_upscale(name,image,scale,atlases,own=False):
    """Redraw glyphs from outlines inside the unchanged FNTX cells; see font_hd."""
    alpha=image.getchannel('A')
    glyphs=atlases[name][1]
    source=FONT_SOURCES.get(name)
    if own:
        hd,report=own_font(name,alpha,glyphs,scale)
        out=Image.new('RGBA',(alpha.width*scale,alpha.height*scale),(255,255,255,255))
        out.putalpha(Image.fromarray((hd.clip(0,1)*255).round().astype('uint8')))
        return out,report
    if source=='liberation':
        sources=[(f'Liberation Sans {style} (OFL)',font_hd.OutlineFont(LIBERATION/f'LiberationSans-{style}.ttf')) for style in ('Regular','Bold')]
        sizes=font_hd.sizes_around(font_hd.cap_height(glyphs)*2048/1467)
    elif source:
        donor_alpha,donor_glyphs=atlases[source]
        sources=[(f'{source} donor atlas',font_hd.DonorAtlas(donor_alpha,donor_glyphs))]
        sizes=font_hd.sizes_around(font_hd.cap_height(glyphs)/font_hd.cap_height(donor_glyphs))
    else:
        sources,sizes=[],[]
    hd,report=font_hd.upscale(alpha,glyphs,scale,sources,sizes,STROKES)
    if name in font_strokes.WEIGHT:
        # Letters and digits from strokes; the signs without strokes keep the trace.
        _,drawn=font_strokes.redraw(hd,name,{g['ch']:g for g in glyphs},np.asarray(alpha,float)/255,font_strokes.LATIN,scale)
        report.update(method='strokes over '+report['method'],stroke_glyphs=len(drawn))
    # These atlases are white masks; transparent BC2 texels can have black
    # RGB, which must not bleed into the anti-aliased edge.
    out=Image.new('RGBA',(alpha.width*scale,alpha.height*scale),(255,255,255,255))
    out.putalpha(Image.fromarray((hd.clip(0,1)*255).round().astype('uint8')))
    return out,report


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game',type=Path,default=ROOT/'source/Need For Speed Underground')
    parser.add_argument('--out',type=Path,default=ROOT/'build/hd')
    parser.add_argument('--overlay',type=Path,default=ROOT/'build/ps5-ui-overlay')
    parser.add_argument('--no-imagegen',action='store_true',help='leave out the imagegen illustrations')
    parser.add_argument('--own',action='store_true',help="only art of this port, for the public package: disc textures keep their look")
    args=parser.parse_args()
    args.out.mkdir(parents=True,exist_ok=True)
    work=ROOT/'build/hd-vectors'
    work.mkdir(parents=True,exist_ok=True)
    sources={}
    records=[]
    report=[]
    unique={}
    approved_path=ROOT/'ps5/assets/hd/approved.json'
    approved=json.loads(approved_path.read_text()) if approved_path.exists() and not args.no_imagegen and not args.own else {}
    files=['GLOBAL/GLOBALB.BUN','GLOBAL/DYNTEX.BIN','FRONTEND/FRONTB.BUN','GLOBAL/INGAMEB.BUN','GLOBAL/GLOBALA.BUN']
    # Font atlases with their FNTX cells, collected first: a donor atlas can
    # live in another file than the font it improves.
    font_atlases={}
    for relative in files:
        data=(args.game/relative).read_bytes()
        tables=font_hd.font_tables(data)
        for row in textures(data):
            if row['name'].startswith(('FONT_','CONDUIT')):
                font_atlases[row['name']]=(texture_image(data,row).getchannel('A'),tables[row['name'].lower()])
    for relative in files:
        data=(args.game/relative).read_bytes()
        sources[relative]=data
        overlay=args.overlay/relative
        alternate=overlay.read_bytes() if overlay.exists() else None
        patched={r['hash']:r for r in textures(alternate)} if alternate else {}
        for row in textures(data):
            name,key=row['name'],f"{row['hash']:08x}"
            image=None
            method=''
            font_report=None
            if name.startswith('XBOX_'):
                svg=ROOT/'ps5/assets/icons'/(name+'.svg')
                raw=cairosvg.svg2png(url=str(svg),output_width=256,output_height=256)
                image=Image.open(io.BytesIO(raw)).convert('RGBA')
                method='native vector, 256px lossless'
            elif name.startswith('LOADING_CONTROLLER_XBOX'):
                raw=cairosvg.svg2png(url=str(ROOT/'ps5/assets/dualsense-diagram.svg'),output_width=1024,output_height=1024,unsafe=True)
                image=Image.open(io.BytesIO(raw)).convert('RGBA')
                method='DualSense artwork and native vector leaders'
            elif args.own and not name.startswith(('FONT_ARIAL','FONT_CONDUIT','CONDUIT')):
                continue
            elif name=='GENERIC_LOADING_BAR2':
                raw=cairosvg.svg2png(url=str(ROOT/'ps5/assets/icons/GENERIC_LOADING_BAR2.svg'),output_width=1024,output_height=512)
                image=Image.open(io.BytesIO(raw)).convert('RGBA')
                method='native vector loading fill; original UV, fitted to restored chrome opening'
            elif name.startswith(('FONT_','CONDUIT')):
                original=texture_image(data,row)
                scale=8 if max(original.size)<=256 else 4
                image,font_report=font_upscale(name,original,scale,font_atlases,args.own)
                method=font_report.pop('method')+'; unchanged FNTX cells, metrics and kerning'
            elif key in approved:
                approval=approved[key]
                if not approval.get('approved'):
                    continue
                path=ROOT/approval['path']
                if hashlib.sha256(path.read_bytes()).hexdigest()!=approval['sha256']:
                    raise ValueError('Approved artwork changed: '+key)
                image=Image.open(path).convert('RGBA')
                target=tuple(approval.get('hd_size',[row['width']*4,row['height']*4]))
                if len(target)!=2 or any(type(n)!=int or n<1 or n>4096 for n in target) or target[0]*target[1]>8*1024*1024:
                    raise ValueError('Invalid approved HD dimensions: '+key)
                if image.size!=target:
                    image=image.resize(target,Image.Resampling.LANCZOS)
                method='imagegen upscale, individually reviewed'
                if approval.get('preserve_source_alpha'):
                    # Cutout is layout data: keep the exact original mask,
                    # using the same generated background for both variants.
                    alpha=texture_image(data,row).getchannel('A').resize(target,Image.Resampling.NEAREST)
                    image.putalpha(alpha)
                    method+='; exact original cutout mask'
            elif name.startswith(('ARROW3','NFSU_LOGO','EA_GAMES_LOGO','SECONDARY_LOGO_','DECAL_ICON_','CARSELECT_MANUFACTURER_','VINYL_GROUP_MANUFACTURA')) or name in ('REP_STAR','SCROLLING_ARROW','CRUMBICON','WARNING_ICON','UNLOCKED_ICON'):
                original=texture_image(data,row)
                scale=8 if max(original.size)<=256 else 4
                image=contour_upscale(original,(original.width*scale,original.height*scale),work/key)
                method='existing glyph/shape contour restoration; unchanged atlas layout'
            if image is None:
                continue
            content=image.tobytes('raw','BGRA')
            filename=hashlib.sha256(content).hexdigest()[:32]+'.rgba'
            target=args.out/filename
            target.write_bytes(struct.pack('<4sIII',b'NHD1',image.width,image.height,len(content))+content)
            image.save(work/(key+'-preview.png'))
            candidates=[(data,row)]
            if alternate and row['hash'] in patched:
                candidates.append((alternate,patched[row['hash']]))
            source_size=row['width']*row['height']*(8 if row['format']==0xC else 16)//16
            for source,source_row in candidates:
                payload=texture_payload(source,source_row)[:source_size]
                signature=(row['format'],row['width'],row['height'],source_size,sampled(payload),fnv(payload))
                if signature in unique:
                    if unique[signature]!=filename:
                        raise ValueError('Conflicting replacement fingerprints: '+name)
                    continue
                unique[signature]=filename
                strong=signature[-1]
                records.append((signature[4],struct.pack('<9I48s',*signature[:5],image.width,image.height,strong&0xffffffff,strong>>32,filename.encode())))
            report.append({'texture':name,'hash':key,'source':relative,'source_size':[row['width'],row['height']],'hd_size':list(image.size),'file':filename,'method':method}|({'font':font_report} if font_report else {}))
    records.sort(key=lambda item:item[0])
    active=set(unique.values())
    for stale in args.out.glob('*.rgba'):
        if re.fullmatch(r'[0-9a-f]{32}\.rgba',stale.name) and stale.name not in active:
            stale.unlink()
    (args.out/'manifest.bin').write_bytes(struct.pack('<4sIII',b'NHDX',1,len(records),84)+b''.join(record for _,record in records))
    (args.out/'manifest.json').write_text(json.dumps({'textures':report,'fingerprints':len(records),'guest_geometry_unchanged':True},indent=2)+'\n')
    print(f'HD pack: {len(report)} textures, {len(records)} fingerprints, lossless BGRA + GPU mipmaps')


if __name__=='__main__':main()
