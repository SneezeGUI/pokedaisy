"""Real SDL save editor/trade flows on synthetic Gen 3 saves, never on user data."""
from pathlib import Path
import os, subprocess, sys, tempfile

binary, fixture, cores, diagnostic = (Path(a).resolve() for a in sys.argv[1:5])
base = dict(os.environ, POCKET_CORE_DIR=str(cores), SDL_AUDIODRIVER='dummy')
base.setdefault('SDL_VIDEODRIVER', 'dummy')
base.setdefault('SDL_RENDER_DRIVER', 'software')
for key in ('POCKET_KEYS', 'POCKET_TEXT', 'POCKET_TEXT_CANCEL', 'POCKET_SAVE_TOUCH', 'POCKET_RESUME'):
    base.pop(key, None)

def make_case(root, name):
    data = root / name
    data.mkdir()
    result = subprocess.run([str(fixture), str(data)], capture_output=True, text=True, timeout=20)
    assert result.returncode == 0, result.stderr
    return data

def run(data, script, frames, extra=None, rom=None):
    env = dict(base)
    if script is not None:
        env['POCKET_KEYS'] = script
    if extra:
        env.update(extra)
    command = [str(binary), '--data', str(data), '--frames', str(frames)]
    if rom:
        command.append(str(rom))
    result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, (result.returncode, result.stderr[-3000:])
    return result.stderr

def inspect(data, game):
    result = subprocess.run([str(fixture), '--inspect', str(data / 'saves' / f'{game}.sav'), 'BPEE'],
                            capture_output=True, text=True, timeout=20)
    assert result.returncode == 0, result.stderr
    return dict(line.split('=', 1) for line in result.stdout.splitlines() if '=' in line)

def backups(data, game):
    return list((data / 'saves/pokedaisy-backups').glob(f'{game}.backup-*.sav'))

with tempfile.TemporaryDirectory(prefix='pd-save-ui-') as temporary:
    root = Path(temporary)
    # Launch SAVE EDITOR, change money, confirm the write, and invalidate both core-specific resume names.
    data = make_case(root, 'editor')
    original = (data / 'saves/first.gba.sav').read_bytes()
    log = run(data, '10:left,20:down,30:a,40:a,70:a,90:select,100:select,110:a,120:up,130:a,140:x,150:a,160:a', 180)
    assert 'Save tools: editor opened' in log and 'Save tools: wrote 1 save' in log, log
    assert '2 unsupported GB/DS entries skipped' in log, log
    assert inspect(data, 'first.gba')['money'] == '3001', inspect(data, 'first.gba')
    assert len(backups(data, 'first.gba')) == 1 and backups(data, 'first.gba')[0].read_bytes() == original
    assert (data / 'states/first.gba.armgba.fresh').exists() and (data / 'states/first.gba.fresh').exists()
    # Reopening after a same-size ROM replacement revalidates the header instead of trusting cached BPEE layout data.
    assert (data / 'save-tools-cache.txt').exists()
    replacement = bytearray((data / 'roms/first.gba').read_bytes())
    replacement[0xAC:0xB0] = b'BPRE'
    (data / 'roms/first.gba').write_bytes(replacement)
    log = run(data, '10:left,20:down,30:a,40:a,70:a', 90)
    assert 'first.gba header BPRE' in log and 'Save tools: wrote' not in log, log
    # The real emulator honors the fresh marker before attempting to deserialize the stale resume file.
    (data / 'roms/first.gba').write_bytes(diagnostic.read_bytes())
    log = run(data, None, 2, {'POCKET_RESUME': '1'}, data / 'roms/first.gba')
    assert 'Resume skipped: imported save' in log and 'Resumed from last session' not in log, log

    # Launch TRADE SAVES and verify a real two-save exchange with independent backups and invalidation.
    data = make_case(root, 'trade')
    original_a = (data / 'saves/first.gba.sav').read_bytes()
    original_b = (data / 'saves/second.gba.sav').read_bytes()
    assert inspect(data, 'first.gba')['box'] == '252' and inspect(data, 'second.gba')['box'] == '255'
    log = run(data, '10:left,20:down,30:a,40:down,50:a,80:a,90:a,110:a,130:x,140:a,150:y,160:select,170:a,180:a', 210)
    assert 'Save tools: trade opened' in log and 'Save tools: wrote 2 saves' in log, log
    assert inspect(data, 'first.gba')['box'] == '255' and inspect(data, 'second.gba')['box'] == '252'
    assert len(backups(data, 'first.gba')) == 1 and backups(data, 'first.gba')[0].read_bytes() == original_a
    assert len(backups(data, 'second.gba')) == 1 and backups(data, 'second.gba')[0].read_bytes() == original_b
    assert (data / 'states/first.gba.armgba.fresh').exists() and (data / 'states/second.gba.armgba.fresh').exists()

    # SDL text acceptance writes a box name; cancellation makes no save or backup and no resume invalidation.
    data = make_case(root, 'text')
    script = '10:left,20:down,30:a,40:a,70:a,80:down,90:down,100:down,110:down,120:down,130:right,140:a,150:a,160:x,170:a,180:a'
    log = run(data, script, 210, {'POCKET_TEXT': 'DAISY'})
    assert 'Save tools: text accepted' in log and 'Save tools: wrote 1 save' in log, log
    assert inspect(data, 'first.gba')['boxName'] == 'DAISY', inspect(data, 'first.gba')
    data = make_case(root, 'cancel')
    before = (data / 'saves/first.gba.sav').read_bytes()
    log = run(data, script[:-5], 200, {'POCKET_TEXT_CANCEL': '1'})
    assert 'Save tools: text cancelled' in log, log
    assert (data / 'saves/first.gba.sav').read_bytes() == before and not backups(data, 'first.gba')
    assert not (data / 'states/first.gba.armgba.fresh').exists()

    # An external write after opening rejects the stale edit and preserves the newer bytes.
    data = make_case(root, 'stale')
    save = data / 'saves/first.gba.sav'
    before = save.read_bytes()
    log = run(data, '10:left,20:down,30:a,40:a,70:a,90:select,100:select,110:a,120:up,130:a,140:x,150:a,170:a', 200,
              {'POCKET_SAVE_TOUCH': f'95:{save}'})
    assert 'stale save injected' in log and 'changed since it was opened' in log, log
    after = save.read_bytes()
    assert after[0] == (before[0] ^ 0x5A) and after[1:] == before[1:]
    assert not backups(data, 'first.gba')
    assert (data / 'states/first.gba.armgba.fresh').exists() # safe fresh boot even though the edit was refused

    # Pointer-only rail/game results complete and return to normal browsing before launching the emulator.
    data = make_case(root, 'pointer')
    log = run(data, '100:menu,110:up,120:a,130:down,140:a', 160,
              {'POCKET_MOUSE': '10:30:92:c,20:200:74:c,40:32:224:c,50:240:288:c,60:240:288:c,80:150:125:c',
               'POCKET_RAWKEY': '90:1073741883'})
    assert 'Save tools: session closed' in log and (data / 'states/first.gba.armgba.ss').exists(), log[-2500:]

    # Unsaved changes require confirmation after a pointer rail leaves the editor, for both START and SDL close.
    edit = '10:left,20:down,30:a,40:a,70:a,90:select,100:select,110:a,120:up,130:a'
    data = make_case(root, 'close-start')
    log = run(data, edit + ',150:start,160:a', 190, {'POCKET_MOUSE': '140:32:224:c'})
    assert 'Save tools: close confirmation requested' in log, log[-2500:]
    data = make_case(root, 'close-sdl')
    log = run(data, edit + ',160:a', 190, {'POCKET_MOUSE': '140:32:224:c', 'POCKET_CLOSE': '150'})
    assert 'Save tools: close confirmation requested' in log, log[-2500:]

print('PASS save editor, trade, SDL text accept/cancel, stale rejection, backups and resume invalidation')
