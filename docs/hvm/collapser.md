# Collapser (CNF Readback)

Interaction Calculus is affine and uses explicit duplication (DUP) and
superposition (SUP). That is great for optimal sharing but hard for humans to
read. The collapser is the readback procedure: it turns one IC term into a
stream of ordinary lambda terms, i.e., collapsed normal form (CNF).

Two insights make this possible:
- Quoting removes DUPs. When a branch is ready to print, we run `cnf(term)`.
  Linked vars become quoted vars (BJV/BJ0/BJ1).
  DUP interactions are defined on quoted dup vars, so DUP nodes are cloned away
  and disappear from the printed term.
- Lifting removes SUPs. We lift the first SUP to the top and enumerate its
  branches. Same-label SUPs annihilate pairwise; different-label SUPs commute and
  create a cross product of branches.

## Algorithm (as implemented)

- `cnf` (`src/hvm.c`, CNF section): reduce to WNF, then lift the first SUP to the top and
  return immediately. ERA propagates upward; INC is left in place for the
  flattener.
- `eval_collapse` (`src/hvm.c`, Eval section): breadth-first traversal with a
  sequential priority queue. Lower numeric keys are popped first; SUP increases
  key, INC decreases key. Branches with the same key are popped FIFO for
  deterministic ordering. When a branch has no SUP, it prints `cnf(term)`.

## Label Behavior (pairwise vs cross product)

Different labels commute (cross product):

```
[&A{1,2}, &B{3,4}]
```

```
[1,3]
[1,4]
[2,3]
[2,4]
```

Same labels annihilate pairwise:

```
[&A{1,2}, &A{3,4}]
```

```
[1,2]
[3,4]
```

## Where To Look

- `src/hvm.c`, `CNF` section: SUP lifting rules.
- `src/hvm.c`, `Eval` section: branch enumeration + SNF quoting for output.
