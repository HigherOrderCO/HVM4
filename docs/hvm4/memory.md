# HVM4 Memory Layout

HVM4 represents every term as a 64-bit word. The same layout is used for dynamic
terms (mutable heap entries) and static terms (immutable book entries).

## Term Pointer Layout (64-bit)

```
SUB (1 bit) | TAG (7 bits) | EXT (24 bits) | VAL (32 bits)
```

- `SUB`: marks a substitution cell; ignored for immediates.
- `TAG`: constructor variant (APP, LAM, SUP, etc.).
- `EXT`: metadata (dup label, ctor/ref name, level, op code, flags).
- `VAL`: payload (heap loc, immediate, or 0).

## Term Memory Table

TERM                    | TAG    | EXT              | VAL                          | NOTES
----------------------- | ------ | ---------------- | ---------------------------- | ---------------------------------------------
application             | APP    | 0                | node: [func, arg]            | dynamic or static
linked variable         | VAR    | 0                | lam body or subst            | dynamic only; follows SUB cells
linked duplicate 0      | DP0    | label            | dup expr or subst            | dynamic only; twin of DP1
linked duplicate 1      | DP1    | label            | dup expr or subst            | dynamic only; twin of DP0
linked get variable 0   | GT0    | 0                | get expr or subst            | dynamic only; twin of GT1
linked get variable 1   | GT1    | 0                | get expr or subst            | dynamic only; twin of GT0
linked lambda           | LAM    | level+flags      | node: [body]                 | dynamic only; binder for VAR
quoted lam              | LAM    | level+flags      | node: [body]                 | quoted; level stored in EXT
superposition           | SUP    | label            | node: [tm0, tm1]             | dynamic or static
linked duplication term | DUP    | label            | node: [expr, body]           | dynamic or static; binder for DP0/DP1
quoted duplication term | DUP    | label            | node: [expr, body]           | expr typically `&L{BJ0,BJ1}` in quoted mode
tuple                   | TUP    | 0                | node: [tm0, tm1]             | dynamic or static
tuple getter            | GET    | 0                | node: [expr, body]           | dynamic or static; binder for GT0/GT1
number literal          | NUM    | 0                | unboxed u32                  | 
constructor arity 0     | C00    | ctor name        | 0                            | tag encodes arity
constructor arity N     | C01-16 | ctor name        | node: [field0..fieldN-1]     | N = tag - C00
pattern match           | MAT    | ctor name        | node: [hit, miss]            | match on constructor
number switch           | SWI    | number literal   | node: [zero, succ]           | parser/printer distinction from MAT
use (unbox)             | USE    | 0                | node: [fun]                  | forces argument, then applies
binary op               | OP2    | op code          | node: [lhs, rhs]             | EXT stores OP_ADD, OP_MUL, etc.
equality                | EQL    | 0                | node: [lhs, rhs]             | structural equality
logical AND             | AND    | 0                | node: [lhs, rhs]             | short-circuit AND
logical OR              | OR     | 0                | node: [lhs, rhs]             | short-circuit OR
reduction               | RED    | 0                | node: [lhs, rhs]             | guarded reduction (f ~> g)
priority wrapper        | INC    | 0                | node: [term]                 | collapse priority wrapper
name literal            | NAM    | name id          | 0                            | literal ^name
stuck application       | DRY    | 0                | node: [fun, arg]             | literal ^(f x)
reference               | REF    | name id          | 0                            | book reference @name
primitive               | PRI    | name id          | node: [arg0..argN-1]         | native function call; arity from prim table
allocation              | ALO    | bind list length | heap slot with packed pair   | low32=book term, high32=bind list head
unscoped binding        | UNS    | 0                | node: [body]                 | helper to construct unscoped lams
wildcard                | ANY    | 0                | 0                            | duplicates itself, equals anything
quoted lam var          | BJV    | 0                | de Bruijn level              | quoted lam-bound var
quoted dup var 0        | BJ0    | label            | de Bruijn level              | quoted dup-bound var
quoted dup var 1        | BJ1    | label            | de Bruijn level              | quoted dup-bound var
quoted get var 0        | BG0    | 0                | de Bruijn level              | quoted get-bound var
quoted get var 1        | BG1    | 0                | de Bruijn level              | quoted get-bound var
dynamic superposition   | DSU    | 0                | node: [lab, tm0, tm1]        | label computed dynamically
dynamic duplication     | DDU    | 0                | node: [lab, val, body]       | label computed dynamically

## Substitution Cells (SUB Bit)

When a linked binder interacts (APP-LAM, DUP-*), its body or expr slot is
replaced by a substitution term with the SUB bit set. Any VAR/DP0/DP1 that
points to that heap location must read the substitution instead of the original
binder. This is how linked variables resolve their binders without extra maps.

## DUP Nodes vs DUP Terms

A DUP term is the syntactic binder `[expr, body]` with tag `DUP`. A DUP node is
the shared expr slot that DP0/DP1 point to; it has no body and no parent, and it
is substituted when a duplication interaction occurs.

## Linked vs Quoted Binders

Linked binders are dynamic lams/dups whose vars point to heap locations. Linked
LAM uses `EXT = level | flags` and its VARs point to the body slot. Linked DUP
uses DP0/DP1 vars that point to the shared expr slot.

Quoted binders are terms encoded with de Bruijn levels in `EXT` (and for BJ0/BJ1,
the dup label). Their variables are BJV/BJ0/BJ1 indices, so there are no heap
links and no interaction with APP-LAM or DUP-SUP.

During collapse, linked LAM/DUP binders are converted into quoted LAM/DUP
binders, and free VAR/DP0/DP1 become BJV/BJ0/BJ1 at the current level. This is
used to turn interaction nets into full lambda terms.

## Dynamic vs Static Terms

Dynamic terms live on the heap and are mutable: binders can be replaced
by substitutions, and DUP nodes evolve as duplication is forced. Static terms
live in the book and are immutable; they store de Bruijn levels and are never
updated in place.

Allocation (ALO) terms bridge the two: they reference a static book term and a
bind list, lazily expanding one layer into a dynamic term when forced. This
keeps static definitions compact while still allowing dynamic sharing during
execution.

## LAM Ext Flags

- `LAM_ERA_MASK` (0x800000): binder is unused in lambda body (erasing lambda).
