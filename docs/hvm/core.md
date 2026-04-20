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
  `%stream_close`) use `#Strm`, `#Pend`, `#Rdy`, `#BYT`, and `#Eof` under
  `#OK{...}`, or `#ERR{String}` (`%stream_close` returns `#OK{#Nil}`); http
  primitives (`%http_request`, `%http_poll`, `%http_wait`, `%http_cancel`) use
  `#Http`, `#Pend`, and `#Rdy` under `#OK{...}`, where `%http_request` expects
  `#Req{method,url,headers,body,opts}` and outcomes are
  `#Resp{status,headers,body}`, `#Fail{reason,msg}`, or `#Canceled`
  (all under `#OK{...}`), or `#ERR{String}`; tcp primitives (`%tcp_connect`,
  `%tcp_connect_poll`, `%tcp_connect_wait`, `%tcp_recv_poll`, `%tcp_recv_wait`,
  `%tcp_send_poll`, `%tcp_send_wait`, `%tcp_close`) use `#Tcp`, `#Pend`, and
  `#Rdy` under `#OK{...}`; `%tcp_connect` expects
  `#TcpReq{host,port,#TcpOpts{connect_timeout_ms,read_timeout_ms,write_timeout_ms,nodelay,keepalive}}`
  and outcomes are `#Conn`, `#Recv`, `#Sent`, `#Eof`, `#Closed`, or
  `#Fail{reason,msg}` with reason in `#Timeout|#Dns|#Refused|#Unreachable|#Reset|#BrokenPipe|#Protocol|#NotConnected|#Sys{errno}`.
