// HVM4 CLI Entry Point
// ====================
//
// This file provides the command-line interface for the HVM4 runtime,
// mirroring the structure of main.hs for the Haskell implementation.
//
// Usage: ./main <file.hvm4> [-s] [-S] [-D] [-C[N]] [-T<N>]
//   -s:  Show statistics (interactions, time, performance)
//   -S:  Silent output (omit term printing)
//   -D:  Step-by-step reduction (print intermediate terms)
//   -C:  Collapse and flatten (enumerate all superposition branches)
//   -CN: Collapse and flatten, limit to N results
//   -T:  Use N threads (e.g. -T4)
//   --to-c: Emit standalone AOT C program to stdout
//   --compile <file>: Emit + compile standalone AOT executable
//   --jit: Emit + compile + run standalone AOT executable once

#include "hvm4.c"

// CLI
// ===

#define FFI_MAX 128

typedef struct {
  int        is_dir;
  const char *path;
} FfiLoad;

typedef struct {
  int   stats;
  int   silent;
  int   do_collapse;
  int   collapse_limit;  // -1 means no limit
  int   debug;
  int   step_by_step;
  int   threads;
  int   jit;
  int   to_c;
  const char *compile_out;
  u32     ffi_loads_len;
  FfiLoad ffi_loads[FFI_MAX];
  char *file;
} CliOpts;

// Parses one option value from either --flag=value or --flag value.
fn const char *parse_opt_value(int argc, char **argv, int *i, const char *flag, int prefix_len) {
  if (argv[*i][prefix_len - 1] == '=') {
    return argv[*i] + prefix_len;
  }
  if (*i + 1 >= argc) {
    fprintf(stderr, "Error: missing value after %s\n", flag);
    exit(1);
  }
  return argv[++(*i)];
}

fn CliOpts parse_opts(int argc, char **argv) {
  CliOpts opts = {
    .stats = 0,
    .silent = 0,
    .do_collapse = 0,
    .collapse_limit = -1,
    .debug = 0,
    .step_by_step = 0,
    .threads = 0,
    .jit = 0,
    .to_c = 0,
    .compile_out = NULL,
    .ffi_loads_len = 0,
    .file = NULL
  };

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-s") == 0) {
      opts.stats = 1;
    } else if (strcmp(argv[i], "-S") == 0) {
      opts.silent = 1;
    } else if (strncmp(argv[i], "-C", 2) == 0) {
      opts.do_collapse = 1;
      if (argv[i][2] != '\0') {
        opts.collapse_limit = atoi(&argv[i][2]);
      }
    } else if (strncmp(argv[i], "-T", 2) == 0) {
      const char *num = argv[i] + 2;
      if (num[0] == '\0') {
        if (i + 1 >= argc) {
          fprintf(stderr, "Error: missing thread count after -T\n");
          exit(1);
        }
        num = argv[++i];
      }
      opts.threads = atoi(num);
      if (opts.threads > MAX_THREADS) {
        fprintf(stderr, "Error: -T value (%d) exceeds MAX_THREADS (%d)\n", opts.threads, MAX_THREADS);
        exit(1);
      }
    } else if (strcmp(argv[i], "-d") == 0) {
      opts.debug = 1;
    } else if (strcmp(argv[i], "-D") == 0) {
      opts.step_by_step = 1;
    } else if (strcmp(argv[i], "--to-c") == 0) {
      opts.to_c = 1;
    } else if (strncmp(argv[i], "--to-c=", 7) == 0) {
      fprintf(stderr, "Error: --to-c does not take a path; it writes C to stdout\n");
      exit(1);
    } else if (strcmp(argv[i], "--compile") == 0 || strncmp(argv[i], "--compile=", 10) == 0) {
      opts.compile_out = parse_opt_value(argc, argv, &i, "--compile", 10);
    } else if (strcmp(argv[i], "-o") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "Error: missing output path after -o\n");
        exit(1);
      }
      opts.compile_out = argv[++i];
    } else if (strcmp(argv[i], "--jit") == 0) {
      opts.jit = 1;
    } else if (strcmp(argv[i], "--ffi") == 0 || strncmp(argv[i], "--ffi=", 6) == 0) {
      const char *path = NULL;
      if (argv[i][5] == '=') {
        path = argv[i] + 6;
      } else {
        if (i + 1 >= argc) {
          fprintf(stderr, "Error: missing path after --ffi\n");
          exit(1);
        }
        path = argv[++i];
      }
      if (opts.ffi_loads_len >= FFI_MAX) {
        fprintf(stderr, "Error: too many --ffi arguments (max %d)\n", FFI_MAX);
        exit(1);
      }
      opts.ffi_loads[opts.ffi_loads_len++] = (FfiLoad){.is_dir = 0, .path = path};
    } else if (strcmp(argv[i], "--ffi-dir") == 0 || strncmp(argv[i], "--ffi-dir=", 10) == 0) {
      const char *path = NULL;
      if (argv[i][9] == '=') {
        path = argv[i] + 10;
      } else {
        if (i + 1 >= argc) {
          fprintf(stderr, "Error: missing path after --ffi-dir\n");
          exit(1);
        }
        path = argv[++i];
      }
      if (opts.ffi_loads_len >= FFI_MAX) {
        fprintf(stderr, "Error: too many FFI loads (max %d)\n", FFI_MAX);
        exit(1);
      }
      opts.ffi_loads[opts.ffi_loads_len++] = (FfiLoad){.is_dir = 1, .path = path};
    } else if (argv[i][0] != '-') {
      if (opts.file == NULL) {
        opts.file = argv[i];
      } else {
        fprintf(stderr, "Error: multiple input files specified\n");
        exit(1);
      }
    } else {
      fprintf(stderr, "Unknown option: %s\n", argv[i]);
      exit(1);
    }
  }

  return opts;
}

// Main
// ====

int main(int argc, char **argv) {
  // Parse command line
  CliOpts opts = parse_opts(argc, argv);

  if (opts.file == NULL) {
    fprintf(stderr, "Usage: ./main <file.hvm4> [-s] [-S] [-D] [-C[N]] [-T<N>] [--to-c] [--compile <file>] [--jit] [--ffi <path>] [--ffi-dir <path>]\n");
    return 1;
  }

  if (opts.step_by_step && opts.do_collapse) {
    fprintf(stderr, "Error: -D is not supported with -C\n");
    return 1;
  }

  int build_modes = (opts.jit ? 1 : 0) + (opts.to_c ? 1 : 0) + (opts.compile_out != NULL ? 1 : 0);
  if (build_modes > 1) {
    fprintf(stderr, "Error: choose only one build mode: --jit, --compile, or --to-c\n");
    return 1;
  }

  // Configure threads (default: 1)
  u32 threads = opts.threads > 0 ? (u32)opts.threads : 1;
  if (opts.step_by_step && threads > 1) {
    fprintf(stderr, "Warning: -D forces single-threaded mode\n");
    threads = 1;
  }
  eval_runtime_init(threads, opts.debug, opts.silent, opts.step_by_step);

  // Load FFI libraries before parsing (needed for arity checks and overrides).
  for (u32 i = 0; i < opts.ffi_loads_len; i++) {
    if (opts.ffi_loads[i].is_dir) {
      ffi_load_dir(opts.ffi_loads[i].path);
    } else {
      ffi_load(opts.ffi_loads[i].path);
    }
  }

  // Read and parse user file
  char *src = sys_file_read(opts.file);
  if (!src) {
    fprintf(stderr, "Error: could not open '%s'\n", opts.file);
    return 1;
  }

  // Parse file source
  char *abs_path = realpath(opts.file, NULL);
  const char *src_path = abs_path ? abs_path : opts.file;
  eval_parse_source(src_path, src);
  if (!eval_check_alo_space()) {
    free(src);
    free(abs_path);
    eval_runtime_free();
    return 1;
  }

  // Check @main exists
  u32 main_id = 0;
  if (!eval_get_main_id(&main_id)) {
    fprintf(stderr, "Error: @main not defined\n");
    free(src);
    free(abs_path);
    eval_runtime_free();
    return 1;
  }

  // Build-only modes (AOT emission/compilation) return early.
  if (opts.to_c) {
    aot_build_to_c(argv[0], src_path, src);
    free(src);
    free(abs_path);
    eval_runtime_free();
    return 0;
  }
  if (opts.compile_out != NULL) {
    aot_build_compile_out(opts.compile_out, argv[0], src_path, src);
    free(src);
    free(abs_path);
    eval_runtime_free();
    return 0;
  }
  if (opts.jit) {
    int rc = aot_build_jit_once(argv[0], src_path, src);
    free(src);
    free(abs_path);
    eval_runtime_free();
    return rc;
  }

  free(src);

  // Evaluate
  struct timespec start, end;
  clock_gettime(CLOCK_MONOTONIC, &start);

  Term main_ref = term_new_ref(main_id);

  if (opts.do_collapse) {
    // Lazy collapse + flatten: handles infinite structures
    eval_collapse(main_ref, opts.collapse_limit, opts.stats, opts.silent);
  } else {
    // Standard evaluation to strong normal form
    Term result = eval_normalize(main_ref);
    if (!opts.silent && !opts.step_by_step) {
      print_term(result);
      printf("\n");
    }
  }

  clock_gettime(CLOCK_MONOTONIC, &end);

  // Print stats if requested
  u64 total_itrs = wnf_itrs_total();
  if (opts.stats) {
    double dt  = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    double ips = total_itrs / dt;
    u64 total_heap = heap_alloc_total();
    printf("- Itrs: %llu interactions\n", total_itrs);
    if (thread_get_count() > 1) {
      for (u32 t = 0; t < thread_get_count(); t++) {
        printf("- Itrs[%u]: %llu interactions\n", t, wnf_itrs_thread(t));
      }
    }
    printf("- Heap: %llu nodes\n", total_heap);
    printf("- Time: %.3f seconds\n", dt);
    printf("- Perf: %.2f M interactions/s\n", ips / 1e6);
  } else if (opts.silent) {
    printf("- Itrs: %llu interactions\n", total_itrs);
  }

  // Cleanup
  free(abs_path);
  eval_runtime_free();

  return 0;
}
