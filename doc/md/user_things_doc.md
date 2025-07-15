# User Things Module Documentation

## Overview

The User Things module provides a trampoline mechanism for kernel procedures that need to execute in user space. It manages the interface between kernel and user mode operations, including process termination, signal handling, shared library management, and XHDI access.

## Files

- **user_things.h**: Structure definitions and interface
- **user_things.S**: Assembly implementation of user-space trampolines

## Architecture

### Core Concept

The module implements a "trampoline" system where:
- Kernel procedures need to execute in user address space
- User processes need access to kernel services
- Code is copied to user memory for execution
- Maintains privilege separation and security

### Data Structures

#### `struct user_things`

```c
struct user_things {
    const long len;                    // Size of structure + code
    BASEPAGE *bp;                     // User basepage pointer
    unsigned long terminateme_p;      // Pterm() pointer
    unsigned long sig_return_p;       // Psigreturn() pointer
    unsigned long pc_valid_return_p;  // Signal handler return
    unsigned long slb_init_and_exit_p; // SLB startup code
    unsigned long slb_open_p;         // SLB open function
    unsigned long slb_close_p;        // SLB close function
    unsigned long slb_close_and_pterm_p; // SLB close + terminate
    unsigned long slb_exec_p;         // SLB function executor
    unsigned long user_xhdi_p;        // XHDI interface
#ifdef JAR_PRIVATE
    struct cookie *user_jar_p;        // Cookie Jar copy
#endif
};
```

## Implementation Details

### Dual Instance System

#### `kernel_things`
- **Location**: Kernel data segment
- **Addressing**: Absolute addresses
- **Usage**: Kernel threads and internal operations
- **Lifetime**: Permanent (not copied)

#### `user_things`
- **Location**: Copied to user process memory
- **Addressing**: Relative addresses (position-independent)
- **Usage**: User processes
- **Lifetime**: Per-process (copied during process creation)

### Function Implementations

#### 1. Process Termination

**`terminateme`**:
```assembly
move.w  4(sp),-(sp)     // Exit code
move.w  #0x004c,-(sp)   // Pterm() function code
trap    #1              // GEMDOS system call
```
- Executes Pterm() system call
- Terminates calling process
- Used by both kernel and user contexts

#### 2. Signal Handling

**`sig_return`**:
```assembly
addq.l  #8,sp           // Remove signal number and format
move.w  #0x011a,-(sp)   // Psigreturn() function code
trap    #1              // Return from signal handler
```

**`pc_valid_return`**:
- Validates return from signal handler
- Terminates process if invalid return detected
- Security mechanism for signal handling

#### 3. Shared Library Management

**`slb_open`**:
- Calls library's open() function in user context
- Handles library initialization
- Manages basepage and name registration
- Returns version number or error code

**`slb_close`**:
- Calls library's close() function
- Performs cleanup operations
- Unregisters library from system

**`slb_close_and_pterm`**:
- Emergency cleanup when process exits without proper close
- Combines library cleanup with process termination
- Prevents resource leaks

**`slb_exec`**:
- Executes specific library functions
- Validates function numbers
- Handles parameter passing
- Returns EINVFN for invalid functions

#### 4. Library Initialization

**`slb_init_and_exit`**:
Comprehensive initialization sequence:

1. **Environment Cleanup**:
   ```assembly
   move.l  B_ENV(a0),d0    // Get environment pointer
   clr.l   B_ENV(a0)       // Clear pointer
   // Free environment memory
   ```

2. **Process Domain Setup**:
   ```assembly
   pea     0x01190001.l    // Pdomain(1)
   trap    #1              // Set process domain
   ```

3. **File Handle Cleanup**:
   ```assembly
   moveq   #0x0004,d7
   loop:
   move.w  d7,-(sp)        // Close handles 0-4
   move.w  #0x003e,-(sp)   // Fclose()
   trap    #1
   ```

4. **Process Group Setup**:
   ```assembly
   clr.l   -(sp)           // Psetpgrp(0,0)
   move.w  #0x010e,-(sp)
   trap    #1
   ```

5. **Binary Format Detection**:
   - Original binutils detection
   - Binutils >= 2.18 detection
   - ELF format support (>= 2.41)
   - Calculates proper entry point

6. **Library Execution**:
   - Calls SLB initialization
   - Stores return value in basepage
   - Calls SLB exit function
   - Terminates process

#### 5. XHDI Interface

**`user_xhdi`**:
```assembly
dc.l    0x27011992      // XHDI magic number
user_xhdi:
move.l  30(sp),-(sp)    // Push 7 parameters
// ... (repeated for all parameters)
move.w  32(sp),-(sp)    // Push opcode
move.w  #0x0001,-(sp)   // XHDI EMU flag
move.w  #0x015f,-(sp)   // System EMU trap
trap    #1              // Execute system call
lea     34(sp),sp       // Clean up stack
rts
```

## Binary Format Support

### Format Detection Logic

The module supports multiple binary formats:

1. **Original Binutils**:
   - Pattern: `0x283a001a`, `0x4efb48fa`
   - Header offset: 228 bytes

2. **Binutils >= 2.18**:
   - Pattern: `0x203a001a`, `0x4efb08fa`
   - Header offset: 228 bytes

3. **ELF Format (>= 2.41)**:
   - Pattern: `0x203a0000`, `0x4efb08fa`
   - Dynamic entry point calculation
   - Supports modern toolchain output

## Usage Patterns

### Process Creation
1. Kernel copies `user_things` to user memory
2. Adjusts pointers to user address space
3. User process can call kernel services
4. Trampolines execute in user context

### Signal Handling
1. Signal delivered to user process
2. User handler executes
3. Returns through `sig_return` trampoline
4. Kernel validates return path

### Library Operations
1. Process calls Slbopen()
2. Library loaded and initialized
3. `slb_init_and_exit` performs setup
4. Library functions available through `slb_exec`

## Security Considerations

### Privilege Separation
- User code cannot directly access kernel
- All kernel access through controlled trampolines
- Validation of return addresses

### Signal Security
- `pc_valid_return` prevents stack manipulation
- Signal handler returns validated
- Process terminated on invalid returns

### Library Security
- Function number validation in `slb_exec`
- Proper cleanup on process exit
- Resource management through basepage

## Error Handling

### Library Errors
- Invalid function numbers return EINVFN (-32)
- Failed opens return GEMDOS error codes
- Cleanup performed on all error paths

### Process Errors
- Invalid signal returns cause termination
- Library cleanup on abnormal exit
- Resource deallocation guaranteed

## Performance Characteristics

### Memory Usage
- Minimal per-process overhead
- Position-independent code
- Efficient trampoline implementation

### Execution Speed
- Direct assembly implementation
- Minimal stack manipulation
- Optimized for common cases

## Development Notes

### Maintenance
- Position-independent code required
- Stack frame calculations critical
- Binary format detection needs updates

### Testing
- Requires multi-process testing
- Signal handling validation
- Library loading/unloading cycles
- Various binary format support

### Future Enhancements
- Additional binary format support
- Enhanced error reporting
- Performance optimizations
- Security improvements

## Related Components

- **FreeMiNT kernel**: Core OS services
- **GEMDOS**: System call interface
- **Signal system**: Process communication
- **Shared libraries**: Dynamic loading
- **Memory management**: Address space handling