"""Actual pokedaisy-linux display/thumbnail pixels on the original GBA diagnostic ROM."""
from pathlib import Path
import os, struct, subprocess, sys, tempfile, zlib

binary, cores, rom = (Path(a).resolve() for a in sys.argv[1:4])
env = dict(os.environ, POCKET_CORE_DIR=str(cores), SDL_AUDIODRIVER='dummy', POCKET_WINDOW='480x320')
env.setdefault('SDL_VIDEODRIVER', 'dummy'); env.setdefault('SDL_RENDER_DRIVER', 'software')

def png(path):
    data = path.read_bytes(); assert data[:8] == b'\x89PNG\r\n\x1a\n'
    pos, idat = 8, b''
    while pos < len(data):
        n, kind = struct.unpack('>I4s', data[pos:pos+8]); body = data[pos+8:pos+8+n]; pos += 12+n
        if kind == b'IHDR': w,h,depth,color = struct.unpack('>IIBB', body[:10])
        if kind == b'IDAT': idat += body
    assert depth == 8 and color in (2,6)
    bpp = 3 if color == 2 else 4; raw = zlib.decompress(idat); stride = w*bpp; rows=[]; prev=bytearray(stride)
    for y in range(h):
        f=raw[y*(stride+1)]; line=bytearray(raw[y*(stride+1)+1:(y+1)*(stride+1)])
        for i in range(stride):
            a=line[i-bpp] if i>=bpp else 0; b=prev[i]; c=prev[i-bpp] if i>=bpp else 0
            if f==1: line[i]=(line[i]+a)&255
            elif f==2: line[i]=(line[i]+b)&255
            elif f==3: line[i]=(line[i]+(a+b)//2)&255
            elif f==4:
                p=a+b-c; pa,pb,pc=abs(p-a),abs(p-b),abs(p-c)
                line[i]=(line[i]+(a if pa<=pb and pa<=pc else b if pb<=pc else c))&255
        rows.append(bytes(line)); prev=line
    return w,h,bpp,rows

def pixel(rows, bpp, x, y):
    return tuple(rows[y][x*bpp:x*bpp+3])

def run(root, mode, save=False):
    root.mkdir()
    (root/'roms').mkdir()
    (root/'roms/diagnostic.gba').write_bytes(rom.read_bytes())
    (root/'host.ini').write_text(f'display_filter={mode}\n')
    keys = '10:savestate' if save else None
    local = dict(env)
    if keys: local['POCKET_KEYS'] = keys
    shot = root/f'mode{mode}.png'
    result = subprocess.run([str(binary),'--data',str(root),str(root/'roms/diagnostic.gba'),'--frames','30','--shot',str(shot)],
                            env=local,capture_output=True,text=True,timeout=60)
    assert result.returncode == 0, result.stderr[-2000:]
    w,h,bpp,rows = png(shot)
    assert (w,h) == (480,320)
    return rows,bpp,result.stderr

def dim(p):
    return tuple((c+1)//2 for c in p)

with tempfile.TemporaryDirectory(prefix='pd-display-host-') as temporary:
    root=Path(temporary)
    off,bpp,_=run(root/'off',0,False)
    # Thumbnail bytes come from the app-owned callback copy and describe the same diagnostic source frame.
    run(root/'thumb',0,True)
    thumb=(root/'thumb/states/diagnostic.gba.armgba.ss.thumb').read_bytes()
    magic,w,h=struct.unpack_from('<4sII',thumb)
    assert magic==b'PDT1' and (w,h)==(96,64),(magic,w,h)
    first=struct.unpack_from('<I',thumb,12)[0]
    assert first==int.from_bytes(pixel(off,bpp,0,0),'big'),(hex(first),pixel(off,bpp,0,0))

    lcd,_,_=run(root/'lcd',1)
    for y in range(320):
        for x in range(480):
            source=pixel(off,bpp,x,y); mask=[239,239,239]; mask[x%3]=255
            if y%3==2: mask=[c*232//255 for c in mask]
            expected=tuple(source[c]*mask[c]//255 for c in range(3))
            assert pixel(lcd,bpp,x,y)==expected,(x,y,pixel(lcd,bpp,x,y),expected)

    scan,_,log=run(root/'scanlines',2)
    assert 'SCANLINES 480x320 (source 240x160)' in log,log
    for y in range(320):
        for x in range(480):
            source=pixel(off,bpp,x,y); expected=dim(source) if y%2==0 else source
            assert pixel(scan,bpp,x,y)==expected,(x,y,pixel(scan,bpp,x,y),expected)

    edge,_,log=run(root/'edge',5)
    assert 'EDGE SMOOTH 480x320 (source 240x160)' in log,log
    def src(x,y): return pixel(off,bpp,x*2,y*2)
    for y in range(320):
        for x in range(480):
            sx,sy=x//2,y//2; e=src(sx,sy)
            b=src(sx,max(0,sy-1)); d=src(max(0,sx-1),sy); f=src(min(239,sx+1),sy); hh=src(sx,min(159,sy+1))
            if not (y&1):
                expected=(d if d==b and b!=f and d!=hh else e) if not (x&1) else (f if b==f and b!=d and f!=hh else e)
            else:
                expected=(d if d==hh and d!=b and hh!=f else e) if not (x&1) else (f if hh==f and d!=hh and b!=f else e)
            assert pixel(edge,bpp,x,y)==expected,(x,y,pixel(edge,bpp,x,y),expected)
print('PASS host LCD, 2x scanlines, full Scale2x and callback-owned thumbnail pixels')
