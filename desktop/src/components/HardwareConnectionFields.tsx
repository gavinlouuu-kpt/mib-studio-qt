import { numericInput } from './hardwareControlModel';

export type Connection = { port: string; baud: string; address: string };
export const initialConnection = (): Connection => ({port: '', baud: '115200', address: '1'});
export function ConnectionFields({value, onChange, disabled, systemPort = false}: {value: Connection; onChange: (v: Connection) => void; disabled: boolean; systemPort?: boolean}) {
  return <div className="hardware-fields">{(['port', 'baud', 'address'] as const).map(key => <label key={key}>{ {port: systemPort ? 'System serial port (COM3 or /dev/ttyUSB0)' : 'COM port number', baud: 'Baud rate', address: 'Device address'}[key] }
    <input type={systemPort && key === 'port' ? 'text' : 'number'} value={value[key]} disabled={disabled} onChange={event => onChange({...value, [key]: event.target.value})} />
  </label>)}</div>;
}
export function connectionArgs(value: Connection, addressMax: number, addressMin = 1): [number, number, number] {
  return [numericInput(value.port, 'COM port', 1, 65535, true), numericInput(value.baud, 'Baud rate', 1, 4000000, true), numericInput(value.address, 'Address', addressMin, addressMax, true)];
}
