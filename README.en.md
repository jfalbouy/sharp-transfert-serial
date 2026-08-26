# sharp_comm — Serial transfer PC ⇄ Sharp PC-E500S

*(Version française : [README.md](README.md))*

A Windows command-line tool to transfer **ASCII text BASIC** programs between a PC and a
**Sharp PC-E500S** pocket computer over an RS-232 serial link.

- **Send** (PC → Sharp): the PC pushes a `.BAS` file, the Sharp receives it with `LOAD`.
- **Receive** (Sharp → PC): the Sharp emits with `SAVE "COM:"`, the PC saves the file.

> The program does **not** drive the Sharp automatically: it prints the command to type on
> the calculator, then waits for you to press Enter. You keep control of the Sharp.

---

## 1. Requirements

- **Windows** with a serial port (native or a **USB-to-serial** adapter).
- A Sharp **cable / level converter** (e.g. CE-130T) linking the PC to the PC-E500S.
- A C compiler (MinGW `gcc` or MSVC `cl`) to build the executable.

### Important USB-to-serial adapter setting

XON/XOFF flow control must react quickly. In **Device Manager** → *Ports (COM & LPT)* →
your port → **Properties** → **Port Settings** → **Advanced…**:

- **Prolific PL2303**: set the **FIFO "Receive Buffer" to Low (1)** (and "Transmit Buffer"
  to Low (1)).
- **FTDI**: set the **Latency Timer to 1 ms**.

Without this, the Sharp's XOFF is detected too late and the transfer may fail (I/O ERROR).

---

## 2. Building

```bash
# MinGW (gcc) — -lwinmm is required (timeBeginPeriod)
gcc sharp_pc_e500s_comm.c -o sharp_comm.exe -lwinmm

# or MSVC (winmm is linked automatically via #pragma)
cl sharp_pc_e500s_comm.c
```

Check:

```bash
sharp_comm.exe --help
sharp_comm.exe --show-serial -p COM1     # print the port's current settings
```

---

## 3. Sharp PC-E500S settings

The PC-E500S must be configured at the **same baud rate** as the PC. The program prints the
exact line to type (with the selected baud). Reference configuration:

| Parameter | Value |
|-----------|-------|
| Baud rate | 9600 (recommended) |
| Format | 8 bits, parity None, 1 stop |
| Code | A (ASCII) |
| Delimiter | **L** (CR + LF) |
| EOF | **&H1A** (Ctrl+Z) |
| XON/XOFF | **X** (enabled) |
| Shift | N |

Open command (typed **on the Sharp**):

```
OPEN "COM:9600,N,8,1,A,L,&H1A,X,N"
```

then `LOAD` to receive, or `SAVE "COM:"` to send.

> At 19200 baud, type `OPEN "COM:19200,..."`. The program reminds you of the exact line.

---

## 4. Usage

### Send: PC → Sharp

```bash
sharp_comm.exe --mode send myprog.bas -p COM1
```

Steps:
1. The program prints the `OPEN … / LOAD` command to type.
2. On the Sharp, type `OPEN "COM:9600,N,8,1,A,L,&H1A,X,N"` then `LOAD`.
3. Press **Enter** on the PC: the file is sent.
4. **Verify with `LIST` on the Sharp** that the program loaded correctly (see §6).

### Receive: Sharp → PC

```bash
sharp_comm.exe --mode receive out.bas -p COM1
```

Steps:
1. The program prints the `OPEN … / SAVE "COM:"` command.
2. On the Sharp, type `OPEN "COM:9600,N,8,1,A,L,&H1A,X,N"`.
3. Press **Enter** on the PC **to arm reception**.
4. **Only then**, run `SAVE "COM:"` on the Sharp. *(Order matters.)*
5. Reception stops on the EOF (`0x1A`) or after a prolonged silence.

---

## 5. Options

```
-p, --port <port>       COM port (e.g. COM1, COM2). Default: COM1
-b, --baud <rate>       Baud: 300, 600, 1200, 2400, 4800, 9600, 19200. Default: 9600
-m, --mode <mode>       'send' or 'receive'
    --char-delay <ms>   Delay between packets (0 = XON/XOFF flow control). Default: 0
    --line-delay <ms>   Delay between BASIC lines. Default: 0
    --chunk <n>         Bytes per serial write. Default: 16
    --idle-timeout <ms> Max silence before stopping (receive). Default: 3000
    --rts <on|off>      RTS (Sharp's CS input, required for XON/XOFF). Default: on
    --dtr <on|off>      DTR (Sharp's CD input). Default: off
-v, --verbose           Print transferred data / lines
-s, --show-serial       Print serial port settings and exit
-h, --help              Print help
```

### Fast profile (default) and fallback

- **Default = fast**: `--rts on --char-delay 0 --line-delay 0 --chunk 16`
  → ~**305 bytes/s**, paced by the Sharp's XON/XOFF. Nothing to specify.
- **Fallback** if `I/O ERROR` on the Sharp (flow control unavailable):
  ```bash
  sharp_comm.exe --mode send myprog.bas -p COM1 --chunk 1 --char-delay 16
  ```
  Blind paced send, ~58 bytes/s, but works without flow control.

---

## 6. Verification — important

**When sending (PC → Sharp), there is NO acknowledgement.** The "Emission terminee (cote PC)"
message only means the PC finished writing its bytes — **not** that the Sharp accepted them.

➡️ **Always verify with `LIST` on the Sharp** that the program is complete and not corrupted.
A fast completion is not proof of success.

---

## 7. Troubleshooting

| Symptom | Likely cause | Fix |
|---------|--------------|-----|
| `I/O ERROR` on the Sharp screen | The Sharp can't throttle (XOFF impossible) because its CS is low | Keep **`--rts on`** (default); set the USB FIFO to Low (1); else fall back to `--chunk 1 --char-delay 16` |
| `LIST` shows a truncated/corrupt program | Sent too fast without effective flow control | Fall back to `--chunk 1 --char-delay 16`, or fix the adapter FIFO setting |
| No bytes received (receive) | `SAVE` launched before arming the PC, or wrong port | Arm the PC **before** `SAVE`; check `-p COMx` with `--show-serial` |
| "Cannot open port" | Port busy (another app) or nonexistent | Close the other app; check the port number |
| Garbage in the received file | PC baud ≠ Sharp baud | Open the Sharp at the **same baud** (`OPEN "COM:<baud>,…"`) |

---

## 8. Technical notes

- **RTS=ON is the key to fast sending.** The Sharp only transmits (its data *and* the XOFF
  that throttles the PC) while its **CS input (pin 5)** is high, driven by the PC's RTS
  (*Technical Reference* p. 54, `SIO send port condition` = 04H). RTS low ⇒ no XOFF ⇒
  overflow ⇒ I/O ERROR.
- **Baud rate does not speed up sending.** `LOAD` tokenizes BASIC at ~305 bytes/s
  (Sharp-CPU-bound): 9600 and 19200 take the same time. Keep 9600.
- **Receiving, however, benefits from baud rate**: `SAVE` dumps memory fast (wire-bound),
  so 19200 is ~2× faster than 9600 when receiving.
- **Line endings**: whatever the input format (`\n`, `\r`, `\r\n`), the program sends
  **CR+LF**, matching the `L` delimiter.
- **Files**: plain ASCII, **no UTF-8 BOM, no accented characters**.

> A PowerShell equivalent exists (`Powershell/toSharp.ps1`, `Powershell/fromSharp.ps1`)
> with the same fast profile.

---

## 9. Optimization journey (58 → 305 bytes/s)

A record of the reasoning that led to the fast profile — useful to understand *why* the
settings are what they are.

**Starting point.** Sending PC → Sharp took **3 min 41 s** for 12,733 bytes, i.e.
**~58 bytes/s**. Baud rate (9600) made no difference.

| Step | Hypothesis tested | Result |
|------|-------------------|--------|
| 1 | Is the per-byte delay really 1 ms? | No: `Sleep(1)` lasts **~15.6 ms** (Windows timer granularity). Added `timeBeginPeriod(1)` for accurate delays. |
| 2 | Lower the per-byte delay (8/4/2 ms) | **I/O ERROR** on the Sharp. The reliable byte-at-a-time floor is ~10–16 ms → a dead end for speed. |
| 3 | Stream in large chunks, `--char-delay 0` | I/O ERROR, even after setting the USB FIFO to Low (1). Without throttling, overflow is inevitable. |
| 4 | **What if the Sharp couldn't send XOFF?** | 🎯 **Found it.** The Sharp only transmits while its **CS input is high** (*Tech Ref* p. 54). CS = the PC's RTS, and RTS was **OFF** when sending → the XOFF never left. |
| 5 | `--rts on` + streaming | ✅ **304 bytes/s, `LIST` intact.** Flow control finally paces the PC at `LOAD`'s speed. |
| 6 | Push further (chunk 32/64, line-delay 0, 19200) | No effect: **305 B/s is `LOAD`'s tokenization ceiling** (CPU-bound). The wire is not the bottleneck. |

**Result: 58 → 305 bytes/s (×5.3), and — crucially — reliable.**

**Key takeaways:**
- Windows timer granularity hid the true behavior of the delays (`Sleep(1)` ≈ 15.6 ms).
- **RTS=ON is essential** for XON/XOFF to work when sending (it powers the Sharp's CS input).
- Once flow control is active, all PC-side settings (chunk, line-delay, baud) become
  irrelevant: the Sharp dictates the pace.
- Sending (`LOAD`, CPU-bound) and receiving (`SAVE`, wire-bound) are **asymmetric**: baud
  rate helps receiving, not sending.
