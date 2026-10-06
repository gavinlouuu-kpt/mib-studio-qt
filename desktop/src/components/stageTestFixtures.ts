import type { StageStatus } from '../bridge';
import { STAGE_MOVE_STATES } from '../bridgeContract';

/** A connected, configured, homed and limit-verified stage at +120.4 µm (test fixture). */
export const homed: StageStatus = {
  valid: true, enabled: true, connected: true, configured: true, referenced: true, limits_verified: true, busy: false,
  model: 'ZC300-1A', serial: '26017', firmware: '1.2', port_name: 'ttyUSB1', move_state: STAGE_MOVE_STATES.Idle,
  position_um: 120.4, limit_positive: false, limit_negative: false, home: false, emergency_stop: false, driver_alarm: false,
  span_um: 6000, soft_min_um: -2900, soft_max_um: 2900, last_error: '',
};
