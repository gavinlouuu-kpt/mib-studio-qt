// The startup restore gate (#883, MIBQT-45): the remembered target ring width waits for the startup profile or method restore.
import { beforeEach, expect, it } from 'vitest';
import { resetStartupRestore, settleStartupRestore, startupRestoreSettled, takeStartupSetpointCapture } from './startupRestoreGate';

const flush = () => new Promise(resolve => setTimeout(resolve, 0));
beforeEach(() => resetStartupRestore());

it('holds waiters until the restore settles, then lets later callers through at once', async () => {
  let done = false;
  void startupRestoreSettled().then(() => { done = true; });
  await flush();
  expect(done).toBe(false);
  settleStartupRestore();
  await flush();
  expect(done).toBe(true);
  let later = false;
  void startupRestoreSettled().then(() => { later = true; });
  await flush();
  expect(later).toBe(true);
});

it('a new backend session waits for its own restore, and a waiter of the old one keeps waiting for it', async () => {
  settleStartupRestore();
  resetStartupRestore();
  let done = false;
  void startupRestoreSettled().then(() => { done = true; });
  await flush();
  expect(done).toBe(false);
  settleStartupRestore();
  await flush();
  expect(done).toBe(true);
});

it('hands the setpoint capture out once per session, and only after the restore settled', () => {
  expect(takeStartupSetpointCapture()).toBe(false);
  settleStartupRestore();
  expect(takeStartupSetpointCapture()).toBe(true);
  expect(takeStartupSetpointCapture()).toBe(false);
  settleStartupRestore();                                   // settling again changes nothing
  expect(takeStartupSetpointCapture()).toBe(false);
  resetStartupRestore(); settleStartupRestore();
  expect(takeStartupSetpointCapture()).toBe(true);
});
