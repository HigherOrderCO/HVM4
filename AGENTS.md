# AGENTS.md

## Quick Onboarding

HVM is a runtime for the Interaction Calculus (IC), a lambda-calculus extension with
explicit duplication (DUP) and superposition (SUP). These primitives enable optimal
sharing for lazy evaluation, even inside lambdas. This repo is the C runtime:
parse source -> build static book terms -> lazily allocate dynamic heap terms ->
reduce with WNF/SNF interactions -> print results.

Key terms:
- static/book term: immutable definition stored in the book (de Bruijn levels).
- dynamic term: mutable heap term used during evaluation (linked by pointers).
- WNF: weak normal form (head reduction with interactions).
- SNF: strong normal form (full reduction).
- CNF: collapsed normal form (full lambda-calculus readback).
- interaction: a rewrite rule (APP-LAM, DUP-SUP, etc.).

## Build and Test

### Unix/Linux/macOS

```bash
# Build
cd clang && clang -O2 -o main main.c

# Run a file
./clang/main test/file.hvm -s -C10

# Run all tests
./test/_all_.sh
```

### Windows

```powershell
# Build with Clang (recommended)
cd clang
clang -O2 -o main.exe main.c

# Or build with MSVC
cd clang
cl /O2 /Fe:main.exe main.c

# Run a file
.\clang\main.exe test\file.hvm -s -C10

# Run all tests
.\scripts\test.ps1

# Run interpreted tests only (skip AOT compilation tests)
.\scripts\test.ps1 -InterpretedOnly
```

## Docs Map

- `README.md`: entry point, build/run examples, links.
- `STYLEGUIDE.md`: authoritative C style rules for `clang/` (mirrored in `clang/STYLE.md`).
- `docs/primer.md`: quick intro to the language and runtime usage.
- `docs/theory/interaction_calculus.md`: IC theory + examples.
- `docs/hvm/core.md`: core term AST and grammar.
- `docs/hvm/syntax.md`: parser syntax, precedence, and desugaring rules.
- `docs/hvm/memory.md`: term layout, heap representation, linked/quoted terms.
- `docs/hvm/collapser.md`: CNF readback and collapse algorithm.
- `docs/hvm/interactions/*.md`: one file per WNF interaction; mirrors the sequent
  calculus comment in the matching `clang/wnf/<name>.c`.

## Code Map (C Runtime)

### Top-Level Entry
- `clang/hvm.c`: single translation unit; defines tags/bit layout/globals and
  includes every module in build order. Start here to understand the whole runtime.
- `clang/main.c`: CLI entry point and runtime setup.

### Term Representation
- `clang/term/new.c`: pack a term word from fields.
- `clang/term/tag.c`: extract tag.
- `clang/term/ext.c`: extract ext.
- `clang/term/val.c`: extract val.
- `clang/term/arity.c`: arity per tag.
- `clang/term/clone.c`: duplication helper.
- `clang/term/sub/get.c`: read SUB bit.
- `clang/term/sub/set.c`: set/clear SUB bit.
- `clang/term/new/*.c`: constructors for each tag; `clang/term/new/_.c` allocates
  heap nodes.

### Parser
- `clang/parse/*.c`: lexer utilities, binding stack, and definition parsing.
- `clang/parse/term/*.c`: term parsers; `clang/parse/term/_.c` dispatches.

### Evaluation
- `clang/wnf/_.c`: stack-based WNF evaluator and interaction dispatch.
- `clang/wnf/*.c`: one interaction per file; see matching doc in
  `docs/hvm/interactions/`.
- `clang/eval/normalize.c`: SNF normalization (WNFs every reachable node).

### Collapse (CNF Readback)
- `clang/cnf/_.c`: lift one SUP to the top, without recursing into branches.
- `clang/eval/collapse.c`: BFS enumeration of SUP branches; prints quoted SNF.
- `clang/data/pq.c`: priority queue for collapse ordering (INC affects priority).
- `clang/data/wspq.c`: work-stealing priority queue for parallel collapse.

### Printing and Names
- `clang/print/term.c`: term pretty-printer (dynamic/static modes).
- `clang/print/name.c`: alpha name generation.
- `clang/print/utf8.c`: UTF-8 printing helpers.
- `clang/nick/*.c`: base64-ish name encoding/decoding utilities.
- `clang/table/*.c`: global name table (id <-> string) with lookup helpers.

### Heap and System
- `clang/heap/alloc.c`: heap allocation helpers.
- `clang/heap/subst_var.c`: install lam substitutions.
- `clang/heap/subst_cop.c`: install dup substitutions.
- `clang/prim/init.c`: register built-in primitives at startup.
- `clang/prim/register.c`: primitive registration helpers.
- `clang/prim/fn/*.c`: primitive implementations.
- `clang/thread/get_count.c`: read worker thread count.
- `clang/thread/set_count.c`: set worker thread count.
- `clang/sys/error.c`: error formatting.
- `clang/sys/file_read.c`: file read utility.
- `clang/sys/path_join.c`: path joining.
- `clang/prelude/_.c`: prelude stub (empty).

## Naming Rule (Critical)

The file path is the function name: replace `/` with `_`, drop `.c`. Example:
`wnf/app_lam.c` defines `wnf_app_lam()`. `_.c` represents the directory root.
