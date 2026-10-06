import type { StageStatus } from '../bridge';
import { STAGE_MOVE_STATES } from '../bridgeContract';

/** A connected, configured stage whose zero is set (no mid-travel declaration) at +120.4 µm; the limit wiring
 *  is unverified, like the bench unit (test fixture). */
export const zeroed: StageStatus = {
  valid: true, enabled: true, connected: true, configured: true, zero_set: true, mid_travel_declared: false,
  limits_verified: false, busy: false,
  model: 'ZC300-1A', serial: '26017', firmware: '1.2', port_name: 'ttyUSB1', move_state: STAGE_MOVE_STATES.Idle,
  position_um: 120.4, limit_positive: false, limit_negative: false, home: true, emergency_stop: false, driver_alarm: false,
  span_um: 6000, envelope_min_um: -1000, envelope_max_um: 1000, last_error: '',
};
