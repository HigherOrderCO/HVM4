# AGENTS.md

## Quick Onboarding

HVM is a runtime for the Interaction Calculus (IC), a lambda-calculus extension with
explicit duplication (DUP) and superposition (SUP). These forms enable optimal
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

```bash
# Build
clang -O2 -o src/hvm src/hvm.c

# Run a file
hvm devs/test/file.hvm -s -C10

# Run tests
./devs/test/_all_.sh

# Run a benchmark file
hvm devs/bench/u32_fib.hvm -s
```

## Docs Map

- `README.md`: entry point, build/run examples, links.
- `docs/primer.md`: quick intro to the language and runtime usage.
- `docs/theory/interaction_calculus.md`: IC theory + examples.
- `docs/hvm/core.md`: core term AST and grammar.
- `docs/hvm/syntax.md`: parser syntax, precedence, and desugaring rules.
- `docs/hvm/memory.md`: term layout, heap representation, linked/quoted terms.
- `docs/hvm/collapser.md`: CNF readback and collapse algorithm.
- `docs/hvm/interactions/*.md`: one file per WNF interaction; mirrors the sequent
  calculus comments in the WNF section of `src/hvm.c`.

## Code Map (C Runtime)

`src/hvm.c` is the only C runtime source. It is organized as broad sections,
inspired by Bend's `Core.ts`:

- `Types`: scalar aliases, term tags, parser/runtime structs, globals.
- `Term`: term packing, tag/ext/val helpers, constructors, clone helpers, OP2.
- `Heap`: allocation, heap access, substitutions, and GC.
- `Nick`, `System`, `Table`, `Print`: names, file/path helpers, intern table,
  and dynamic/static pretty-printing.
- `Runtime`: process setup, program preparation, entry lookup, and @main eval.
- `Parse`: lexer, bindings, syntax sugar, includes, and top-level definitions.
- `WNF`: stack evaluator and interaction rules.
- `Data`: small runtime data structures used by evaluation.
- `CNF`: one-step collapsed normal form readback and SUP lifting.
- `Eval`: SNF traversal and CNF branch enumeration.
- `CLI`: option parsing and program entry point.

There is no external call layer. HVM now runs pure programs and prints the result
of evaluating `@main`.
