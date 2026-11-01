# SimpleSmartLoader Design Document

## Group
- Group ID: 113
- GitHub (private repository): https://github.com/somya25498/SimpleSmartLoader_Group113

## Contribution of each member
- Member 1: Somya Arora, 2025498
  - Core loader in 'loader.c': SIGSEGV page-fault handler, page-by-page 'mmap' allocation, copying segment contents from the file buffer, zero-filled bss.
  - Statistics (page faults, page allocations, internal fragmentation) and 'loader_cleanup'.
- Member 2: Anaya Jacob, 2025072
  - ELF validation: 'check_elf' in 'launch.c' and 'validate_headers' in 'loader.c', including testing with invalid inputs (64-bit binary, truncated file, missing file).
  - Testing: building with 'make', running 'fib' and 'sum', and writing and running additional testcases.
  - Setting up the private GitHub repository and reviewing this design document.

## Overview
SimpleSmartLoader runs a 32-bit static ELF executable without mapping any segment upfront. Memory is mapped lazily, one 4KB page at a time, from a SIGSEGV handler. The program is not terminated on a fault: after the handler maps and fills the page, the faulting instruction is retried and execution continues.

## Files
- 'launch.c': entry point. Checks the command line and the ELF header, calls the loader, then calls cleanup.
- 'loader.h': shared includes and the loader function prototypes.
- 'loader.c': loading logic, page-fault handler, statistics and cleanup.
- 'fib.c', 'sum.c': testcases.
- 'Makefile': builds the loader and the testcases.

## Flow
1. 'launch.c' checks the ELF header (magic, 32-bit, little-endian, ET_EXEC, EM_386) and calls 'load_and_run_elf'.
2. 'loader.c' reads the whole file into a heap buffer and validates it: header version and size, program header table bounds, PT_LOAD segments ('p_filesz <= p_memsz', offsets and addresses do not overflow, no overlapping segments), at least one PT_LOAD, and the entry point lying inside an executable (PF_X) PT_LOAD segment.
3. A SIGSEGV handler is installed with 'sigaction' (SA_SIGINFO). The previous handler is saved and restored in cleanup.
4. 'e_entry' is cast to 'int (*)(void)' and called directly. This faults because nothing is mapped yet.

## Page fault handling
1. Take the fault address from 'siginfo_t.si_addr' and count a page fault.
2. If the address is not inside any PT_LOAD segment ('p_vaddr' to 'p_vaddr + p_memsz'), print an error and exit. This is a real invalid access, not a demand page.
3. Align the address down to a 4KB boundary and 'mmap' exactly one anonymous page there with 'MAP_FIXED_NOREPLACE'.
4. For every PT_LOAD segment overlapping that page, copy the file-backed part ('p_filesz') from the file buffer. The remainder (bss, 'p_memsz - p_filesz', and any gap) stays zero from the anonymous mapping. Handling all overlapping segments makes a page shared by two segments load correctly.
5. Record the page, count one page allocation, and add the unused bytes of the page (4096 minus bytes covered by segments) to the internal fragmentation total.
6. Return from the handler so the faulting instruction is retried.

A segment larger than one page therefore causes several faults, one per page touched. Pages never accessed are never allocated.

## Statistics
After '_start' returns, the loader prints the return value, the total number of page faults, the total number of page allocations, and the total internal fragmentation in KB. For a successful run every fault allocates exactly one new page, so page faults equal page allocations.

## Cleanup
'loader_cleanup' restores the previous SIGSEGV handler, unmaps every page allocated by the handler, frees the file buffer and closes the file descriptor.

## Build and run
```
make
./launch ./fib
./launch ./sum
make clean
```
The loader is built as a 32-bit PIE ('-m32 -fPIE -pie') so it does not collide with the testcases, which are linked at 0x08048000 ('-m32 -no-pie -nostdlib').

## Testing
Besides 'fib' and 'sum', the loader was tested with programs having a large bss, initialized and const data spanning several pages, a 1MB sparsely accessed array, many functions spanning several text pages, page-aligned functions called through a function pointer table, and deep recursion. Both '-O0' and '-O2' builds were used, and the return values matched native execution.
