# FreeMiNT cpu.S - Developer Documentation

## Overview

This file contains low-level CPU and cache management routines for the FreeMiNT operating system. It provides hardware abstraction for different Motorola 68000 family processors (68020, 68030, 68040, 68060) and ColdFire processors, handling CPU-specific features like superscalar execution, cache management, and memory translation.

## File Information

- **Source**: cpu.S
- **Project**: FreeMiNT (Free MiNT)
- **Target Architecture**: Motorola 68000 family and ColdFire
- **Language**: GNU Assembler (GAS)
- **Conditional Compilation**: Uses `#ifdef` blocks for different CPU targets

## Architecture Support

### Supported Processors
- **68020**: Basic cache support (instruction cache only)
- **68030**: Full cache support (instruction + data cache)
- **68040**: Advanced cache with copyback support
- **68060**: Enhanced cache with superscalar dispatch
- **ColdFire v4e**: Modern RISC-like processor with 68K compatibility

### Conditional Compilation Flags
- `M68000`: Excludes code for 68000 (no cache/MMU)
- `__mcoldfire__`: Enables ColdFire-specific code paths

## Function Reference

### Superscalar Management

#### `get_superscalar()`
**Purpose**: Enables superscalar dispatch on 68060 and newer processors

**Behavior**:
- Checks if CPU is 68060 or newer
- Reads PCR (Processor Control Register) via `movec pcr,d0`
- Sets bit 0 to enable superscalar dispatch
- Writes back to PCR via `movec d0,pcr`

**ColdFire Handling**: 
- Checks `coldfire_68k_emulation` flag
- Uses 32-bit operations (`ori.l`) instead of 16-bit (`ori.w`)

#### `is_superscalar()`
**Purpose**: Checks if superscalar dispatch is currently enabled

**Returns**: 
- `d0 = 1` if superscalar is enabled
- `d0 = 0` if disabled or not supported

**Implementation**: Reads PCR and masks bit 0

### Cache Management System

#### `init_cache()`
**Purpose**: Initializes cache management vectors based on detected CPU type

**Initialization Process**:
1. Detects CPU type from `mcpu` global variable
2. Sets up function pointers in `_cachevec` and `_cacheveci`
3. Configures CCW (Cache Control Word) conversion routines
4. Sets `hascaches` flag if cache is present

**Vector Setup**:
```assembly
_cachevec:    // Main cache flush routine
_cacheveci:   // Cache flush + instruction invalidate
l2cv:         // CCW to CACR conversion
c2lv:         // CACR to CCW conversion
```

#### Cache Control Word (CCW) System

The CCW provides a unified interface for cache control across different processors:

**CCW Bit Layout**:
```
Bit  | Description                           | 020 | 030 | 040 | 060 | CF
-----|---------------------------------------|-----|-----|-----|-----|----
0    | Instruction cache enable              |  x  |  x  |  x  |  x  |  x
1    | Data cache enable                     |  -  |  x  |  x  |  x  |  x
2    | Branch cache enable                   |  -  |  -  |  -  |  x  |  x
3    | Instruction cache freeze              |  x  |  x  |  -  |  -  |  -
4    | Data cache freeze                     |  -  |  x  |  -  |  -  |  -
5    | Instruction burst enable              |  -  |  x  |  -  |  -  |  -
6    | Data burst enable                     |  -  |  x  |  -  |  -  |  -
7    | Data write allocate enable            |  -  |  x  |  -  |  -  |  -
8    | Instruction cache full mode           |  -  |  -  |  -  |  x  |  -
9    | Instruction cache R/W allocate        |  -  |  -  |  -  |  x  |  -
10   | Data cache full mode                  |  -  |  -  |  -  |  x  |  -
11   | Data cache R/W allocate               |  -  |  -  |  -  |  x  |  -
12   | Branch cache invalidate all           |  -  |  -  |  -  |  x  |  x
13   | Branch cache invalidate user          |  -  |  -  |  -  |  x  |  -
14   | CPUSH invalidate enable (data)        |  -  |  -  |  -  |  x  |  x
15   | Store buffer enable                   |  -  |  -  |  -  |  x  |  x
16-24| ColdFire-specific bits                |  -  |  -  |  -  |  -  |  x
```

#### `ccw_set(control, mask)`
**Purpose**: Sets cache control bits according to CCW and mask

**Parameters**:
- `control`: Desired CCW value
- `mask`: Bits to modify (1 = modify, 0 = leave unchanged)

**Algorithm**:
1. Read current CCW state
2. Apply mask to clear bits to be changed
3. OR in new control bits
4. Convert CCW to processor-specific CACR format
5. Handle cache invalidation for state changes
6. Write to CACR register

**Special Cache Handling**:
- **68040/060**: Invalidates caches when enabling (`cinva ic/dc`)
- **68040/060**: Flushes caches when disabling (`cpusha ic/dc`)
- **68020/030**: Sets clear bits when enabling write-through cache

#### `ccw_get()`
**Purpose**: Reads current cache state as CCW

**Returns**: Current CCW value in `d0`

**Implementation**: Reads CACR and converts to CCW format

#### `ccw_getdmask()`
**Purpose**: Returns default CCW mask for current CPU

**Default Masks**:
- 68020: `0x00000009`
- 68030: `0x000000fb`
- 68040: `0x00000003`
- 68060: `0x0000ff07`
- ColdFire: `0x01ffd007`

### Cache Flush Operations

#### `cpush(base, length)`
**Purpose**: Flushes both instruction and data caches

**Parameters**:
- `base`: Starting address
- `length`: Number of bytes (-1 for entire cache)

**Implementation**: Jumps to processor-specific routine via `_cachevec`

#### `cpushi(base, length)`
**Purpose**: Flushes data cache and invalidates instruction cache

**Parameters**: Same as `cpush`

**Use Case**: Modifying executable code (self-modifying code, dynamic loading)

**Implementation**: Jumps to processor-specific routine via `_cacheveci`

### Processor-Specific Cache Routines

#### 68020 Cache (`_cache000`)
**Behavior**: No-op (68020 has no cache or simple cache)

#### 68030 Cache (`_cache030`)
**Strategy**: 
- For full flush: Sets CI (Clear Instruction) and CD (Clear Data) bits
- For selective: Uses CAAR (Cache Address Register) to flush specific lines
- Threshold: 64 longwords (256 bytes)

#### 68040 Cache (`_cache040`)
**Strategy**:
- For full flush: Uses `cpusha dc` (Cache Push All)
- For selective: Uses `cpushl dc,(a0)` per cache line
- **Physical Address Translation**: Converts logical to physical addresses
- Cache line size: 16 bytes
- Threshold: 256 lines (4KB)

#### 68060 Cache (`_cache060`)
**Strategy**: Similar to 68040 but with different thresholds
- Threshold: 512 lines (8KB)
- Uses same physical address translation

#### ColdFire Cache (`_cache_coldfire`)
**Architecture**: 
- 32KB instruction cache + 32KB data cache
- 4-way set associative, 512 sets per way
- 16-byte cache lines

**Flush Algorithm**:
```assembly
// For each way (0-3)
//   For each set (0-511)
//     cpushl %bc,(a0)  // Push both caches
//     increment set
//   increment way
```

### Memory Management

#### `log2phys(address)`
**Purpose**: Converts logical address to physical address

**Parameters**: 
- `a0`: Logical address (input/output)

**Returns**: 
- `a0`: Physical address (-1 if unmapped)

**68040 Implementation**:
- Uses `ptestr` (Page Test Read) instruction
- Checks MMU status register for valid translation
- Handles transparent translation registers

**68060 Implementation**:
- Uses `plpar` (Page Load Physical Address) instruction
- **Exception Handling**: Installs temporary access exception handler
- `plpar` triggers bus error for invalid addresses

**Page Size Detection**:
- Reads TC (Translation Control) register
- Supports 4KB and 8KB page sizes
- Extracts page frame and offset

### Utility Functions

#### `setstack(new_stack)`
**Purpose**: Changes stack pointer (used for OS switching)

**Implementation**:
```assembly
move.l  (sp)+,a0    // Pop return address
move.l  (sp)+,sp    // Set new stack pointer
jmp     (a0)        // Return
```

#### `get_usp()`
**Purpose**: Returns current User Stack Pointer

**Returns**: USP value in `d0`

#### `get_ssp()` (DEBUG only)
**Purpose**: Returns current Supervisor Stack Pointer

**Returns**: SSP value in `d0`

## Data Structures

### Global Variables

#### `_cachevec` / `_cacheveci`
**Type**: Function pointers
**Purpose**: Vectored cache flush routines
**Initialization**: Set by `init_cache()`

#### `l2cv` / `c2lv`
**Type**: Function pointers
**Purpose**: CCW ↔ CACR conversion routines

#### `hascaches`
**Type**: Word (16-bit)
**Purpose**: Boolean flag indicating cache presence

#### `cacr_saved` (ColdFire only)
**Type**: Long (32-bit)
**Purpose**: Saved CACR value for ColdFire processors

### Conversion Tables

#### `ccw_dmasks`
**Purpose**: Default CCW masks per processor type
**Format**: Array of 32-bit values indexed by CPU type

#### `cacr30` / `ccw30`
**Purpose**: 68030 CCW ↔ CACR conversion tables
**Format**: Parallel arrays of 16-bit values

#### `cacr60` / `ccw60`
**Purpose**: 68060 CCW ↔ CACR conversion tables
**Format**: Parallel arrays (32-bit CACR, 16-bit CCW)

#### `cacrv4e` / `ccwv4e`
**Purpose**: ColdFire v4e CCW ↔ CACR conversion tables
**Format**: Parallel arrays of 32-bit values

## Assembly Techniques

### Conditional Assembly
```assembly
#ifdef __mcoldfire__
    // ColdFire-specific code
#else
    // 68K-specific code
#endif
```

### Privileged Instructions
- `movec`: Move to/from control registers
- `cpusha`: Cache push all
- `cpushl`: Cache push line
- `cinva`: Cache invalidate all
- `plpar`: Page load physical address (68060)
- `ptestr`: Page test read (68040)

### Instruction Encoding
Raw instruction encoding used for processor-specific instructions:
```assembly
dc.w    0x4e7a,0x0002   // movec cacr,d0
dc.w    0xF478          // cpusha dc
dc.w    0xF468          // cpushl dc,(a0)
```

## Error Handling

### Cache Operations
- **Invalid addresses**: Return -1 from `log2phys()`
- **MMU disabled**: Physical address = logical address
- **Cache thresholds**: Fall back to full cache flush

### Exception Handling
- **68060 Translation**: Temporary access exception handler
- **Stack management**: Proper register save/restore
- **Interrupt safety**: Critical sections with interrupt disable

## Performance Considerations

### Cache Flush Thresholds
- **68030**: 64 longwords (selective vs. full flush)
- **68040**: 256 cache lines (4KB)
- **68060**: 512 cache lines (8KB)

### Optimization Strategies
- **Vectored dispatch**: Function pointers avoid runtime CPU checks
- **Selective flushing**: Only flush necessary cache lines
- **Physical address caching**: Could be optimized for page boundaries

## Usage Examples

### Enable Data Cache
```c
// Enable data cache on all processors
ccw_set(0x02, 0x02);  // Set bit 1 with mask bit 1
```

### Flush Code After Modification
```c
// Flush data cache and invalidate instruction cache
cpushi(code_address, code_size);
```

### Check Cache Support
```c
// Get default cache mask for current processor
long mask = ccw_getdmask();
if (mask != 0) {
    // Processor has cache support
}
```

## Integration with FreeMiNT

### System Call Interface
The cache control functions are likely exposed through the `ssystem()` system call with `S_CTRLCACHE` command.

### Memory Management Integration
- Physical address translation supports virtual memory
- Cache coherency for shared memory and DMA operations
- Process switching cache management

### Boot Process Integration
- `init_cache()` called during system initialization
- CPU type detection from `mcpu` global variable
- Cache enabled based on system configuration

## Maintenance Notes

### Adding New Processor Support
1. Add CPU detection in `init_cache()`
2. Create processor-specific cache routines
3. Add CCW conversion functions
4. Update default mask table
5. Test cache coherency operations

### Debugging Cache Issues
- Use `get_ssp()` for stack debugging
- Check `hascaches` flag for cache presence
- Verify CCW masks match processor capabilities
- Monitor cache flush thresholds for performance

### Thread Safety
- Cache operations are not inherently thread-safe
- Interrupt disabling may be required for atomic operations
- Consider SMP implications for multi-core systems