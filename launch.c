#include "loader.h"

static void check_elf(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    perror("fopen");
    exit(1);
  }
  Elf32_Ehdr h;
  if (fread(&h, sizeof(h), 1, f) != 1) {
    fprintf(stderr, "Not a valid ELF file\n");
    fclose(f);
    exit(1);
  }
  fclose(f);

  if (memcmp(h.e_ident, ELFMAG, SELFMAG) != 0) {
    fprintf(stderr, "Not an ELF file\n");
    exit(1);
  }
  if (h.e_ident[EI_CLASS] != ELFCLASS32) {
    fprintf(stderr, "Not a 32-bit ELF file\n");
    exit(1);
  }
  if (h.e_ident[EI_DATA] != ELFDATA2LSB) {
    fprintf(stderr, "Not a little-endian ELF file\n");
    exit(1);
  }
  if (h.e_type != ET_EXEC) {
    fprintf(stderr, "Not an ELF executable (ET_EXEC)\n");
    exit(1);
  }
  if (h.e_machine != EM_386) {
    fprintf(stderr, "Not an x86 (i386) ELF file\n");
    exit(1);
  }
}

int main(int argc, char **argv) {
  if (argc != 2) {
    printf("Usage: %s <ELF Executable> \n", argv[0]);
    exit(1);
  }

  check_elf(argv[1]);
  load_and_run_elf(argv[1]);
  loader_cleanup();
  return 0;
}
