# ZUKED D720 — Modbus RTU Reference (distilled from manual)

Used for: **main 22 kW cutting motor** (high-performance current vector VFD).

## Bus / comm parameters (group Pd)
| Param | Meaning | Values |
|---|---|---|
| Pd-00 | Baud rate | 0:300 1:600 2:1200 3:2400 4:4800 **5:9600** 6:19200 7:38400 8:57600 9:115200 |
| Pd-01 | Data format | 0: 8N2, 1: 8E1, 2: 8O1, **3: 8N1** |
| Pd-02 | Local address | 1..247 (0 = broadcast) |
| Pd-03 | Response delay | 0..20 ms |
| Pd-04 | Comm timeout | 0.0 = disabled, 0.1..60.0 s |
| Pd-05 | Protocol | 0: non-standard, **1: standard Modbus** |

## Mode / source parameters
| Param | Meaning | Setting for this project |
|---|---|---|
| P0-02 | Run command channel | **2 = serial communication** |
| P0-03 | Main frequency source | 5 = communication (speed mode) |
| d0-00 | Speed/Torque control select | **1 = torque control** |
| d0-01 | Torque reference source | **5 = communication reference** |
| d0-03 | Torque digital setting | -200.0% .. 200.0% (fallback if d0-01=0) |
| d0-05 | Fwd max freq in torque mode | 0..max Hz (SAFETY speed limit) |
| d0-06 | Rev max freq in torque mode | 0..max Hz (SAFETY speed limit) |
| d0-07 / d0-08 | Torque accel/decel time | 0..650 s (0 = fast response) |
| P0-10 | Max frequency | scale base for % references |

Note: in torque mode, when load torque < motor torque the motor accelerates —
d0-05/d0-06 MUST be set to safe mechanical limits.

## Modbus framing
- RTU, word-only. Read = **0x03**, write = **0x06** (no 0x10, no bit access).
- Multi-read up to 12 words, must not cross parameter groups.
- Error response: function | 0x80, error code: 01 cmd, 02 addr, 03 data, 04 busy.
- CRC16 (Modbus poly 0xA001), low byte first.

## Parameter address mapping
| Group | Comm address | RAM-only write address |
|---|---|---|
| P0..PE | 0xF000 + grp*0x100 + nn | 0x0000 + grp*0x100 + nn (high byte F→0) |
| d0..dC | 0xA000 + grp*0x100 + nn | 0x4000 + ... (high byte A→4) |
| U0 (read-only) | 0x7000 + nn | — |

Example: P3-12 = 0xF30C. RAM-only writes do NOT wear EEPROM.
Group U read-only. Group PF factory (no access).

## Fixed control/status registers
| Addr | Dir | Meaning |
|---|---|---|
| **2000H** | W | Control word: 1=FWD run, 2=REV run, 3=FWD jog, 4=REV jog, 5=coast stop, **6=decel stop**, 7=fault reset |
| **3000H** | R | Run status: 1=FWD running, 2=REV running, 3=stopped |
| **8000H** | R | Fault code (see P9-14 list; 0 = none) |
| **1000H** | W | Communication set value: -10000..10000 = -100.00%..+100.00%. Frequency: % of P0-10. Torque: % of rated (P2-10 motor1 / d2-48 motor2) |

## Monitoring registers (read 0x03)
| Addr | Meaning |
|---|---|
| 1001H | Running frequency |
| 1002H | Bus voltage |
| 1003H | Output voltage |
| 1004H | Output current |
| 1005H | Output power |
| **1006H** | **Output torque** |
| 1007H | Running speed |
| 1014H | Feedback speed (0.1 Hz) |
| 101DH | Communication reference |

## Fault codes (8000H)
0000 none, 0002/0003/0004 overcurrent (acc/dec/const), 0005/0006/0007 overvoltage,
0008 buffer resistor overload, 0009 undervoltage, 000A inverter overload,
000B motor overload, 000C/000D phase loss in/out, 000E IGBT overheat,
000F external fault, 0010 communication fault, 001E load lost, 005E speed fb error.

## Key facts for driver
- Run requires P0-02=2. Torque mode requires d0-00=1 + d0-01=5, then write 1000H.
- Stop: write 2000H=6 (decel stop) — preferred safe stop.
- Fault reset: write 2000H=7.
- Values big-endian (H---L then L---H in frame).
