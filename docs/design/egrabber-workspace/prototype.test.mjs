// Dependency-free behavior smoke tests for the offline design reference only.
// These are not DOM/browser, React integration, SDK or hardware tests.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import { test } from 'node:test';

const html = readFileSync(new URL('./prototype.html', import.meta.url), 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)?.[1];
assert.ok(script, 'One inline simulation script is required');

function demo() {
  const listeners = {}, timers = [], nodes = new Map();
  const node = id => {
    if (!nodes.has(id)) nodes.set(id, { innerHTML: '', textContent: '', focus() {} });
    return nodes.get(id);
  };
  const document = {
    activeElement: null, body: {},
    getElementById: node,
    querySelector: () => null,
    querySelectorAll: () => [],
    addEventListener: (event, handler) => { listeners[event] = handler; },
  };
  const context = vm.createContext({
    document, location: { hash: '#overview' }, console,
    window: { addEventListener() {}, scrollTo() {} },
    setTimeout: fn => { timers.push(fn); return timers.length; },
  });
  vm.runInContext(script, context, { timeout: 1000 });
  const evaluate = code => vm.runInContext(code, context, { timeout: 1000 });
  const action = name => listeners.click({ target: { closest: () => ({
    tagName: 'BUTTON', disabled: false, dataset: { action: name },
  }) } });
  const flush = () => { while (timers.length) timers.shift()(); };
  return { evaluate, action, flush, timers };
}

test('reference is offline, public-safe and separate from live IPC', () => {
  assert.doesNotMatch(html, /https?:\/\/|\.ts\.net|\bfetch\s*\(|\bWebSocket\b|\bXMLHttpRequest\b|__TAURI|@tauri|\binvoke\s*\(/i);
  assert.match(html, /OFFLINE SIMULATION/);
  assert.match(html, /application integration is pending/);
  assert.equal((html.match(/<script>/g) || []).length, 1);
});

test('all seven refinements have renderers and malformed hashes recover', () => {
  const d = demo();
  assert.equal(d.evaluate('PAGES.length'), 7);
  assert.ok(d.evaluate('PAGES.every(p => typeof renderers[p.key] === "function")'));
  for (const key of ['overview', 'source', 'stream', 'genapi', 'buffers', 'recorder', 'scatter']) {
    assert.match(d.evaluate(`renderers[${JSON.stringify(key)}]()`), /section|grid/);
  }
  assert.equal(d.evaluate('location.hash = "%E0%A4%A"; current()'), 'overview');
});

test('provider retry does not establish capture receipt or reset active capture', () => {
  const d = demo();
  d.action('retry-provider');
  assert.equal(d.evaluate('state.providerOpening'), true);
  assert.equal(d.evaluate('state.providerReady'), false);
  d.flush();
  assert.equal(d.evaluate('state.providerReady'), true);
  assert.equal(d.evaluate('state.acq'), 'receiving');
});

test('forbidden feature context sends no write', () => {
  const d = demo();
  d.evaluate('startFeatureWrite()');
  assert.equal(d.evaluate('state.writeSequence'), 0);
  assert.equal(d.evaluate('state.writePending'), null);
  d.evaluate('state.sel = "Line3"; setAcquisition("stopped"); startFeatureWrite()');
  assert.equal(d.evaluate('state.writeSequence'), 0);
});

test('invalid feature values are rejected relative to minimum and step', () => {
  const d = demo();
  d.evaluate('state.sel = "Line1"; setAcquisition("stopped")');
  for (const value of ['', '701', '99999', 'NaN']) {
    d.evaluate(`state.requested = ${JSON.stringify(value)}; startFeatureWrite()`);
    assert.equal(d.evaluate('state.writeSequence'), 0);
    assert.equal(d.evaluate('state.featureValues.Line1'), 3200);
  }
});

test('requested value remains unconfirmed until scoped delayed readback', () => {
  const d = demo();
  d.evaluate('state.sel = "Line1"; setAcquisition("stopped"); state.requested = "4500"; startFeatureWrite()');
  assert.equal(d.evaluate('state.featureValues.Line1'), 3200);
  assert.ok(d.evaluate('state.writePending'));
  d.evaluate('state.sel = "Line2"');
  d.flush();
  assert.equal(d.evaluate('state.featureValues.Line1'), 4500);
  assert.equal(d.evaluate('state.featureValues.Line2'), 3990);
  assert.equal(d.evaluate('state.sel'), 'Line2');
});

test('feature rejection preserves confirmed value', () => {
  const d = demo();
  d.evaluate('state.sel = "Line1"; setAcquisition("stopped"); state.writeFailure = true; startFeatureWrite()');
  d.flush();
  assert.equal(d.evaluate('state.featureValues.Line1'), 3200);
  assert.match(d.evaluate('state.writeMessages.Line1'), /rejection/);
});

test('preview conversion does not change acquisition or recording', () => {
  const d = demo();
  d.action('preview');
  assert.equal(d.evaluate('state.previewReady'), true);
  assert.equal(d.evaluate('state.acq'), 'receiving');
  assert.equal(d.evaluate('state.recordStage'), 'recording');
});

test('buffer plans require stopped capture and adequate reserve; no auto-resume', () => {
  const d = demo();
  d.evaluate('state.draftHistory = 13');
  d.action('apply-buffers');
  assert.equal(d.evaluate('state.history'), 12);
  d.evaluate('setAcquisition("stopped")');
  d.action('apply-buffers');
  assert.equal(d.evaluate('state.history'), 13);
  assert.equal(d.evaluate('state.acq'), 'stopped');
  d.evaluate('state.draftHistory = 15');
  d.action('apply-buffers');
  assert.equal(d.evaluate('state.history'), 13);
});

test('inspection holds reduce free slots without inventing a lost-frame count', () => {
  const d = demo();
  const before = d.evaluate('bufferStats().free');
  d.action('pin-copy');
  assert.equal(d.evaluate('bufferStats().held'), 1);
  assert.equal(d.evaluate('bufferStats().free'), before - 1);
  d.action('pool-stall');
  assert.equal(d.evaluate('bufferStats().free'), 0);
});

test('recording stop/drain/flush/error/retry conserve accepted records', () => {
  const d = demo();
  d.action('recorder-stop');
  d.action('recorder-close'); // Not allowed before complete drain.
  assert.equal(d.evaluate('state.recordStage'), 'stopping');
  assert.equal(d.evaluate('state.recordPending'), 8);
  d.action('recorder-drain-start');
  d.action('recorder-drain');
  assert.equal(d.evaluate('state.recordPending'), 4);
  d.action('recorder-drain');
  assert.equal(d.evaluate('state.recordStage'), 'finalizing');
  d.evaluate('state.flushFailure = true');
  d.action('recorder-close');
  assert.equal(d.evaluate('state.recordStage'), 'error');
  assert.equal(d.evaluate('writtenCount()'), 6508);
  d.evaluate('state.flushFailure = false');
  d.action('recorder-retry');
  d.action('recorder-close');
  assert.equal(d.evaluate('state.recordStage'), 'closed');
  assert.equal(d.evaluate('state.recordReceived'), 6508);
  assert.equal(d.evaluate('state.acq'), 'receiving');
});

test('copying 100 percent waits for verification and cancel/failure are explicit', () => {
  const d = demo();
  d.action('export-start');
  for (let i = 0; i < 4; i++) d.action('export-advance');
  assert.equal(d.evaluate('state.exportStage'), 'verifying');
  d.action('export-verify');
  assert.equal(d.evaluate('state.exportStage'), 'complete');
  d.action('export-start'); d.action('export-advance'); d.action('cancel-export');
  assert.equal(d.evaluate('state.exportStage'), 'cancelled');
  assert.equal(d.evaluate('state.exportPercent'), 25);
  d.action('export-start'); d.action('export-fail');
  assert.equal(d.evaluate('state.exportStage'), 'failed');
  assert.equal(d.evaluate('state.recordReceived'), 6508);
});

test('scatter coordinates derive from metrics and inspection leaves the gate intact', () => {
  const d = demo();
  assert.ok(Math.abs(d.evaluate('plotX(records["R-024210"].area)') - 383.22) < 1e-6);
  assert.ok(Math.abs(d.evaluate('plotY(records["R-024210"].def)') - 226.112) < 1e-6);
  const gate = d.evaluate('GATE_PATH');
  d.evaluate('chooseRecord("R-024231")');
  assert.equal(d.evaluate('GATE_PATH'), gate);
});

test('old image response cannot overwrite newer selection; availability is per record', () => {
  const d = demo();
  d.evaluate('chooseRecord("R-024210"); chooseRecord("R-024231")');
  // Complete B before A, regardless of their demo timer durations.
  const a = d.timers.shift(), b = d.timers.shift();
  b(); a();
  assert.equal(d.evaluate('state.load.key'), 'R-024231');
  assert.equal(d.evaluate('state.ignoredImages'), 1);
  d.evaluate('state.evidence["R-024210"] = "expired"; chooseRecord("R-024210")');
  assert.equal(d.evaluate('state.load.phase'), 'unavailable');
  assert.equal(d.evaluate('state.evidence["R-024231"]'), 'saved');
});
