// Epic #868 HW-016 / HW-030: the pump and autofocus values Qt keeps in config.json
// (SyringePumpTab.cpp:354-410, NanopositionerTab.cpp:387-392) survive a restart in the desktop
// preferences file, under the `hardware` key. Writes are read-modify-write of the whole file, so
// they are chained here to keep two quick saves from overwriting each other.
import {bridge} from '../bridge';

export type PumpPrefs = {rate?: string; unit?: number; direction?: number; volume?: string; volumeUnit?: number};
export type HardwarePrefs = {pumps?: PumpPrefs[]; focusSetpoint?: number; jogStep?: number};

export async function loadHardwarePrefs(): Promise<HardwarePrefs> {
  try {
    const value = (await bridge.getPreferences())?.hardware;
    return value && typeof value === 'object' ? value as HardwarePrefs : {};
  } catch { return {}; }
}

let chain: Promise<unknown> = Promise.resolve();
export function saveHardwarePrefs(change: (old: HardwarePrefs) => HardwarePrefs): Promise<void> {
  const next = chain.then(async () => {
    const all = (await bridge.getPreferences()) ?? {};
    const old = all.hardware && typeof all.hardware === 'object' ? all.hardware as HardwarePrefs : {};
    await bridge.setPreferences({...all, hardware: change(old)});
  });
  chain = next.catch(() => {});
  return next;
}

export function withPump(old: HardwarePrefs, id: number, value: PumpPrefs): HardwarePrefs {
  const pumps = [...(old.pumps ?? [{}, {}])];
  pumps[id] = {...(pumps[id] ?? {}), ...value};
  return {...old, pumps};
}
