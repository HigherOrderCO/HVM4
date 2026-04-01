// Parses an HVM source file using the CPU runtime and writes a binary dump
// of the HEAP and BOOK for loading by the CUDA evaluator.
//
// Build: clang -O2 -I../clang -o dump dump.c -lpthread
// Usage: ./dump <file.hvm> <output.bin>

#include "../clang/hvm.c"

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "Usage: %s <file.hvm> <output.bin>\n", argv[0]);
    return 1;
  }

  runtime_init(1, 0, 1, 0);

  char *src = sys_file_read(argv[1]);
  if (!src) {
    fprintf(stderr, "Error: could not open '%s'\n", argv[1]);
    return 1;
  }

  char *abs_path = realpath(argv[1], NULL);
  u32 main_id = 0;
  if (!runtime_prepare(&main_id, abs_path ? abs_path : argv[1], src)) {
    free(src);
    free(abs_path);
    runtime_free();
    return 1;
  }

  u64 heap_used = HEAP_NEXT_AT(0);

  u32 book_count = 0;
  for (u32 i = 0; i < BOOK_CAP; i++) {
    if (BOOK[i] != 0) book_count++;
  }

  FILE *f = fopen(argv[2], "wb");
  if (!f) {
    fprintf(stderr, "Error: could not create '%s'\n", argv[2]);
    return 1;
  }

  fwrite(&heap_used, sizeof(u64), 1, f);
  fwrite(&main_id, sizeof(u32), 1, f);
  fwrite(&book_count, sizeof(u32), 1, f);
  fwrite(HEAP, sizeof(u64), heap_used, f);
  for (u32 i = 0; i < BOOK_CAP; i++) {
    if (BOOK[i] != 0) {
      fwrite(&i, sizeof(u32), 1, f);
      u64 val = BOOK[i];
      fwrite(&val, sizeof(u64), 1, f);
    }
  }

  fclose(f);
  fprintf(stderr, "Dumped: heap_used=%llu main_id=%u book_entries=%u\n",
          (unsigned long long)heap_used, main_id, book_count);

  free(src);
  free(abs_path);
  runtime_free();
  return 0;
}
