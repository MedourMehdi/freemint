# acia.S - IKBD/MIDI ACIA Interrupt Handler

## Overview

This file implements the IKBD (Intelligent Keyboard) and MIDI ACIA (Asynchronous Communications Interface Adapter) interrupt service routine for FreeMiNT. It replaces the original TOS handler on systems with TOS < 2.0 to provide additional vectors for AKP (Advanced Keyboard Protocol) routines.

## Purpose

The implementation is specifically designed to work around keyboard handling issues on FireTOS and CT60 TOS 2 systems, where the ROM ACIA handler directly installs the ikbdsys handler without calling the undocumented kbdvec vector handler responsible for keyboard scancode processing.

## Key Components

### Global Symbols and External References

```assembly
.globl  SYM(syskey),SYM(keyrec)
.globl  SYM(old_acia),SYM(new_acia)
.extern SYM(has_kbdvec)
.extern SYM(newkeys)
```

### Vector Offsets

The code defines offsets for various interrupt vectors relative to the syskey structure:

- `KBDVEC` (-4): Keyboard vector
- `MIDIVEC` (0): MIDI vector  
- `VKBDERR` (4): Keyboard error vector
- `VMIDERR` (8): MIDI error vector
- `STATVEC` (12): Status vector
- `MOUSEVEC` (16): Mouse vector
- `CLOCKVEC` (20): Clock vector
- `JOYVEC` (24): Joystick vector
- `VMIDISYS` (28): MIDI system vector
- `VIKBDSYS` (32): IKBD system vector
- `IKBDSTATE` (36): IKBD state byte
- `IKBDTODO` (37): IKBD todo counter

## Main Functions

### new_acia()

The main ACIA interrupt handler that:

1. **Preserves registers**: Saves d0-d3/a0-a3 registers
2. **Calls system vectors**: Invokes both MIDI and IKBD system handlers
3. **Polls for completion**: Continues processing while bit 4 of 0xfa01 is clear
4. **Clears interrupt**: Resets bit 6 of 0xfa11 to acknowledge interrupt
5. **Returns from interrupt**: Uses `rte` instruction

```assembly
SYM(new_acia):
    movem.l d0-d3/a0-a3,-(sp)
again:
    move.l  SYM(syskey),a3
    move.l  VMIDISYS(a3),a0
    jsr     (a0)
    move.l  VIKBDSYS(a3),a0
    jsr     (a0)
    btst    #04,0xfa01.w
    beq.s   again
    movem.l (sp)+,d0-d3/a0-a3
    bclr    #6,0xfa11.w
    rte
```

### ikbdsys_handler()

Handles IKBD system interrupts with sophisticated packet processing:

#### Interrupt Detection
- Reads ACIA control register (0xfc00)
- Checks for interrupt request (bit 7)
- Handles receiver full (bit 0) and overrun (bit 5) conditions

#### Packet Classification
Uses lookup tables to classify incoming data:

**Type Table**: `[1,2,3,3,3,3,4,5,6,7]`
**Todo Table**: `[7,5,2,2,2,2,6,2,1,1]`

#### Data Processing Logic

1. **Single-byte packets** (0x00-0xf5): Passed to keyboard vector or newkeys
2. **Multi-byte packets** (0xf6-0xff): Processed using state machine
3. **Mouse packets** (0xf8-0xfb): Stored in mouse_2_buf
4. **Joystick packets** (0xfd-0xff): Stored in joy_buf

#### Multi-byte Packet Handling

The handler maintains state through:
- `IKBDSTATE`: Current packet type
- `IKBDTODO`: Remaining bytes to collect

Packets are assembled in dedicated buffers and dispatched to appropriate vectors when complete.

## Data Structures

### Buffer Layout

```assembly
stat_buf:       ds.b 7 + 1      ; Status buffer
mouse_1_buf:    ds.b 5 + 1      ; Mouse buffer 1
mouse_2_buf:    ds.b 3 + 1      ; Mouse buffer 2  
clock_buf:      ds.b 6          ; Clock buffer
joy_buf:        ds.b 2          ; Joystick buffer
dump_buf:       ds.b 8          ; Universal buffer
```

### Buffer Descriptor Table

```assembly
buffers:
    dc.l stat_buf,       stat_buf + 7, STATVEC
    dc.l mouse_1_buf, mouse_1_buf + 5, MOUSEVEC
    dc.l mouse_2_buf, mouse_2_buf + 3, MOUSEVEC
    dc.l clock_buf,     clock_buf + 6, CLOCKVEC
    dc.l joy_buf,         joy_buf + 2, JOYVEC
```

Each entry contains:
- Buffer start address
- Buffer end address  
- Vector offset for completion callback

## Architecture Support

### Coldfire Compatibility

The code includes conditional compilation for Coldfire processors:

```assembly
#ifdef __mcoldfire__
    lea     -32(sp),sp
    movem.l d0-d3/a0-a3,(sp)
#else
    movem.l d0-d3/a0-a3,-(sp)
#endif
```

### M68000 Timing Workaround

Includes NOP instruction for M68000 timing requirements:

```assembly
#ifdef M68000
    nop     // see intr.S and syscall.S
#endif
```

## Error Handling

- **Receiver overrun**: Calls keyboard error vector (VKBDERR)
- **Invalid states**: Graceful degradation with return to idle state
- **Buffer overflow**: Protected by buffer size checking

## Performance Considerations

- **Minimal register usage**: Only saves necessary registers
- **Efficient packet classification**: Uses lookup tables instead of conditional logic
- **Optimized loops**: Uses dbra instruction for counted loops
- **Stack management**: Careful stack pointer manipulation for Coldfire compatibility

## Integration Points

- **TOS compatibility**: Maintains same calling conventions as ROM handlers
- **Vector chaining**: Preserves original ACIA vector in old_acia
- **Keyboard processing**: Integrates with FreeMiNT's newkeys routine
- **Device drivers**: Provides vectors for mouse, joystick, and clock handlers

## Limitations

- **MIDI processing**: Relies on ROM MIDI handler due to complexity
- **Architecture specific**: Only enabled on FireTOS and CT60 TOS 2
- **Non-reentrant**: Requires careful interrupt level management
- **Buffer sizes**: Fixed buffer sizes may limit packet handling

## Usage Context

This handler is conditionally compiled and installed only when:
1. Running on FireTOS or CT60 TOS 2
2. AKP keyboard support is enabled
3. Original ROM handler is insufficient

The implementation bridges the gap between modern FreeMiNT requirements and legacy TOS keyboard handling limitations.