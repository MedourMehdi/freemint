# kernel.S - Kernel Entry/Exit Management

## Overview

This file implements critical kernel entry and exit routines for FreeMiNT, replacing the original C implementations from main.c for performance optimization. These routines manage kernel reentrancy and process state transitions.

## Author & History

- **Author**: Konrad M. Kokoszkiewicz <draco@atari.org>
- **Started**: 1998-09-05
- **Purpose**: Speed optimization of kernel entry/exit mechanisms

## Critical Design Considerations

### Non-Reentrancy

**IMPORTANT**: These routines are primarily responsible for maintaining kernel non-reentrancy. This represents a fundamental limitation that requires future architectural changes for full reentrancy support.

### Interrupt Level Management

The implementation eliminates the need for IPL (Interrupt Priority Level) manipulation by using atomic flag operations. This reduces overhead while maintaining thread safety through hardware-level atomicity.

## Global Variables

### in_kernel Flag

```assembly
SYM(in_kernel):
    dc.w    0
```

- **Type**: 16-bit word
- **Purpose**: Prevents multiple simultaneous kernel entries
- **Atomicity**: Managed through atomic bit operations
- **Thread Safety**: Hardware-guaranteed read-modify-write cycles

## Functions

### enter_gemdos()

Manages entry into the kernel from GEMDOS system calls.

```assembly
SYM(enter_gemdos):
    bset    #0x07,SYM(in_kernel)
    bne.s   g_exit
    move.l  SYM(curproc),a0
    move.w  #0x0001,P_INDOS(a0)
g_exit:
    rts
```

#### Operation Flow

1. **Atomic flag test**: Uses `bset` instruction to atomically test and set bit 7 of in_kernel
2. **Reentrancy check**: If bit was already set, skip process state update
3. **Process state update**: Sets P_INDOS flag in current process structure
4. **Return**: Simple RTS instruction

#### Key Features

- **Atomic operation**: `bset` provides hardware-level atomicity
- **Conditional execution**: Only updates process state on first entry
- **Performance optimized**: Minimal instruction count
- **Reentrancy safe**: Handles nested calls gracefully

### leave_kernel()

Manages exit from the kernel back to user mode or signal handlers.

```assembly
SYM(leave_kernel):
    move.l  SYM(curproc),a0
    clr.w   P_INDOS(a0)
#ifndef M68000
    clr.w   P_INVDI(a0)
#endif
    clr.w   SYM(in_kernel)
    rts
```

#### Operation Flow

1. **Load current process**: Gets curproc address into a0
2. **Clear DOS flag**: Resets P_INDOS in process structure
3. **Clear VDI flag**: Resets P_INVDI (on non-M68000 systems)
4. **Release kernel lock**: Clears in_kernel flag
5. **Return**: Simple RTS instruction

#### Assembly Module Dependencies

**Critical**: Other assembly modules rely on specific behaviors:
- **Register a0**: Must contain curproc address upon return
- **Register d0**: Must be preserved unchanged
- **Usage example**: Referenced by syscall.spp

#### Architecture Considerations

**M68000 Exclusion**: The P_INVDI clearing is conditional:
```assembly
#ifndef M68000
    clr.w   P_INVDI(a0)
#endif
```

This suggests VDI-related functionality is not supported on original M68000 systems.

## Performance Optimizations

### Instruction Count Reduction

The assembly implementation eliminates approximately 10 instructions per enter/leave cycle compared to the original C version, including:
- 9 memory-to-memory long moves
- IPL manipulation overhead
- Function call overhead

### Atomic Operations

Using `bset` instruction provides:
- **Hardware atomicity**: No need for interrupt disabling
- **Single instruction**: Atomic test-and-set operation
- **Conditional branching**: Immediate status feedback

### Register Usage

Efficient register utilization:
- **Minimal register pressure**: Uses only a0 and implicit flags
- **Preserved state**: Maintains d0 for caller compatibility
- **Stack efficiency**: No stack frame overhead

## Integration Points

### Trap Handlers

The in_kernel flag is checked by:
- **GEMDOS trap handlers**: Redirect to ROM when kernel busy
- **VBL routine**: Respects kernel state during vertical blank
- **Interrupt handlers**: Coordinate with kernel entry state

### Process Management

Interacts with process structure fields:
- **P_INDOS**: Indicates DOS system call in progress
- **P_INVDI**: Indicates VDI system call in progress  
- **curproc**: Current process pointer

### System Call Flow

1. **Trap entry**: Trap handler checks in_kernel flag
2. **Kernel entry**: enter_gemdos() called if kernel available
3. **System call execution**: Kernel processes request
4. **Kernel exit**: leave_kernel() called before return
5. **User mode**: Control returns to user process

## Thread Safety Considerations

### Hardware Atomicity

The `bset` instruction provides:
- **Atomic test-and-set**: Single bus cycle operation
- **Interrupt immunity**: Cannot be interrupted mid-operation
- **Memory ordering**: Proper synchronization semantics

### Race Condition Prevention

The design prevents:
- **Multiple kernel entries**: Only one process can enter kernel
- **Inconsistent state**: Atomic flag operations prevent partial updates
- **Interrupt conflicts**: Hardware-level synchronization

## Architectural Limitations

### Non-Reentrancy

Current limitations include:
- **Single-threaded kernel**: Only one process can execute kernel code
- **No SMP support**: Design assumes single processor
- **Limited concurrency**: Blocks concurrent system calls

### Future Considerations

The comment indicates future architectural changes needed:
- **Full reentrancy**: Would require significant redesign
- **Per-process kernel stacks**: Enable concurrent kernel execution
- **Fine-grained locking**: Replace global kernel lock

## Error Handling

The implementation assumes:
- **Valid curproc**: No null pointer checking
- **Proper nesting**: Assumes correct enter/leave pairing
- **Hardware functionality**: Relies on atomic instruction support

## Usage Patterns

### Typical Call Sequence

```
User Program → System Call → Trap Handler → enter_gemdos() → 
Kernel Code → leave_kernel() → Trap Handler → User Program
```

### Error Conditions

```
User Program → System Call → Trap Handler → enter_gemdos() → 
[kernel busy] → ROM Handler → User Program
```

## Compatibility Notes

### Historical Context

This implementation replaced C code for performance reasons while maintaining:
- **API compatibility**: Same function signatures
- **Behavioral compatibility**: Identical semantics
- **Integration compatibility**: Works with existing trap handlers

### Architecture Support

- **M68000**: Basic support with VDI limitations
- **M68020+**: Full functionality including VDI support
- **Coldfire**: Likely compatible but not explicitly mentioned

The implementation represents a critical performance optimization while maintaining the fundamental architectural limitations of the FreeMiNT kernel's non-reentrant design.