// Opening the hardware settings dialogs from anywhere (the pump panel's button, the Settings menu): one window event each,
// answered by <HardwareDialogs>, which the shell mounts once. No import cycle between the panel, the menu and the dialogs.
export const OPEN_PUMP_SETTINGS_EVENT = 'mib:open-pump-settings';
export const OPEN_BOOT_SERVICES_EVENT = 'mib:open-boot-services';
export const openPumpSettings = () => window.dispatchEvent(new Event(OPEN_PUMP_SETTINGS_EVENT));
export const openBootServices = () => window.dispatchEvent(new Event(OPEN_BOOT_SERVICES_EVENT));
