# HVM4 Core Terms

This document defines the core surface syntax for HVM4 terms. These terms are
parsed into static (book) terms and later instantiated as dynamic terms during
execution. See `docs/hvm4/memory.md` for the dynamic/static memory layout.

## Grammar

```
Term ::=
  | Var  Name                                        -- variable
  | Dp0  Name "₀"                                    -- first dup variable
  | Dp1  Name "₁"                                    -- second dup variable
  | Ref  "@" Name                                    -- reference
  | Pri  "%" Name                                    -- primitive (native) function
  | Nam  "^" Name                                    -- name (stuck head)
  | Dry  "^" "(" Term " " Term ")"                   -- dry (stuck application)
  | Era  "&{}"                                       -- erasure
  | Sup  "&" Label "{" Term "," Term "}"             -- superposition
  | Dup  "!" Name "&" Label "=" Term ";" Term        -- duplication term
  | Ctr  "#" Name "{" Term,* "}"                     -- constructor
  | Mat  "λ" "{" "#" Name ":" Term ";" Term "}"      -- pattern match
  | Swi  "λ" "{" Num ":" Term ";" Term "}"           -- number switch
  | Use  "λ" "{" Term "}"                            -- use (unbox)
  | Lam  "λ" Name "." Term                           -- lambda
  | App  "(" Term " " Term ")"                       -- application
  | Num  integer                                     -- number literal
  | Op2  "(" Term Oper Term ")"                      -- binary operation
  | Eql  "(" Term "==" Term ")"                      -- equality test
  | And  "(" Term ".&." Term ")"                     -- short-circuit AND
  | Or   "(" Term ".|." Term ")"                     -- short-circuit OR
  | DSu  "&" "(" Term ")" "{" Term "," Term "}"      -- dynamic superposition
  | DDu  "!" Name "&" "(" Term ")" "=" Term ";" Term -- dynamic duplication term
  | Red  Term "~>" Term                              -- reduction
  | Inc  "↑" Term                                    -- priority wrapper
  | Alo  "@" "{" Name,* "}" Term                     -- allocation
  | Uns  "!" "$" "{" Name "," Name "}" ";" Term      -- unscoped binding

Name  ::= [_A-Za-z0-9]+
Label ::= Name
Oper  ::= "+" | "-" | "*" | "/" | "%" | "&&" | "||"
        | "^" | "~" | "<<" | ">>" | "==" | "!="
        | "<" | "<=" | ">" | ">="
```

## Notes

- Variables are affine: each variable is used at most once.
- Variables are global: a variable can occur outside its binder's lexical scope.
- Labels determine how duplications and superpositions interact; equal labels
  annihilate, different labels commute.
- Primitives (`%name`) are native functions and must be fully applied with the
  correct arity; `%log` prints a string and yields `#Nil`; timer primitives
  (`%timer_start`, `%timer_poll`, `%timer_wait`) implement one-shot async timers
  with `#Time`, `#Pend`, and `#Rdy` constructors; keyboard primitives
  (`%key_open`, `%key_poll`, `%key_wait`) use `#Key`, `#Pend`, and `#Rdy`;
  process primitives (`%proc_spawn`, `%proc_poll`, `%proc_wait`, `%proc_kill`)
  use `#Proc`, `#Pend`, `#Rdy`, and `#Exit`; TCP primitives
  (`%tcp_connect`, `%tcp_poll`, `%tcp_wait`, `%tcp_send`, `%tcp_recv_poll`,
  `%tcp_recv_wait`, `%tcp_close`) use `#Tcp`, `#Pend`, `#Rdy`, and `#Sent`.
- Surface sugar accepts `λ$x. body` as an unscoped lambda, equivalent to
  `! f = λ x ; f(body)` with fresh `f` (see `docs/hvm4/syntax.md`).
