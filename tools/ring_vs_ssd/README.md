# ring_vs_ssd

Compares the frames Studio's playback served from the frame ring with the records of the **same SSD run** (M1 gate 5, #693). Host only, nothing here touches the board.
It reads files the slot saved and reuses the decoder vendored for `tools/pzrec_to_h5` (`third_party/pz7035-decoder`, checked against its `PROVENANCE.json`); nothing new is vendored.

```
python3 tools/ring_vs_ssd/ring_vs_ssd.py \
  --ring OUT/ring --status OUT/ring-status-1.json \
  --ring2 OUT/ring2 --status2 OUT/ring-status-2.json [--status3 OUT/ring-status-3.json] \
  --run DL/run19.bin --runs OUT/post.runs.json --run-id 19 --out OUT/ring-vs-ssd
```

Exit 0 only when every check passes, 1 on any FAIL, 2 on unusable input (a ring status without `epoch`: bridge ABI 37 is needed; an empty or headerless export; a run table without the run or with a filter other than ALL). Output: `ring-vs-ssd.json` and `summary.txt`.

## Inputs (what the slot saves)

| Input | Content |
|---|---|
| `--ring DIR` | `seq-<n>.mibr`: the unmodified `fetch_ring_frame {seq}` packets (`MIBR` v1) of **every** sequence `first_seq..last_seq`, read after the SSD run closed and the ring was held |
| `--status F` | `fetch_ring_status` of that read: `frozen`, `run_frozen`, `first_seq`, `last_seq`, `count`, `capacity_frames`, **`epoch`** (ABI 37) |
| `--ring2 DIR`, `--status2 F` | the status and a spread subset (about 50 sequences) re-read **after** the export; `--status3 F` a later one (ring idle 10 minutes). Required unless `--no-reread` |
| `--run F`, `--runs F`, `--run-id N` | the export (`GET /ssd/runs/{id}/records`, whole 59,392 B records) and the run table (`post.runs.json`, required: record count, `last_frame_id`, tick rate; the run must have used the ALL filter, else exit 2) |

## Key and criteria

The key is **(epoch, frame_id)**. `MIBR` carries no epoch, so the ring side takes it from the status (a frozen ring holds one ARM); the SSD side from each record's FRAME header. The SSD tail's epoch must equal it.
Criteria follow `pzrec_records.md` "Frames in both the ring and the SSD" and the plan on #693:

- **a** every ring frame inside the SSD run's final consecutive tail exists on the SSD and is equal in MONO8 bytes, MASK1 bits, the 19 words of every RESULT cell, ticks and frame flags (the SSD-only STORED bit aside); every SSD tail record inside the ring's range is in the ring; the stream has the run table's record count and ends at `last_frame_id`. Ticks, frame flags and the flag agreement are compared for flagged frames too.
- **b** ring frames older than the tail are equal when the SSD has them and absent otherwise (counted; at most `--guard`, 256).
- **c** the ring is complete and consecutive: a packet for every sequence, each packet's own sequence equals its file, `frame_id - seq` is constant (a dropped sensor frame, FRAMES_LOST, inside the ring window trips this as well as a corrupt packet); the status says frozen, held (`run_frozen`) and valid; the packets' tick rate is the run table's; a run with more records than the ring leaves a full ring (`count == capacity_frames`).
- **d** at least one ring frame is compared in the tail; overlap >= ring frames - `--guard` (256) - max(measured delta, 0), where the measured delta is how far the ring's newest frame is ahead of the run's `last_frame_id`; that distance is capped by `--delta-max` (32).
- **e** after the export the ring-defining status (`frozen`, `run_frozen`, `invalid`, `epoch`, `first_seq`, `last_seq`, `count`, `head`, `state`, `capacity_frames`, `stop_incomplete`) is identical and every re-read packet is byte-equal to the first read (the export did not disturb the ring); the later idle status too. Other status fields (`sensor_fps` is a live reading) are reported as INFO only.
- **f** frames flagged cut / mask incomplete / invalid are listed by name and excluded from the pixel, mask and cell comparison (`--strict-flags` makes them failures); a flag the two sides disagree on fails.

A cell of the `MIBR` packet is `payload[0..14]`, `y<<16|x`, `h<<16|w`, `cells<<24|index<<16|valid`, payload validity mask. A frame with RESULTS_OVERFLOW gets no packet at all (`readFrame` rejects it), so such a frame shows up as a missing packet under **c**.

## Tests

`python3 tools/test_ring_vs_ssd.py` is the CI entry point (qt-ci runs `tools/test_*.py`; CTest registers it as `tools.ring_vs_ssd`; numpy only). The C++ test `processing.ring_packet_golden` keeps `testdata/` honest: `testdata/records.bin` is four consecutive REAL stored records of the export slot's run 19 and `seq-<n>.mibr` the packets Studio's `PzFrameRing::readFrame` + `buildRingPacket` made from them (regenerate on purpose with `mib_backend_tests ring_packet_golden_test tools/ring_vs_ssd/testdata --write`); the Python tests compare those packets with the records, so the RESULT mapping is checked against the C++ builder.


`python3 -m pytest tools/ring_vs_ssd` (the same): synthetic `MIBR` packets and synthetic stored records (built with the vendored ABI encoder), a passing baseline and a mutant per criterion (a gray byte, a mask bit, a result word, ticks, frame flags, a missing packet, a wrong epoch, an altered older frame, a non-consecutive ring, a truncated packet, a too-small overlap, a ring far ahead of the run end, a changed status, a differing re-read packet, a flag disagreement); plus a smoke run on the export slot's real run 19 when it is on the HDD.
