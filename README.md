# HVM

HVM is a high-performance runtime for the [Interaction Calculus](docs/theory/interaction_calculus.md).

**NOTE: you're here before launch. Use at your own risk.**

## Building and Running

```bash
# Build
clang -O2 -o src/hvm src/hvm.c

# Run a file (use collapse mode by default)
hvm devs/test/file.hvm -s -C10

# Run all tests
./devs/test/_all_.sh

# Run a benchmark file
hvm devs/bench/u32_fib.hvm -s
```

Flags:
- `-s` shows performance stats
- `-D` prints each intermediate reduction step with interaction labels
- `-C10` collapses and flattens superpositions (limit to 10 lines)

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
- Core language: [docs/hvm/core.md](docs/hvm/core.md)
- Memory layout: [docs/hvm/memory.md](docs/hvm/memory.md)
- Interaction rules: [docs/hvm/interactions/](docs/hvm/interactions/)
