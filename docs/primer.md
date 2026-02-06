# HVM4 Primer

HVM4 is a runtime for the Interaction Calculus, a model of computation that
extends lambda calculus to achieve **optimal reduction**: no work is ever
duplicated, even when copying functions. It's lazy like Haskell, but sharing
extends inside lambdas, not just at the data level.

## Running HVM4

```bash
cd clang && clang -O2 -o main main.c   # build
./clang/main file.hvm4 -s -C10         # run with collapse
```

**Flags:**
- `-s` : show performance stats
- `-D` : print intermediate reduction steps
- `-C10` : collapse superpositions (limit output to 10 lines)

**Modes:**
- *Normalize*: reduce to SNF. Keeps SUPs and DUPs in output.
- *Collapse*: eliminate SUPs and DUPs, enumerate as pure lambda terms.

## Grammar

```
File     ::= (Include | Def)*
Include  ::= "#include" '"' Path '"'
Def      ::= "@" Name "=" Term

Term ::=
  | Name                                        -- variable
  | Name "₀" | Name "₁"                         -- dup variable (first/second)
  | Integer                                     -- number literal
  | "@" Name                                    -- reference to definition
  | "*"                                         -- wildcard (erased)

  -- Lambda & Application
  | "λ" Params "." Term                         -- lambda: λx.body
  | "λ" "&" Name "." Term                       -- cloned lambda: λ&x.body
  | "λ" Name "&" Label "." Term                 -- lambda with inline dup
  | Term "(" Args ")"                           -- application: f(x,y)

  -- Pattern Matching
  | "λ" "{" Cases "}"                           -- match lambda
  | "λ" "{" Term "}"                            -- use (strict unbox)

  -- Duplication & Superposition
  | "!" Name "&" Label "=" Term ";" Term        -- dup: !x&L=val;body
  | "!" Name "&" "(" Term ")" "=" Term ";" Term -- dynamic dup
  | "&" Label "{" Term "," Term "}"             -- superposition: &L{a,b}
  | "&" "(" Term ")" "{" Term "," Term "}"      -- dynamic superposition
  | "&" "{" "}"                                 -- erasure

  -- Let Bindings (sugar)
  | "!" Name "=" Term ";" Term                  -- let: !x=val;body
  | "!" "&" Name "=" Term ";" Term              -- cloned let
  | "!!" Name "=" Term ";" Term                 -- strict let

  -- Constructors
  | "#" Name "{" Args "}"                       -- constructor: #Pair{a,b}
  | "#" Name "{" "}"                            -- nullary: #Zer{} or #True{}

  -- Operators
  | Term Op Term                                -- binary: (a + b), (x == y)
  | Term "===" Term                             -- structural equality
  | Term ".&." Term                             -- short-circuit AND
  | Term ".|." Term                             -- short-circuit OR

  -- Special
  | "↑" Term                                    -- priority (collapse order)
  | Term "~>" Term                              -- guarded reduction
  | "&" Label "λ" Names "{" Term ";" Term "}"   -- fork
  | "[" Args "]"                                -- list: [a,b,c]
  | '"' Chars '"'                               -- string
  | Integer "n"                                 -- nat: 2n -> #Suc{#Suc{#Zer}}

Cases    ::= (Pattern ":" Term ";")* [Default]
Pattern  ::= "#" Name | Integer | "[]" | "<>" | Integer "n" | Integer "n+"

Operators (low to high): || && == != < <= > >= << >> + - * / % ^
```

## Key Concepts

### Affine Variables

Variables are **affine**: used at most once.

```hvm4
// WRONG: x used twice
@bad = λx. (x + x)

// RIGHT: use cloned binder
@square = λ&x. (x * x)
@main = @square(5)
//25
```

The `&` prefix tells the parser to auto-insert duplication nodes.

**Common affinity errors:**

1. Using a variable in multiple match branches:
```hvm4
// WRONG: x appears in both branches
@bad = λx. λ{
  #T: x
  #F: x
}

// RIGHT: match first, then bind x in each branch
@good = λ{
  #T: λx. x
  #F: λx. x
}
@main = @good(#T{}, 42)
//42
```

2. Using unnecessary clones:
```hvm4
// BAD: inline λ{...}(x) is poor style

@filter = λ&f. λ{
  []: []
  <>: λ&h. λ&t.
    λ{
      #T: h <> @filter(f, t)
      #F: @filter(f, t)
    }(f(h))
}

// GOOD: split into two functions

@keep = λ{
  #T: λh. λt. h <> t
  #F: λh. λt. t
}

@filter = λ&f. λ{
  []: []
  <>: λ&h. λt. @keep(f(h), h, @filter(f, t))
}

```

It is very important, on HVM, to avoid dups as much as possible. As such, it is
considered a good style to format all top-level definitions as case-trees,
rearranging arguments to avoid unecessary clones, and avoiding inline λ-match
applications (such as `λ{...}(x)`).

### Superposition (SUP)

A SUP `&L{a,b}` represents two values in one location:

```hvm4
@main = (&X{10, 20} + 5)
//15
//25
```

When collapsed (`-C` flag), each branch prints separately.

### Label Mechanics

Labels control how DUPs interact with SUPs:
- **Same label**: DUP and SUP annihilate (pair extraction)
- **Different labels**: they commute (cross product)

To observe this, use explicit DUP syntax `!x&L = val; body` which
binds `x₀` and `x₁`:

```hvm4
// Same label: pairwise extraction
@main =
  !x&A = &A{1, 2};
  [x₀, x₁]
//[1,2]
```

```hvm4
// Different labels: the DUP stays inside each SUP branch
@main =
  !x&A = &B{1, 2};
  [x₀, x₁]
//[1,1]
//[2,2]
```

```hvm4
// Cross product from two SUPs with different labels (4 results)
@main = [&A{1, 2}, &B{10, 20}]
//[1,10]
//[1,20]
//[2,10]
//[2,20]
```

**Prefer `λ&x`** for general functions. Use explicit DUPs only when you
need label control (pairwise extraction or explicit SUP/DUP interactions).

### Pattern Matching

Match on constructors with `λ{...}`:

```hvm4
@not = λ{
  #T: #F{}
  #F: #T{}
}
@main = @not(#T{})
//#F{}
```

```hvm4
@pred = λ{
  0n:  0n
  1n+: λp. p
}
@main = @pred(3n)
//2n
```

Match cases bind constructor fields as lambda arguments.

### Peano Naturals

`Nn` is sugar for `#Suc{...#Suc{#Zer}...}`. Patterns: `0n:` matches
`#Zer`, `1n+:` matches `#Suc` and binds predecessor:

```hvm4
@add = λ{
  0n:  λb. b
  1n+: λa. λb. 1n+@add(a, b)
}
@main = @add(2n, 3n)
//5n
```

### Lists and Strings

Lists use `#Con` and `#Nil`. Sugar: `[a,b,c]`, `"str"`, `a <> b` (cons).

```hvm4
@len = λ{
  []: 0
  <>: λh. λt. (1 + @len(t))
}
@main = @len([1, 2, 3, 4, 5])
//5
```

### Numeric Patterns

Switch on machine ints with `0:`, `1:`, etc., and `λn.` for default:

```hvm4
@fib = λ{
  0: 0
  1: 1
  λ&n. (@fib((n - 1)) + @fib((n - 2)))
}
@main = @fib(10)
//55
```

### Fork Notation

`&Lλx,y{A;B}` is sugar for parallel branching. A uses `x₀,y₀`; B uses
`x₁,y₁`:

```hvm4
@main = (&Lλx,y{(x + y); (x * y)})(3, 4)
//7
//12
```

This desugars to:

```hvm4
@main =
  !x&L = 3;
  !y&L = 4;
  &L{(x₀ + y₀), (x₁ * y₁)}
//7
//12
```

### Unscoped Lambdas

Unscoped lambdas allow a variable to be used outside its binder's lexical
scope. You can write them directly as:

```
λ$x. body
```

This is sugar for:

```
! f = λ x ; body
```

This declares in `body`:
- `f` = `λy. λ$x. y` — a function that wraps its argument in an unscoped lambda
- `x` = `$x` — an unscoped variable usable anywhere

So `λ$x.body` is equivalent to constructing it as `f(body)`:

```hvm4
@main = (λ$x. (x + 1))(10)
//11
```

Applying the unscoped lambda to `10` binds `x` to `10`, yielding `11`.
(Equivalent sugar: `! f = λ x ; f(x + 1)(10)`.)

Another example using the unscoped variable in a pair:

```hvm4
@main = ! f = λ v ; #P{f(1, 2), v}
//#P{1,2}
```

Here `f(1, 2)` is `f(1)(2)` which produces `λ$v.1` then applies it to `2`,
binding `v` to `2`. The pair contains `1` and the captured value `2`.

This is useful for continuation-passing or "exporting" a binding out of a
nested context.

## Examples

### Factorial with Peano Naturals

```hvm4
@add = λ{
  0n:  λb. b
  1n+: λa. λb. 1n+@add(a, b)
}

@mul = λ{
  0n:  λb. 0n
  1n+: λa. λ&b. @add(b, @mul(a, b))
}

@fac = λ{
  0n:  1n
  1n+: λ&p. @mul(1n+p, @fac(p))
}

@main = @fac(3n)
//6n
```

### List Map

```hvm4
@map = λ&f. λ{
  []: []
  <>: λh. λt. (f(h) <> @map(f, t))
}
@main = @map(λx. (x * 2), [1, 2, 3])
//[2,4,6]
```

### Enumerating Binary Strings

Superpositions can enumerate multiple results; each branch prints separately
in collapse mode:

```hvm4
@bits = λ{
  0n:  #E{}
  1n+: λ&p.
    &B{#O{@bits(p)}, #I{@bits(p)}}
}
@main = @bits(2n)
//#O{#O{#E{}}}
//#O{#I{#E{}}}
//#I{#O{#E{}}}
//#I{#I{#E{}}}
```

### Church Numerals

Standard lambda encoding with a cloned binder for `s`:

```hvm4
@c2   = λ&s. λz. s(s(z))
@cmul = λa. λb. λs. λz. a(b(s), z)
@main = @cmul(@c2, @c2)
//λa.λb.a(a(a(a(b))))
```

### Solving Equations via Enumeration

```hvm4
@add = λ{
  0n:  λb. b
  1n+: λa. λb. 1n+@add(a, b)
}

@eq = λ{
  0n:  λ{
    0n:  #T{}
    1n+: λp. #F{}
  }
  1n+: λa. λ{
    0n:  #F{}
    1n+: λb. @eq(a, b)
  }
}

@if = λ{
  #T: λt. λf. t
  #F: λt. λf. f
}

// All naturals: 0, 1, 2, ...
@X = &N{0n, 1n+@X}

// Solve X + 2 = 4
@main = @if(@eq(@add(@X, 2n), 4n), @X, &{})
//2n
```

### Filter

```hvm4
@even = λ{
  0: #T{}
  1: #F{}
  λp. @even((p - 2))
}

@filter = λ&f. λ{
  []: []
  <>: λ&h. λt. @keep(f(h), h, @filter(f, t))
}

@keep = λ{
  #T: λh. λr. (h <> r)
  #F: λh. λr. r
}

@main = @filter(@even, [0, 1, 2, 3, 4, 5, 6])
//[0,2,4,6]
```

## Common Patterns

- `λ&x.`: Cloned binder (auto-dup)
- `!x&L = v; body`: Explicit dup with label L
- `x₀`, `x₁`: Dup variable branches
- `&L{a,b}`: Superposition
- `&{}`: Erasure (discard value)
- `a <> b`: List cons
- `@name`: Reference to definition

Constructor match:

```hvm4
λ{
  #C: ...
}
```

Peano natural match:

```hvm4
λ{
  0n:  ...
  1n+: λp. ...
}
```

Numeric switch (machine ints):

```hvm4
λ{
  0:  ...
  1:  ...
  λn. ...
}
```

## Gotchas

1. **No whitespace application**: use `f(x)` not `f x`
2. **Variables are affine**: use `&` for multiple uses
3. **Subscripts required**: dup-bound vars need `₀` or `₁`
4. **Labels matter**: same labels annihilate, different multiply branches
5. **Constructor syntax**: always `#Name{...}`, even nullary `#Zer{}`
6. **Pattern arms are lambdas**: `1n+: λpred. body` "binds" the field
7. **No inline match application**: avoid writting `λ{...}(x)`

## Reduction Modes

- **WNF**: weak normal form (head only)
- **SNF**: strong normal form (full, keeps SUPs/DUPs)
- **CNF**: collapsed normal form (enumerate as pure lambda terms)

Use `-C` flag for collapse mode, omit for normalize mode.
