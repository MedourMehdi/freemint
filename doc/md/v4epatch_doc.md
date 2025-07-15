# V4E Patch Module Documentation

## Overview

The V4E patch module provides ColdFire processor compatibility fixes for FreeMiNT. It addresses instruction set differences between the original 68000 and ColdFire V4e processors, particularly for Pure C compiler compatibility and AES function calls.

## File: v4epatch.S

### Target Architecture
- **Primary**: ColdFire (`__mcoldfire__`)
- **Purpose**: Runtime patching of incompatible code sequences

## Functions

### 1. `patch_memset_purec`

**Purpose**: Patches Pure C compiler's memset implementation for ColdFire compatibility

#### Parameters
- `32(sp)`: Pointer to BASEPAGE structure

#### Functionality

**Search Algorithm**:
1. Extracts text segment information from BASEPAGE
2. Searches through program text for specific Pure C memset pattern
3. Locates and patches incompatible instruction sequences

**Critical Patch**:
```assembly
// Original incompatible sequence:
move.b D0,-(SP)    // 0x1400
move.w (SP)+,D2    // 0xE18A (not supported on ColdFire)

// Patched sequence:
move.b D0,D2       // 0x1400
lsl.l #8,D2        // 0xE18A (ColdFire compatible)
```

**Implementation Details**:
- Searches through text segment word by word
- Uses pattern matching with 12 long-word signature
- Patches at offset 0x14 from pattern start
- Preserves all registers during operation

#### Pattern Recognition

The function searches for this specific Pure C memset signature:
```assembly
memset_purec:
dc.l 0x2F08D1C1,0x24080802,0x00006708,0x53816500
dc.l 0x00AC1100,0x1F00341F,0x14003002,0x48423400
// ... continues with complete pattern
```

### 2. `call_stack_func`

**Purpose**: Provides stack switching mechanism for AES function calls

#### Parameters
```c
void call_stack_func(AESCALL func, AESPB* pb, my_aes_CLIENT* c, void* stack);
```

- `func`: Function pointer to AES call
- `pb`: Pointer to AES Parameter Block
- `c`: Client structure
- `stack`: New stack area pointer

#### Functionality

**Stack Management**:
1. Switches to provided stack area
2. Preserves return address and parameters
3. Calls target function with new stack
4. Restores original stack on return

**Implementation**:
```assembly
move.l 16(sp),a0     // Get new stack pointer
move.l sp, -(a0)     // Save current stack
move.l 12(sp), -(a0) // Save client parameter
move.l 8(sp), -(a0)  // Save pb parameter
move.l #ret_stack_func, -(a0)  // Set return address
move.l 4(sp),a1      // Get function pointer
move.l a0,sp         // Switch to new stack
jmp (a1)             // Call function
```

## Architecture-Specific Considerations

### ColdFire Limitations

**Instruction Incompatibilities**:
- No predecrement addressing with byte operations to stack
- Different instruction encoding for some operations
- Stack operations must be word-aligned

**Solutions Implemented**:
- Runtime code patching for compiler-generated code
- Alternative instruction sequences
- Stack management helpers

### Memory Management

**BASEPAGE Structure Access**:
- `0xC(basepage)`: p_tlen (text segment length)
- `256(basepage)`: Start of text segment
- Used for locating and patching code

## Usage Scenarios

### Pure C Compatibility
The patch is automatically applied when:
1. A Pure C compiled program is loaded
2. The memset pattern is detected
3. ColdFire processor is detected

### AES Integration
Stack switching is used for:
1. AES function calls requiring specific stack layout
2. Maintaining compatibility with different AES implementations
3. Isolating AES operations from main program stack

## Error Handling

**Pattern Matching**:
- Continues search if pattern doesn't match completely
- Safely handles programs without the target pattern
- No modification if pattern not found

**Stack Operations**:
- Preserves all registers
- Maintains stack integrity
- Handles return path correctly

## Performance Impact

### Runtime Patching
- One-time cost during program loading
- Minimal impact on program execution
- Improves overall compatibility

### Stack Switching
- Minimal overhead for AES calls
- Efficient register usage
- Direct assembly implementation

## Development Notes

### Maintenance
- Pattern signature must match Pure C exactly
- Patch instruction must be ColdFire compatible
- Stack frame size must be calculated correctly

### Testing
- Requires Pure C compiled test programs
- ColdFire hardware or emulator needed
- AES functionality testing required

### Future Considerations
- Additional compiler patterns may need patches
- New ColdFire variants may require updates
- AES evolution may need stack handling changes

## Related Components

- **FreeMiNT kernel**: Core OS providing BASEPAGE
- **Pure C compiler**: Target of compatibility fixes
- **AES**: Application Environment Services
- **ColdFire processor**: Target hardware architecture

## Technical References

- ColdFire Programmer's Reference Manual
- Pure C Compiler Documentation
- Atari AES Documentation
- FreeMiNT Kernel Internals