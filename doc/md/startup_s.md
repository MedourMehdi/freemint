# FreeMiNT Startup Module Documentation

## Overview

This documentation covers the FreeMiNT startup module, consisting of two files that handle the initial bootstrap process for the FreeMiNT operating system kernel. FreeMiNT is a free Unix-like operating system for Atari computers, evolved from the original MiNT (MiNT is Not TOS) system.

## Files Overview

### startup.h
- **Purpose**: Header file for startup module definitions
- **Author**: Frank Naumann <fnaumann@freemint.de>
- **Created**: 2000-11-28
- **Status**: Currently contains only header guards (implementation placeholder)

### startup.S
- **Purpose**: Assembly language startup code for FreeMiNT kernel initialization
- **Author**: Konrad M. Kokoszkiewicz <draco@atari.org>
- **Created**: 2000-02-08
- **Language**: Motorola 68000 Assembly

## Technical Analysis

### Architecture Target
- **Processor**: Motorola 68000 family (m68k)
- **Platform**: Atari computers running TOS/FreeMiNT
- **Binary Format**: Supports both traditional and ELF formats

### startup.S Detailed Analysis

#### Entry Point
```assembly
_main:
```
The `_main` symbol serves as the primary entry point for the FreeMiNT kernel, directly referenced in the build system's Makefile.

#### Memory Layout Calculation
The startup code performs critical memory management tasks:

1. **Basepage Retrieval**:
   ```assembly
   move.l	0x04(sp),a0		// basepage address
   move.l	a0,SYM(_base)
   ```
   - Retrieves the basepage address from the stack
   - Stores it in the global `_base` variable for later use

2. **Memory Size Calculation**:
   ```assembly
   move.l	0x000c(a0),d0		// size of the TEXT segment
   add.l	0x0014(a0),d0		// size of the DATA segment
   add.l	0x001c(a0),d0		// size of the BSS segment
   add.l	#0x00003101,d0		// stack size + basepage size + 1
   bclr	#0,d0			// round
   ```
   
   This calculates the total memory requirement by summing:
   - TEXT segment size (executable code)
   - DATA segment size (initialized data)
   - BSS segment size (uninitialized data)
   - Additional space for stack and basepage (0x3101 bytes)
   - Rounds to even boundary using `bclr #0,d0`

#### Stack Setup
```assembly
lea	(a0,d0.l),sp		// new stack pointer
```
Establishes the new stack pointer at the calculated memory boundary.

#### Memory Management System Call
```assembly
move.l	d0,-(sp)		// new size
move.l	a0,-(sp)		// start address
clr.w	-(sp)
move.w	#0x004a,-(sp)
trap	#1
lea	12(sp),sp
```

This sequence performs a TOS system call:
- **Function**: `Mshrink` (function 0x4a)
- **Purpose**: Shrink/resize the program's memory allocation
- **Parameters**: 
  - New size (d0)
  - Start address (a0)
  - Process ID (0 = current process)
- **Stack cleanup**: `lea 12(sp),sp` removes 12 bytes from stack

#### Kernel Initialization Transfer
```assembly
jmp	SYM(init)
```
Transfers control to the main kernel initialization routine.

## Build System Integration

### Symbol Definitions
- **Global Symbols**:
  - `_main`: Primary entry point
- **External References**:
  - `SYM(init)`: Kernel initialization function
  - `SYM(_base)`: Global basepage storage

### Compiler Compatibility
- **ELF Support**: Conditional compilation for ELF binary format
- **Function Metadata**: Includes type and size information for debugging
- **CFI Directives**: Call Frame Information for stack unwinding

## Memory Layout Understanding

### Atari TOS Basepage Structure
The basepage (referenced at offset 0x04 from stack) contains:
- **0x0C**: TEXT segment size
- **0x14**: DATA segment size  
- **0x1C**: BSS segment size

### Memory Allocation Strategy
The startup code calculates total memory needs and requests appropriate allocation from the underlying TOS system before transferring control to the FreeMiNT kernel proper.

## Development Notes

### Key Constants
- `0x3101`: Combined stack and basepage overhead (12,545 bytes)
- `0x004a`: TOS system call number for `Mshrink`

### Error Handling
The current implementation assumes successful system calls and does not include explicit error handling for the memory allocation request.

### Portability Considerations
- Uses Motorola 68000 assembly syntax
- Relies on TOS system call interface
- Conditional compilation for different binary formats

## Integration with FreeMiNT

This startup module serves as the bridge between:
1. **TOS Boot Process**: Receiving control from the Atari TOS bootloader
2. **FreeMiNT Kernel**: Transferring control to the main kernel initialization

The module ensures proper memory allocation and stack setup before the main kernel begins operation, making it a critical component in the FreeMiNT boot sequence.

## Licensing

Both files are licensed under the GNU General Public License (GPL) version 2 or later, consistent with the FreeMiNT project's open-source nature.

## Historical Context

These files were created in 2000 as part of the FreeMiNT project's evolution from the original MiNT 1.12 distribution, representing the ongoing development of Unix-like capabilities for Atari platforms.