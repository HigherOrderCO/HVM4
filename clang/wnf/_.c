// WNF uses an explicit stack to avoid recursion.
// - Enter/reduce: walk into the head position, pushing eliminators as frames.
//   APP/OP2/EQL/AND/OR/DSU/DDU push their term as a frame and descend into the
//   left/strict field (APP fun, OP2 lhs, etc). DP0/DP1 push and descend into the
//   shared dup expr. MAT/USE add specialized frames when their scrutinee is ready.
// - Apply: once WHNF is reached, pop frames and dispatch the interaction using
//   the WHNF result. Frames reuse existing heap nodes to avoid allocations.
__attribute__((cold, noinline)) static Term wnf_rebuild(Term cur, Term *stack, u32 s_pos, u32 base) {
  while (s_pos > base) {
    Term frame = stack[--s_pos];

    switch (term_tag(frame)) {
      case APP: {
        u64  loc = term_val(frame);
        Term arg = heap_read(loc + 1);
        cur = term_new_app_at(loc, cur, arg);
        break;
      }
      case MAT:
      case SWI:
      case USE: {
        cur = term_new_app(frame, cur);
        break;
      }
      case DP0:
      case DP1: {
        u64 loc = term_val(frame);
        heap_set(loc, cur);
        cur = frame;
        break;
      }
      case OP2: {
        u64 loc = term_val(frame);
        heap_set(loc + 0, cur);
        cur = frame;
        break;
      }
      case F_OP2_NUM: {
        u32  opr = term_ext(frame);
        Term x   = term_new_num((u32)term_val(frame));
        cur = term_new_op2(opr, x, cur);
        break;
      }
      case EQL:
      case AND:
      case OR:
      case DSU:
      case DDU: {
        u64 loc = term_val(frame);
        heap_set(loc + 0, cur);
        cur = frame;
        break;
      }
      case F_EQL_R: {
        u64 loc = term_val(frame);
        heap_set(loc + 1, cur);
        cur = term_new(0, EQL, 0, loc);
        break;
      }
      default: {
        break;
      }
    }
  }

  WNF_S_POS = s_pos;
  return cur;
}

fn Term wnf_pri(Term pri);
fn int aot_try_call(u32 id, Term *stack, u32 *s_pos, u32 base, Term *out);

#if WNF_CAP >= 0x100000000ULL
  #define WNF_STACK_CAP_TERMS 0xFFFFFFFFu
#else
  #define WNF_STACK_CAP_TERMS ((u32)WNF_CAP)
#endif

fn void wnf_stack_push(Term *stack, u32 *s_pos, Term frame) {
  if (__builtin_expect(*s_pos >= WNF_STACK_CAP_TERMS, 0)) {
    fprintf(stderr, "RUNTIME_ERROR: wnf stack overflow (cap=%llu terms)\n",
            (unsigned long long)WNF_STACK_CAP_TERMS);
    exit(1);
  }
  stack[(*s_pos)++] = frame;
}

#ifndef __has_attribute
  #define __has_attribute(x) 0
#endif

#if __has_attribute(hot)
  #define WNF_HOT __attribute__((hot))
#else
  #define WNF_HOT
#endif

WNF_HOT fn Term wnf(Term term) {
  wnf_stack_init();
  Term *stack = WNF_STACK;
  u32  s_pos  = WNF_S_POS;
  u32  base   = s_pos;
  Term next   = term;
  Term whnf;

  enter: {
    if (__builtin_expect(STEPS_ITRS_LIM != 0, 0) && ITRS >= STEPS_ITRS_LIM) {
      return wnf_rebuild(next, stack, s_pos, base);
    }
    if (__builtin_expect(DEBUG, 0)) {
      printf("wnf_enter: ");
      print_term(next);
      printf("\n");
    }

    switch (term_tag(next)) {
      case VAR: {
        u64 loc = term_val(next);
        Term cell = heap_read(loc);
        if (term_sub_get(cell)) {
          next = term_sub_set(cell, 0);
          goto enter;
        }
        whnf = next;
        goto apply;
      }

      case DP0:
      case DP1: {
        u64 loc = term_val(next);
        Term cell = heap_take(loc);
        if (term_sub_get(cell)) {
          next = term_sub_set(cell, 0);
          goto enter;
        }
        wnf_stack_push(stack, &s_pos, next);
        next = cell;
        goto enter;
      }

      case APP: {
        u64  loc = term_val(next);
        Term fun = heap_read(loc);
        wnf_stack_push(stack, &s_pos, next);
        next = fun;
        goto enter;
      }

      case DUP: {
        u64  loc  = term_val(next);
        Term body = heap_read(loc + 1);
        next = body;
        goto enter;
      }

      case UNS: {
        next = wnf_uns(next);
        goto enter;
      }

      case REF: {
        u32 nam = term_ext(next);
        Term aot_out;
        if (aot_try_call(nam, stack, &s_pos, base, &aot_out)) {
          next = aot_out;
          goto enter;
        }
        if (BOOK[nam] != 0) {
          next = term_new_alo(0, 0, BOOK[nam]);
          goto enter;
        }
        whnf = next;
        goto apply;
      }

      case PRI: {
        WNF_S_POS = s_pos;
        next = wnf_pri(next);
        goto enter;
      }

      case ALO: {
        u32 len     = term_ext(next);
        u64 alo_loc;
        u64 tm_loc;
        u64 ls_loc;
        if (len == 0) {
          alo_loc = 0;
          tm_loc = term_val(next);
          ls_loc = 0;
        } else {
          alo_loc = term_val(next);
          u64 pair = heap_read(alo_loc);
          ls_loc = (pair >> ALO_TM_BITS) & ALO_LS_MASK;
          tm_loc = pair & ALO_TM_MASK;
        }
        Term book    = heap_read(tm_loc);

        switch (term_tag(book)) {
          case VAR:
          case BJV: {
            next = wnf_alo_var(ls_loc, len, book);
            goto enter;
          }
          case DP0:
          case DP1:
          case BJ0:
          case BJ1: {
            next = wnf_alo_cop(ls_loc, len, book);
            goto enter;
          }
          case LAM: {
            next = wnf_alo_lam(alo_loc, ls_loc, len, book);
            goto enter;
          }
          case DUP: {
            next = wnf_alo_dup(alo_loc, ls_loc, len, book);
            goto enter;
          }
          case APP:
          case DRY:
          case SUP:
          case MAT:
          case SWI:
          case USE:
          case UNS:
          case INC:
          case C00 ... C16:
          case PRI:
          case OP2:
          case EQL:
          case AND:
          case OR:
          case DSU:
          case DDU: {
            next = wnf_alo_nod(alo_loc, ls_loc, len, book);
            goto enter;
          }
          case NAM:
          case NUM:
          case REF:
          case ERA:
          case ANY: {
            next = book;
            goto enter;
          }
        }
      }

      case OP2: {
        u64  loc = term_val(next);
        Term x   = heap_read(loc + 0);
        wnf_stack_push(stack, &s_pos, next);
        next = x;
        goto enter;
      }

      case EQL: {
        u64  loc = term_val(next);
        Term a   = heap_read(loc + 0);
        wnf_stack_push(stack, &s_pos, next);
        next = a;
        goto enter;
      }

      case AND: {
        u64  loc = term_val(next);
        Term a   = heap_read(loc + 0);
        wnf_stack_push(stack, &s_pos, next);
        next = a;
        goto enter;
      }

      case OR: {
        u64  loc = term_val(next);
        Term a   = heap_read(loc + 0);
        wnf_stack_push(stack, &s_pos, next);
        next = a;
        goto enter;
      }

      case DSU: {
        u64  loc = term_val(next);
        Term lab = heap_read(loc + 0);
        wnf_stack_push(stack, &s_pos, next);
        next = lab;
        goto enter;
      }

      case DDU: {
        u64  loc = term_val(next);
        Term lab = heap_read(loc + 0);
        wnf_stack_push(stack, &s_pos, next);
        next = lab;
        goto enter;
      }

      case NAM:
      case BJV:
      case BJ0:
      case BJ1:
      case DRY:
      case ERA:
      case SUP:
      case LAM:
      case NUM:
      case MAT:
      case SWI:
      case USE:
      case INC:
      case C00 ... C16: {
        whnf = next;
        goto apply;
      }

      default: {
        whnf = next;
        goto apply;
      }
    }
  }

  apply: {
    if (__builtin_expect(DEBUG, 0)) {
      printf("wnf_apply: ");
      print_term(whnf);
      printf("\n");
    }

    while (s_pos > base) {
      if (__builtin_expect(STEPS_ITRS_LIM != 0, 0) && ITRS >= STEPS_ITRS_LIM) {
        return wnf_rebuild(whnf, stack, s_pos, base);
      }
      Term frame = stack[--s_pos];

      switch (term_tag(frame)) {
        // -----------------------------------------------------------------------
        // APP frame: (□ x) - we reduced func, now dispatch
        // -----------------------------------------------------------------------
        case APP: {
          u64  app_loc = term_val(frame);
          Term arg     = heap_read(app_loc + 1);

          switch (term_tag(whnf)) {
            case ERA: {
              whnf = wnf_app_era();
              continue;
            }
            case NAM:
            case BJV:
            case BJ0:
            case BJ1: {
              whnf = wnf_app_nam(app_loc, whnf);
              continue;
            }
            case DRY: {
              whnf = wnf_app_dry(app_loc, whnf);
              continue;
            }
            case LAM: {
              next = wnf_app_lam(whnf, arg);
              goto enter;
            }
            case SUP: {
              whnf = wnf_app_sup(app_loc, whnf, arg);
              continue;
            }
            case INC: {
              whnf = wnf_app_inc(frame, whnf);
              continue;
            }
            case MAT:
            case SWI: {
              wnf_stack_push(stack, &s_pos, whnf);
              next = arg;
              goto enter;
            }
            case USE: {
              wnf_stack_push(stack, &s_pos, whnf);
              next = arg;
              goto enter;
            }
            case NUM: {
              fprintf(stderr, "RUNTIME_ERROR: cannot apply a number\n");
              exit(1);
            }
            case C00 ... C16: {
              fprintf(stderr, "RUNTIME_ERROR: cannot apply a constructor\n");
              exit(1);
            }
            default: {
              heap_set(app_loc + 0, whnf);
              whnf = frame;
              continue;
            }
          }
        }

        // -----------------------------------------------------------------------
        // MAT/SWI frame: (mat □) - we reduced arg, dispatch mat interaction
        // -----------------------------------------------------------------------
        case MAT:
        case SWI: {
          Term mat = frame;
          switch (term_tag(whnf)) {
            case ERA: {
              whnf = wnf_app_era();
              continue;
            }
            case SUP: {
              whnf = wnf_app_mat_sup(mat, whnf);
              continue;
            }
            case INC: {
              whnf = wnf_mat_inc(mat, whnf);
              continue;
            }
            case C00 ... C16: {
              next = wnf_app_mat_ctr(mat, whnf);
              goto enter;
            }
            case NUM: {
              next = wnf_app_mat_num(mat, whnf);
              goto enter;
            }
            case NAM:
            case BJV:
            case BJ0:
            case BJ1:
            case DRY: {
              // (mat ^n) or (mat ^(f x)): stuck, produce DRY
              whnf = term_new_dry(mat, whnf);
              continue;
            }
            default: {
              whnf = term_new_app(mat, whnf);
              continue;
            }
          }
        }

        // -----------------------------------------------------------------------
        // USE frame: (use □) - we reduced arg, dispatch use interaction
        // -----------------------------------------------------------------------
        case USE: {
          Term use = frame;
          switch (term_tag(whnf)) {
            case ERA: {
              whnf = wnf_use_era();
              continue;
            }
            case SUP: {
              whnf = wnf_use_sup(use, whnf);
              continue;
            }
            case INC: {
              whnf = wnf_use_inc(use, whnf);
              continue;
            }
            default: {
              next = wnf_use_val(use, whnf);
              goto enter;
            }
          }
        }

        // -----------------------------------------------------------------------
        // DP0/DP1 frame: DUP node - we reduced the expr, dispatch dup interaction
        // -----------------------------------------------------------------------
        case DP0:
        case DP1: {
          u8  side = (term_tag(frame) == DP0) ? 0 : 1;
          u64 loc  = term_val(frame);
          u32 lab  = term_ext(frame);

          switch (term_tag(whnf)) {
            case NAM:
            case BJV:
            case BJ0:
            case BJ1: {
              whnf = wnf_dup_nam(lab, loc, side, whnf);
              continue;
            }
            case LAM: {
              whnf = wnf_dup_lam(lab, loc, side, whnf);
              continue;
            }
            case SUP: {
              next = wnf_dup_sup(lab, loc, side, whnf);
              goto enter;
            }
            case ERA:
            case ANY:
            case PRI:
            case NUM: {
              whnf = wnf_dup_nod(lab, loc, side, whnf);
              continue;
            }
            // case APP: // !! DO NOT ADD: DP0/DP1 do not interact with APP.
            case DRY:
            case MAT:
            case SWI:
            case USE:
            case INC:
            case OP2:
            case DSU:
            case DDU:
            case C00 ... C16: {
              next = wnf_dup_nod(lab, loc, side, whnf);
              goto enter;
            }
            default: {
              heap_set(loc, whnf);
              whnf = frame;
              continue;
            }
          }
        }

        // -----------------------------------------------------------------------
        // OP2 frame: (□ op y) - we reduced x, dispatch or transition to F_OP2_NUM
        // -----------------------------------------------------------------------
        case OP2: {
          u32  opr = term_ext(frame);
          u64  loc = term_val(frame);
          Term y   = heap_read(loc + 1);

          switch (term_tag(whnf)) {
            case ERA: {
              whnf = wnf_op2_era();
              continue;
            }
            case NUM: {
              u8 y_tag = term_tag(y);
              if (y_tag == NUM) {
                whnf = wnf_op2_num_num_raw(opr, (u32)term_val(whnf), (u32)term_val(y));
                continue;
              }
              // x is NUM, now reduce y: push F_OP2_NUM frame
              wnf_stack_push(stack, &s_pos, term_new(0, F_OP2_NUM, opr, term_val(whnf)));
              next = y;
              goto enter;
            }
            case SUP: {
              whnf = wnf_op2_sup(loc, opr, whnf, y);
              continue;
            }
            case INC: {
              whnf = wnf_op2_inc_x(opr, whnf, y);
              continue;
            }
            default: {
              heap_set(loc + 0, whnf);
              whnf = frame;
              continue;
            }
          }
        }

        // -----------------------------------------------------------------------
        // F_OP2_NUM frame: (x op □) - x is NUM, we reduced y, dispatch
        // -----------------------------------------------------------------------
        case F_OP2_NUM: {
          u32 opr   = term_ext(frame);
          u32 x_val = (u32)term_val(frame);

          switch (term_tag(whnf)) {
            case ERA: {
              whnf = wnf_op2_num_era();
              continue;
            }
            case NUM: {
              whnf = wnf_op2_num_num_raw(opr, x_val, (u32)term_val(whnf));
              continue;
            }
            case SUP: {
              Term x = term_new_num(x_val);
              whnf = wnf_op2_num_sup(opr, x, whnf);
              continue;
            }
            case INC: {
              Term x = term_new_num(x_val);
              whnf = wnf_op2_inc_y(opr, x, whnf);
              continue;
            }
            default: {
              // Stuck: (x op y) where x is NUM, y is not
              Term x = term_new_num(x_val);
              whnf = term_new_op2(opr, x, whnf);
              continue;
            }
          }
        }

        // -----------------------------------------------------------------------
        // EQL frame: (□ === b) - we reduced a, transition to F_EQL_R or dispatch
        // -----------------------------------------------------------------------
        case EQL: {
          u64  loc = term_val(frame);
          Term b   = heap_read(loc + 1);

          switch (term_tag(whnf)) {
            case ERA: {
              whnf = wnf_eql_era_l();
              continue;
            }
            case ANY: {
              whnf = wnf_eql_any_l();
              continue;
            }
            case SUP: {
              whnf = wnf_eql_sup_l(loc, whnf, b);
              continue;
            }
            case INC: {
              whnf = wnf_eql_inc_l(loc, whnf, b);
              continue;
            }
            default: {
              // Store a's WHNF location, push F_EQL_R, enter b
              // We store a in heap_read(loc+0) for later retrieval
              heap_set(loc + 0, whnf);
              wnf_stack_push(stack, &s_pos, term_new(0, F_EQL_R, 0, loc));
              next = b;
              goto enter;
            }
          }
        }

        // -----------------------------------------------------------------------
        // F_EQL_R frame: (a === □) - we reduced b, now compare both WHNFs
        // -----------------------------------------------------------------------
        case F_EQL_R: {
          u64  loc = term_val(frame);
          Term a   = heap_read(loc + 0);  // a's WHNF was stored here

          switch (term_tag(whnf)) {
            case ERA: {
              whnf = wnf_eql_era_r();
              continue;
            }
            case ANY: {
              whnf = wnf_eql_any_r();
              continue;
            }
            case SUP: {
              whnf = wnf_eql_sup_r(loc, a, whnf);
              continue;
            }
            case INC: {
              whnf = wnf_eql_inc_r(loc, a, whnf);
              continue;
            }
            default: {
              // Both a and b are WHNF, now dispatch based on types
              u8 a_tag = term_tag(a);
              u8 b_tag = term_tag(whnf);

              // ANY === x or x === ANY
              if (a_tag == ANY || b_tag == ANY) {
                whnf = wnf_eql_any_r();
                continue;
              }
              // NUM === NUM
              if (a_tag == NUM && b_tag == NUM) {
                whnf = wnf_eql_num(a, whnf);
                continue;
              }
              // LAM === LAM
              if (a_tag == LAM && b_tag == LAM) {
                next = wnf_eql_lam(a, whnf);
                goto enter;
              }
              // CTR === CTR
              if (a_tag >= C00 && a_tag <= C16 && b_tag >= C00 && b_tag <= C16) {
                next = wnf_eql_ctr(loc, a, whnf);
                goto enter;
              }
              // MAT/SWI === MAT/SWI
              if ((a_tag == MAT || a_tag == SWI) && (b_tag == MAT || b_tag == SWI)) {
                next = wnf_eql_mat(loc, a, whnf);
                goto enter;
              }
              // USE === USE
              if (a_tag == USE && b_tag == USE) {
                next = wnf_eql_use(loc, a, whnf);
                goto enter;
              }
              // NAM/BJ* === NAM/BJ*
              if ((a_tag == NAM || a_tag == BJV || a_tag == BJ0 || a_tag == BJ1) &&
                  (b_tag == NAM || b_tag == BJV || b_tag == BJ0 || b_tag == BJ1)) {
                whnf = wnf_eql_nam(a, whnf);
                continue;
              }
              // DRY === DRY
              if (a_tag == DRY && b_tag == DRY) {
                next = wnf_eql_dry(loc, a, whnf);
                goto enter;
              }
              // Otherwise: not equal
              ITRS_INC("EQL-NOT");
              whnf = term_new_num(0);
              continue;
            }
          }
        }

        // -----------------------------------------------------------------------
        // DSU frame: &(□){a,b} - we reduced lab, dispatch
        // -----------------------------------------------------------------------
        case DSU: {
          u64  loc = term_val(frame);
          Term a   = heap_read(loc + 1);
          Term b   = heap_read(loc + 2);

          switch (term_tag(whnf)) {
            case ERA: {
              whnf = wnf_dsu_era();
              continue;
            }
            case NUM: {
              whnf = wnf_dsu_num(whnf, a, b);
              continue;
            }
            case SUP: {
              whnf = wnf_dsu_sup(whnf, a, b);
              continue;
            }
            case INC: {
              whnf = wnf_dsu_inc(whnf, a, b);
              continue;
            }
            default: {
              heap_set(loc + 0, whnf);
              whnf = frame;
              continue;
            }
          }
        }

        // -----------------------------------------------------------------------
        // DDU frame: ! x &(□) = val; bod - we reduced lab, dispatch
        // -----------------------------------------------------------------------
        case DDU: {
          u64  loc = term_val(frame);
          Term val = heap_read(loc + 1);
          Term bod = heap_read(loc + 2);

          switch (term_tag(whnf)) {
            case ERA: {
              whnf = wnf_ddu_era();
              continue;
            }
            case NUM: {
              next = wnf_ddu_num(whnf, val, bod);
              goto enter;
            }
            case SUP: {
              whnf = wnf_ddu_sup(whnf, val, bod);
              continue;
            }
            case INC: {
              whnf = wnf_ddu_inc(whnf, val, bod);
              continue;
            }
            default: {
              heap_set(loc + 0, whnf);
              whnf = frame;
              continue;
            }
          }
        }

        // -----------------------------------------------------------------------
        // AND frame: (□ .&. b) - we reduced a, dispatch
        // -----------------------------------------------------------------------
        case AND: {
          u64  loc = term_val(frame);
          Term b   = heap_read(loc + 1);

          switch (term_tag(whnf)) {
            case ERA: {
              whnf = wnf_and_era();
              continue;
            }
            case SUP: {
              whnf = wnf_and_sup(loc, whnf, b);
              continue;
            }
            case INC: {
              whnf = wnf_and_inc(loc, whnf, b);
              continue;
            }
            case NUM: {
              next = wnf_and_num(whnf, b);
              goto enter;
            }
            default: {
              heap_set(loc + 0, whnf);
              whnf = frame;
              continue;
            }
          }
        }

        // -----------------------------------------------------------------------
        // OR frame: (□ .|. b) - we reduced a, dispatch
        // -----------------------------------------------------------------------
        case OR: {
          u64  loc = term_val(frame);
          Term b   = heap_read(loc + 1);

          switch (term_tag(whnf)) {
            case ERA: {
              whnf = wnf_or_era();
              continue;
            }
            case SUP: {
              whnf = wnf_or_sup(loc, whnf, b);
              continue;
            }
            case INC: {
              whnf = wnf_or_inc(loc, whnf, b);
              continue;
            }
            case NUM: {
              next = wnf_or_num(whnf, b);
              goto enter;
            }
            default: {
              heap_set(loc + 0, whnf);
              whnf = frame;
              continue;
            }
          }
        }

        default: {
          continue;
        }
      }
    }
  }

  WNF_S_POS = s_pos;
  return whnf;
}

fn Term wnf_at(u64 loc) {
  Term cur = heap_read(loc);
  switch (term_tag(cur)) {
    case NAM:
    case BJV:
    case BJ0:
    case BJ1:
    case DRY:
    case ERA:
    case SUP:
    case LAM:
    case NUM:
    case MAT:
    case SWI:
    case USE:
    case INC:
    case C00 ... C16: {
      return cur;
    }
    default: {
      break;
    }
  }
  Term res = wnf(cur);
  if (res != cur) {
    heap_set(loc, res);
  }
  return res;
}

#define STEPS_DASH_LEN 40

__attribute__((cold, noinline)) fn void steps_print_line(str itr) {
  for (u32 i = 0; i < STEPS_DASH_LEN; i++) {
    fputc('-', stdout);
  }
  if (itr != NULL) {
    fputc(' ', stdout);
    fputs(itr, stdout);
  }
  fputc('\n', stdout);
}

__attribute__((cold, noinline)) fn Term wnf_steps_at(u64 loc) {
  Term cur = heap_read(loc);
  switch (term_tag(cur)) {
    case NAM:
    case BJV:
    case BJ0:
    case BJ1:
    case DRY:
    case ERA:
    case SUP:
    case LAM:
    case NUM:
    case MAT:
    case SWI:
    case USE:
    case INC:
    case C00 ... C16: {
      return cur;
    }
    default: {
      break;
    }
  }

  for (;;) {
    u64 itrs = ITRS;
    STEPS_LAST_ITR = NULL;
    STEPS_ITRS_LIM = itrs + 1;
    Term res = wnf(cur);
    STEPS_ITRS_LIM = 0;
    if (res != cur) {
      heap_set(loc, res);
      cur = res;
    }
    if (ITRS == itrs) {
      break;
    }
    if (!SILENT && STEPS_ROOT_LOC != 0) {
      steps_print_line(STEPS_LAST_ITR);
      print_term(heap_read(STEPS_ROOT_LOC));
      printf("\n");
    }
  }

  return cur;
}
