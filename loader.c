#include "loader.h"

#define PAGE_SIZE 4096u
#define PAGE_MASK (~(PAGE_SIZE - 1u))
#define MAX_PAGES 65536

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

Elf32_Ehdr *ehdr;
Elf32_Phdr *phdr;
int fd = -1;

static char *file_buf = NULL;
static size_t file_size = 0;

static uintptr_t mapped_pages[MAX_PAGES];
static volatile size_t mapped_count = 0;

static volatile unsigned long page_faults = 0;
static volatile unsigned long page_allocs = 0;
static volatile unsigned long frag_bytes = 0;

static struct sigaction old_sa;
static int handler_installed = 0;

static void fatal(const char *msg, size_t len) {
  write(STDERR_FILENO, msg, len);
  _exit(139);
}

static int page_is_mapped(uintptr_t pg) {
  for (size_t i = 0; i < mapped_count; i++) {
    if (mapped_pages[i] == pg) {
      return 1;
    }
  }
  return 0;
}

static int addr_in_segment(uintptr_t addr) {
  for (int i = 0; i < ehdr->e_phnum; i++) {
    if (phdr[i].p_type != PT_LOAD) {
      continue;
    }
    uintptr_t start = phdr[i].p_vaddr;
    uintptr_t end = start + phdr[i].p_memsz;
    if (addr >= start && addr < end) {
      return 1;
    }
  }
  return 0;
}

static void segv_handler(int sig, siginfo_t *si, void *ctx) {
  (void)sig;
  (void)ctx;
  uintptr_t addr = (uintptr_t)si->si_addr;
  uintptr_t pg = addr & PAGE_MASK;

  page_faults++;

  if (!addr_in_segment(addr)) {
    static const char m[] = "Invalid memory access: address not in any PT_LOAD segment\n";
    fatal(m, sizeof(m) - 1);
  }

  if (page_is_mapped(pg)) {
    static const char m[] = "Fault on an already mapped page\n";
    fatal(m, sizeof(m) - 1);
  }

  if (mapped_count >= MAX_PAGES) {
    static const char m[] = "Too many pages allocated\n";
    fatal(m, sizeof(m) - 1);
  }

  void *p = mmap((void *)pg, PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  if (p == MAP_FAILED || (uintptr_t)p != pg) {
    static const char m[] = "mmap failed while handling page fault\n";
    fatal(m, sizeof(m) - 1);
  }

  uintptr_t covered = 0;
  for (int i = 0; i < ehdr->e_phnum; i++) {
    if (phdr[i].p_type != PT_LOAD) {
      continue;
    }
    uintptr_t seg_start = phdr[i].p_vaddr;
    uintptr_t seg_mem_end = seg_start + phdr[i].p_memsz;
    uintptr_t seg_file_end = seg_start + phdr[i].p_filesz;
    uintptr_t pg_end = pg + PAGE_SIZE;

    uintptr_t s = seg_start > pg ? seg_start : pg;
    uintptr_t e = seg_mem_end < pg_end ? seg_mem_end : pg_end;
    if (s >= e) {
      continue;
    }
    covered += e - s;

    uintptr_t fe = seg_file_end < pg_end ? seg_file_end : pg_end;
    if (s < fe) {
      memcpy((void *)s, file_buf + phdr[i].p_offset + (s - seg_start), fe - s);
    }
  }

  mapped_pages[mapped_count++] = pg;
  page_allocs++;
  if (covered < PAGE_SIZE) {
    frag_bytes += PAGE_SIZE - covered;
  }
}

void loader_cleanup(void) {
  if (handler_installed) {
    sigaction(SIGSEGV, &old_sa, NULL);
    handler_installed = 0;
  }
  for (size_t i = 0; i < mapped_count; i++) {
    munmap((void *)mapped_pages[i], PAGE_SIZE);
  }
  mapped_count = 0;

  if (file_buf) {
    free(file_buf);
    file_buf = NULL;
  }
  if (fd >= 0) {
    close(fd);
    fd = -1;
  }
  ehdr = NULL;
  phdr = NULL;
}

static void read_whole_file(void) {
  struct stat st;
  if (fstat(fd, &st) < 0) {
    perror("fstat");
    exit(1);
  }
  if (st.st_size < (off_t)sizeof(Elf32_Ehdr)) {
    fprintf(stderr, "File too small to be an ELF\n");
    exit(1);
  }
  file_size = (size_t)st.st_size;
  file_buf = malloc(file_size);
  if (!file_buf) {
    perror("malloc");
    exit(1);
  }
  size_t done = 0;
  while (done < file_size) {
    ssize_t n = read(fd, file_buf + done, file_size - done);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("read");
      exit(1);
    }
    if (n == 0) {
      fprintf(stderr, "Unexpected end of file\n");
      exit(1);
    }
    done += (size_t)n;
  }
}

static void validate_headers(void) {
  ehdr = (Elf32_Ehdr *)file_buf;

  if (ehdr->e_version != EV_CURRENT || ehdr->e_ehsize != sizeof(Elf32_Ehdr)) {
    fprintf(stderr, "Invalid ELF header version or size\n");
    exit(1);
  }
  if (ehdr->e_phentsize != sizeof(Elf32_Phdr) || ehdr->e_phnum == 0) {
    fprintf(stderr, "Invalid program header table\n");
    exit(1);
  }
  uint64_t ph_end = (uint64_t)ehdr->e_phoff + (uint64_t)ehdr->e_phnum * sizeof(Elf32_Phdr);
  if (ph_end > file_size) {
    fprintf(stderr, "Program header table out of bounds\n");
    exit(1);
  }
  phdr = (Elf32_Phdr *)(file_buf + ehdr->e_phoff);

  int entry_found = 0;
  int load_count = 0;
  for (int i = 0; i < ehdr->e_phnum; i++) {
    if (phdr[i].p_type != PT_LOAD) {
      continue;
    }
    load_count++;

    uint64_t file_end = (uint64_t)phdr[i].p_offset + phdr[i].p_filesz;
    uint64_t mem_end = (uint64_t)phdr[i].p_vaddr + phdr[i].p_memsz;
    if (phdr[i].p_filesz > phdr[i].p_memsz || file_end > file_size ||
        mem_end > 0xFFFFFFFFull) {
      fprintf(stderr, "Corrupt PT_LOAD segment\n");
      exit(1);
    }

    for (int j = 0; j < i; j++) {
      if (phdr[j].p_type != PT_LOAD) {
        continue;
      }
      uint64_t other_end = (uint64_t)phdr[j].p_vaddr + phdr[j].p_memsz;
      if (phdr[i].p_vaddr < other_end && phdr[j].p_vaddr < mem_end) {
        fprintf(stderr, "Overlapping PT_LOAD segments\n");
        exit(1);
      }
    }

    if (ehdr->e_entry >= phdr[i].p_vaddr && ehdr->e_entry < mem_end) {
      if (!(phdr[i].p_flags & PF_X)) {
        fprintf(stderr, "Entry point segment is not executable\n");
        exit(1);
      }
      entry_found = 1;
    }
  }
  if (load_count == 0) {
    fprintf(stderr, "No PT_LOAD segment found\n");
    exit(1);
  }
  if (!entry_found) {
    fprintf(stderr, "Entry point is not inside any PT_LOAD segment\n");
    exit(1);
  }
}

static void install_handler(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = segv_handler;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGSEGV, &sa, &old_sa) < 0) {
    perror("sigaction");
    exit(1);
  }
  handler_installed = 1;
}

void load_and_run_elf(char *exe) {
  fd = open(exe, O_RDONLY);
  if (fd < 0) {
    perror("open");
    exit(1);
  }

  read_whole_file();
  validate_headers();
  install_handler();

  int (*start_fn)(void) = (int (*)(void))(uintptr_t)ehdr->e_entry;
  int result = start_fn();

  printf("User _start return value = %d\n", result);
  printf("Total page faults = %lu\n", page_faults);
  printf("Total page allocations = %lu\n", page_allocs);
  printf("Total internal fragmentation = %.2f KB\n", (double)frag_bytes / 1024.0);
}
