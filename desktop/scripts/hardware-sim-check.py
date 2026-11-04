#!/usr/bin/env python3
"""Pump panel check on the simulated serial bench (#855, epic #868), production webview + tauri-driver.

Connects the Sample pump (sim COM3, address 1), auto-applies a rate, Start -> the polled state shows
'Forward', Stop -> 'Stopped', and hold-to-purge sends purge on press and stop-purge only on release.

    xvfb-run -a python3 desktop/scripts/hardware-sim-check.py \
        desktop/src-tauri/target/debug/mib-studio-desktop ~/.cargo/bin/tauri-driver build/hardware-sim-check

Runs the app with MIB_SERIAL_MODE=sim and MIB_CAMERA_MODE=mock (docs/howto/simulated-serial.md).
"""
import json, os, socket, subprocess, sys, time, urllib.request, urllib.error
from pathlib import Path
binary, driver, root = Path(sys.argv[1]), sys.argv[2], Path(sys.argv[3]).resolve()
root.mkdir(parents=True, exist_ok=False)
def free():
    with socket.socket() as s: s.bind(('127.0.0.1', 0)); return s.getsockname()[1]
env = dict(os.environ, XDG_DATA_HOME=str(root/'data'), XDG_CONFIG_HOME=str(root/'config'), MIB_CAMERA_MODE='mock',
           MIB_SERIAL_MODE='sim', WEBKIT_DISABLE_DMABUF_RENDERER='1', GSETTINGS_BACKEND='memory',
           MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL='file:///nonexistent/x.json')
port, nport = free(), free()
log = (root/'driver.log').open('w')
proc = subprocess.Popen([driver, '--port', str(port), '--native-port', str(nport)], env=env, stdout=log, stderr=subprocess.STDOUT)
url = f'http://127.0.0.1:{port}'
def req(path, data=None, method=None):
    r = urllib.request.Request(url+path, data=json.dumps(data).encode() if data is not None else None, headers={'Content-Type':'application/json'}, method=method)
    try:
        with urllib.request.urlopen(r, timeout=45) as resp: return json.load(resp)['value']
    except urllib.error.HTTPError as e: raise RuntimeError(e.read().decode())
def wait(fn, label, s=30):
    end = time.monotonic()+s
    while time.monotonic() < end:
        v = fn()
        if v: return v
        time.sleep(.1)
    raise AssertionError('Timed out: '+label)
try:
    for _ in range(100):
        try: urllib.request.urlopen(url+'/status', timeout=1); break
        except Exception: time.sleep(.1)
    sid = req('/session', {'capabilities': {'alwaysMatch': {'tauri:options': {'application': str(binary.resolve())}}}})['sessionId']
    js = lambda script, *a: req(f'/session/{sid}/execute/sync', {'script': script, 'args': list(a)})
    sample = 'const f=[...document.querySelectorAll("fieldset")].find(f=>f.querySelector("legend")?.textContent==="Sample pump");'
    status = lambda: js(sample + 'return f?f.querySelector("p").textContent:"";')
    btn = lambda text: js(sample + 'const b=[...f.querySelectorAll("button")].find(b=>b.textContent.trim()===arguments[0]);return b?!b.disabled:false;', text)
    def click(text):
        wait(lambda: btn(text), 'enabled '+text)
        js(sample + '[...f.querySelectorAll("button")].find(b=>b.textContent.trim()===arguments[0]).click();', text)
    def typ(label, value):
        js(sample + 'const i=[...f.querySelectorAll("label")].find(l=>l.textContent.startsWith(arguments[0])).querySelector("input");'
           'Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,"value").set.call(i,arguments[1]);i.dispatchEvent(new Event("input",{bubbles:true}));i.dispatchEvent(new FocusEvent("focusout",{bubbles:true}));', label, value)
    invoke = lambda name, a=None: req(f'/session/{sid}/execute/async', {'script': 'const d=arguments[arguments.length-1];window.__TAURI_INTERNALS__.invoke(arguments[0],arguments[1]).then(x=>d(x),e=>d({error:String(e)}));', 'args': [name, a or {}]})
    wait(lambda: 'Not connected' in status(), 'pump status shown', 60)
    typ('System serial port', 'COM3')
    time.sleep(1.5)  # COM3 enumerates after 300 ms
    click('Connect')
    wait(lambda: status().startswith('Connected'), 'sample pump connected', 30); print('connected:', status())
    typ('Flow rate', '50')
    wait(lambda: invoke('fetch_pump_status', {'pump': 0}).get('configured_flow_rate') == 50, 'rate auto-applied', 10); print('rate auto-applied: 50')
    click('Start pump')
    wait(lambda: 'Forward' in status() or 'Backward' in status(), 'polled running state', 10); print('after Start:', status())
    click('Stop pump')
    wait(lambda: 'Stopped' in status(), 'polled stopped state', 10); print('after Stop:', status())
    body = lambda: (root/'data/bio.yofo.mib-studio/logs/desktop-shell.log').read_text() if (root/'data/bio.yofo.mib-studio/logs/desktop-shell.log').exists() else ''
    js(sample + 'const b=[...f.querySelectorAll("button")].find(b=>b.textContent.startsWith("Purge"));b.dispatchEvent(new PointerEvent("pointerdown",{bubbles:true}));')
    wait(lambda: 'Purge pump: Pump purge started' in body(), 'purge started on press', 10)
    time.sleep(1.0); assert 'Stop purge: Pump purge stopped' not in body(), 'stopped while still held'
    print('held 1 s: purge started, not stopped')
    js(sample + 'const b=[...f.querySelectorAll("button")].find(b=>b.textContent.startsWith("Purge"));b.dispatchEvent(new PointerEvent("pointerup",{bubbles:true}));')
    wait(lambda: 'Stop purge: Pump purge stopped' in body(), 'purge stopped on release', 10); print('released: purge stopped')
    print('(the simulated dLSP pump does not model the purge run state, so it is checked by the commands sent)')
    print('PASS: #855 on the simulated serial bench')
finally:
    try: req(f'/session/{sid}', method='DELETE')
    except Exception: pass
    proc.terminate()
