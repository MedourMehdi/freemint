# June 2025

## Interface Indexing & Multicast Enhancements

### Added
- **Socket Infrastructure**
  - New `sockaddr_storage` structure with CPU-specific alignment (68020+/68000)
  - `sa_family_t` typedef for socket address families
  - Multicast address detection macro `IS_MULTICAST()`
- **Network Interface Indexing**
  - Added `index` field to `struct netif` tracking registration order
  - New ioctls: `SIOCGIFINDEX` (get index) and `SIOCGIFNAME_IFREQ` (get name)
  - Helper functions: `if_name2index()`, `if_index2name()`, `is_valid_ifindex()`
- **Enhanced Multicast Support**
  - New socket options: `MCAST_JOIN_GROUP`, `MCAST_LEAVE_GROUP`
  - Added `struct ip_mreqn` and `struct group_req` for index-based joins
  - Refactored IGMP join/leave to support interface indices

### Changed
- **Interface Handling**
  - Rewrote `if_name2if()` using new `if_sanitizename()` helper
  - Added index-based lookup to IGMP join/leave operations
- **Error Handling**
  - Improved input validation in ioctl handlers
  - Added error aggregation in multicast operations
  - Enhanced multicast address validation
- **Code Structure**
  - Split IGMP functions into per-interface operations
  - Standardized struct alignment in `msghdr`

## Memory and sysconf fixes and improvements

### Fixed
- **sysconf Implementation**  
  - Corrected `_SC_PHYS_PAGES` to report **total physical memory** instead of free memory (`dos.c`)
  - Added missing `_SC_AVPHYS_PAGES` support to report **available physical memory** (`dos.c`)

### Changed
- **Code Readability**  
  - Replaced magic numbers in `sys_s_ysconf()` with named constants for sysconf parameters (`dos.c`)
  - Added sync comment referencing `mintlib/include/bits/confname.h` (`dos.c`)

### Added
- **Memory API**  
  - New `totalphysmem()` function to calculate total physical RAM (`memory.c`, `memory.h`)

## Context Switch optimizations

Below is a summary of the cycle count differences between the original and optimized context.S assembly files for 68K routines, broken down by function and platform (M68000, M68020+, ColdFire). The savings are measured in CPU cycles and focus on key optimizations:

### Key Changes in Optimized Code:
- **Stack switching**: Reduced `tst.b` instructions for stack checks.
- **Frame restoration**: Optimized loops for internal state copies (unrolled/jump tables).
- **Register usage**: Efficient `movem`/`lea` usage and reduced branching.
- **FPU handling**: Macros for FPU save/restore (no cycle savings).
- **Conditionals**: Streamlined flow for common cases.

### Cycle Savings Summary (Per Function)
| Function          | M68000 (cycles) | M68020+ (cycles) | ColdFire (cycles) |
|-------------------|-----------------|------------------|-------------------|
| **build_context** | 0               | 6                | 0                 |
| **save_context**  | 0               | 0                | 0                 |
| **restore_context**| 18             | 4                | 12                |
| **change_context**| 18              | 4                | 12                |

### Savings Breakdown:
1. **build_context**:
   - **M68020+**: 6 cycles saved by unrolling small internal-state copies (3-word frame).
   - *M68000/ColdFire*: No savings (loop structure unchanged).

2. **save_context**:
   - *All platforms*: No significant changes (minor FPU macro refactor).

3. **restore_context** & **change_context**:
   - **M68000**: 18 cycles saved:
     - **Stack switching**: 8 cycles (reduced to 1 `tst.b`).
     - **Frame copy**: 10 cycles (longword+word moves vs. word loop).
   - **M68020+**: 4 cycles saved (stack switching only).
   - **ColdFire**: 12 cycles saved:
     - **Stack switching**: 4 cycles.
     - **Frame copy**: 8 cycles (block moves vs. loop).

### Diagram: Total Savings per Platform
```plaintext
Platform  build_context  restore_context  change_context  Total
───────────────────────────────────────────────────────────────
M68000         0               18               18         36
M68020+        6                4                4         14
ColdFire       0               12               12         24
```

### Savings per Function (Graph)
```plaintext
build_context:
  M68000   : (0 cycles)
  M68020+  : ██████████████ (6 cycles)
  ColdFire : (0 cycles)

restore_context:
  M68000   : ██████████████████████████████████ (18 cycles)
  M68020+  : ████████ (4 cycles)
  ColdFire : ████████████████████████ (12 cycles)

change_context:
  M68000   : ██████████████████████████████████ (18 cycles)
  M68020+  : ████████ (4 cycles)
  ColdFire : ████████████████████████ (12 cycles)
```

### Conclusions:
- **Largest gains**: `restore_context`/`change_context` on **M68000** (18 cycles each), thanks to stack-touch reduction and loop unrolling.
- **M68020+**: Minor gains (6 cycles in `build_context`, 4 elsewhere).
- **ColdFire**: Consistent savings (12 cycles) in context-switching.
- **Overall**: Optimizations target stack management and data-copy paths, with the most impact on older CPUs (M68000). FPU changes maintain parity.