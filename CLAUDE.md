# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Tools to transfer BASIC programs (ASCII text) between a Windows PC and a **Sharp PC-E500S** pocket computer over an RS-232 serial link. The repository contains **two independent, parallel implementations of the same transfer protocol** plus the Sharp-side bootstrap program:

- `Powershell/toSharp.ps1` + `Powershell/fromSharp.ps1` — the reference implementation (`System.IO.Ports.SerialPort`). Comments here document the empirically validated timing/handshake settings.
- `sharp_pc_e500s_comm.c` — a single-file Win32 (`CreateFileA`/`WriteFile`/`ReadFile`, DCB) CLI port (`--mode send|receive`), rewritten to match the PowerShell protocol exactly (CR+LF line endings, `0x1A` EOF, throttled send, initial XON on receive, manual OPEN/LOAD/SAVE prompt). The two implementations should stay behaviorally in sync.
- `SerialTool/INIT.BAS` — BASIC program that runs *on the Sharp* to configure it as the serial peer.
- `Documentation/` — Sharp PC-E500 manuals (PDF). `SerialTool/*.png` — reference screenshots of a working serial-terminal config.

There is no build system, test suite, or package manifest. Changes are validated by running against real hardware.

## Running

PowerShell scripts (run from the `Powershell/` directory):
```powershell
# PC -> Sharp: on the Sharp run OPEN "COM:9600,N,8,1,A,L,&1A,X,N" then LOAD, then:
.\toSharp.ps1 -InputFile .\PROG.BAS -PortName COM1

# Sharp -> PC: START THIS SCRIPT FIRST, then run SAVE "COM:" on the Sharp:
.\fromSharp.ps1 -OutputFile .\PROG.BAS -PortName COM1
```

C program (no Makefile — compile the single source file):
```bash
gcc sharp_pc_e500s_comm.c -o sharp_comm.exe -lwinmm  # MinGW (-lwinmm for timeBeginPeriod)
# or: cl sharp_pc_e500s_comm.c                        # MSVC (winmm linked via #pragma)

./sharp_comm.exe --mode send    input.bas  -p COM1 -v
./sharp_comm.exe --mode receive output.bas -p COM1 -v
./sharp_comm.exe --show-serial -p COM1                # dump current port DCB/timeouts
```

Both `send` and `receive` print the `OPEN`/`LOAD`/`SAVE` command to type on the Sharp (with the *actual* selected baud) and then wait for the user to press Enter (matching the PowerShell workflow) — they do **not** drive the Sharp automatically. `send` defaults to the fast flow-controlled profile (`--rts on --char-delay 0 --line-delay 0 --chunk 16`, ~305 B/s); tune with `--char-delay` / `--line-delay` / `--chunk` / `--rts` / `--dtr`, and the receive idle timeout with `--idle-timeout`. There is **no send acknowledgement** — a fast "Emission terminee" only means the PC finished writing; always verify the load with `LIST` on the Sharp.

## Critical domain constraints

These are hardware-validated and must be preserved in any change to the transfer path — they are not arbitrary and cannot be inferred from the code alone:

- **Serial line: `9600,N,8,1` with XON/XOFF handshake.** The Sharp side is opened with `OPEN "COM:9600,N,8,1,A,L,&1A,X,N"`. The `L` means the Sharp expects **CR+LF** line endings; senders must emit `\r\n`. `&1A` (0x1A / Ctrl+Z) is the **EOF marker** — it terminates both send and receive.
- **RTS=ON is what makes send-side XON/XOFF work — this is the key finding.** The Sharp only transmits (data *and* the XOFF that throttles us) while its **CS input (pin 5) is high**, driven by the PC's **RTS** (Technical Reference p.54, `SIO send port condition` = 04H → "transmit when CS high"). With `RTS=OFF` during send, the Sharp's buffer overflows and it *cannot* send XOFF → **I/O ERROR on the Sharp screen**. So the C `send` default is now **RTS=ON** (`--rts on`), which lets `fOutX` flow control pace the PC correctly. `fromSharp.ps1` already used RTS=ON for the same reason. DTR stays OFF by default (`--dtr` to override).
- **Fast send is flow-controlled, not delay-paced.** With RTS=ON + `--char-delay 0`, the PC streams and the Sharp's XOFF governs the rate entirely — `--chunk` and `--line-delay` then have *no measurable effect* (16/32/64 all identical). Measured ceiling ≈ **305 bytes/s**, which is the Sharp's `LOAD` tokenization rate (CPU-bound), ~5× the old blind byte-at-a-time profile. **C send defaults: `--rts on --char-delay 0 --line-delay 0 --chunk 16`.** Fallback if flow control is ever unavailable (I/O ERROR): blind paced mode `--chunk 1 --char-delay 16` (~58 B/s, always works).
- **Baud rate does not help send, but does help receive.** Send is `LOAD`-CPU-bound: 9600 and 19200 both give ~305 B/s, so keep **9600** (more XOFF timing margin). Receive (Sharp `SAVE`) is wire-bound and streams fast, so it *does* scale with baud (e.g. ~20 s at 9600 vs ~12 s at 19200 for the same dump). The two directions are asymmetric by nature: `SAVE` dumps memory quickly, `LOAD` parses/tokenizes slowly.
- **Windows timer granularity.** Default resolution is ~15.6 ms, so a `Sleep(1)` / `Start-Sleep -Milliseconds 1` really waits ~15.6 ms — which is why the old "`CharDelayMs=1`" profile actually ran at ~15.6 ms/byte. The C program calls `timeBeginPeriod(1)` around the transfer (needs `-lwinmm`) so `--char-delay`/`--line-delay` are honored as true ms. Mostly moot now that the default fast path uses `--char-delay 0`, but it matters for the paced fallback.
- **Streaming vs. paced send (`write_bytes_controlled`).** `--char-delay > 0`: drain each chunk with `FlushFileBuffers` then `Sleep` (tight pacing, blind fallback). `--char-delay 0`: no per-chunk drain — `WriteFile` fills the driver buffer and `fOutX` pauses on the Sharp's XOFF; one `FlushFileBuffers` runs before the EOF. This is the default fast path.
- **USB-serial adapter flow-control latency.** On this setup the adapter is a **Prolific PL2303GC** (no FTDI-style latency timer). Its Device Manager → Port Settings → Advanced **FIFO "Receive Buffer"** defaults to High (14), which delays inbound-XOFF detection; set it (and Transmit Buffer) to **Low (1)** for responsive flow control. On FTDI adapters the equivalent is the *Latency Timer* → 1 ms. This is a system setting, not code.
- **Never send the `OPEN`/`CLOSE`/`LOAD`/`SAVE` commands over the wire.** They must be typed on the Sharp keyboard. Sending them down the serial line corrupts the transferred program (the earlier C version did this and failed).
- **Receiver must send an initial XON (0x11)** to authorize the Sharp to transmit, then read until `0x1A` or an idle-silence timeout (`fromSharp.ps1 -IdleTimeoutMs`, default 3000).
- **Ordering matters for receive:** start the PC receiver *before* issuing `SAVE "COM:"` on the Sharp.
- **File payloads are plain ASCII** — no UTF-8 BOM, no accented characters. `Get-Content -Encoding ASCII` / `Encoding.ASCII` are used deliberately.

## Sharp-side setup

`SerialTool/INIT.BAS` runs on the calculator to prepare it as the serial peer: it remaps function keys, POKEs low-level config, and opens/closes the COM channel with the same `OPEN "COM:9600,N,8,1,A,L,&1A,X,N"` string. Keep this OPEN string in sync across `INIT.BAS`, the PowerShell scripts, and the prompt strings printed by the C program.

## Reference: serial protocol details

The behavior above is grounded in the bundled manuals (`Documentation/`, scanned images — extract pages with PyMuPDF, they have no text layer):
- **PC-E500 manual, printed p. 303-304** — full `OPEN "COM:..."` field syntax (baud, parity, word length, stop, code `A`, delimiter `C`/`F`/`L`, EOF code, XON `N`/`X`, shift `S`/`N`).
- **Technical Reference, printed p. 53-59** (physical PDF pages 58-64; printed-to-physical offset is +5) — SIO hardware: XON=`0x11`, XOFF=`0x13`, EOF=`0x1A`; connector pinout showing the Sharp watches its **CS (pin 5)** and **CD (pin 8)** inputs before transmitting/receiving; tunable timing registers (`SIO send n byte wait` BFD60, `SIO send delay` BFD39, `SIO open/close wait` BFD40 — 20 ms default for the CE-130T level converter).

## Conventions

User-facing strings and code comments are in **French**; match that when editing existing files.
