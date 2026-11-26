#!/usr/bin/env python3
from pathlib import Path
import subprocess,shutil,hashlib,struct,os
root=Path(__file__).resolve().parent.parent
out=root/'build/tests';out.mkdir(parents=True,exist_ok=True)
headers=out/'include';headers.mkdir(exist_ok=True)
api=headers/'ps5platform'
if not api.exists(): api.symlink_to((root/'build/deps/PS5_Vulkan/.deps/native/ps5-payload-sdk/target/include/ps5platform').resolve())
common=[shutil.which('clang'),'-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-pthread','-DNFSU1_PS5=1','-I'+str(root/'ps5/compat'),'-I'+str(root/'ps5/src'),'-I'+str(root/'upstream/xboxrecomp/src'),'-I'+str(headers)]
for name,sources in [('port_ui',['ps5/src/port_ui.c']),('xbe_check',['ps5/src/xbe_check.c','ps5/src/sha256.c']),('path',['upstream/xboxrecomp/src/kernel/kernel_path.c']),('disc_image',['ps5/src/disc_image.c']),('ui_text',['ps5/src/ui_text.c','ps5/src/port_ui.c']),('fmv',['ps5/src/fmv_replace.c','ps5/src/fmv_video.c']),('pad',['ps5/src/pad_map.c']),('input',['ps5/src/input_ps5.c','ps5/src/pad_map.c']),('audio',['ps5/src/audio_ps5.c']),('audio_surround',['ps5/src/audio_ps5.c']),('audio_native',['ps5/src/audio_native.c']),('audio_enhance',[]),('apu_surround',['upstream/xboxrecomp/src/apu/apu_dsp.c']),('memory',['ps5/src/memory_ps5.c','upstream/xboxrecomp/src/kernel/guest_vmem.c']),('hd',['ps5/src/hd_textures.c','ps5/src/sha256.c']),('fast_time',[])]:
    binary=out/name
    extra=[]
    if name in ('hd','fmv'):
        fixture=out/f'{name}-fixture';fixture.mkdir(exist_ok=True)
        extra=['-DNFS_HD_ROOT="'+str(fixture)+'"']
    if name=='fmv':
        dav1d=Path(subprocess.check_output(['brew','--prefix','dav1d'],text=True).strip())
        extra+=['-I'+str(dav1d/'include'),'-L'+str(dav1d/'lib'),'-ldav1d','-Wl,-rpath,'+str(dav1d/'lib')]
        subprocess.run(['ffmpeg','-v','error','-y','-f','lavfi','-i','color=gray:s=64x64:r=30,geq=lum=30+N:cb=128:cr=128',
                        '-frames:v','120','-pix_fmt','yuv420p','-c:v','libsvtav1','-preset','12','-crf','4','-g','120',
                        str(fixture/'test.ivf')],check=True,stderr=subprocess.DEVNULL)
    subprocess.run(common+extra+[str(root/'ps5/tests'/f'{name}_test.c')]+[str(root/s) for s in sources]+['-o',str(binary)],check=True)
    result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=25)
    (out/f'{name}.log').write_text(result.stdout+result.stderr)
    print(result.stdout+result.stderr,end='');result.check_returncode()
    if name=='audio_surround':
        result=subprocess.run([str(binary),'fallback'],capture_output=True,text=True,timeout=25)
        print(result.stdout+result.stderr,end='');result.check_returncode()
    if name in ('hd','fmv'):
        result=subprocess.run([str(binary),'invalid'],capture_output=True,text=True,timeout=25)
        print(result.stdout+result.stderr,end='');result.check_returncode()
    if name=='hd':
        for mode in ('preload','p8'):
            result=subprocess.run([str(binary),mode],capture_output=True,text=True,timeout=25)
            print(result.stdout+result.stderr,end='');result.check_returncode()
        # hdpatch on the console (hd_textures.c), keyed and encrypted here with hashlib as tools/hdpatch does
        top=out/'hd-patch';shutil.rmtree(top,ignore_errors=True);(top/'hdpatch/files').mkdir(parents=True);(top/'hd').mkdir()
        (top/'hd/old.txt').write_text('previous pack')
        (top/'hd/manifest.bin').write_bytes(b'an older pack')  # a pack: moved aside, never deleted
        key=lambda src:hashlib.sha256(b'reUndr hdpatch 1\0'+src).digest()
        def fnv(b):
            h=0xcbf29ce484222325
            for c in b: h=((h^c)*0x100000001b3)&(2**64-1)
            return h
        src=[bytes(10*r+k+1 for k in range(8)) for r in range(4)]
        names=['a'*32+'.rgba','a'*32+'.rgba','b'*32+'.rgba','c'*32+'.rgba']
        image=b'NHD1'+struct.pack('<III',8,8,256)+bytes(k*7&255 for k in range(256))
        (fixture/names[2]).write_bytes(image)
        recs=b''.join(struct.pack('<9I48s',12,4,4,8,0x1111*(r+1),8,8,fnv(src[r])&0xffffffff,fnv(src[r])>>32,names[r].encode()) for r in range(4))
        (top/'hdpatch/manifest.bin').write_bytes(struct.pack('<4sIII',b'NHDX',1,4,84)+recs)
        pad=lambda r:hashlib.shake_256(key(src[r])+names[r].encode()).digest(len(image))
        enc=lambda r:bytes(x^y for x,y in zip(image,pad(r)))
        (top/'hdpatch/files'/names[0]).write_bytes(enc(0))
        (top/'hdpatch/files'/names[3]).write_bytes(enc(3)[:-1]+bytes([enc(3)[-1]^1]))
        sha=hashlib.sha256(image).digest();wrap1=bytes(x^y for x,y in zip(key(src[0]),key(src[1])))
        rows=[bytes(32)+sha+struct.pack('<II',len(image),1),wrap1+sha+struct.pack('<II',len(image),1),
              bytes(32)+sha+struct.pack('<II',len(image),2),bytes(32)+sha+struct.pack('<II',len(image),1)]
        (top/'hdpatch/index.bin').write_bytes(struct.pack('<4sIII',b'NHDP',1,4,72)+b''.join(rows))
        env=dict(os.environ,NFSU1_HD_DIR=str(top/'hd'))
        for run in (1,2):
            result=subprocess.run([str(binary),'patch'],capture_output=True,text=True,timeout=25,env=env)
            print(result.stdout+result.stderr,end='');result.check_returncode()
            baks=sorted(top.glob('hd.bak-*'))
            assert len(baks)==1 and (baks[0]/'old.txt').read_text()=='previous pack',baks
            assert (top/'hd'/names[0]).read_bytes()==image==(top/'hd'/names[2]).read_bytes() and not (top/'hd'/names[3]).exists()
            assert (top/'hd/manifest.bin').read_bytes()==(top/'hdpatch/manifest.bin').read_bytes()
            assert (top/'hd/hdpatch.id').read_text().upper()==hashlib.sha256((top/'hdpatch/index.bin').read_bytes()).hexdigest().upper()
        print('HD patch: previous hd kept once, files opened once and kept')
