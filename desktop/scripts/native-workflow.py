#!/usr/bin/env python3
"""Production-WebKit workflow acceptance with real GTK file dialogs.
Run under xvfb-run. Uses W3C WebDriver directly (stdlib, no Selenium dependency).
All application/backend commands, dialogs, rendering, files and state are real.
"""
import argparse
import base64
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import time
import urllib.error
import urllib.request
import zlib


def png(path, offset):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    width, height = 512, 96
    rows = b''.join(b'\0' + bytes(40 if (x - 240 - offset) ** 2 + (y - 48) ** 2 < 225 else 200 for x in range(width)) for y in range(height))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 0, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('binary', type=Path)
    parser.add_argument('--driver', default='tauri-driver')
    parser.add_argument('--artifacts', type=Path, required=True)
    args = parser.parse_args()
    root = args.artifacts.resolve()
    root.mkdir(parents=True, exist_ok=False)
    frames = root / 'frames'
    frames.mkdir()
    for i in range(5):
        png(frames / f'frame{i}.png', i * 3)
    env = dict(os.environ, XDG_DATA_HOME=str(root / 'app-data'), XDG_CONFIG_HOME=str(root / 'app-config'),
               MIB_CAMERA_MODE='mock', MIB_MOCK_CAMERA_DIR=str(frames), MIB_MOCK_CAMERA_INTERVAL_MS='10',
               MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL='file:///nonexistent/mib-lut-manifest.json',
               WEBKIT_DISABLE_DMABUF_RENDERER='1')
    port, native_port = free_port(), free_port()
    url = f'http://127.0.0.1:{port}'
    session = ''
    log = (root / 'driver.log').open('w')
    driver = subprocess.Popen([args.driver, '--port', str(port), '--native-port', str(native_port)], env=env, stdout=log, stderr=subprocess.STDOUT)

    def request(path, data=None, method=None):
        req = urllib.request.Request(url + path, data=json.dumps(data).encode() if data is not None else None,
                                     headers={'Content-Type': 'application/json'}, method=method)
        try:
            with urllib.request.urlopen(req, timeout=45) as response:
                result = json.load(response)['value']
        except urllib.error.HTTPError as error:
            raise RuntimeError(error.read().decode()) from error
        if isinstance(result, dict) and result.get('error') and 'ok' not in result:
            raise RuntimeError(result)
        return result

    def js(script, *values):
        return request(f'/session/{session}/execute/sync', {'script': script, 'args': list(values)})

    def wait(fn, label, seconds=30):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            result = fn()
            if result:
                return result
            time.sleep(.1)
        raise AssertionError('Timed out: ' + label)

    next_picker = None

    def click(text):
        nonlocal next_picker
        wait(lambda: js('return [...document.querySelectorAll("button")].some(b=>b.textContent.trim()===arguments[0]&&!b.disabled)', text), text)
        js('const b=[...document.querySelectorAll("button")].find(b=>b.textContent.trim()===arguments[0]&&!b.disabled);b.scrollIntoView({block:"center"});b.click();', text)
        if next_picker is not None:
            title = {"Start Experiment": "Save Experiment Data", "Select HDF File…": "Open recording", "Export All…": "Choose export parent folder"}[text]
            def find_dialog():
                result = subprocess.run(['xdotool', 'search', '--onlyvisible', '--name', title], capture_output=True, text=True)
                return result.stdout.splitlines()[-1] if result.returncode == 0 and result.stdout.strip() else None
            window = wait(find_dialog, 'native dialog ' + title)
            subprocess.run(['xdotool', 'windowfocus', '--sync', window], check=True)
            subprocess.run(['xdotool', 'key', '--clearmodifiers', 'ctrl+l'], check=True)
            subprocess.run(['xdotool', 'key', '--clearmodifiers', 'ctrl+a'], check=True)
            subprocess.run(['xdotool', 'type', '--clearmodifiers', '--delay', '1', next_picker], check=True)
            subprocess.run(['xdotool', 'key', '--clearmodifiers', 'Return'], check=True)
            time.sleep(.3)
            if find_dialog():
                subprocess.run(['xdotool', 'key', '--clearmodifiers', 'Return'], check=True)
            next_picker = None

    def stage(prefix):
        js('document.querySelector("button[aria-label^=\\""+arguments[0]+":\\"]").click()', prefix)

    def invoke(name, values=None):
        result = request(f'/session/{session}/execute/async', {'script': 'const done=arguments[arguments.length-1];window.__TAURI_INTERNALS__.invoke(arguments[0],arguments[1]).then(x=>done({ok:true,result:x}),e=>done({ok:false,error:String(e)}));', 'args': [name, values or {}]})
        assert result['ok'], result
        return result['result']

    def picker(value):
        nonlocal next_picker
        next_picker = str(value)

    evidence = {'interaction': 'Real production WebKit, backend IPC and native GTK dialogs; no API mocks'}
    try:
        def available():
            try:
                request('/status')
                return True
            except (OSError, RuntimeError):
                return False
        wait(available, 'driver startup')
        session = request('/session', {'capabilities': {'alwaysMatch': {'tauri:options': {'application': str(args.binary.resolve())}}}})['sessionId']
        wait(lambda: 'backend: ready' in js('return document.body.innerText'), 'embedded app/backend initialization')
        stage('Hardware Preflight')
        click('Cameras')
        click('Configure Mock…')
        js('const i=document.querySelector("input[placeholder^=\\"mock frame\\"]");Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,"value").set.call(i,arguments[0]);i.dispatchEvent(new Event("input",{bubbles:true}));', str(frames))
        click('Apply')
        click('Start Camera')
        wait(lambda: invoke('fetch_preview_buffer')['available'], 'real mock frames')
        stage('Experiment')
        click('Set Background')
        output = root / 'experiment.h5'
        picker(output)
        click('Start Experiment')
        wait(lambda: invoke('fetch_experiment_status')['state'] == 2, 'experiment active')
        active_before = invoke('fetch_experiment_status')
        # A native close request must not discard an active experiment.
        click('File')
        click('Exit')
        time.sleep(.3)
        assert invoke('fetch_experiment_status')['state'] == 2
        # Reconstruct the entire webview while native work remains active.
        request(f'/session/{session}/refresh', {})
        wait(lambda: 'backend: ready' in js('return document.body.innerText'), 'webview recovery')
        recovered = invoke('fetch_experiment_status')
        assert recovered['state'] == 2 and recovered['output_path'] == active_before['output_path'], recovered
        evidence['reload_recovered_active_run'] = True
        # Navigation must not stop backend work or lose operation ownership.
        stage('Camera & Alignment')
        time.sleep(4)
        stage('Experiment')
        click('Stop Experiment')
        status = wait(lambda: (lambda s: s if s.get('terminal') else None)(invoke('fetch_experiment_status')), 'finalization')
        assert status['finalization_ok'], status
        assert int(status['persistence_admitted']) > 0, status
        assert status['persistence_admitted'] == status['persistence_committed'], status
        evidence['experiment'] = status
        output = Path(status['output_path'])
        assert output.parent == root
        assert output.is_file() and output.stat().st_size > 0
        click('Stop Camera')
        stage('Review')
        picker(output)
        click('Select HDF File…')
        metadata = wait(lambda: (lambda m: m if m.get('file_open') and m.get('file_path') == str(output) else None)(invoke('fetch_review_metadata')), 'review reopen')
        evidence['review'] = metadata
        picker(root)
        click('Export All…')
        export = wait(lambda: (lambda s: s if s.get('state') in ('completed', 'failed', 'cancelled') else None)(json.loads(invoke('review_export_status_json'))), 'export terminal')
        assert export['state'] == 'completed', export
        assert Path(export['final_path']).exists(), export
        evidence['export'] = export
        wait(lambda: 'Export: completed' in js('return document.body.innerText'), 'frontend export reconciliation')
        click('Close File')
        assert not invoke('fetch_review_metadata')['file_open']
        evidence['closed'] = True
        (root / 'screenshot.png').write_bytes(base64.b64decode(request(f'/session/{session}/screenshot')))
        (root / 'evidence.json').write_text(json.dumps(evidence, indent=2))
        print('PASS: native production webview configure → capture → experiment → finalize → reopen → export → close')
    except BaseException:
        if session:
            try:
                js('document.querySelector(".log-toggle")?.click()')
                (root / 'failure-body.txt').write_text(js('return document.body.innerText'))
                (root / 'failure.png').write_bytes(base64.b64decode(request(f'/session/{session}/screenshot')))
            except Exception:
                pass
        raise
    finally:
        if session:
            try:
                request(f'/session/{session}', method='DELETE')
            except Exception:
                pass
        driver.terminate()
        try:
            driver.wait(timeout=10)
        except subprocess.TimeoutExpired:
            driver.kill(); driver.wait()
        log.close()


if __name__ == '__main__':
    main()
