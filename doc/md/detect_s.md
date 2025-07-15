# FreeMiNT Hardware Detection Module Documentation

## Overview

The FreeMiNT hardware detection module (`detect.S` and `detect.h`) provides low-level assembly routines for detecting various hardware components on Atari ST/TT/Falcon computers and compatible systems. This module is essential for the FreeMiNT operating system to properly configure itself based on the available hardware.

## File Information

- **Primary File**: `detect.S` (Assembly source)
- **Header File**: `detect.h` (C interface definitions)
- **Author**: Jörg Westheide <joerg_westheide@su.maus.de>
- **Started**: 1999-06-11
- **License**: FreeMiNT project
- **Compilation Requirements**: Must be compiled with `-m68030` flag

## Architecture Support

The module supports multiple Motorola 68000 family processors:
- 68000, 68010, 68020, 68030, 68040, 68060
- Apollo 68080 (detected but reported as 68040)
- ColdFire processors (limited support)

## API Reference

### Memory Test Functions

#### `test_byte_rd(long addr)`
**Purpose**: Safely test if a byte can be read from a memory address without causing a system crash.

**Parameters**:
- `addr`: Memory address to test (32-bit)

**Returns**:
- `0`: Memory access failed (bus error occurred)
- `1`: Memory access successful

**Usage**: Used internally by detection routines to probe hardware registers safely.

#### `test_word_rd(long addr)`
**Purpose**: Safely test if a word (16-bit) can be read from a memory address.

**Parameters**:
- `addr`: Memory address to test (must be word-aligned)

**Returns**:
- `0`: Memory access failed
- `1`: Memory access successful

#### `test_long_rd(long addr)`
**Purpose**: Safely test if a long word (32-bit) can be read from a memory address.

**Parameters**:
- `addr`: Memory address to test (must be long-aligned)

**Returns**:
- `0`: Memory access failed
- `1`: Memory access successful

### Hardware Detection Functions

#### `detect_hardware(void)`
**Purpose**: Detects peripheral hardware components and returns a bitmask.

**Returns**: 32-bit bitmask where each bit represents a detected component:
- Bit 0: ST-ESCC (Harun Scheutzow's serial extension)
- Bits 1-31: Reserved (currently unused)

**Detection Logic**:
1. Temporarily installs bus error handler
2. Disables interrupts
3. Attempts to access ST-ESCC registers
4. Tests MFP compatibility
5. Updates global register addresses if ST-ESCC is found

**Global Variables Updated**:
- `ControlRegA`, `DataRegA`: Channel A register addresses
- `ControlRegB`, `DataRegB`: Channel B register addresses

#### `detect_cpu(void)`
**Purpose**: Identifies the CPU model installed in the system.

**Returns**: CPU identification code:
- `0x00` (0): 68000
- `0x0A` (10): 68010
- `0x14` (20): 68020
- `0x1E` (30): 68030
- `0x28` (40): 68040
- `0x3C` (60): 68060

**Detection Algorithm**:
1. **68000 Detection**: Attempts `move %ccr,d1` (illegal on 68000)
2. **68010+ Detection**: Tests for CACR register availability
3. **68020+ Detection**: Uses cache control register features
4. **68030 Detection**: Tests data cache availability and TC register
5. **68040+ Detection**: Uses CINVA instruction and PCR register
6. **68060 Detection**: Tests for 68060-specific instructions
7. **Apollo 68080**: Special detection for this enhanced processor

**Special Cases**:
- Apollo 68080 is detected but reported as 68040 for compatibility
- ColdFire processors return 60 when `coldfire_68k_emulation = true`

#### `detect_fpu(void)`
**Purpose**: Detects hardware floating-point unit (FPU) presence and type.

**Returns**: FPU cookie value:
- `0x00000000`: No FPU present
- `0x00040000`: 68881 FPU
- `0x00060000`: 68882 FPU
- `0x00080000`: 68040 internal FPU
- `0x00100000`: 68060 internal FPU

**Detection Method**:
1. Installs temporary Line-F exception handler
2. Executes `FNOP` instruction
3. If no exception: FPU is present in coprocessor mode
4. Uses CPU type and FPU state frame analysis for specific identification
5. Special handling for 68060 PCR register and broken LC/EC060 variants

**Important Notes**:
- Only detects hardware FPU, ignores software emulation
- Requires CPU detection to be performed first
- Handles 68LC/EC060 "broken" variants correctly

#### `detect_sfp(void)`
**Purpose**: Detects software FPU emulation (SFP-004 or compatible).

**Returns**: Compatible FPU cookie values:
- `0x00000000`: No software FPU
- `0x00040000`: 68881 emulation
- `0x00060000`: 68882 emulation

**Detection Method**:
1. Installs temporary bus error handler
2. Reads SFP-004 save register at `0xFA44`
3. Analyzes format word to determine emulation type
4. Waits for coprocessor to complete processing if busy

#### `detect_pmmu(void)`
**Purpose**: Detects Memory Management Unit (PMMU) presence.

**Returns**:
- `0`: No PMMU present
- `1`: PMMU present

**Detection Logic**:
- **68000-68020**: Always returns 0 (no PMMU)
- **68030**: Always returns 1 (PMMU always present on Atari systems)
- **68040/68060**: Checks TC register 'E' bit

## Global Variables

### ST-ESCC Register Addresses
These variables are updated by `detect_hardware()` when ST-ESCC is detected:

```c
extern void *ControlRegA;  // Default: 0xFFFF8C81, ST-ESCC: 0xFFFFFA35
extern void *DataRegA;     // Default: 0xFFFF8C83, ST-ESCC: 0xFFFFFA37
extern void *ControlRegB;  // Default: 0xFFFF8C85, ST-ESCC: 0xFFFFFA31
extern void *DataRegB;     // Default: 0xFFFF8C87, ST-ESCC: 0xFFFFFA33
```

### CPU Information
```c
extern long mcpu;          // Set by detect_cpu(), used by other functions
extern short is_apollo_68080; // Set to 1 if Apollo 68080 detected
```

## Implementation Details

### Bus Error Handling
All detection functions use sophisticated bus error handling:
- Temporarily replaces bus error vector at `0x08`
- Preserves original vector and stack state
- Ensures safe recovery from hardware probes
- Flushes write pipelines on 68040+ processors

### Interrupt Management
Critical sections disable interrupts using:
- `ori.w #0x0700,sr` (68000-68030)
- `ori.l #0x0700,d2; move.w d2,sr` (ColdFire)

### ColdFire Compatibility
The code includes conditional compilation for ColdFire processors:
- Uses different instruction sequences where necessary
- Handles register restrictions
- Provides limited functionality compared to 68K versions

## Usage Examples

### Basic Hardware Detection
```c
#include "detect.h"

void system_init(void) {
    long cpu_type = detect_cpu();
    long fpu_type = detect_fpu();
    long hardware_flags = detect_hardware();
    short pmmu_present = detect_pmmu();
    
    printf("CPU: ");
    switch(cpu_type) {
        case 0:  printf("68000\n"); break;
        case 10: printf("68010\n"); break;
        case 20: printf("68020\n"); break;
        case 30: printf("68030\n"); break;
        case 40: printf("68040\n"); break;
        case 60: printf("68060\n"); break;
    }
    
    if (fpu_type) {
        printf("FPU: Type 0x%08lx\n", fpu_type);
    }
    
    if (hardware_flags & 1) {
        printf("ST-ESCC detected\n");
    }
    
    if (pmmu_present) {
        printf("PMMU present\n");
    }
}
```

### Safe Memory Testing
```c
// Test if custom hardware is present
if (test_long_rd(0xF00000)) {
    printf("Custom hardware at 0xF00000\n");
    // Safe to access the hardware
}
```

## Technical Notes

### Memory Addresses
- **MFP Registers**: `0xFFFFFA01 - 0xFFFFFA3F`
- **ST-ESCC Registers**: `0xFFFFFA31, 0xFFFFFA33, 0xFFFFFA35, 0xFFFFFA37`
- **SFP-004 Register**: `0xFA44`
- **Exception Vectors**: `0x08` (Bus Error), `0x10` (Illegal Instruction), `0x2C` (Line-F)

### Compiler Requirements
- Must be compiled with `-m68030` flag
- Requires FreeMiNT build environment
- Assembly syntax compatible with GNU assembler

### Limitations
- ST-ESCC detection requires ST-compatible MFP
- FPU detection may not work with all software emulators
- PMMU detection on 68040/60 uses simplified method
- ColdFire support is limited and experimental

## Error Handling

All functions are designed to be safe and non-destructive:
- Bus errors are caught and handled gracefully
- Original system state is always restored
- No permanent changes to system configuration
- Interrupt state is preserved

## Performance Considerations

- Detection functions are typically called once during system initialization
- Memory test functions use bus error handling (relatively slow)
- CPU detection involves multiple privilege instruction attempts
- FPU detection requires FPU state manipulation

## Maintenance Notes

- Code must remain compatible with all supported 68K variants
- Bus error handling is critical for system stability
- Exception vector management requires careful attention
- ColdFire compatibility may need updates for newer variants

## References

- Motorola 68000 Family Programmer's Reference Manual
- Atari ST/TT/Falcon Technical Documentation
- FreeMiNT Operating System Documentation
- Apollo 68080 Technical Specifications