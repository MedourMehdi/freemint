# FreeMiNT Kernel and Filesystem Documentation

## Table of Contents
1. [Kernel Interface (kerinfo)](#1-kernel-interface-kerinfo)
2. [FAT Filesystem (NEWFATFS)](#2-fat-filesystem-newfatfs)
3. [DMA Interface](#3-dma-interface)
4. [Cookie Jar](#4-cookie-jar)
5. [Block I/O (Buffer Cache)](#5-block-io-buffer-cache)

---

## 1. Kernel Interface (kerinfo)
The `kerinfo` structure provides kernel functions and information to drivers and filesystems.

### Calling Conventions
- Parameters passed on stack (16-bit aligned)
- Registers d0-d1 and a0-a1 are scratch registers
- Return value in register d0

### Data Types
| Type    | Size      |
|---------|-----------|
| int     | 16-bit    |
| short   | 16-bit    |
| long    | 32-bit    |
| ushort  | 16-bit unsigned |
| ulong   | 32-bit unsigned |

### Key Structure Members
```c
struct kerinfo {
    // Version info
    short maj_version;      // Kernel major version
    short min_version;      // Kernel minor version
    
    // OS entry points
    Func *bios_tab;         // BIOS entry points
    Func *dos_tab;          // GEMDOS entry points
    
    // Media change notification
    void _cdecl (*drvchng)(ushort);
    
    // Debugging functions
    void _cdecl (*trace)(const char *, ...);
    void _cdecl (*debug)(const char *, ...);
    void _cdecl (*alert)(const char *, ...);
    EXITING _cdecl (*fatal)(const char *, ...) NORETURN;
    
    // Memory management
    void* _cdecl (*kmalloc)(ulong);  // Allocate kernel memory
    void _cdecl (*kfree)(void *);    // Free kernel memory
    void* _cdecl (*umalloc)(ulong);  // Allocate process memory
    void _cdecl (*ufree)(void *);    // Free process memory
    
    // String utilities
    int _cdecl (*strnicmp)(const char *, const char *, int);
    int _cdecl (*stricmp)(const char *, const char *);
    char* _cdecl (*strlwr)(char *);
    char* _cdecl (*strupr)(char *);
    int _cdecl (*sprintf)(char *, const char *, ...);
    
    // Time utilities
    void _cdecl (*millis_time)(ulong, short *);
    long _cdecl (*unixtime)(ushort, ushort);
    long _cdecl (*dostime)(long);
    
    // Process management
    void _cdecl (*nap)(unsigned);            // Sleep temporarily
    int _cdecl (*sleep)(int que, long cond); // Wait on system queue
    void _cdecl (*wake)(int que, long cond); // Wake processes
    
    // Filesystem utilities
    int _cdecl (*denyshare)(FILEPTR *, FILEPTR *);
    LOCK* _cdecl (*denylock)(LOCK *, LOCK *);
    
    // Timeouts
    TIMEOUT* _cdecl (*addtimeout)(long, void _cdecl (*)());
    void _cdecl (*canceltimeout)(TIMEOUT *);
    
    // Extensions
    BIO* bio;                // Buffered block I/O
    DMA* dma;                // DMA interface
    long _cdecl (*get_toscookie)(ulong tag, ulong *val);
};
```

---

## 2. FAT Filesystem (NEWFATFS)
FreeMiNT 1.15 introduced an integrated FAT filesystem with support for VFAT, FAT32, and large partitions.

### Key Features
- Supports partitions >2GB
- VFAT extensions for long filenames
- Write-back cache
- Runtime configuration

### Activation
Add to `MiNT.CNF`:
```ini
NEWFATFS=A,D,E,F    # Activate on drives A,D,E,F
VFAT=A,D            # Enable VFAT on A and D
WB_ENABLE=C         # Enable write-back cache on C
CACHE=500           # Set cache to 500KB
```

### Dcntl Interface
```c
long Dcntl(short opcode, const char *path, long arg);
```

#### Supported Opcodes
| Opcode          | Value   | Description                          |
|-----------------|---------|--------------------------------------|
| MX_KER_XFSNAME  | 0x6d05  | Get filesystem name                  |
| VFAT_CNFLN      | 0x5601  | Enable/disable VFAT for drive        |
| V_CNTR_SLNK     | 0x5602  | Toggle symbolic links support        |
| V_CNTR_MODE     | 0x5604  | Set filename mode (GEMDOS/ISO/MSDOS) |
| V_CNTR_FAT32    | 0x560a  | Configure FAT32 features             |
| FUTIME          | 0x4603  | Set file timestamps                  |
| FTRUNCATE       | 0x4604  | Truncate file                        |
| FS_INFO         | 0xf100  | Get filesystem info                  |
| FS_USAGE        | 0xf101  | Get storage usage stats              |
| V_CNTR_WB       | 0x5665  | Toggle write-back cache              |

**Usage Example**:
```c
// Get current VFAT status
long status = Dcntl(0x5601, "C:", -1);
```

---

## 3. DMA Interface
Provides standardized DMA handling via the `dma` pointer in `kerinfo`.

### Structure
```c
struct dma {
    ulong _cdecl (*get_channel)(void);       // Get unique channel
    long _cdecl (*free_channel)(ulong);      // Release channel
    void _cdecl (*dma_start)(ulong);         // Start DMA operation
    void _cdecl (*dma_end)(ulong);           // End DMA operation
    void* _cdecl (*block)(ulong, ulong, void _cdecl (*)(PROC *p));
    void _cdecl (*deblock)(ulong, void *);   // Unblock from ISR
};
```

### Workflow
1. **Register**: Call `get_channel()`
2. **Start**: Call `dma_start()`
3. **Block**: Call `block()` to wait for completion
4. **ISR**: Call `deblock()` when operation completes
5. **End**: Call `dma_end()`
6. **Unregister**: Call `free_channel()`

---

## 4. Cookie Jar
Shared BIOS resource for system information.

### Access Methods
```c
// BIOS method
long jar_address = Setexc(0x0168, -1L);

// MiNT method
long jar_address = Ssystem(S_GETLVAL, 0x05a0L, 0L);
```

### Cookie Operations
```c
// Get cookie value
long value;
Ssystem(S_GETCOOKIE, tag, &value);

// Set new cookie
Ssystem(S_SETCOOKIE, tag, value);

// Delete cookie
Ssystem(S_DELCOOKIE, tag, 0L);
```

### Important Notes
- Applications should NOT create new cookies
- Never store pointers in cookies
- Cookies must remain valid after program termination

---

## 5. Block I/O (Buffer Cache)
Global block cache introduced in FreeMiNT 1.15.

### Key Features
- Shared by NEWFATFS and MinixFS
- Automatic size calculation
- Write-back optimization
- Cache consistency

### Initialization
```c
// Get Device Identifier (DI)
DI* di = bio->get_di(drive);

// Set logical sector size
bio->set_lshift(di, 1024);  // 1024-byte blocks
```

### Basic Operations
| Function            | Description                          |
|---------------------|--------------------------------------|
| `read()`            | Read block into cache                |
| `write()`           | Write modified block                 |
| `mark_modified()`   | Flag block as dirty                  |
| `sync_drv()`        | Write all dirty blocks for drive     |
| `lock()`/`unlock()` | Prevent block invalidation           |

### Usage Example
```c
// Read block
UNIT* u = bio->read(di, sector, 1024);

// Modify data
memcpy(u->data, new_data, 1024);

// Mark as dirty
bio->mark_modified(u);

// Write changes
bio->sync_drv(di);
```

### Configuration
- Set cache size with `CACHE=<kb>` in `MiNT.CNF`
- Default: 100KB
- Recommended: 500KB for multiple filesystems