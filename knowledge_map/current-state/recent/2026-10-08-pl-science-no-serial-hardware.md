## 2026-10-08 — A PL-science instrument refuses nanopositioner, pulse-generator and ZC300 discovery and stage connect

For YOFO Studio as the PZ7035's standing state, the backend no longer relies on the UI hiding the
controls: with science on the PL (`hostProcessingAvailable()` false) the discovery service refuses
requests for the nanopositioner, pulse-generator and motion-stage kinds before any provider touches a
port (`DeviceDiscoveryService::setBlockedKinds`, set in `AppBackend::initialize`), and a ZC300
`stage_connect` is refused, both with the reason the UI shows (`AppBackend::plScienceSerialBlockReason`):
the RS485 bus carries the pumps. Camera discovery and the pump commands are unchanged. Tested in
`instrument_modes_test`. See [[architecture/AppBackend]], [[services/DeviceDiscoveryService]].
