### FreeMiNT Interrupt Handling Module Documentation (`intr.S` and `intr.h`)

#### **Overview**
This module provides low-level interrupt and exception handling for the FreeMiNT kernel. It implements hardware vector hooks for critical system events (timers, exceptions, I/O) and manages context switching between kernel and user processes. Key features include:
- Timer-based process scheduling
- Hardware exception handling
- Keyboard/mouse input processing
- System reset/reboot routines
- BIOS-level disk operation hooks

---

#### **File Information**
- **Primary Files**:  
  - `intr.S`: Assembly implementation of interrupt handlers  
  - `intr.h`: C interface definitions and vector declarations
- **Author**: Eric R. Smith, Atari Corporation contributors
- **Architectures**: 68030, ColdFire (conditional compilation via `__mcoldfire__`)
- **Critical Dependencies**: 
  - `magic/magic.i`: Magic numbers for stack frames
  - `mint/asmdefs.h`: MiNT-specific assembly definitions

---

#### **Core Components**

##### **1. Timer Interrupts**
**`mint_5ms` (5ms Timer Handler)**  
- **Purpose**: Core system scheduler tick
- **Behavior**:
  - Updates process time counters (`P_USRTIME`/`P_SYSTIME`)
  - Emulates VBL every 20ms (4th interrupt)
  - Manages system uptime (`SYM(uptime)`)
  - Triggers preemption via `SYM(preempt)` when time slice expires
- **Critical Variables**:
  - `vblcnt`: VBL emulation counter (4 → 1)
  - `SYM(proc_clock)`: Process time quantum
  - `SYM(in_kernel)`: Kernel mode flag

**`mint_vbl` (VBL Emulation Handler)**  
- **Purpose**: 20ms periodic tasks
- **Behavior**:
  - Polls keyboard buffer (`SYM(keyrec)`)
  - Updates timeout lists (`SYM(tlist)`)
  - Triggers floppy operations (`SYM(flopvbl)`) 
  - Calculates load average (`SYM(calc_load_average)`)

---

##### **2. System Control**
**`reset` / `reboot`**  
- **Purpose**: Warm system restart
- **Behavior**:
  - Restores original interrupt vectors via `SYM(restr_intr)`
  - Jumps to TOS reset vector (address in `a6`)
- **Stack Handling**: Uses temporary stack at `0x0600-0x06FF`

---

##### **3. Input Handling**
**`newmvec` (Mouse Vector)**  
- **Purpose**: Three-button mouse support
- **Behavior**:
  - Merges middle button state from `third_button`
  - Calls `SYM(mouse_handler)` with modified packet

**`newjvec` (Joystick Vector)**  
- **Purpose**: Middle button detection
- **Behavior**:
  - Reads button state from joystick port (bit 0)
  - Generates synthetic mouse packet
  - Chains to original joystick handler (`SYM(oldjvec)`)

**`newkeys` (Keyboard Vector)**  
- **Purpose**: Scancode processing
- **Behavior**:
  - Filters legal keycodes (0x00-0x1F)
  - Calls `SYM(ikbd_scan)` for processing

---

##### **4. Exception Handlers**
###### **Common Exception Flow**:
1. Set `SYM(sig_routine)` to appropriate C handler
2. Save exception context in `curproc` structure
3. Handle in-kernel vs. user-mode exceptions
4. Trigger signal delivery or kernel panic

###### **Key Handlers**:
| Vector          | Signal Raised   | Handler Routine    | Notes                          |
|-----------------|-----------------|--------------------|--------------------------------|
| Bus Error       | `SIGBUS`       | `_mmu_sigbus`      | Handles MMU faults             |
| Address Error   | `SIGSEGV`      | `SYM(sigaddr)`     | Invalid memory access          |
| Illegal Opcode  | `SIGILL`       | `SYM(sigill)`      | CPU instruction violation      |
| Divide by Zero  | `SIGFPE`       | `SYM(sigfpe)`      | Arithmetic exception           |
| Privilege Viol. | `SIGPRIV`      | `SYM(sigpriv)`     | User-mode kernel instruction   |
| Trace Trap      | `SIGTRAP`      | `SYM(sigtrap)`     | Debugging                      |
| Format Error    | Kernel Panic   | `SYM(haltformat)`  | Invalid stack frame            |

---

##### **5. BIOS Hooks**
**`new_getbpb` / `new_mediach` / `new_rwabs`**  
- **Purpose**: Virtual drive support (e.g., `U:`)
- **Behavior**:
  - Intercepts drive numbers 0x00-0x1F
  - Remaps via `SYM(aliasdrv)` array
  - Fakes responses for virtual drives (returns 0/success)

---

#### **Critical Global Variables**
| Variable              | Location | Purpose                          |
|-----------------------|----------|----------------------------------|
| `SYM(old_5ms)`        | `intr.S` | Original 5ms timer vector        |
| `SYM(vblcnt)`         | `intr.S` | VBL counter (4→1)                |
| `SYM(third_button)`   | `intr.S` | Middle mouse button state        |
| `SYM(in_kernel)`      | `intr.S` | Kernel mode flag (bit 7)         |
| `SYM(curproc)`        | `intr.S` | Current process descriptor       |
| `SYM(aliasdrv)`       | `intr.S` | Drive remapping table            |
| `old_*` vectors       | `intr.h` | Original hardware vector pointers|

---

#### **Context Switching**
**`build_context` / `restore_context`**  
- **Purpose**: Process state preservation
- **Registers Saved**:
  - **Full Set**: `d0-d7/a0-a7` + PC/SR
  - **ColdFire**: Optimized 16-register save
- **Stack Handling**:
  - User → Kernel: Switches to `curproc->sysstack`
  - Kernel → User: Restores user stack pointer

---

#### **ColdFire Specifics**
- **Stack Frames**: Uses 68060-style frames when `coldfire_68k_emulation` set
- **Register Handling**: 
  - 32-bit `move.l` replaces 16-bit `move.w`
  - `mvz.w` for zero-extended word moves
- **Interrupt Control**: Explicit IPL masking via `ori.l #0x0400,sr`

---

#### **Usage Examples**
**Installing a Vector Hook**:
```c
// From C code
extern void new_5ms_handler(void);
SYM(old_5ms) = Setexc(0x114, new_5ms_handler); 
```

**Handling Exceptions**:
```c
// Signal handler dispatch
void sigbus_handler(int sig) {
    struct proc *p = curproc;
    printf("Bus error at %lx\n", p->exc_addr);
    // ... recovery logic ...
}
```

---

#### **Technical Notes**
1. **Stack Frame Formats**:
   - **68000**: Variable length (check `0x59E` flag)
   - **68040**: 46-byte frame with SSW/FA
   - **ColdFire**: 8-byte base frame + extended regs

2. **Atomic Operations**:
   ```asm
   ; Critical section entry
   ori.w   #0x0700,sr     ; Disable interrupts
   ; ... critical code ...
   move.w  (sp)+,sr       ; Restore
   ```

3. **Three-Button Mouse Workflow**:
   ```
   Joystick Port → newjvec → third_button → newmvec → mouse_handler
                     ↑                      ↓
               (State capture)      (Packet injection)
   ```

---

#### **Limitations**
- **68000 Support**: Limited exception handling (no MMU support)
- **ColdFire**: No FPU/PMMU detection in handlers
- **Virtual Drives**: Hardcoded drive `U:` (0x14)

---

#### **Debugging Tips**
1. **Trace Exceptions**: Set `SYM(sig_exc)` to vector number before handling
2. **Kernel Mode Checks**: Test `SYM(in_kernel)` bit 7
3. **Timer Debug**: Monitor `SYM(uptimetick)` for drift
