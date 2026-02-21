# HVM4

HVM4 is a high-performance runtime for the [Interaction Calculus](docs/theory/interaction_calculus.md).

**NOTE: you're here before launch. Use at your own risk.**

## Building and Running

```bash
# Build
cd clang && clang -O2 -o main main.c
# On Linux, add: -ldl

# Run a file (use collapse mode by default)
./clang/main test/file.hvm4 -s -C10

# Run all tests
./test/_all_.sh
```

Flags:
- `-s` shows performance stats
- `-D` prints each intermediate reduction step with interaction labels
- `-C10` collapses and flattens superpositions (limit to 10 lines)
- `--to-c` emits a standalone AOT C program to stdout
- `--compile <file>` emits + compiles a standalone executable
- `--jit` emits + compiles + runs a standalone executable once (keeps `.c`, deletes temp executable)
- `--ffi <path>` loads one FFI shared library before parsing
- `--ffi-dir <path>` loads all FFI shared libraries in a directory before parsing

## Examples

```hvm
@main = ((@add 1) 2)
//3
```

```hvm
@main = (&{1, 2} + 10)
//11
//12
```

```hvm
@main = (! x &A= 3; (x₀ + x₁))
//6
```

## Documentation

- Theory: [docs/theory/interaction_calculus.md](docs/theory/interaction_calculus.md)
- Core language: [docs/hvm4/core.md](docs/hvm4/core.md)
- Memory layout: [docs/hvm4/memory.md](docs/hvm4/memory.md)
- Interaction rules: [docs/hvm4/interactions/](docs/hvm4/interactions/)
