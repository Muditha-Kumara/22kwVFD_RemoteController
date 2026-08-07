# SF100 (NEW_SF100) — Modbus RTU Reference (distilled from manual)

Used for: **travel (traversing) motor** (small VFD).
Manual default protocol is ASCII — MUST set RTU mode via P3.09.

## Bus / comm parameters (group P3)
| Param | Meaning | Values |
|---|---|---|
| P3.09 | Comm config (3-digit LED) | Ones=baud: 0:1200 1:2400 2:4800 **3:9600** 4:19200 5:38400; Tens=format: 0:7N2 1:7O1 2:7E1 3:8N2 4:8O1 5:8E1 **6:8N1**; Hundreds=mode: 0:ASCII **1:RTU** → use **163** (9600,8N1,RTU) |
| P3.10 | Station address | 0..248 (0=broadcast, 248=master mode) |
| P3.11 | Comm timeout detect | 0.0..1000.0 s (0=disabled) |

## Mode / source parameters
| Param | Meaning | Setting for this project |
|---|---|---|
| P0.00 | Control mode | 0:V/F, 1:sensorless vector, **2:direct torque control** |
| P0.01 | Frequency source | **4 = digital 3, serial port** |
| P0.03 | Run command channel | **2 = serial port** |
| Pb.05 | Torque setting source | **4 = RS485 comm** |
| Pb.06 | Keypad torque setting | -200.0%..200.0% (fallback) |
| P0.19 | Upper freq limit | safety cap |
| P0.20 | Lower freq limit | |

## Modbus framing
- RTU: START/END gap ≥ 10 ms. CRC16 Modbus, **low byte first**.
- Function codes: **03H** read 1 reg, **06H** write 1 reg, **08H** loopback test, **10H** read multiple regs (≤8 bytes).
- Error codes: 01H bad function, 02H bad address, 03H bad data.

## Fixed control/status registers
| Addr | Dir | Meaning |
|---|---|---|
| **2000H** | W | Command word: bit0-1: 01=stop, 10=run, 11=jog; bit4-5: 01=FWD, 10=REV, 11=toggle. (FWD run = 0x12, REV run = 0x22, stop = 0x01) |
| **2001H** | W | Serial frequency command (0.01 Hz units; in torque mode with Pb.05=4 this carries torque reference) |
| **2002H** | W | Fault reset: 01=EF., 02=fault reset |
| **2100H** | R | Fault code (E-01..E-18) |
| **2101H** | R | Status bits: BIT0 running (1=run), BIT1 undervoltage, BIT2 direction (1=rev), BIT3 jog, BIT9 main freq from comm, BIT11 run cmd from comm |
| **2102H** | R | Set frequency |
| **2103H** | R | Output frequency |
| **2104H** | R | Output current (0.1 A) |
| **2105H** | R | Bus voltage |
| **2106H** | R | Output voltage |
| **2107H** | R | Motor speed (rpm) |
| **2108H** | R | Module temperature |

## Parameter read/write
- Function code address = GGnnH: GG=group letter (P0..Pd), nn=number.
  Example: P0.02 = 0x0002, P3.09 = 0x0309, Pb.05 = 0x0B05.
- Read param: 03H at GGnnH. Write param: 06H at GGnnH.

## Fault codes (2100H)
E-01 acc overcurrent, E-02 dec overcurrent, E-03 const overcurrent,
E-04/05/06 overvoltage, E-07 control power overvolt, E-08 overheat,
E-09 inverter overload, E-10 motor overload, E-11 undervoltage,
E-12 module protection, E-13 external fault, E-14 current detect fault,
E-15 comm fault, E-16 interference, E-17 EEPROM error, E-18 usage time expired.

## Key facts for driver
- Run: write 2000H = 0x12 (FWD run). Stop: 2000H = 0x01.
- Fault reset: write 2002H = 0x02.
- Freq/torque ref: 2001H. **VERIFY on hardware**: torque reference scale via
  comm when Pb.05=4 (panel Pb.06 scale is -200.0%..200.0%, 0.1% units →
  assume signed 16-bit with 1000 = 100.0% until verified).
- Factory default is ASCII 9600 7N2 — first commissioning step is setting P3.09=163.
