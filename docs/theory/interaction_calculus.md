# Interaction Calculus

The Interaction Calculus is a model of computation that extends the Lambda
Calculus with **duplications** and **superpositions**, two core forms that
enable optimal lazy evaluation.

## Contents

- [Optimal Lazy Evaluation](#optimal-lazy-evaluation)
- [Duplications](#duplications)
- [Superpositions](#superpositions)
- [Duplicating Lambdas](#duplicating-lambdas)
- [Dup/Sup Labels](#dupsup-labels)
- [The Four Core Interactions](#the-four-core-interactions)
- [Relation to Interaction Combinators](#relation-to-interaction-combinators)
- [Complete Examples](#complete-examples)

## Optimal Lazy Evaluation

Programs can be evaluated **strictly** (compute everything immediately) or
**lazily** (compute only when needed). Lazy evaluation avoids unnecessary work,
but has a flaw: when a value is used twice, its computation might happen twice.
Haskell solves this with *memoization* (thunks), caching results so repeated
access doesn't recompute. This works for data, but breaks down inside lambdas.

The Interaction Calculus achieves **optimal sharing**: work is never duplicated,
even inside lambdas. Two dual forms make this possible:

- **Duplications**: allow a single value to exist in multiple locations
- **Superpositions**: allow multiple values to exist in a single location

These correspond to Lafont's "fan nodes" in Interaction Combinators (1997), with
opposite polarities. Together, they provide the mechanics for lazy cloning that
extends into lambda bodies.

## Duplications

Duplications allow a **single value** to exist in **two locations**. The
`! x &= v; t` construct duplicates `v`, making it available as `x₀` and `x₁`
in `t`:

```
! x &= 2;
(x₀ + x₁)
--------- DUP-NUM
(2 + 2)
--------- OP2-NUM
4
```

Here, the number `2` was duplicated, then added to itself.

Duplication is incremental - it happens layer by layer, on demand:

```
! x &= [1, 2, 3];
(x₀, x₁)
--------------------------------------- DUP-CTR, DUP-NUM
! t &= [2, 3];
(1 <> t₀, 1 <> t₁)
--------------------------------------- DUP-CTR, DUP-NUM
! t &= [3];
(1 <> 2 <> t₀, 1 <> 2 <> t₁)
--------------------------------------- DUP-CTR, DUP-NUM
! t &= [];
(1 <> 2 <> 3 <> t₀, 1 <> 2 <> 3 <> t₁)
--------------------------------------- DUP-CTR
([1, 2, 3], [1, 2, 3])
```

The list was cloned element by element. Each step peels off one layer -
duplicating the head and creating a duplication for the tail. The tail
duplication only triggers when both copies are accessed.

## Superpositions

Superpositions allow **two values** to exist in **one location**. The `&{a, b}`
construct creates a superposition of `a` and `b`:

```
(&{1, 2} + 10)
--------------------- OP2-SUP
! x &= 10;
&{(1 + x₀), (2 + x₁)}
--------------------- DUP-NUM
&{(1 + 10), (2 + 10)}
--------------------- OP2-NUM, OP2-NUM
&{11, 12}
```

Here, we added `10` to a superposition of `1` and `2`. The addition applied to
both values, producing a superposition of results. Notice that `10` had to be
duplicated; SUPs generate DUP nodes (DP0/DP1) as byproducts, and vice-versa.

Superpositions and duplications are duals. When a DUP meets a SUP, they
annihilate, extracting the two values:

```
! x &= &{1, 2};
(x₀ + x₁)
--------------- DUP-SUP
(1 + 2)
--------------- OP2-NUM
3
```

This is like a pair projection: `x₀` gets the first element, `x₁` gets the
second. SUPs and DUP nodes create and eliminate each other, just like LAMs and
APPs.

## Duplicating Lambdas

Lambdas are also duplicated incrementally. When that happens, the bound variable
becomes superposed, and temporarily escapes its scope:

```
! f &= λx.(x + 1);
(f₀(10), f₁(20))
------------------------------ DUP-LAM
! b &= (&{$x0, $x1} + 1);
((λ$x0.b₀)(10), (λ$x1.b₁)(20))
------------------------------ APP-LAM, APP-LAM
! b &= (&{10, 20} + 1);
(b₀, b₁)
------------------------------ OP2-SUP, DUP-NUM
! b &= &{(10 + 1), (20 + 1)};
(b₀, b₁)
------------------------------ OP2-NUM, OP2-NUM
! b &= &{11, 21};
(b₀, b₁)
------------------------------ DUP-SUP
(11, 21)
```

Notice how, on the first step, the variables `$x0` and `$x1` are bound *outside*
the lambda's body. This is why the Interaction Calculus needs globally scoped
variables. It's also what enables **optimal sharing inside lambdas**: the body
is now shared by `b`, so any computation there only happens once.

## Dup/Sup Labels

Consider this example:

```
(&A{1, 2} + &A{10, 20})
----------------------- OP2-SUP
! x &A= &A{10, 20};
&A{(1 + x₀), (2 + x₁)}
----------------------- DUP-SUP (same label: annihilate)
&A{(1 + 10), (2 + 20)}
----------------------- OP2-NUM, OP2-NUM
&A{11, 22}
```

Here, the superpositions *annihilated*: the first element paired with the first,
and the second with the second. But what if we wanted all combinations instead?

**Labels** control this behavior. When a DUP meets a SUP with the *same* label,
they annihilate. With *different* labels, they commute:

```
(&A{1, 2} + &B{10, 20})
-------------------------------------------------- OP2-SUP
! x &A= &B{10, 20};
&A{(1 + x₀), (2 + x₁)}
-------------------------------------------------- DUP-SUP (different labels: commute)
! a &A= 10;
! b &A= 20;
&A{(1 + &B{a₀, b₀}), (2 + &B{a₁, b₁})}
-------------------------------------------------- DUP-NUM, DUP-NUM
&A{(1 + &B{10, 20}), (2 + &B{10, 20})}
-------------------------------------------------- OP2-SUP, OP2-SUP
&A{&B{(1 + 10), (1 + 20)}, &B{(2 + 10), (2 + 20)}}
-------------------------------------------------- OP2-NUM (x4)
&A{&B{11, 21}, &B{12, 22}}
```

Now we get a nested superposition containing all four results. Labels let us
control whether superpositions collapse together or stay independent.

## The Four Core Interactions

The minimal Interaction Calculus has just four rules. Two create computation
(APP-LAM, DUP-SUP) and two propagate it (APP-SUP, DUP-LAM):

**APP-LAM** - Application eliminates lambda:

```
(λx.body)(arg)
-------------- APP-LAM
x ← arg
body
```

**DUP-SUP** - Duplication eliminates superposition (same label):

```
! x &L= &L{a, b}; t
------------------- DUP-SUP
x₀ ← a
x₁ ← b
t
```

**APP-SUP** - Application propagates through superposition:

```
(&L{a, b})(c)
------------------- APP-SUP
! x &L= c;
&L{a(x₀), b(x₁)}
```

**DUP-LAM** - Duplication propagates through lambda:

```
! f &L= λx.body; t
------------------ DUP-LAM
f₀ ← λ$X0.B₀
f₁ ← λ$X1.B₁
x  ← &L{$X0, $X1}
! B &L= body;
t
```

When labels differ, DUP-SUP commutes instead of annihilating:

```
! x &L= &R{a, b}; t
-------------------- DUP-SUP (L ≠ R)
! A &L= a;
! B &L= b;
x₀ ← &R{A₀, B₀}
x₁ ← &R{A₁, B₁}
t
```

These four rules form a complete system. Every other interaction in HVM is an
extension for practical constructs: numbers, constructors, pattern-matching.

## Relation to Interaction Combinators

The Interaction Calculus is similar to Interaction Combinators, the graph
rewriting model described by Lafont (1997). This similarity can be visualized as:

```
┌─────┬─────────────────────┬──────────────────┐
│     │     INTERACTION     │   INTERACTION    │
│ ITR │     COMBINATORS     │    CALCULUS      │
├─────┼─────────────────────┼──────────────────┤
│     │  ↓   a      ↓   a   │ (λx.f)(a)        │
│     │  |___|      |   |   │ --------         │
│ APP │   \ /        \ /    │ x ← a            │
│  X  │    |    =>    X     │ f                │
│ LAM │   / \        / \    │                  │
│     │  |‾‾‾|      |   |   │                  │
│     │  x   f      x   f   │                  │
├─────┼─────────────────────┼──────────────────┤
│     │  ↓   x      ↓   x   │ (&L{a,b} x)      │
│     │  |___|      |   |   │ -----------      │
│ APP │   \ /      /L\ /L\  │ ! X &L= x        │
│  X  │    |   =>  |_ X _|  │ &L{a(X₀),b(X₁)}  │
│ SUP │   /L\      \ / \ /  │                  │
│     │  |‾‾‾|      |   |   │                  │
│     │  a   b      a   b   │                  │
├─────┼─────────────────────┼──────────────────┤
│     │  F₁  F₀     F₁  F₀  │ ! F &L= λx.g     │
│     │  |___|      |   |   │ ------------     │
│ DUP │   \L/      /_\ /_\  │ F₀ ← λ$y.G₀      │
│  X  │    |   =>  |_ X _|  │ F₁ ← λ$z.G₁      │
│ LAM │   / \      \L/ \L/  │ x  ← &L{$y,$z}   │
│     │  |‾‾‾|      |   |   │ ! G &L= g        │
│     │  x   g      x   g   │                  │
├─────┼─────────────────────┼──────────────────┤
│     │  S₁  S₀     S₁  S₀  │ ! S &L= &L{a,b}  │
│     │  |___|      |   |   │ ---------------  │
│ DUP │   \L/        \ /    │ S₀ ← a           │
│  X  │    |    =>    X     │ S₁ ← b           │
│ SUP │   /L\        / \    │ t                │
│     │  |‾‾‾|      |   |   │                  │
│     │  a   b      a   b   │                  │
└─────┴─────────────────────┴──────────────────┘
```

It can also be seen as a completion of the λ-Calculus, giving a computational
meaning to previously undefined expressions: applying a pair and projecting a
lambda. The Interaction Calculus provides sensible reduction rules for these,
inspired by its Interaction Combinator equivalence: applying a pair
(superposition) distributes over both elements (APP-SUP), and duplicating a
lambda creates two lambdas with a superposed bound variable (DUP-LAM). This
makes every possible interaction well-defined.

## Complete Examples

Here's an example demonstrating optimal sharing. We duplicate a lambda
containing `(2 + 2)` and apply the copies to different arguments:

```
! F &= (λx. λy. ! z &= x; ((z₀ + z₁), y) 2);
(F₀(10), F₁(20))
---------------------------------------------- APP-LAM
! z &= 2;
! F &= λy.((z₀ + z₁), y);
(F₀(10), F₁(20))
---------------------------------------------- DUP-NUM
! F &= λy.((2 + 2), y);
(F₀(10), F₁(20))
---------------------------------------------- DUP-LAM
! B &= ((2 + 2), &{y0, y1});
((λy0.B₀ 10), (λy1.B₁ 20))
---------------------------------------------- APP-LAM (x2)
! B &= ((2 + 2), &{10, 20});
(B₀, B₁)
---------------------------------------------- DUP-CTR
! H &= (2 + 2);
! T &= &{10, 20};
((H₀, T₀), (H₁, T₁))
---------------------------------------------- OP2-NUM (shared!)
! H &= 4;
! T &= &{10, 20};
((H₀, T₀), (H₁, T₁))
---------------------------------------------- DUP-NUM, DUP-SUP
((4, 10), (4, 20))
```

Notice that `(2 + 2)` was computed only **once**, even though the lambda was
duplicated and each copy was applied to a different argument. The result `4`
flowed to both copies through the DUP-NUM interaction.

### Church 2²

Here's a more complex example - computing 2² using Church numerals:

```
(λf. ! F &L= f; λx.(F₀ (F₁ x)) λg. ! G &K= g; λy.(G₀ (G₁ y)))
------------------------------------------------------------- APP-LAM
! F &L= λg. ! G &K= g; λy.(G₀ (G₁ y));
λx.(F₀ (F₁ x))
------------------------------------------------------------- DUP-LAM
! G &K= &L{g0, g1};
! F &L= λy.(G₀ (G₁ y));
λx.((λg0.F₀) (λg1.F₁ x))
------------------------------------------------------------- APP-LAM
! G &K= &L{(λg1.F₁ x), g1};
! F &L= λy.(G₀ (G₁ y));
λx.F₀
------------------------------------------------------------- DUP-LAM
! G &K= &L{(λg1.λy1.F₁ x), g1};
! F &L= (G₀ (G₁ &L{y0, y1}));
λx.λy0.F₀
------------------------------------------------------------- DUP-SUP (L ≠ K)
! A &K= (λg1.λy1.F₁ x);
! B &K= g1;
! F &L= (&L{A₀, B₀} (&L{A₁, B₁} &L{y0, y1}));
λx.λy0.F₀
------------------------------------------------------------- APP-SUP
! A &K= (λg1.λy1.F₁ x);
! B &K= g1;
! U &L= (&L{A₁, B₁} &L{y0, y1});
! F &L= &L{(A₀ U₀), (B₀ U₁)};
λx.λy0.F₀
------------------------------------------------------------- DUP-SUP (L = L)
! A &K= (λg1.λy1.(B₀ U₁) x);
! B &K= g1;
! U &L= (&L{A₁, B₁} &L{y0, y1});
λx.λy0.(A₀ U₀)
------------------------------------------------------------- APP-LAM
! A &K= λy1.(B₀ U₁);
! B &K= x;
! U &L= (&L{A₁, B₁} &L{y0, y1});
λx.λy0.(A₀ U₀)
------------------------------------------------------------- DUP-LAM
! A &K= (B₀ U₁);
! B &K= x;
! U &L= (&L{λy11.A₁, B₁} &L{y0, &K{y10, y11}});
λx.λy0.((λy10.A₀) U₀)
------------------------------------------------------------- APP-LAM
! A &K= (B₀ U₁);
! B &K= x;
! U &L= (&L{λy11.A₁, B₁} &L{y0, &K{U₀, y11}});
λx.λy0.A₀
------------------------------------------------------------- APP-SUP
! A &K= (B₀ U₁);
! B &K= x;
! V &L= &L{y0, &K{U₀, y11}};
! U &L= &L{((λy11.A₁) V₀), (B₁ V₁)};
λx.λy0.A₀
------------------------------------------------------------- DUP-SUP (L = L), APP-LAM
! A &K= (B₀ U₁);
! B &K= x;
! U &L= &L{(A₁ y0), (B₁ &K{U₀, y11})};
λx.λy0.A₀
------------------------------------------------------------- DUP-SUP (L = L)
! A &K= (B₀ (B₁ &K{(A₁ y0), y11}));
! B &K= x;
λx.λy0.A₀
```

At this point, the computation is done. The result is a lambda `λx.λy0.A₀`
connected to a small network of nodes. This compressed form represents the
answer but is hard to read directly. To see the familiar Church numeral 4, we
can **collapse** the Interaction Calculus term back into a proper λ-Term, by
applying 2 extra interactions, DUP-VAR and DUP-APP:

```
...
--------------------------------- DUP-VAR
! A &K= (x (x &K{(A₁ y0), y11}));
λx.λy0.A₀
--------------------------------- DUP-APP, DUP-VAR
! X &K= (x &K{((x X₁) y0), y11});
λx.λy0.(x X₀)
--------------------------------- DUP-APP, DUP-VAR
! Y &K= &K{((x (x Y₁)) y0), y11};
λx.λy0.(x (x Y₀))
--------------------------------- DUP-SUP (K = K)
λx.λy0.(x (x ((x (x y11)) y0)))
--------------------------------- APP-LAM (x2)
λx.λy0.(x (x (x (x y0))))
```

The Church numeral 2 (`λf.λx.(f (f x))`) was applied to itself, yielding 4
(`λf.λx.(f (f (f (f x))))`). Despite temporarily escaping variables, the system
correctly computes the result. This is the kind of symbolic computation where
sharing matters most, and is often used as a benchmark in the literature. HVM
completes it in 14 interactions, which is optimal.
