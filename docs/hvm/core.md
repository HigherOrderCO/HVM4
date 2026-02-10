# HVM Core Terms

This document defines the core surface syntax for HVM terms. These terms are
parsed into static (book) terms and later instantiated as dynamic terms during
execution. See `docs/hvm/memory.md` for the dynamic/static memory layout.

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
  correct arity; `%log` prints a string and yields `#Nil`.
- Surface sugar accepts `λ$x. body` as an unscoped lambda, equivalent to
  `! f = λ x ; f(body)` with fresh `f` (see `docs/hvm/syntax.md`).
  correct arity; `%log` prints a string and yields `#Nil`; process primitives
  (`%process_spawn`, `%process_poll`, `%process_wait`, `%process_kill`) use
  `#Proc`, `#Pend`, `#Rdy`, `#Exit`, and `#Sig` under `#OK{...}`, or `#ERR{String}`;
  timer primitives (`%timer_start`, `%timer_poll`, `%timer_wait`) use `#Time`,
  `#Pend`, and `#Rdy` under `#OK{...}`, or `#ERR{String}`; stream primitives
  (`%stream_stdin_open`, `%stream_file_open`, `%stream_poll`, `%stream_wait`,
  `%stream_close`) use `#Strm`, `#Pend`, `#Rdy`, `#Byt`, and `#Eof` under
  `#OK{...}`, or `#ERR{String}` (`%stream_close` returns `#OK{#Nil}`); https
  primitives (`%https_get`, `%https_poll`, `%https_wait`, `%https_cancel`) use
  `#Http`, `#Pend`, and `#Rdy` under `#OK{...}`, with outcomes `#Resp{status,#Nil,body}`,
  `#Fail{code,msg}`, or `#Canceled`, or return `#ERR{String}`.
