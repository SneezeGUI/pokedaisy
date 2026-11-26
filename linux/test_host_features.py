"""Focused host input/state integration on the original diagnostic ROMs, with no commercial game data."""
from pathlib import Path
import os, shutil, subprocess, sys, tempfile

binary, cores, diag = (Path(a).resolve() for a in sys.argv[1:4])
env = dict(os.environ, POCKET_CORE_DIR=str(cores), SDL_AUDIODRIVER='dummy')
env.setdefault('SDL_VIDEODRIVER', 'dummy')
env.setdefault('SDL_RENDER_DRIVER', 'software')
for key in ('POCKET_KEYS', 'POCKET_PAD', 'POCKET_RAWKEY', 'POCKET_RESUME'):
    env.pop(key, None)

def run(data, *args, expected=0):
    result = subprocess.run([str(binary), '--data', str(data), *map(str, args)], env=env,
                            capture_output=True, text=True, timeout=60)
    assert result.returncode == expected, (args, result.returncode, result.stderr[-3000:])
    return result.stderr

def keys(script):
    if script is None:
        env.pop('POCKET_KEYS', None)
    else:
        env['POCKET_KEYS'] = script

def pads(script):
    if script is None:
        env.pop('POCKET_PAD', None)
    else:
        env['POCKET_PAD'] = script

def settings(data):
    path = data / 'host.ini'
    return dict(line.split('=', 1) for line in path.read_text().splitlines() if '=' in line)

def fnv(data):
    value = 1469598103934665603
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return value

def sign_resume(data):
    state = (data / 'states/diagnostic.gb.resume.ss').read_bytes()
    battery = (data / 'states/diagnostic.gb.resume.sav').read_bytes()
    (data / 'states/diagnostic.gb.resume.ready').write_text(f'PDR1 {fnv(state):016x} {fnv(battery):016x}\n')

def controller_capture(data, raw_control, frames=115):
    # MENU, five DOWNs to CONTROLLER, A to open, A on the first row (game A), then a physical control.
    keys('10:menu,20:down,30:down,40:down,50:down,60:down,70:a,80:a')
    pads(f'90:{raw_control}')
    log = run(data, data / 'roms/diagnostic.gb', '--frames', str(frames))
    keys(None); pads(None)
    return log

with tempfile.TemporaryDirectory(prefix='pd-host-features-') as temporary:
    root = Path(temporary)
    def data_dir(name, *roms):
        data = root / name
        (data / 'roms').mkdir(parents=True)
        for rom in roms or ('diagnostic.gb',):
            shutil.copy(diag / rom, data / 'roms' / rom)
        return data

    # Controller capture moves A off its old SDL button. The real mapped A then chooses SAVE STATE in the menu.
    data = data_dir('remap')
    controller_capture(data, 10)  # SDL_CONTROLLER_BUTTON_RIGHTSHOULDER
    ini = settings(data)
    assert ini['pad_rightshoulder'] == 'a' and ini['pad_b'] == 'off', ini
    keys('10:menu,20:down'); pads('30:10')
    log = run(data, data / 'roms/diagnostic.gb', '--frames', '55')
    keys(None); pads(None)
    assert 'State saved in slot 1' in log, log
    assert (data / 'states/diagnostic.gb.ss').exists()
    # SDL A is game B by position, so it cancels capture without changing the persisted map.
    before = (data / 'host.ini').read_bytes()
    controller_capture(data, 0)
    assert (data / 'host.ini').read_bytes() == before
    # SDL Back is SELECT by default and clears the row.
    controller_capture(data, 4)
    ini = settings(data)
    assert all(value != 'a' for key, value in ini.items() if key.startswith('pad_')), ini
    assert ini['pad_rightshoulder'] == 'off'

    # D-pad capture followed by an analog-stick release leaves no direction owned by the old binding.
    data = data_dir('dpad-release')
    keys('10:menu,' + ','.join(f'{20 + 10*i}:down' for i in range(5)) + ',70:a,' +
         ','.join(f'{80 + 10*i}:down' for i in range(13)) + ',210:a')
    pads('220:12:40')
    env['POCKET_STICK'] = '270:1:-32767:10'
    env['POCKET_INPUT_DEBUG'] = '1'
    log = run(data, data / 'roms/diagnostic.gb', '--frames', '300')
    keys(None); pads(None); env.pop('POCKET_STICK'); env.pop('POCKET_INPUT_DEBUG')
    assert 'Input: padHeld=0 held=0' in log, log[-2000:]

    # Clicking outside key capture cancels it, so a real SDL A launches and F2 saves instead of being captured.
    data = data_dir('mouse-cancel')
    keys('10:y,' + ','.join(f'{20 + 10*i}:down' for i in range(9)) +
         ',110:a,120:a,140:b,170:menu,180:up,190:a,200:down,210:a')
    env['POCKET_MOUSE'] = '130:1:1:c'
    env['POCKET_RAWKEY'] = '150:122,160:1073741883'
    run(data, '--frames', '230')
    keys(None); env.pop('POCKET_MOUSE'); env.pop('POCKET_RAWKEY')
    assert (data / 'states/diagnostic.gb.ss').exists(), 'outside click left keyboard capture active'

    # A touch-only game pick completes into the emulator rather than latching its pending result.
    data = data_dir('pointer-pick')
    env['POCKET_MOUSE'] = '10:150:125:c'
    env['POCKET_RAWKEY'] = '20:1073741883'
    keys('30:menu,40:up,50:a,60:down,70:a')
    run(data, '--frames', '90')
    keys(None); env.pop('POCKET_MOUSE'); env.pop('POCKET_RAWKEY')
    assert (data / 'states/diagnostic.gb.ss').exists(), 'pointer game pick did not complete'

    # The same capture page works from the game list's SETTINGS before any core is launched.
    data = data_dir('library-remap')
    keys('10:y,' + ','.join(f'{20 + 10*i}:down' for i in range(8)) + ',100:a,110:a')
    pads('120:10')
    run(data, '--frames', '140')
    keys(None); pads(None)
    ini = settings(data)
    assert ini['pad_rightshoulder'] == 'a' and ini['pad_b'] == 'off', ini

    # Existing keyboard defaults still go through real SDL key events (F2 = SDL_SCANCODE_TO_KEYCODE(59)).
    data = data_dir('keyboard')
    env['POCKET_RAWKEY'] = '10:1073741883'
    run(data, data / 'roms/diagnostic.gb', '--frames', '35')
    env.pop('POCKET_RAWKEY')
    assert (data / 'states/diagnostic.gb.ss').exists()
    assert (data / 'states/diagnostic.gb.ss.thumb').read_bytes()[:4] == b'PDT1'

    # Slots 1 and 2, persistent selection, thumbnails, disk-backed save undo, and load undo.
    data = data_dir('slots')
    keys('10:savestate,20:slotnext,30:savestate')
    run(data, data / 'roms/diagnostic.gb', '--frames', '50')
    keys(None)
    one = data / 'states/diagnostic.gb.ss'
    two = data / 'states/diagnostic.gb.slot2.ss'
    thumb = data / 'states/diagnostic.gb.slot2.ss.thumb'
    assert one.exists() and two.exists()
    assert (data / 'states/diagnostic.gb.selected').read_text() == '2\n'
    assert thumb.read_bytes()[:4] == b'PDT1'
    first, first_thumb = two.read_bytes(), thumb.read_bytes()
    keys('20:savestate')
    run(data, data / 'roms/diagnostic.gb', '--frames', '40')
    keys(None)
    assert two.read_bytes() != first
    assert (data / 'states/diagnostic.gb.slot2.ss.bak').read_bytes() == first
    assert (data / 'states/diagnostic.gb.slot2.ss.bak.thumb').read_bytes() == first_thumb
    keys('10:undosave')
    log = run(data, data / 'roms/diagnostic.gb', '--frames', '30')
    keys(None)
    assert 'Save undone' in log and two.read_bytes() == first and thumb.read_bytes() == first_thumb
    assert not (data / 'states/diagnostic.gb.slot2.ss.bak').exists()
    keys('10:loadstate,20:undoload')
    log = run(data, data / 'roms/diagnostic.gb', '--frames', '40')
    keys(None)
    assert 'State loaded' in log and 'Load undone' in log, log
    assert (data / 'states/diagnostic.gb.undo-load.ss').stat().st_size > 0
    # From slot 2, eight NEXT actions reach slot 10. Its thumbnail appears in the slot picker.
    keys(','.join(f'{10 + 10*i}:slotnext' for i in range(8)) + ',90:savestate')
    run(data, data / 'roms/diagnostic.gb', '--frames', '110')
    keys(None)
    assert (data / 'states/diagnostic.gb.slot10.ss').exists()
    assert (data / 'states/diagnostic.gb.selected').read_text() == '10\n'
    keys('10:menu,' + ','.join(f'{20 + 10*i}:down' for i in range(6)) + ',80:a')
    run(data, data / 'roms/diagnostic.gb', '--frames', '100', '--shot', data / 'slots.png')
    keys(None)
    assert (data / 'slots.png').read_bytes()[:8] == b'\x89PNG\r\n\x1a\n'
    assert not list((data / 'states').glob('*.tmp.*'))

    # armGBA keeps its distinct state format and legacy slot-1 name.
    data = data_dir('gba', 'diagnostic.gba')
    keys('10:savestate,20:slotnext,30:savestate')
    run(data, data / 'roms/diagnostic.gba', '--frames', '50')
    keys(None)
    assert (data / 'states/diagnostic.gba.armgba.ss').exists()
    assert (data / 'states/diagnostic.gba.armgba.slot2.ss').exists()
    keys('10:loadstate,20:undoload')
    log = run(data, data / 'roms/diagnostic.gba', '--frames', '40')
    keys(None)
    assert 'State loaded' in log and 'Load undone' in log, log
    # armGBA also accepts its pre-button-history layout (16 bytes shorter here), not only retro_serialize_size().
    legacy = data_dir('legacy-gba', 'diagnostic.gba')
    keys('10:savestate')
    run(legacy, legacy / 'roms/diagnostic.gba', '--frames', '30')
    keys(None)
    legacy_state = legacy / 'states/diagnostic.gba.armgba.ss'
    current = legacy_state.read_bytes()
    assert len(current) > 16
    legacy_state.write_bytes(current[:-16])
    keys('10:loadstate')
    log = run(legacy, legacy / 'roms/diagnostic.gba', '--frames', '30')
    keys(None)
    assert 'State loaded' in log, log
    env['POCKET_RESUME'] = '1'
    run(data, data / 'roms/diagnostic.gba', '--frames', '10')
    log = run(data, data / 'roms/diagnostic.gba', '--frames', '5')
    assert 'Resumed from last session' in log, log
    env.pop('POCKET_RESUME')

    # DSperate uses the same raw state path, including automatic resume and load undo.
    data = data_dir('ds', 'diagnostic.nds')
    env['POCKET_RESUME'] = '1'
    run(data, data / 'roms/diagnostic.nds', '--frames', '15')
    log = run(data, data / 'roms/diagnostic.nds', '--frames', '5')
    assert 'Resumed from last session' in log, log
    keys('5:savestate,15:loadstate,25:undoload')
    log = run(data, data / 'roms/diagnostic.nds', '--frames', '45')
    keys(None)
    assert 'State loaded' in log and 'Load undone' in log, log
    env.pop('POCKET_RESUME')

    # Clean-exit resume, failed-state reboot, changed-battery precedence, and explicit bypass.
    data = data_dir('resume')
    rom = data / 'roms/diagnostic.gb'
    env['POCKET_RESUME'] = '1'
    log = run(data, rom, '--frames', '30')
    resume = data / 'states/diagnostic.gb.resume.ss'
    assert 'Resume state saved' in log and resume.exists()
    assert (data / 'states/diagnostic.gb.resume.sav').exists()
    assert (data / 'states/diagnostic.gb.resume.ss.thumb').read_bytes()[:4] == b'PDT1'
    log = run(data, rom, '--frames', '6')
    assert 'Resumed from last session' in log, log
    assert (data / 'states/diagnostic.gb.resume.ready').exists()
    # Fail after publishing the new battery but before the state. The next launch must reject the mixed bundle.
    save = data / 'saves/diagnostic.gb.sav'
    previous_state = resume.read_bytes()
    changed = bytearray(save.read_bytes()); changed[100] ^= 0xff
    save.write_bytes(changed)
    env['POCKET_RESUME_FAIL'] = 'state'
    log = run(data, rom, '--frames', '6')
    assert 'Resume save failed' in log and resume.read_bytes() == previous_state
    env.pop('POCKET_RESUME_FAIL')
    log = run(data, rom, '--frames', '6')
    assert 'Resume skipped: battery save changed' in log and 'Resumed from last session' not in log, log
    log = run(data, rom, '--frames', '6')
    assert 'Resumed from last session' in log, log
    resume.write_bytes(b'bad')
    log = run(data, rom, '--frames', '6')
    assert 'Resume skipped: battery save changed' in log and 'Resume state saved' in log, log
    # A same-length corrupt state with a valid commit reaches the core; its failure still reboots safely.
    resume.write_bytes(bytes(resume.stat().st_size))
    sign_resume(data)
    log = run(data, rom, '--frames', '6')
    assert 'Resume load failed; game restarted' in log and 'Resume state saved' in log, log
    save = data / 'saves/diagnostic.gb.sav'
    changed = bytearray(save.read_bytes()); changed[0] ^= 0xff
    save.write_bytes(changed)
    log = run(data, rom, '--frames', '6')
    assert 'Resume skipped: battery save changed' in log, log
    env['POCKET_RESUME'] = '0'
    before = resume.read_bytes()
    log = run(data, rom, '--frames', '4')
    assert 'Resumed from last session' not in log and resume.read_bytes() == before

    # Import and restore share the same validated atomic battery path and force a fresh boot past stale resume data.
    imported = data / 'import.sav'
    imported.write_bytes(b'\x11' * len(save.read_bytes()))
    log = run(data, rom, '--import-save', imported, '--frames', '2')
    assert save.read_bytes() == imported.read_bytes()
    assert (data / 'states/diagnostic.gb.fresh').exists()
    env['POCKET_RESUME'] = '1'
    log = run(data, rom, '--frames', '5')
    assert 'Resume skipped: imported save' in log, log
    assert not (data / 'states/diagnostic.gb.fresh').exists()
    env['POCKET_RESUME'] = '0'
    restored = data / 'restore.sav'
    restored.write_bytes(b'\x22' * len(save.read_bytes()))
    run(data, rom, '--restore-save', restored, '--frames', '2')
    assert save.read_bytes() == restored.read_bytes()
    bad = data / 'bad.sav'; bad.write_bytes(b'bad')
    before = save.read_bytes()
    log = run(data, rom, '--import-save', bad, '--frames', '2', expected=1)
    assert 'Save import failed' in log and save.read_bytes() == before
    env.pop('POCKET_RESUME')

print('PASS controller remapping, keyboard compatibility, ten slots, thumbnails, undo, resume and save import')
