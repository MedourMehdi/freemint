# MiNT Operating System Documentation

## Introduction
MiNT is a multitasking OS extension for Atari ST/TT/Falcon systems that replaces most of GEMDOS. It provides enhanced services including:
- File and process management
- Multitasking capabilities
- Interprocess communication
- Job control
- Flexible file system interface

```c
// MiNT kernel copyright information
MiNT kernel is derived from MiNT, Copyright 1990-1992 Eric R. Smith.
All rights reserved. Used and modified by Atari under license.
New code and features Copyright 1992-1994 Atari Corporation.
```

## Pseudo Drives
MiNT provides a virtual "U:" drive ("unified") with special directories:

### U:\DEV
Access point for BIOS devices:
- `CENTR`: Parallel printer port
- `MODEM1`: RS232 serial port
- `MIDI`: MIDI port
- `PRN`: Printer device
- `CON`: Current control terminal
- `NULL`: Null device (like Unix's /dev/null)

### U:\PIPE
Contains FIFO queues for interprocess communication:
- Temporary files deleted when last program closes them
- Used by window managers and print spoolers

### U:\PROC
Represents executing processes:
- Filename format: `PROCESSNAME.PID` (e.g., `GEM.001`)
- Attribute bits indicate process state:
  - `0x00`: Running
  - `0x01`: Ready
  - `0x20`: Waiting for event
  - `0x22`: Zombie (exited)
  - `0x02`: Terminated and resident (TSR)

### U:\SHM
Shared memory directory:
- Fast interprocess communication method
- Files represent shared memory blocks
- Persists until deleted with `Fdelete()`

## System Features

### Background Processes
Run programs in background via:
- MultiTOS desktop
- Shells like TCSH
```bash
# TCSH example
% make >& errors &  # Run 'make' in background
```

### Pipes
Memory-based communication channels:
- Faster than temporary files
- Atomic writes up to 1KB
```bash
# Pipeline example
% ls -lt | head  # Show 10 most recent files
```

### Job Control
Manage processes with signals:
- `^Z` (Ctrl+Z): Suspend process
- `fg`: Bring to foreground
- `bg`: Run in background
- `^C` (Ctrl+C): Send SIGINT
- `^\` (Ctrl+\): Send SIGQUIT

## Programming with MiNT

### Testing for MiNT Presence
Check cookie jar for MiNT cookie:
```c
#define MINT_COOKIE 0x4d694e54  // 'MiNT' in ASCII
// Cookie value format: 0x0000MMmm (major/minor version)
```

### File Handles and Devices
Special file handles:
- `-1`: Current control terminal
- `-4`: MIDI input
- `-5`: MIDI output
Redirect with:
```c
Fforce(-1, Fopen("U:\\DEV\\MODEM1", 2));  // Redirect to modem
```

### Interprocess Communication
#### Signals
32 defined signals with specific purposes:
```c
#define SIGHUP  1   // Terminal disconnected
#define SIGINT  2   // User interrupt (Ctrl+C)
#define SIGKILL 9   // Unblockable termination
#define SIGTERM 15  // Polite termination
#define SIGSTOP 17  // Unblockable suspend
```

System calls for signal handling:
```c
Pkill(pid, sig);      // Send signal to process
Psignal(sig, handler); // Set signal handler
Psigblock(mask);      // Block signals
```

#### FIFOs
Create with `Fcreate()`:
```c
// Flags for FIFO creation
#define UNIDIRECTIONAL 0x01
#define NONBLOCKING    0x02
#define PSEUDO_TTY     0x04
#define UNIX_READ      0x20
```

Example client-server communication:
```c
// Server creates FIFO
fd = Fcreate("U:\\PIPE\\FORTUNE", 0);

// Client accesses FIFO
fd = Fopen("U:\\PIPE\\FORTUNE", 2);
```

#### Shared Memory
Create and access shared blocks:
```c
// Server creates shared memory
fd = Fcreate("U:\\SHM\\MY.SHARE", 0);
blk = Malloc(128L);
Fcntl(fd, blk, SHMSETBLK);

// Client accesses shared memory
fd = Fopen("U:\\SHM\\MY.SHARE", 2);
blk = Fcntl(fd, 0L, SHMGETBLK);
```

## Time Management

### Obsolete System Calls
Deprecated but still supported:
- `Tgettime()`/`Tsettime()`: Time of day
- `Tgetdate()`/`Tsetdate()`: Current date

### New System Calls
#### `Tgettimeofday()`
```c
struct timeval {
  long tv_sec;   // Seconds since epoch (Jan 1, 1970 UTC)
  long tv_usec;  // Microseconds
};

struct timezone {
  int tz_minuteswest;  // UTC offset in minutes
  int tz_dsttime;      // Daylight saving time flag
};

long Tgettimeofday(struct timeval *tv, struct timezone *tzp);
```

#### `Tsettimeofday()`
```c
long Tsettimeofday(struct timeval *tv, struct timezone *tzp);
```
- Requires super-user privileges
- Valid range: Jan 1, 1980 - 2038
- Timezone offset: ±720 minutes

### Error Conditions
- `EACCDN`: Permission denied
- `ERANGE`: Argument out of range

## Best Practices
1. Minimize memory usage with `Mshrink()`
2. Avoid global system modifications
3. Use supervisor mode sparingly
4. Use documented output calls (AES/VDI/BIOS)
5. Handle SIGTERM for clean termination
6. Don't access unauthorized memory

## New System Calls
Key MiNT extensions:
```c
Fpipe()         // Create pipe
Fcntl()         // File control
Pgetpid()       // Get process ID
Pkill()         // Send signal
Pvfork()        // Shared memory fork
Fselect()       // File descriptor monitoring
Tgettimeofday() // Precise time retrieval
```