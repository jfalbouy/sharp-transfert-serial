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

> The Sharp does **not** accept a baud rate above 9600 in the `OPEN "COM:xxxx,..."`
> command. To use 19200 baud, you must change the SIO baud value at address `0BFD33h`,
> as described on page 53 of the Technical Reference.
>
> To switch to **19200 baud**, type on the Sharp:
>
> ```
> POKE &HBFD33, PEEK &HBFD33 OR &H70
> ```
>
> Resulting value `&H78` / 120d, i.e. bits 6, 5 and 4 set to 1.
>
> To go back to **9600 baud** without touching the other parameters, type on the Sharp:
>
> ```
> POKE &HBFD33, (PEEK &HBFD33 AND &H8F) OR &H60
> ```
>
> Resulting value `&H68` / 104d, i.e. bits 6, 5, 4 back to 110.

Then type `OPEN "COM:19200,..."` on the Sharp. The program reminds you of the exact line
to type, with the baud rate actually selected.

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

---

## 10. Measured transfer figures (Sharp ⇄ PC)

| File | Size (bytes) | Baud (bits/s) | Direction | Time (s) | Throughput (bits/s) |
|---------|-----------------|-----------------|----------------|-----------|------------------|
| ISOTOP  | 17487           | 9600            | PC -> Sharp | 56.27     | 311              |
| ISOTOP  | 17487           | 9600            | Sharp -> PC | 27.23     | 642              |
| ISOTOP  | 17487           | 19200           | PC -> Sharp | 55.27     | 316              |
| ISOTOP  | 17487           | 19200           | Sharp -> PC | 16.30     | 1073             |
| ELECTR  | 9297            | 9600            | PC -> Sharp | 28.05     | 331              |
| ELECTR  | 9297            | 9600            | Sharp -> PC | 13.97     | 666              |
| ELECTR  | 9297            | 19200           | PC -> Sharp | 27.56     | 337              |
| ELECTR  | 9297            | 19200           | Sharp -> PC | 8.36      | 1112             |
| 62015_2 | 20299           | 9600            | PC -> Sharp | 68.67     | 296              |
| 62015_2 | 20299           | 9600            | Sharp -> PC | 31.11     | 652              |
| 62015_2 | 20299           | 19200           | PC -> Sharp | 67.50     | 301              |
| 62015_2 | 20299           | 19200           | Sharp -> PC | 18.41     | 1103             |

---

## 11. Sharp internal communication parameters

- **SIO timer master**: **0BFD31h and 0BFD32h**
  - Time n on error timer * 0.5s. However, 0FFFFh is unlimited. Default value = 0FFFFh (unlimited)
- **SIO baud rate**: **0BFD33h**
  - Specify baud rate, length and parity. Default value = 3Ch / 60d / 00111100b
  - Bits 6, 5, 4: baud rate -> 000 = None, 001 = 300, 010 = 600, 011 = 1200, 100 = 2400, 101 = 4800, 110 = 9600, 111 = 19200
  - Bits 3, 2: parity -> 00 = Even parity, 01 = Odd parity, 10 = Non parity, 11 = Non parity
  - Bit 1: length -> 0 = 8 bits, 1 = 7 bits
  - Bit 0: stop bit -> 0 = 1 bit, 1 = 2 bits
- **SIO setup**: **0BFD34h**
  - Specify shift in/out, X on/off. Specify transfer of transmission code at open/close. Default value = 21h / 33d
  - Bit 6 = 0: 1 byte data stored in SIO open send data is not transmitted at open state
  - Bit 6 = 1: transferred SIO open send data = 0BFD61h
  - Bit 4 = 0: 1 byte data stored in SIO close send data is not transmitted at close state
  - Bit 4 = 1: transferred SIO close send data = 0BFD62h
  - Bit 2 = 0: without X on/off designation at receiving
  - Bit 2 = 1: with designation
  - Bit 1 = 0: without X on/off designation at sending
  - Bit 1 = 1: with designation
  - Bit 0 = 0: without shift in/out designation
  - Bit 0 = 1: with designation
- **SIO receive port condition**: **0BFD35h**
  - Control of receive port. Default value = 02h
  - Bit 2 CS = 0: don't care
  - Bit 2 CS = 1: take in as receiving data when the CS signal is high and ignore at low
  - Bit 1 CD = 0: don't care
  - Bit 1 CD = 1: take in as receiving data when the CD signal is high and ignore at low
- **SIO receive port control**: **0BFD36h**
  - Control of receive port. Default value = 0DFh
  - Bit 6 ER = 0: when receiving buffer becomes full, ER signal becomes low
  - Bit 6 ER = 1: don't care
  - Bit 5 RR = 0: when receiving buffer becomes full, RR signal becomes low
  - Bit 5 RR = 1: don't care
  - Bit 4 RS = 0: when receiving buffer becomes full, RS signal becomes low
  - Bit 4 RS = 1: don't care
- **SIO send port condition**: **0BFD37h**
  - Control of send port. Default value = 04h
  - Bit 2 CS = 0: don't care
  - Bit 2 CS = 1: transmit when the CS signal is low, wait until it becomes high
  - Bit 1 CD = 0: don't care
  - Bit 1 CD = 1: transmit when the CD signal becomes high. When the CD signal is low, wait until it becomes high
- **SIO send port control**: **0BFD38h**
  - Control of send port. Default value = 050h
  - Bit 6 ER = 0: don't care
  - Bit 6 ER = 1: ER signal becomes high before transfer of transmission data block and becomes low after transfer
  - Bit 5 RR = 0: don't care
  - Bit 5 RR = 1: RR signal becomes high before transfer of transmission data block and becomes low after transfer
  - Bit 4 RS = 0: don't care
  - Bit 4 RS = 1: RS signal becomes high before transfer of transmission data block and becomes low after transfer
- **SIO send delay**: **0BFD39h**
  - <00-0FFh> * 2 ms wait time is specified before or after transmission data block at transmission. Default value = 01h (2 ms)
- **SIO crlf**: **0BFD3Bh**
  - Specify the delimiter. External code is converted into internal delimiter (0Dh + 0Ah). Default value = 01h
  - Bits 1, 0: 00 = not used, 01 = 0Dh, 10 = 0Ah, 11 = 0Dh + 0Ah
- **SIO eof code**: **0BFD3Ch**
  - To specify the end code. Default value = 1Ah
- **SIO open close wait**: **0BFD40h**
  - Wait n * 0.5 ms immediately after opening or immediately before closing. Default value = 04h (20 ms)
- **SIO open port control**: **0BFD41h**
  - Open of SIO port. Default value = 41h
  - Bit 6 ER = 0: don't care
  - Bit 6 ER = 1: ER signal becomes high at open and low at close
  - Bit 5 RR = 0: don't care
  - Bit 5 RR = 1: RR signal becomes high at open and low at close
  - Bit 4 RS = 0: don't care
  - Bit 4 RS = 1: RS signal becomes high at open and low at close
- **SIO send n byte wait**: **0BFD60h**
  - Specify insertion time of <00-0FFh> * 2 ms wait between send data 1 byte at sending. Default value = 00h (no wait)
- **SIO open send data**: **0BFD61h**
  - Default value = 11h
  - When SIO setup bit 6 is 1, SIO open send data is transferred by 1 byte at open
- **SIO close send data**: **0BFD62h**
  - Default value = 13h
  - When SIO setup bit 4 is 1, SIO close send data is transferred by 1 byte at close

**Transmission of 1 byte:**

In case Xon-Xoff is specified, if the Xoff code is being received, the transmitting side
keeps waiting until the Xon code is received and the line is released.

And if the signal to be monitored (the port specified with `SIO send port condition`) is not
set ON (high level), it keeps waiting.

When the above conditions are satisfied and the CPU is ready and empty, 1 byte of data is output.

---

## 12. License

This project is released under the **MIT** license — see the [LICENSE](LICENSE) file.

The Sharp manuals in the `Documentation/` folder are **not** covered by this license:
they are copyrighted documents, kept locally and excluded from the repository.

