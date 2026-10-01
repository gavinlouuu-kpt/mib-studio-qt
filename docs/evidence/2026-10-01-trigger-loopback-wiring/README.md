# Trigger loopback wiring — 2026-10-01

Where the SSG3021X PULSE OUT loopback goes on this rig, from the user's
photos (annotated by the container agent; nothing was wired yet).

| Photo | What it shows |
|---|---|
| [where_to_plug_1_ssg.jpg](where_to_plug_1_ssg.jpg) | SSG3021X rear panel: loopback cable on **PULSE IN/OUT** (currently only an empty BNC adapter); sort TTL stays in **TRIG IN/OUT**; LAN for SCPI |
| [where_to_plug_2_breakout.jpg](where_to_plug_2_breakout.jpg) | **HL-DB26T-mini** terminal board = HD26 breakout of the Coaxlink **External I/O** connector: loopback signal → **terminal 25 (TTLIO11)**, shield → **terminal 24 (GND)**; existing red/black on 17/18 = TTLIO12 + GND (sort output) |

Pin source: Euresys Coaxlink Hardware Manual 12.5 (D205), "External I/O
Connector" (26-pin 3-row HD sub-D): 17 TTLIO12, 18 GND, 24 GND, 25 TTLIO11.
The red/black pair on 17/18 matches TTLIO12, which confirms the mapping.

**Open — check before wiring:** a green and a yellow wire already land on the
same terminal strip around 23–25 (red pen marks "+" / "−" next to them). If
the yellow wire is on 25, TTLIO11 is already used by something else; identify
it before reusing the pin. Fallback input if TTLIO11 is taken: isolated
input IIN11 (HD26 pin 3 = IIN11+, pin 12 = IIN11−; current-sense input,
compatible with TTL/5 V CMOS drivers, `LineInputToolSource = IIN11`).

Electrical limit for TTLIO11: 0–5 V absolute maximum (HIGH > 2.0 V, LOW <
0.8 V). Scope the SSG PULSE OUT before connecting it.
