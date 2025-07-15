# scsidrv_emu.S - SCSI Driver Emulation Interface

## Overview

This file implements the assembly-level interface for SCSI driver emulation in FreeMiNT. It provides a standardized way to access SCSI functionality through the FreeMiNT system call interface, emulating the behavior of native SCSI drivers.

## Author & History

- **Author**: Frank Naumann <fnaumann@freemint.de>
- **Started**: 2000-03-31
- **License**: GNU General Public License version 2+

## Purpose

The implementation serves as a bridge between applications expecting standard SCSI driver interfaces and FreeMiNT's internal SCSI emulation system. It translates SCSI driver function calls into FreeMiNT system calls.

## Architecture

### Function Mapping

Each SCSI driver function is mapped to a specific emulation call number:

| Function | Call Number | Purpose |
|----------|-------------|---------|
| `emu_scsidrv_In` | 1 | SCSI input operation |
| `emu_scsidrv_Out` | 2 | SCSI output operation |
| `emu_scsidrv_InquireSCSI` | 3 | Query SCSI subsystem |
| `emu_scsidrv_InquireBUS` | 4 | Query SCSI bus information |
| `emu_scsidrv_CheckDev` | 5 | Check device availability |
| `emu_scsidrv_RescanBus` | 6 | Rescan SCSI bus |
| `emu_scsidrv_Open` | 7 | Open SCSI device |
| `emu_scsidrv_Close` | 8 | Close SCSI device |
| `emu_scsidrv_Error` | 9 | Handle SCSI errors |
| `emu_scsidrv_Install` | 10 | Install target handler |
| `emu_scsidrv_Deinstall` | 11 | Remove target handler |
| `emu_scsidrv_GetCmd` | 12 | Get SCSI command |
| `emu_scsidrv_SendData` | 13 | Send data to target |
| `emu_scsidrv_GetData` | 14 | Receive data from target |
| `emu_scsidrv_SendStatus` | 15 | Send status response |
| `emu_scsidrv_SendMsg` | 16 | Send SCSI message |
| `emu_scsidrv_GetMsg` | 17 | Receive SCSI message |

## Implementation Details

### Common Function Template

All functions follow the same basic template:

```assembly
SYM(emu_scsidrv_FunctionName):
    moveq   #CALL_NUMBER,d0
    bra.s   _do_trap
```

This design provides:
- **Minimal overhead**: Single instruction to set call number
- **Unified handling**: All functions use the same trap mechanism
- **Easy maintenance**: Adding new functions requires minimal code

### The _do_trap Handler

The central trap handler manages the system call interface:

```assembly
_do_trap:
    move.l  sp,a0               ; Save current stack pointer
    move.l  a2,-(sp)            ; Preserve a2 (PureC convention)
    lea     -12(sp),sp          ; Ensure consistent stack space
    move.l  16(a0),-(sp)        ; Push argument 4
    move.l  12(a0),-(sp)        ; Push argument 3
    move.l  8(a0),-(sp)         ; Push argument 2
    move.l  4(a0),-(sp)         ; Push argument 1
    move.w  d0,-(sp)            ; Push emulation call number
    move.w  #0x0002,-(sp)       ; SCSIDRV EMU identifier
    move.w  #0x015f,-(sp)       ; System EMU trap number
    trap    #1                  ; Execute GEMDOS trap
    lea     34(sp),sp           ; Restore stack (34 bytes total)
    move.l  (sp)+,a2            ; Restore a2
    rts
```

#### Stack Management

The handler carefully manages the stack to:

1. **Preserve caller state**: Saves a2 register per PureC convention
2. **Align arguments**: Ensures proper argument alignment for system call
3. **Pass parameters**: Transfers up to 4 function arguments
4. **Add metadata**: Includes emulation type and call number
5. **Restore state**: Cleans up stack and restores registers

#### System Call Structure

The trap call pushes the following stack frame:

```
Stack (top to bottom):
- Trap number (0x015f)
- Emulation type (0x0002 = SCSIDRV)
- Function number (1-17)
- Argument 1
- Argument 2
- Argument 3
- Argument 4
- 12 bytes padding
- Saved a2 register
```

## Function Specifications

### Data Transfer Functions

#### emu_scsidrv_In / emu_scsidrv_Out
- **Purpose**: Handle SCSI data input/output operations
- **Parameters**: SCSICMD structure pointer
- **Return**: Status code
- **Usage**: Primary data transfer interface

### Inquiry Functions

#### emu_scsidrv_InquireSCSI
- **Purpose**: Query SCSI subsystem capabilities
- **Parameters**: Query type, BUSINFO structure pointer
- **Return**: Information about SCSI subsystem

#### emu_scsidrv_InquireBUS
- **Purpose**: Query specific SCSI bus information
- **Parameters**: Query type, bus number, DEVINFO structure pointer
- **Return**: Bus-specific information

### Device Management

#### emu_scsidrv_CheckDev
- **Purpose**: Verify device availability and capabilities
- **Parameters**: Bus number, SCSI ID, name buffer, features pointer
- **Return**: Device status and feature information

#### emu_scsidrv_RescanBus
- **Purpose**: Rescan SCSI bus for new devices
- **Parameters**: Bus number
- **Return**: Scan results

### Connection Management

#### emu_scsidrv_Open / emu_scsidrv_Close
- **Purpose**: Establish/terminate SCSI device connections
- **Parameters**: Bus number, SCSI ID, maximum transfer length
- **Return**: Handle or status code

#### emu_scsidrv_Error
- **Purpose**: Handle SCSI error conditions
- **Parameters**: Handle, read/write flag, error number
- **Return**: Error handling result

### Target Mode Functions

#### emu_scsidrv_Install / emu_scsidrv_Deinstall
- **Purpose**: Install/remove SCSI target handlers
- **Parameters**: Bus number, TARGET handler structure
- **Return**: Installation status

#### emu_scsidrv_GetCmd
- **Purpose**: Receive SCSI commands in target mode
- **Parameters**: Bus number, command buffer
- **Return**: Command data

#### emu_scsidrv_SendData / emu_scsidrv_GetData
- **Purpose**: Transfer data in target mode
- **Parameters**: Bus number, data buffer, length
- **Return**: Transfer status

#### emu_scsidrv_SendStatus / emu_scsidrv_SendMsg / emu_scsidrv_GetMsg
- **Purpose**: Handle SCSI status and message phases
- **Parameters**: Bus number, status/message data
- **Return**: Phase completion status

## Calling Convention

### Register Usage

- **d0**: Used internally for function number
- **a0**: Temporary stack pointer storage
- **a2**: Preserved across calls (PureC convention)
- **Stack**: Used for parameter passing

### Parameter Passing

- **Arguments**: Passed on stack in standard order
- **Return values**: Returned in d0 register
- **Stack cleanup**: Handled by callee

### Compiler Compatibility

The implementation specifically mentions PureC convention:
- **Register a2**: Must be preserved across function calls
- **Stack alignment**: Maintains proper alignment for various compilers
- **Calling convention**: Compatible with standard C calling conventions

## Error Handling

### System Call Errors

The implementation relies on the underlying system call mechanism for error handling:
- **Invalid parameters**: Handled by kernel validation
- **Device errors**: Propagated through return values
- **System errors**: Mapped to appropriate SCSI error codes

### Stack Overflow Protection

The consistent stack space allocation (12 bytes + parameters) prevents:
- **Stack corruption**: Fixed allocation prevents overruns
- **Alignment issues**: Proper alignment maintained
- **Register corruption**: Careful register preservation

## Performance Considerations

### Minimal Overhead

The design minimizes performance impact through:
- **Single branch**: All functions use same trap handler
- **Efficient parameter passing**: Direct stack manipulation
- **Minimal register usage**: Only necessary registers touched

### Memory Efficiency

- **Small code size**: Shared trap handler reduces memory footprint
- **No dynamic allocation**: Stack-based parameter passing
- **Efficient argument copying**: Direct memory operations

## Integration Points

### FreeMiNT System Calls

The interface integrates with FreeMiNT's system call mechanism:
- **Trap #1**: Standard GEMDOS trap interface
- **Emulation framework**: Uses sys_emu infrastructure
- **Parameter validation**: Leverages kernel validation

### SCSI Subsystem

Connects to FreeMiNT's SCSI emulation:
- **Device drivers**: Interfaces with virtual SCSI devices
- **Protocol handling**: Manages SCSI protocol details
- **Error mapping**: Translates between error domains

## Usage Example

```c
// Application code
SCSICMD cmd;
long result;

// Initialize SCSI command structure
cmd.handle = device_handle;
cmd.cmd = command_buffer;
cmd.cmdlen = command_length;
cmd.buffer = data_buffer;
cmd.buflen = data_length;

// Execute SCSI command
result = emu_scsidrv_Out(&cmd);
```

The assembly interface seamlessly handles the transition from C function call to FreeMiNT system call, providing transparent SCSI driver emulation functionality.