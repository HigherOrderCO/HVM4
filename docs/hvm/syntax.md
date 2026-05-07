# HVM Parser Syntax

Desugarings use the core surface syntax from `docs/hvm/core.md`.
Core application is written there as `(f x)`, so this doc uses that notation
for desugaring only; the parser itself does not accept whitespace application.

## Parsing model (how terms group)

- Base unit: an atom.
- Postfix call: `f(...)` binds tightest.
- `<>` binds tighter than infix operators.
- `===`, `.&.`, `.|.` bind tighter than infix operators (same precedence class
  as `==`, `&&`, `||`, but separate nodes).
- Infix operators are left-associative, by the precedence table below.
- No whitespace application: `f x` is not an application; use `f(x)`.

## Lexical rules

- Whitespace: space, tab, newline, `\r`.
- Line comments: `// ...`.
- Names: `_a-zA-Z0-9$`.
  - A term starting with a digit is parsed as a number literal, not a name.

## Atoms

- Variable: `x`
- Dup variables: `x₀` or `x₁` (see “Dup variables”)
- Grouping: `(term)`
- Constructor: `#Name` or `#Name{a,b,c}` (`,` and `;` are equivalent, trailing separator ok)
- Reference: `@Name`
- Name head: `^Name`
- Dry app: `^(f x)`
- Priority wrapper: `↑atom`
- Wildcard: `*`
- Lambda / dup / sup / match / literals (see below)

## Application

- `f(a, b, c)` desugars to `((f a) b) c`.
  - `,` and `;` are equivalent, and separators are optional: `f(a b c)` is the same.
  - `f()` is allowed and desugars to `f`.

## Literals

### Numbers

- `123` is a numeric literal.

### Naturals (Peano)

Using built-in constructors `#Zer` and `#Suc`:

- `Nn` desugars to `#Suc{...#Suc{#Zer}...}` (N times)
- `Nn+term` desugars to `#Suc{...#Suc{term}...}` (N times)
  - `n+` must be contiguous; `3n + x` is not a natural literal.

Examples:

```
3n    => #Suc{#Suc{#Suc{#Zer}}}
2n+xs => #Suc{#Suc{xs}}
```

### Characters

- `'c'` desugars to `#Chr{#<codepoint>}`.

Escapes:
- Standard escapes: `\n`, `\t`, `\r`, `\0`, `\\`, `\'`, `\"`.
- Other backslashes escape the next byte verbatim.
- Otherwise UTF-8 sequences are decoded into a numeric codepoint.

### Strings

Strings are lists of `#Chr`:

- `"str"` desugars to `#Con{#Chr{#s}, #Con{#Chr{#t}, #Con{#Chr{#r}, #Nil}}}`.

Same escape rules as characters.

### Lists

Lists use built-in `#Nil` and `#Con`:

- `[]` desugars to `#Nil`.
- `[a, b, c]` desugars to `#Con{a, #Con{b, #Con{c, #Nil}}}`.
  - `,` and `;` are equivalent; trailing separator allowed.
- `a <> b` desugars to `#Con{a, b}` (cons sugar).

## Lambdas and binders

### Lambda

- `λx. body` is core `Lam`.
- `λx,y,z. body` desugars to `λx. λy. λz. body`.
  - `,` and `;` are equivalent between binders.
- `λ$x. body` is an unscoped lambda binder (see “Unscoped lambda (UNS)”).

### Cloned binders (auto-dup)

Prefix `&` allows multiple uses; the parser inserts dup nodes.

- `λ&x. body` desugars to `λx. body` plus auto-duplication of `x`.
- Auto-duplication expands a cloned variable with N uses into N-1 nested dups
  using fresh labels. Example for three uses:
  `body[x,x,x]` becomes `!d0&=x; !d1&=d0₁; body[d0₀,d1₀,d1₁]`.

### Inline duplication on lambdas

Short form for inserting a dup on the bound variable:

- `λx&L. body` desugars to `λx. !x&L = x; body`.
- `λx&. body` uses a fresh label.
- `λx&(lab). body` desugars to `λx. !x&(lab) = x; body`.

All of these combine with cloning: `λ&x&L. ...`.

## Duplication and let (`!`)

### Let sugar

- `!x = v; body` desugars to `((λx. body) v)`.
- `!!x = v; body` desugars to `((λ{λx. body}) v)` (strict let).
- Cloned forms `!&x = ...` and `!!&x = ...` allow multiple uses of `x` and
  insert auto-dup as needed.

### Duplication (static label)

- `!x&L = v; body` is core `Dup`.
- `!x& = v; body` uses a fresh label.
- Optional `,` or `;` after `v` is allowed.
- Cloned dup binder: `!&x&L = v; body` (or `!&x& = ...`).

### Dup destructuring sugar

Short form for binding each side of a duplicated value as a plain variable:

- `! &L{x,y} = v; body` desugars like `!x_y&L = v; body[x := x_y₀, y := x_y₁]`.
- `! &{x,y} = v; body` uses a fresh label.
- `! &(lab){x,y} = v; body` uses a dynamic label.
- Branch binders can be cloned independently: `! &L{&x,y} = v; body`.
- `,` and `;` are equivalent between branch binders; trailing separator allowed.

### Duplication (dynamic label)

- `!x&(lab) = v; body` is core `DDu`.
- `!&x&(lab) = v; body` is the cloned form.

### Unscoped lambda (UNS)

- `! f = λ x ; body` desugars to `!${f,x}; body`.
- `λ$x. body` desugars to `! f = λ x ; f(body)` with fresh `f`.
  - `,` and `;` are allowed between binders: `λ$x,y. body` (x unscoped, y scoped).
  - The unscoped variable is referenced as plain `x` inside the body.

## Superposition (`&`) and fork

### Superposition and erasure

- `&L{a, b}` is core `Sup`.
- `&(lab){a, b}` is core `DSu`.
- `&{}` is core `Era`.
  - `,` and `;` are equivalent; trailing separator allowed.

### Auto-fork sugar

```
&L!{A;B}
```

desugars to `&L[&x,&y,&z,...]{A;B}` where `x,y,z,...` are all variables
currently in scope (lambda/let bindings and fork bindings, excluding
raw dup bindings that require subscripts). All captured variables are
automatically cloned. Dynamic labels are supported: `&(lab)!{A;B}`.

This is a convenience for the common pattern of forking the entire
context into a superposition's two branches.

### Fork sugar

```
&Lλx,y{A;B}
```

desugars to:

```
λx&L. λy&L. &L{A', B'}
```

where `A'` uses `x₀, y₀` and `B'` uses `x₁, y₁`.

Inside each branch you can omit `₀`/`₁` and the parser chooses the side
automatically. You can also prefix branches with `&₀:` or `&₁:` to force the
side for all un-subscripted dup variables in that branch. `,` and `;` are
equivalent between branches; trailing separator allowed.

Dynamic labels are supported: `&(lab)λx,y{A;B}`.

## Match / switch / use (`λ{...}`)

### Erasure

- `λ{}` desugars to `&{}`.

### Use / unbox

- `λ{term}` is core `Use`.
- `λ{_: term}` also desugars to `λ{term}` if it is the only entry.

### Pattern match and numeric switch

Syntax:

```
λ{
  pat1: t1;
  pat2: t2;
  ...
  _ : default
}
```

Patterns:

- `#Name:`  match constructor `#Name`
- `[]:`     match `#Nil`
- `<>:`     match `#Con`
- `0n:`     match `#Zer`
- `1n+:`    match `#Suc` (digits before `n+` are ignored)
- `N:`      numeric switch on literal `N`
- `_:`      default branch

Desugaring is right-nested:

```
λ{#A: t1; #B: t2; d}
=> λ{#A: t1; λ{#B: t2; d}}

λ{0: t0; 1: t1; d}
=> λ{0: t0; λ{1: t1; d}}
```

`,` and `;` are equivalent between cases; trailing separator allowed, including
after the final default. The final default can be written as `_ : d` or as a
bare `d`.

## Names and references

- `@Name` is a book reference.
- `^Name` is a stuck name head.
- `^(f x)` is a dry application.

## Priority wrapper and wildcard

- `↑atom` is core `Inc` (binds to a single atom).
- `*` is core `Any`.

## Infix operators

All infix operators are left-associative.

Precedence (higher binds tighter):

- 8: `^`
- 7: `*` `/` `%`
- 6: `+` `-`
- 5: `<<` `>>`
- 4: `<` `<=` `>` `>=`
- 3: `==` `!=`
- 2: `&&`
- 1: `||`

Notes:

- `~` parses as `OP_NOT` but is infix. Evaluation ignores the left operand and
  uses only the right one (`~b`). For unary not, write `0 ~ x`.

## Structural equality and short-circuit ops

These are separate nodes (not `Op2`):

- `a === b` structural equality
- `a .&. b` short-circuit AND
- `a .|. b` short-circuit OR

## Dup variables and subscripts

- Variables bound by `Dup` (or inline `λx&L`) must be referenced as `x₀` or
  `x₁`.
- Inside fork branches, the parser can auto-select the side if you omit the
  subscript.
- Dynamic dup variables (`!x&(lab)=...`) always require a subscript.

## Not parsed directly

Some internal terms have print forms but are not parsed directly (for example
`@{...}` for `ALO`). The list above reflects what the parser accepts today.
