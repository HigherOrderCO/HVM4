# GC Soundness: Why Ref Counting is Complete for HVM4

This document proves that reference counting alone is sufficient for complete garbage collection in HVM4, without requiring cycle detection or tracing GC.

## Core Claim

**Theorem**: The HVM4 heap is always a DAG (Directed Acyclic Graph). Therefore, reference counting is complete — every unreachable node will eventually have refcount 0.

## Background

Traditional ref counting fails on cycles:
```
A → B → A   // Both have refcount=1 forever, leaked
```

Tracing GC solves this by periodically walking the entire heap to find unreachable cycles. This introduces pause times and complexity.

HVM4 avoids this entirely: **cycles are structurally impossible**.

## Proof

### Lemma 1: Allocation Order

Every node in the HVM4 heap is allocated at a monotonically increasing address (or timestamp). Call this the node's *birth time* `t(n)`.

### Lemma 2: Reference Direction

When node A references node B, we have `t(B) < t(A)`. In other words, **nodes can only reference previously-allocated nodes**.

*Proof*: In IC reduction:
- `@name` references a statically-defined node (birth time 0)
- Lambda application `f(x)` creates a new node referencing existing `f` and `x`
- DUP creates a SUP node referencing the original (older) node
- No operation creates a reference to a "future" node

### Lemma 3: No Self-Reference

A node cannot reference itself: `t(A) < t(A)` is a contradiction.

### Theorem: DAG Property

**Proof by contradiction**: Assume a cycle exists: `A₁ → A₂ → ... → Aₙ → A₁`

By Lemma 2:
- `t(A₁) > t(A₂)` (A₁ references A₂)
- `t(A₂) > t(A₃)`
- ...
- `t(Aₙ) > t(A₁)`

Chaining these: `t(A₁) > t(A₂) > ... > t(Aₙ) > t(A₁)`

This implies `t(A₁) > t(A₁)`, a contradiction. ∎

### Corollary: Ref Counting is Complete

In a DAG:
1. If a node is unreachable from roots, there exists a topological ordering where it can be freed
2. When a node's refcount hits 0, all nodes it references can have their refcounts decremented
3. This cascades through the DAG until all unreachable nodes are freed

No cycle can "protect" unreachable nodes from collection.

## What About Recursion?

Recursive definitions like the Y combinator don't create heap cycles:

```hvm4
@Y = λ&f. f(@Y(f))
```

Each recursive call allocates a *new* thunk:
```
t=0: Y defined
t=1: @Y(f) called → new thunk T₁ referencing f (t < 1)
t=2: T₁ reduces, calls @Y(f) → new thunk T₂ referencing f
...
```

The chain `T₁ → f`, `T₂ → f`, etc. forms a tree (or DAG), not a cycle. The "infinite recursion" is infinite *unfolding*, not circular reference.

## What About DUP/SUP?

Duplication creates explicit sharing via superposition:

```hvm4
!&x = expensive_computation;
[x, x]  // x used twice
```

This creates:
```
SUP_node → expensive_computation
result_list → SUP_node (twice)
```

The SUP node references the *original* computation (older). When both uses of `x` are consumed, SUP's refcount drops to 0, then the original's refcount decrements.

## Epoch Allocator

The epoch-based allocator leverages this guarantee:

1. **Epoch N**: Allocate nodes freely
2. **Epoch N+1**: Any node from epoch N with refcount=0 is bulk-freed

No scanning, no marking, no tracing. Just batched refcount checks.

## FFI Considerations

The DAG guarantee holds for pure HVM4 code. External FFI with mutable state requires care:
- FFI-allocated objects should be wrapped with explicit ref management
- Or use epoch pinning to prevent premature collection

## Conclusion

HVM4's interaction combinator semantics structurally guarantee a DAG heap. This is not a runtime property to be checked — it's an invariant maintained by the reduction rules themselves.

**Reference counting + epoch batching = complete, pauseless GC.**

## References

- Lamping, J. (1990). An algorithm for optimal lambda calculus reduction
- Asperti, A., & Guerrini, S. (1998). The optimal implementation of functional programming languages
- Levy, J. J. (1980). Optimal reductions in the lambda calculus
