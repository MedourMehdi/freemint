# XHDI Emulation Module Documentation

## Overview

The XHDI emulation module provides a standardized interface for hard disk access in FreeMiNT. XHDI (eXtended Hard Disk Interface) is a standard protocol for accessing storage devices on Atari systems.

## Files

- **xhdi_emu.h**: Header file with function declarations and configuration
- **xhdi_emu.S**: Assembly implementation of XHDI emulation functions

## Architecture

### Header File (xhdi_emu.h)

#### Key Components

```c
long emu_xhdi (ushort opcode, ...);
```

**Purpose**: Main XHDI emulation entry point
- **Parameters**: 
  - `opcode`: XHDI function code (16-bit)
  - `...`: Variable arguments depending on the opcode
- **Returns**: Long integer result code

#### Configuration Options

```c
#if 0 /* set to 1 to enable XHDI_MON */
#define XHDI_MON
#endif
```

**XHDI_MON**: Conditional compilation flag for monitoring functionality
- When enabled, provides `xhdi_mon_dispatcher` function
- Used for debugging and monitoring XHDI calls

### Assembly Implementation (xhdi_emu.S)

#### Magic Number
```assembly
dc.l	0x27011992	// XHDIMAGIC
```
This magic number identifies the XHDI interface and is used by applications to locate the XHDI entry point.

#### Main Function: `emu_xhdi`

**Stack Layout**:
```
Original Stack:
- opcode (16-bit)
- param1 (32-bit)
- param2 (32-bit)
- ...
- param7 (32-bit)
```

**Implementation Details**:
1. Pushes 7 long parameters from positions 30(sp) through stack
2. Pushes the opcode from position 32(sp)
3. Sets up system call parameters:
   - `0x0001`: XHDI EMU identifier
   - `0x015f`: System EMU trap number
4. Executes `trap #1` (GEMDOS system call)
5. Cleans up stack (34 bytes total)
6. Returns result

#### Monitoring System (Optional)

When `XHDI_MON` is defined, additional functionality is available:

**`xhdi_mon_dispatcher`**:
- Provides call monitoring and dispatching
- Uses a function table (`xhdi_mon_dispatcher_table`)
- Supports up to 20 XHDI functions (0-19)
- Implements dynamic stack allocation for parameters

**Stack Frame Management**:
```assembly
link	a6,#-4          // Create stack frame
movem.l	a0-a5/d1-d7,-(sp)  // Save registers
```

**Function Dispatch Logic**:
1. Validates function number (0-19)
2. Calculates table offset (function_num * 8)
3. Retrieves function pointer and parameter size
4. Allocates stack space for parameters
5. Copies parameters to new stack location
6. Calls the target function
7. Restores stack and registers

## Usage

### Basic XHDI Call
```c
// Example: Get XHDI version
long result = emu_xhdi(0x0000);  // XHGetVersion

// Example: Read sectors
long result = emu_xhdi(0x0002,   // XHReadWrite
                       major,     // Major device number
                       minor,     // Minor device number
                       start_sector,
                       count,
                       buffer,
                       rwflag);
```

### Integration with FreeMiNT

The XHDI emulation integrates with FreeMiNT's system call interface:
- Uses trap #1 with function code 0x015f
- Follows GEMDOS calling conventions
- Provides transparent access to underlying storage drivers

## Error Handling

The module relies on the underlying system EMU for error handling:
- Returns standard GEMDOS error codes
- Negative values indicate errors
- Positive values indicate success or data

## Performance Considerations

- Direct assembly implementation for minimal overhead
- Stack-based parameter passing for efficiency
- Optional monitoring can be disabled for production use
- Minimal register usage to preserve system state

## Compatibility

- Designed for m68k architecture
- Compatible with FreeMiNT kernel
- Supports standard XHDI specification
- Integrates with GEMDOS system call interface

## Development Notes

### Debugging
- Enable `XHDI_MON` for call tracing
- Monitor function table must be provided separately
- Stack frame inspection available in monitor mode

### Maintenance
- Magic number must remain constant (0x27011992)
- Stack cleanup must match parameter count
- Register preservation is critical for system stability

## Related Components

- **FreeMiNT kernel**: Core operating system
- **GEMDOS**: System call interface
- **Storage drivers**: Actual hardware access
- **Cookie Jar**: System service location mechanism