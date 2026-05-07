// lambda_eval.hvm compiled to TypeScript — direct 1:1 correspondence
// Each @fn maps to one function. Pattern match → switch. Constructors → tagged objects.

const VAR = 0, LAM = 1, APP = 2;
type Term = { tag: 0; k: number } | { tag: 1; bod: Term } | { tag: 2; fun: Term; arg: Term };

function shift_above(tm: Term, by: number, dep: number): Term {
  switch (tm.tag) {
    case VAR: return tm.k < dep ? tm : { tag: VAR, k: tm.k + by };
    case LAM: return { tag: LAM, bod: shift_above(tm.bod, by, dep + 1) };
    case APP: return { tag: APP, fun: shift_above(tm.fun, by, dep), arg: shift_above(tm.arg, by, dep) };
  }
}

function subst_dep(tm: Term, sub: Term, dep: number): Term {
  switch (tm.tag) {
    case VAR:
      if (tm.k === dep) return shift_above(sub, dep, 0);
      if (tm.k > dep) return { tag: VAR, k: tm.k - 1 };
      return tm;
    case LAM: return { tag: LAM, bod: subst_dep(tm.bod, sub, dep + 1) };
    case APP: return { tag: APP, fun: subst_dep(tm.fun, sub, dep), arg: subst_dep(tm.arg, sub, dep) };
  }
}

const _stk: Term[] = [];

function wnf(tm: Term): Term {
  const base = _stk.length;
  for (;;) {
    if (tm.tag === APP) { _stk.push(tm.arg); tm = tm.fun; continue; }
    if (tm.tag === LAM && _stk.length > base) { tm = subst_dep(tm.bod, _stk.pop()!, 0); continue; }
    for (let i = _stk.length - 1; i >= base; i--) tm = { tag: APP, fun: tm, arg: _stk[i] };
    _stk.length = base;
    return tm;
  }
}

function nf(tm: Term): Term {
  const w = wnf(tm);
  switch (w.tag) {
    case VAR: return w;
    case LAM: return { tag: LAM, bod: nf(w.bod) };
    case APP: return { tag: APP, fun: nf(w.fun), arg: nf(w.arg) };
  }
}

const c_zero: Term = { tag: LAM, bod: { tag: LAM, bod: { tag: VAR, k: 0 } } };

const c_succ: Term = { tag: LAM, bod: { tag: LAM, bod: { tag: LAM, bod:
  { tag: APP, fun: { tag: VAR, k: 1 }, arg:
    { tag: APP, fun: { tag: APP, fun: { tag: VAR, k: 2 }, arg: { tag: VAR, k: 1 } },
      arg: { tag: VAR, k: 0 } } } } } };

const c_add: Term = { tag: LAM, bod: { tag: LAM, bod: { tag: LAM, bod: { tag: LAM, bod:
  { tag: APP, fun: { tag: APP, fun: { tag: VAR, k: 3 }, arg: { tag: VAR, k: 1 } },
    arg: { tag: APP, fun: { tag: APP, fun: { tag: VAR, k: 2 }, arg: { tag: VAR, k: 1 } },
      arg: { tag: VAR, k: 0 } } } } } } };

const c_mul: Term = { tag: LAM, bod: { tag: LAM, bod: { tag: LAM, bod: { tag: LAM, bod:
  { tag: APP, fun: { tag: APP, fun: { tag: VAR, k: 3 },
    arg: { tag: APP, fun: { tag: VAR, k: 2 }, arg: { tag: VAR, k: 1 } } },
    arg: { tag: VAR, k: 0 } } } } } };

function church(n: number): Term {
  let t: Term = c_zero;
  for (let i = 0; i < n; i++) t = { tag: APP, fun: c_succ, arg: t };
  return t;
}

function count_apps(tm: Term): number {
  switch (tm.tag) {
    case VAR: return 0;
    case LAM: return count_apps(tm.bod);
    case APP:
      if (tm.fun.tag === VAR && tm.fun.k === 1) return count_apps(tm.arg) + 1;
      return count_apps(tm.fun) + count_apps(tm.arg);
  }
}

function church_to_u32(tm: Term): number {
  if (tm.tag !== LAM || tm.bod.tag !== LAM) return 0;
  return count_apps(tm.bod.bod);
}

function eval_expr(a: Term, b: Term, c: Term): number {
  return church_to_u32(nf(
    { tag: APP, fun: { tag: APP, fun: c_mul, arg:
      { tag: APP, fun: { tag: APP, fun: c_add, arg:
        { tag: APP, fun: { tag: APP, fun: c_mul, arg: a }, arg: b } },
        arg: c } },
      arg: church(2) }
  ));
}

function rng(seed: number): number {
  return (Math.imul(seed, 1664525) + 1013904223) >>> 0;
}

function main(): number {
  let seed = 1, acc = 0;
  for (let r = 0; r < 4; r++) {
    const na = 19 + ((seed >>> 0) % 4);
    const nb = 18 + ((seed >>> 3) % 4);
    const nc = 16 + ((seed >>> 5) % 3);
    const r0 = eval_expr(church(na), church(nb), church(nc));
    const r1 = eval_expr(church(nb), church(nc), church(na));
    acc = ((Math.imul((acc ^ r0) >>> 0, 16777619) + Math.imul(r1, 131) + seed) >>> 0);
    seed = rng(seed);
  }
  return acc;
}

const t0 = performance.now();
const result = main();
const t1 = performance.now();
console.log(result);
console.log(`Time: ${((t1 - t0) / 1000).toFixed(3)} seconds`);
