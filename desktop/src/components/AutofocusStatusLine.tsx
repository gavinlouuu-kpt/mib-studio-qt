import type { AutofocusStatus } from "../bridge";

// Qt shows the nanopositioner's runtime messages in the Nanopositioner tab through
// AutofocusService::setStatusCallback ("Connected to ...", "Voltage change failed: ...", "Failed to
// apply safe shutdown voltage: ..."). The bridge keeps the latest message and its sequence on the
// autofocus status (ABI 45, HW-033); this line shows it under the autofocus controls. It renders
// nothing until the service has said something, and a pre-ABI-45 backend never sends a message.

/** The backend's own rule for a message that is also an event: it contains "failed" (any case) or
 *  starts with "No nanopositioner". The routine "Voltage: ... V" line is not a failure. */
export function isAutofocusFailureMessage(message: string): boolean {
  return /failed/i.test(message) || message.startsWith("No nanopositioner");
}

export function AutofocusStatusLine({ status }: { status: AutofocusStatus | null }) {
  const message = status?.status_message?.trim();
  if (!message) return null;
  // A failure is announced and coloured as an alert (.hardware-controls [role="alert"]); anything
  // else is a quiet status line.
  return (
    <p className="autofocus-status-line" role={isAutofocusFailureMessage(message) ? "alert" : "status"}
       data-sequence={status?.status_sequence ?? 0}>
      Autofocus status: {message}
    </p>
  );
}
