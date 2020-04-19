/*
* Copyright (c) 2020 Calvin Rose
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to
* deal in the Software without restriction, including without limitation the
* rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
* sell copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
* IN THE SOFTWARE.
*/

#ifndef JANET_AMALG
#include "features.h"
#include <janet.h>
#include "state.h"
#include "fiber.h"
#include "gc.h"
#include "symcache.h"
#include "util.h"
#endif

#include <math.h>

/* VM state */
JANET_THREAD_LOCAL JanetTable *janet_vm_core_env;
JANET_THREAD_LOCAL JanetTable *janet_vm_registry;
JANET_THREAD_LOCAL JanetTable *janet_vm_abstract_registry;
JANET_THREAD_LOCAL int janet_vm_stackn = 0;
JANET_THREAD_LOCAL JanetFiber *janet_vm_fiber = NULL;
JANET_THREAD_LOCAL JanetFiber *janet_vm_root_fiber = NULL;
JANET_THREAD_LOCAL Janet *janet_vm_return_reg = NULL;
JANET_THREAD_LOCAL jmp_buf *janet_vm_jmp_buf = NULL;

/* Virtual registers
 *
 * One instruction word
 * CC | BB | AA | OP
 * DD | DD | DD | OP
 * EE | EE | AA | OP
 */
#define A ((*pc >> 8)  & 0xFF)
#define B ((*pc >> 16) & 0xFF)
#define C (*pc >> 24)
#define D (*pc >> 8)
#define E (*pc >> 16)

/* Signed interpretations of registers */
#define CS (*((int32_t *)pc) >> 24)
#define DS (*((int32_t *)pc) >> 8)
#define ES (*((int32_t *)pc) >> 16)

/* How we dispatch instructions. By default, we use
 * a switch inside an infinite loop. For GCC/clang, we use
 * computed gotos. */
#if defined(__GNUC__) && !defined(__EMSCRIPTEN__)
#define JANET_USE_COMPUTED_GOTOS
#endif

#ifdef JANET_USE_COMPUTED_GOTOS
#define VM_START() { goto *op_lookup[first_opcode];
#define VM_END() }
#define VM_OP(op) label_##op :
#define VM_DEFAULT() label_unknown_op:
#define vm_next() goto *op_lookup[*pc & 0xFF]
#define opcode (*pc & 0xFF)
#else
#define VM_START() uint8_t opcode = first_opcode; for (;;) {switch(opcode) {
#define VM_END() }}
#define VM_OP(op) case op :
#define VM_DEFAULT() default:
#define vm_next() opcode = *pc & 0xFF; continue
#endif

/* Commit and restore VM state before possible longjmp */
#define vm_commit() do { janet_stack_frame(stack)->pc = pc; } while (0)
#define vm_restore() do { \
    stack = fiber->data + fiber->frame; \
    pc = janet_stack_frame(stack)->pc; \
    func = janet_stack_frame(stack)->func; \
} while (0)
#define vm_return(sig, val) do { \
    janet_vm_return_reg[0] = (val); \
    vm_commit(); \
    return (sig); \
} while (0)

/* Next instruction variations */
#define maybe_collect() do {\
    if (janet_vm_next_collection >= janet_vm_gc_interval) janet_collect(); } while (0)
#define vm_checkgc_next() maybe_collect(); vm_next()
#define vm_pcnext() pc++; vm_next()
#define vm_checkgc_pcnext() maybe_collect(); vm_pcnext()

/* Handle certain errors in main vm loop */
#define vm_throw(e) do { vm_commit(); janet_panic(e); } while (0)
#define vm_assert(cond, e) do {if (!(cond)) vm_throw((e)); } while (0)
#define vm_assert_type(X, T) do { \
    if (!(janet_checktype((X), (T)))) { \
        vm_commit(); \
        janet_panicf("expected %T, got %v", (1 << (T)), (X)); \
    } \
} while (0)
#define vm_assert_types(X, TS) do { \
    if (!(janet_checktypes((X), (TS)))) { \
        vm_commit(); \
        janet_panicf("expected %T, got %v", (TS), (X)); \
    } \
} while (0)

/* Templates for certain patterns in opcodes */
#define vm_binop_immediate(op)\
    {\
        Janet op1 = stack[B];\
        if (!janet_checktype(op1, JANET_NUMBER)) {\
            vm_commit();\
            Janet _argv[2] = { op1, janet_wrap_number(CS) };\
            stack[A] = janet_mcall(#op, 2, _argv);\
            vm_checkgc_pcnext();\
        } else {\
            double x1 = janet_unwrap_number(op1);\
            stack[A] = janet_wrap_number(x1 op CS);\
            vm_pcnext();\
        }\
    }
#define _vm_bitop_immediate(op, type1)\
    {\
        Janet op1 = stack[B];\
        if (!janet_checktype(op1, JANET_NUMBER)) {\
            vm_commit();\
            Janet _argv[2] = { op1, janet_wrap_number(CS) };\
            stack[A] = janet_mcall(#op, 2, _argv);\
            vm_checkgc_pcnext();\
        } else {\
            type1 x1 = (type1) janet_unwrap_integer(op1);\
            stack[A] = janet_wrap_integer(x1 op CS);\
            vm_pcnext();\
        }\
    }
#define vm_bitop_immediate(op) _vm_bitop_immediate(op, int32_t);
#define vm_bitopu_immediate(op) _vm_bitop_immediate(op, uint32_t);
#define _vm_binop(op, wrap)\
    {\
        Janet op1 = stack[B];\
        Janet op2 = stack[C];\
        if (janet_checktype(op1, JANET_NUMBER) && janet_checktype(op2, JANET_NUMBER)) {\
            double x1 = janet_unwrap_number(op1);\
            double x2 = janet_unwrap_number(op2);\
            stack[A] = wrap(x1 op x2);\
            vm_pcnext();\
        } else {\
            vm_commit();\
            stack[A] = janet_binop_call(#op, "r" #op, op1, op2);\
            vm_checkgc_pcnext();\
        }\
    }
#define vm_binop(op) _vm_binop(op, janet_wrap_number)
#define _vm_bitop(op, type1)\
    {\
        Janet op1 = stack[B];\
        Janet op2 = stack[C];\
        if (janet_checktype(op1, JANET_NUMBER) && janet_checktype(op2, JANET_NUMBER)) {\
            type1 x1 = (type1) janet_unwrap_integer(op1);\
            int32_t x2 = janet_unwrap_integer(op2);\
            stack[A] = janet_wrap_integer(x1 op x2);\
            vm_pcnext();\
        } else {\
            vm_commit();\
            stack[A] = janet_binop_call(#op, "r" #op, op1, op2);\
            vm_checkgc_pcnext();\
        }\
    }
#define vm_bitop(op) _vm_bitop(op, int32_t)
#define vm_bitopu(op) _vm_bitop(op, uint32_t)
#define vm_compop(op) \
    {\
        Janet op1 = stack[B];\
        Janet op2 = stack[C];\
        if (janet_checktype(op1, JANET_NUMBER) && janet_checktype(op2, JANET_NUMBER)) {\
            double x1 = janet_unwrap_number(op1);\
            double x2 = janet_unwrap_number(op2);\
            stack[A] = janet_wrap_boolean(x1 op x2);\
            vm_pcnext();\
        } else {\
            vm_commit();\
            stack[A] = janet_wrap_boolean(janet_compare(op1, op2) op 0);\
            vm_checkgc_pcnext();\
        }\
    }

/* Trace a function call */
static void vm_do_trace(JanetFunction *func, int32_t argc, const Janet *argv) {
    if (func->def->name) {
        janet_printf("trace (%S", func->def->name);
    } else {
        janet_printf("trace (%p", janet_wrap_function(func));
    }
    for (int32_t i = 0; i < argc; i++) {
        janet_printf(" %p", argv[i]);
    }
    janet_printf(")\n");
}

/* Invoke a method once we have looked it up */
static Janet janet_method_invoke(Janet method, int32_t argc, Janet *argv) {
    switch (janet_type(method)) {
        case JANET_CFUNCTION:
            return (janet_unwrap_cfunction(method))(argc, argv);
        case JANET_FUNCTION: {
            JanetFunction *fun = janet_unwrap_function(method);
            return janet_call(fun, argc, argv);
        }
        case JANET_ABSTRACT: {
            JanetAbstract abst = janet_unwrap_abstract(method);
            const JanetAbstractType *at = janet_abstract_type(abst);
            if (NULL != at->call) {
                return at->call(abst, argc, argv);
            }
        }
        /* fallthrough */
        case JANET_STRING:
        case JANET_BUFFER:
        case JANET_TABLE:
        case JANET_STRUCT:
        case JANET_ARRAY:
        case JANET_TUPLE: {
            if (argc != 1) {
                janet_panicf("%v called with %d arguments, possibly expected 1", method, argc);
            }
            return janet_in(method, argv[0]);
        }
        default: {
            if (argc != 1) {
                janet_panicf("%v called with %d arguments, possibly expected 1", method, argc);
            }
            return janet_in(argv[0], method);
        }
    }
}

/* Call a non function type from a JOP_CALL or JOP_TAILCALL instruction.
 * Assumes that the arguments are on the fiber stack. */
static Janet call_nonfn(JanetFiber *fiber, Janet callee) {
    int32_t argc = fiber->stacktop - fiber->stackstart;
    fiber->stacktop = fiber->stackstart;
    return janet_method_invoke(callee, argc, fiber->data + fiber->stacktop);
}

/* Get a callable from a keyword method name and ensure that it is valid. */
static Janet resolve_method(Janet name, JanetFiber *fiber) {
    int32_t argc = fiber->stacktop - fiber->stackstart;
    if (argc < 1) janet_panicf("method call (%v) takes at least 1 argument, got 0", name);
    Janet callee = janet_get(fiber->data[fiber->stackstart], name);
    if (janet_checktype(callee, JANET_NIL))
        janet_panicf("unknown method %v invoked on %v", name, fiber->data[fiber->stackstart]);
    return callee;
}

/* Lookup method on value x */
static Janet janet_method_lookup(Janet x, const char *name) {
    Janet kname = janet_ckeywordv(name);
    return janet_get(x, kname);
}

/* Call a method first on the righthand side, and then on the left hand side with a prefix */
static Janet janet_binop_call(const char *lmethod, const char *rmethod, Janet lhs, Janet rhs) {
    Janet lm = janet_method_lookup(lhs, lmethod);
    if (janet_checktype(lm, JANET_NIL)) {
        /* Invert order for rmethod */
        Janet lr = janet_method_lookup(rhs, rmethod);
        Janet argv[2] = { rhs, lhs };
        if (janet_checktype(lr, JANET_NIL)) {
            janet_panicf("could not find method :%s for %v, or :%s for %v",
                         lmethod, lhs,
                         rmethod, rhs);
        }
        return janet_method_invoke(lr, 2, argv);
    } else {
        Janet argv[2] = { lhs, rhs };
        return janet_method_invoke(lm, 2, argv);
    }
}

/* Forward declaration */
static JanetSignal janet_check_can_resume(JanetFiber *fiber, Janet *out);
static JanetSignal janet_continue_no_check(JanetFiber *fiber, Janet in, Janet *out);

/* Interpreter main loop */
static JanetSignal run_vm(JanetFiber *fiber, Janet in) {

    /* opcode -> label lookup if using clang/GCC */
#ifdef JANET_USE_COMPUTED_GOTOS
    static void *op_lookup[255] = {
        &&label_JOP_NOOP,
        &&label_JOP_ERROR,
        &&label_JOP_TYPECHECK,
        &&label_JOP_RETURN,
        &&label_JOP_RETURN_NIL,
        &&label_JOP_ADD_IMMEDIATE,
        &&label_JOP_ADD,
        &&label_JOP_SUBTRACT,
        &&label_JOP_MULTIPLY_IMMEDIATE,
        &&label_JOP_MULTIPLY,
        &&label_JOP_DIVIDE_IMMEDIATE,
        &&label_JOP_DIVIDE,
        &&label_JOP_MODULO,
        &&label_JOP_REMAINDER,
        &&label_JOP_BAND,
        &&label_JOP_BOR,
        &&label_JOP_BXOR,
        &&label_JOP_BNOT,
        &&label_JOP_SHIFT_LEFT,
        &&label_JOP_SHIFT_LEFT_IMMEDIATE,
        &&label_JOP_SHIFT_RIGHT,
        &&label_JOP_SHIFT_RIGHT_IMMEDIATE,
        &&label_JOP_SHIFT_RIGHT_UNSIGNED,
        &&label_JOP_SHIFT_RIGHT_UNSIGNED_IMMEDIATE,
        &&label_JOP_MOVE_FAR,
        &&label_JOP_MOVE_NEAR,
        &&label_JOP_JUMP,
        &&label_JOP_JUMP_IF,
        &&label_JOP_JUMP_IF_NOT,
        &&label_JOP_JUMP_IF_NIL,
        &&label_JOP_JUMP_IF_NOT_NIL,
        &&label_JOP_GREATER_THAN,
        &&label_JOP_GREATER_THAN_IMMEDIATE,
        &&label_JOP_LESS_THAN,
        &&label_JOP_LESS_THAN_IMMEDIATE,
        &&label_JOP_EQUALS,
        &&label_JOP_EQUALS_IMMEDIATE,
        &&label_JOP_COMPARE,
        &&label_JOP_LOAD_NIL,
        &&label_JOP_LOAD_TRUE,
        &&label_JOP_LOAD_FALSE,
        &&label_JOP_LOAD_INTEGER,
        &&label_JOP_LOAD_CONSTANT,
        &&label_JOP_LOAD_UPVALUE,
        &&label_JOP_LOAD_SELF,
        &&label_JOP_SET_UPVALUE,
        &&label_JOP_CLOSURE,
        &&label_JOP_PUSH,
        &&label_JOP_PUSH_2,
        &&label_JOP_PUSH_3,
        &&label_JOP_PUSH_ARRAY,
        &&label_JOP_CALL,
        &&label_JOP_TAILCALL,
        &&label_JOP_RESUME,
        &&label_JOP_SIGNAL,
        &&label_JOP_PROPAGATE,
        &&label_JOP_IN,
        &&label_JOP_GET,
        &&label_JOP_PUT,
        &&label_JOP_GET_INDEX,
        &&label_JOP_PUT_INDEX,
        &&label_JOP_LENGTH,
        &&label_JOP_MAKE_ARRAY,
        &&label_JOP_MAKE_BUFFER,
        &&label_JOP_MAKE_STRING,
        &&label_JOP_MAKE_STRUCT,
        &&label_JOP_MAKE_TABLE,
        &&label_JOP_MAKE_TUPLE,
        &&label_JOP_MAKE_BRACKET_TUPLE,
        &&label_JOP_GREATER_THAN_EQUAL,
        &&label_JOP_LESS_THAN_EQUAL,
        &&label_JOP_NEXT,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op
    };
#endif

    /* Interpreter state */
    register Janet *stack;
    register uint32_t *pc;
    register JanetFunction *func;
    vm_restore();

    if (fiber->flags & JANET_FIBER_DID_LONGJUMP) {
        if (janet_fiber_frame(fiber)->func == NULL) {
            /* Inside a c function */
            janet_fiber_popframe(fiber);
            vm_restore();
        }
        /* Check if we were at a tail call instruction. If so, do implicit return */
        if ((*pc & 0xFF) == JOP_TAILCALL) {
            /* Tail call resume */
            int entrance_frame = janet_stack_frame(stack)->flags & JANET_STACKFRAME_ENTRANCE;
            janet_fiber_popframe(fiber);
            if (entrance_frame) {
                fiber->flags &= ~JANET_FIBER_FLAG_MASK;
                vm_return(JANET_SIGNAL_OK, in);
            }
            vm_restore();
        }
    }

    if (!(fiber->flags & JANET_FIBER_RESUME_NO_USEVAL)) stack[A] = in;
    if (!(fiber->flags & JANET_FIBER_RESUME_NO_SKIP)) pc++;

    uint8_t first_opcode = *pc & ((fiber->flags & JANET_FIBER_BREAKPOINT) ? 0x7F : 0xFF);

    fiber->flags &= ~JANET_FIBER_FLAG_MASK;

    /* Main interpreter loop. Semantically is a switch on
     * (*pc & 0xFF) inside of an infinite loop. */
    VM_START();

    VM_DEFAULT();
    fiber->flags |= JANET_FIBER_BREAKPOINT | JANET_FIBER_RESUME_NO_USEVAL | JANET_FIBER_RESUME_NO_SKIP;
    vm_return(JANET_SIGNAL_DEBUG, janet_wrap_nil());

    VM_OP(JOP_NOOP)
    vm_pcnext();

    VM_OP(JOP_ERROR)
    vm_return(JANET_SIGNAL_ERROR, stack[A]);

    VM_OP(JOP_TYPECHECK)
    vm_assert_types(stack[A], E);
    vm_pcnext();

    VM_OP(JOP_RETURN) {
        Janet retval = stack[D];
        int entrance_frame = janet_stack_frame(stack)->flags & JANET_STACKFRAME_ENTRANCE;
        janet_fiber_popframe(fiber);
        if (entrance_frame) vm_return(JANET_SIGNAL_OK, retval);
        vm_restore();
        stack[A] = retval;
        vm_checkgc_pcnext();
    }

    VM_OP(JOP_RETURN_NIL) {
        Janet retval = janet_wrap_nil();
        int entrance_frame = janet_stack_frame(stack)->flags & JANET_STACKFRAME_ENTRANCE;
        janet_fiber_popframe(fiber);
        if (entrance_frame) vm_return(JANET_SIGNAL_OK, retval);
        vm_restore();
        stack[A] = retval;
        vm_checkgc_pcnext();
    }

    VM_OP(JOP_ADD_IMMEDIATE)
    vm_binop_immediate(+);

    VM_OP(JOP_ADD)
    vm_binop(+);

    VM_OP(JOP_SUBTRACT)
    vm_binop(-);

    VM_OP(JOP_MULTIPLY_IMMEDIATE)
    vm_binop_immediate(*);

    VM_OP(JOP_MULTIPLY)
    vm_binop(*);

    VM_OP(JOP_DIVIDE_IMMEDIATE)
    vm_binop_immediate( /);

    VM_OP(JOP_DIVIDE)
    vm_binop( /);

    VM_OP(JOP_MODULO) {
        Janet op1 = stack[B];
        Janet op2 = stack[C];
        if (janet_checktype(op1, JANET_NUMBER) && janet_checktype(op2, JANET_NUMBER)) {
            double x1 = janet_unwrap_number(op1);
            double x2 = janet_unwrap_number(op2);
            double intres = x2 * floor(x1 / x2);
            stack[A] = janet_wrap_number(x1 - intres);
            vm_pcnext();
        } else {
            vm_commit();
            stack[A] = janet_binop_call("mod", "rmod", op1, op2);
            vm_checkgc_pcnext();
        }
    }

    VM_OP(JOP_REMAINDER) {
        Janet op1 = stack[B];
        Janet op2 = stack[C];
        if (janet_checktype(op1, JANET_NUMBER) && janet_checktype(op2, JANET_NUMBER)) {
            double x1 = janet_unwrap_number(op1);
            double x2 = janet_unwrap_number(op2);
            stack[A] = janet_wrap_number(fmod(x1, x2));
            vm_pcnext();
        } else {
            vm_commit();
            stack[A] = janet_binop_call("%", "r%", op1, op2);
            vm_checkgc_pcnext();
        }
    }

    VM_OP(JOP_BAND)
    vm_bitop(&);

    VM_OP(JOP_BOR)
    vm_bitop( |);

    VM_OP(JOP_BXOR)
    vm_bitop(^);

    VM_OP(JOP_BNOT) {
        Janet op = stack[E];
        vm_assert_type(op, JANET_NUMBER);
        stack[A] = janet_wrap_integer(~janet_unwrap_integer(op));
        vm_pcnext();
    }

    VM_OP(JOP_SHIFT_RIGHT_UNSIGNED)
    vm_bitopu( >>);

    VM_OP(JOP_SHIFT_RIGHT_UNSIGNED_IMMEDIATE)
    vm_bitopu_immediate( >>);

    VM_OP(JOP_SHIFT_RIGHT)
    vm_bitop( >>);

    VM_OP(JOP_SHIFT_RIGHT_IMMEDIATE)
    vm_bitop_immediate( >>);

    VM_OP(JOP_SHIFT_LEFT)
    vm_bitop( <<);

    VM_OP(JOP_SHIFT_LEFT_IMMEDIATE)
    vm_bitop_immediate( <<);

    VM_OP(JOP_MOVE_NEAR)
    stack[A] = stack[E];
    vm_pcnext();

    VM_OP(JOP_MOVE_FAR)
    stack[E] = stack[A];
    vm_pcnext();

    VM_OP(JOP_JUMP)
    pc += DS;
    vm_next();

    VM_OP(JOP_JUMP_IF)
    if (janet_truthy(stack[A])) {
        pc += ES;
    } else {
        pc++;
    }
    vm_next();

    VM_OP(JOP_JUMP_IF_NOT)
    if (janet_truthy(stack[A])) {
        pc++;
    } else {
        pc += ES;
    }
    vm_next();

    VM_OP(JOP_JUMP_IF_NIL)
    if (janet_checktype(stack[A], JANET_NIL)) {
        pc += ES;
    } else {
        pc++;
    }
    vm_next();

    VM_OP(JOP_JUMP_IF_NOT_NIL)
    if (janet_checktype(stack[A], JANET_NIL)) {
        pc++;
    } else {
        pc += ES;
    }
    vm_next();

    VM_OP(JOP_LESS_THAN)
    vm_compop( <);

    VM_OP(JOP_LESS_THAN_EQUAL)
    vm_compop( <=);

    VM_OP(JOP_LESS_THAN_IMMEDIATE)
    stack[A] = janet_wrap_boolean(janet_unwrap_integer(stack[B]) < CS);
    vm_pcnext();

    VM_OP(JOP_GREATER_THAN)
    vm_compop( >);

    VM_OP(JOP_GREATER_THAN_EQUAL)
    vm_compop( >=);

    VM_OP(JOP_GREATER_THAN_IMMEDIATE)
    stack[A] = janet_wrap_boolean(janet_unwrap_integer(stack[B]) > CS);
    vm_pcnext();

    VM_OP(JOP_EQUALS)
    stack[A] = janet_wrap_boolean(janet_equals(stack[B], stack[C]));
    vm_pcnext();

    VM_OP(JOP_EQUALS_IMMEDIATE)
    stack[A] = janet_wrap_boolean(janet_unwrap_integer(stack[B]) == CS);
    vm_pcnext();

    VM_OP(JOP_COMPARE)
    stack[A] = janet_wrap_integer(janet_compare(stack[B], stack[C]));
    vm_pcnext();

    VM_OP(JOP_NEXT)
    stack[A] = janet_next(stack[B], stack[C]);
    vm_pcnext();

    VM_OP(JOP_LOAD_NIL)
    stack[D] = janet_wrap_nil();
    vm_pcnext();

    VM_OP(JOP_LOAD_TRUE)
    stack[D] = janet_wrap_true();
    vm_pcnext();

    VM_OP(JOP_LOAD_FALSE)
    stack[D] = janet_wrap_false();
    vm_pcnext();

    VM_OP(JOP_LOAD_INTEGER)
    stack[A] = janet_wrap_integer(ES);
    vm_pcnext();

    VM_OP(JOP_LOAD_CONSTANT) {
        int32_t cindex = (int32_t)E;
        vm_assert(cindex < func->def->constants_length, "invalid constant");
        stack[A] = func->def->constants[cindex];
        vm_pcnext();
    }

    VM_OP(JOP_LOAD_SELF)
    stack[D] = janet_wrap_function(func);
    vm_pcnext();

    VM_OP(JOP_LOAD_UPVALUE) {
        int32_t eindex = B;
        int32_t vindex = C;
        JanetFuncEnv *env;
        vm_assert(func->def->environments_length > eindex, "invalid upvalue environment");
        env = func->envs[eindex];
        vm_assert(env->length > vindex, "invalid upvalue index");
        vm_assert(janet_env_valid(env), "invalid upvalue environment");
        if (env->offset > 0) {
            /* On stack */
            stack[A] = env->as.fiber->data[env->offset + vindex];
        } else {
            /* Off stack */
            stack[A] = env->as.values[vindex];
        }
        vm_pcnext();
    }

    VM_OP(JOP_SET_UPVALUE) {
        int32_t eindex = B;
        int32_t vindex = C;
        JanetFuncEnv *env;
        vm_assert(func->def->environments_length > eindex, "invalid upvalue environment");
        env = func->envs[eindex];
        vm_assert(env->length > vindex, "invalid upvalue index");
        vm_assert(janet_env_valid(env), "invalid upvalue environment");
        if (env->offset > 0) {
            env->as.fiber->data[env->offset + vindex] = stack[A];
        } else {
            env->as.values[vindex] = stack[A];
        }
        vm_pcnext();
    }

    VM_OP(JOP_CLOSURE) {
        JanetFuncDef *fd;
        JanetFunction *fn;
        int32_t elen;
        int32_t defindex = (int32_t)E;
        vm_assert(defindex < func->def->defs_length, "invalid funcdef");
        fd = func->def->defs[defindex];
        elen = fd->environments_length;
        fn = janet_gcalloc(JANET_MEMORY_FUNCTION, sizeof(JanetFunction) + ((size_t) elen * sizeof(JanetFuncEnv *)));
        fn->def = fd;
        {
            int32_t i;
            for (i = 0; i < elen; ++i) {
                int32_t inherit = fd->environments[i];
                if (inherit == -1) {
                    JanetStackFrame *frame = janet_stack_frame(stack);
                    if (!frame->env) {
                        /* Lazy capture of current stack frame */
                        JanetFuncEnv *env = janet_gcalloc(JANET_MEMORY_FUNCENV, sizeof(JanetFuncEnv));
                        env->offset = fiber->frame;
                        env->as.fiber = fiber;
                        env->length = func->def->slotcount;
                        frame->env = env;
                    }
                    fn->envs[i] = frame->env;
                } else {
                    fn->envs[i] = func->envs[inherit];
                }
            }
        }
        stack[A] = janet_wrap_function(fn);
        vm_checkgc_pcnext();
    }

    VM_OP(JOP_PUSH)
    janet_fiber_push(fiber, stack[D]);
    stack = fiber->data + fiber->frame;
    vm_checkgc_pcnext();

    VM_OP(JOP_PUSH_2)
    janet_fiber_push2(fiber, stack[A], stack[E]);
    stack = fiber->data + fiber->frame;
    vm_checkgc_pcnext();

    VM_OP(JOP_PUSH_3)
    janet_fiber_push3(fiber, stack[A], stack[B], stack[C]);
    stack = fiber->data + fiber->frame;
    vm_checkgc_pcnext();

    VM_OP(JOP_PUSH_ARRAY) {
        const Janet *vals;
        int32_t len;
        if (janet_indexed_view(stack[D], &vals, &len)) {
            janet_fiber_pushn(fiber, vals, len);
        } else {
            janet_panicf("expected %T, got %v", JANET_TFLAG_INDEXED, stack[D]);
        }
    }
    stack = fiber->data + fiber->frame;
    vm_checkgc_pcnext();

    VM_OP(JOP_CALL) {
        Janet callee = stack[E];
        if (fiber->stacktop > fiber->maxstack) {
            vm_throw("stack overflow");
        }
        if (janet_checktype(callee, JANET_KEYWORD)) {
            vm_commit();
            callee = resolve_method(callee, fiber);
        }
        if (janet_checktype(callee, JANET_FUNCTION)) {
            func = janet_unwrap_function(callee);
            if (func->gc.flags & JANET_FUNCFLAG_TRACE) {
                vm_do_trace(func, fiber->stacktop - fiber->stackstart, stack);
            }
            janet_stack_frame(stack)->pc = pc;
            if (janet_fiber_funcframe(fiber, func)) {
                int32_t n = fiber->stacktop - fiber->stackstart;
                janet_panicf("%v called with %d argument%s, expected %d",
                             callee, n, n == 1 ? "" : "s", func->def->arity);
            }
            stack = fiber->data + fiber->frame;
            pc = func->def->bytecode;
            vm_checkgc_next();
        } else if (janet_checktype(callee, JANET_CFUNCTION)) {
            vm_commit();
            int32_t argc = fiber->stacktop - fiber->stackstart;
            janet_fiber_cframe(fiber, janet_unwrap_cfunction(callee));
            Janet ret = janet_unwrap_cfunction(callee)(argc, fiber->data + fiber->frame);
            janet_fiber_popframe(fiber);
            stack = fiber->data + fiber->frame;
            stack[A] = ret;
            vm_checkgc_pcnext();
        } else {
            vm_commit();
            stack[A] = call_nonfn(fiber, callee);
            vm_pcnext();
        }
    }

    VM_OP(JOP_TAILCALL) {
        Janet callee = stack[D];
        if (fiber->stacktop > fiber->maxstack) {
            vm_throw("stack overflow");
        }
        if (janet_checktype(callee, JANET_KEYWORD)) {
            vm_commit();
            callee = resolve_method(callee, fiber);
        }
        if (janet_checktype(callee, JANET_FUNCTION)) {
            func = janet_unwrap_function(callee);
            if (func->gc.flags & JANET_FUNCFLAG_TRACE) {
                vm_do_trace(func, fiber->stacktop - fiber->stackstart, stack);
            }
            if (janet_fiber_funcframe_tail(fiber, func)) {
                janet_stack_frame(fiber->data + fiber->frame)->pc = pc;
                int32_t n = fiber->stacktop - fiber->stackstart;
                janet_panicf("%v called with %d argument%s, expected %d",
                             callee, n, n == 1 ? "" : "s", func->def->arity);
            }
            stack = fiber->data + fiber->frame;
            pc = func->def->bytecode;
            vm_checkgc_next();
        } else {
            Janet retreg;
            int entrance_frame = janet_stack_frame(stack)->flags & JANET_STACKFRAME_ENTRANCE;
            vm_commit();
            if (janet_checktype(callee, JANET_CFUNCTION)) {
                int32_t argc = fiber->stacktop - fiber->stackstart;
                janet_fiber_cframe(fiber, janet_unwrap_cfunction(callee));
                retreg = janet_unwrap_cfunction(callee)(argc, fiber->data + fiber->frame);
                janet_fiber_popframe(fiber);
            } else {
                retreg = call_nonfn(fiber, callee);
            }
            janet_fiber_popframe(fiber);
            if (entrance_frame)
                vm_return(JANET_SIGNAL_OK, retreg);
            vm_restore();
            stack[A] = retreg;
            vm_checkgc_pcnext();
        }
    }

    VM_OP(JOP_RESUME) {
        Janet retreg;
        vm_assert_type(stack[B], JANET_FIBER);
        JanetFiber *child = janet_unwrap_fiber(stack[B]);
        if (janet_check_can_resume(child, &retreg)) {
            vm_commit();
            janet_panicv(retreg);
        }
        fiber->child = child;
        JanetSignal sig = janet_continue_no_check(child, stack[C], &retreg);
        if (sig != JANET_SIGNAL_OK && !(child->flags & (1 << sig))) {
            vm_return(sig, retreg);
        }
        fiber->child = NULL;
        stack = fiber->data + fiber->frame;
        stack[A] = retreg;
        vm_checkgc_pcnext();
    }

    VM_OP(JOP_SIGNAL) {
        int32_t s = C;
        if (s > JANET_SIGNAL_USER9) s = JANET_SIGNAL_USER9;
        if (s < 0) s = 0;
        vm_return(s, stack[B]);
    }

    VM_OP(JOP_PROPAGATE) {
        Janet fv = stack[C];
        vm_assert_type(fv, JANET_FIBER);
        JanetFiber *f = janet_unwrap_fiber(fv);
        JanetFiberStatus sub_status = janet_fiber_status(f);
        if (sub_status > JANET_STATUS_USER9) {
            vm_commit();
            janet_panicf("cannot propagate from fiber with status :%s",
                         janet_status_names[sub_status]);
        }
        fiber->child = f;
        vm_return((int) sub_status, stack[B]);
    }

    VM_OP(JOP_PUT)
    vm_commit();
    fiber->flags |= JANET_FIBER_RESUME_NO_USEVAL;
    janet_put(stack[A], stack[B], stack[C]);
    fiber->flags &= ~JANET_FIBER_RESUME_NO_USEVAL;
    vm_checkgc_pcnext();

    VM_OP(JOP_PUT_INDEX)
    vm_commit();
    fiber->flags |= JANET_FIBER_RESUME_NO_USEVAL;
    janet_putindex(stack[A], C, stack[B]);
    fiber->flags &= ~JANET_FIBER_RESUME_NO_USEVAL;
    vm_checkgc_pcnext();

    VM_OP(JOP_IN)
    vm_commit();
    stack[A] = janet_in(stack[B], stack[C]);
    vm_pcnext();

    VM_OP(JOP_GET)
    vm_commit();
    stack[A] = janet_get(stack[B], stack[C]);
    vm_pcnext();

    VM_OP(JOP_GET_INDEX)
    vm_commit();
    stack[A] = janet_getindex(stack[B], C);
    vm_pcnext();

    VM_OP(JOP_LENGTH)
    vm_commit();
    stack[A] = janet_lengthv(stack[E]);
    vm_pcnext();

    VM_OP(JOP_MAKE_ARRAY) {
        int32_t count = fiber->stacktop - fiber->stackstart;
        Janet *mem = fiber->data + fiber->stackstart;
        stack[D] = janet_wrap_array(janet_array_n(mem, count));
        fiber->stacktop = fiber->stackstart;
        vm_checkgc_pcnext();
    }

    VM_OP(JOP_MAKE_TUPLE)
    /* fallthrough */
    VM_OP(JOP_MAKE_BRACKET_TUPLE) {
        int32_t count = fiber->stacktop - fiber->stackstart;
        Janet *mem = fiber->data + fiber->stackstart;
        const Janet *tup = janet_tuple_n(mem, count);
        if (opcode == JOP_MAKE_BRACKET_TUPLE)
            janet_tuple_flag(tup) |= JANET_TUPLE_FLAG_BRACKETCTOR;
        stack[D] = janet_wrap_tuple(tup);
        fiber->stacktop = fiber->stackstart;
        vm_checkgc_pcnext();
    }

    VM_OP(JOP_MAKE_TABLE) {
        int32_t count = fiber->stacktop - fiber->stackstart;
        Janet *mem = fiber->data + fiber->stackstart;
        if (count & 1) {
            vm_commit();
            janet_panicf("expected even number of arguments to table constructor, got %d", count);
        }
        JanetTable *table = janet_table(count / 2);
        for (int32_t i = 0; i < count; i += 2)
            janet_table_put(table, mem[i], mem[i + 1]);
        stack[D] = janet_wrap_table(table);
        fiber->stacktop = fiber->stackstart;
        vm_checkgc_pcnext();
    }

    VM_OP(JOP_MAKE_STRUCT) {
        int32_t count = fiber->stacktop - fiber->stackstart;
        Janet *mem = fiber->data + fiber->stackstart;
        if (count & 1) {
            vm_commit();
            janet_panicf("expected even number of arguments to struct constructor, got %d", count);
        }
        JanetKV *st = janet_struct_begin(count / 2);
        for (int32_t i = 0; i < count; i += 2)
            janet_struct_put(st, mem[i], mem[i + 1]);
        stack[D] = janet_wrap_struct(janet_struct_end(st));
        fiber->stacktop = fiber->stackstart;
        vm_checkgc_pcnext();
    }

    VM_OP(JOP_MAKE_STRING) {
        int32_t count = fiber->stacktop - fiber->stackstart;
        Janet *mem = fiber->data + fiber->stackstart;
        JanetBuffer buffer;
        janet_buffer_init(&buffer, 10 * count);
        for (int32_t i = 0; i < count; i++)
            janet_to_string_b(&buffer, mem[i]);
        stack[D] = janet_stringv(buffer.data, buffer.count);
        janet_buffer_deinit(&buffer);
        fiber->stacktop = fiber->stackstart;
        vm_checkgc_pcnext();
    }

    VM_OP(JOP_MAKE_BUFFER) {
        int32_t count = fiber->stacktop - fiber->stackstart;
        Janet *mem = fiber->data + fiber->stackstart;
        JanetBuffer *buffer = janet_buffer(10 * count);
        for (int32_t i = 0; i < count; i++)
            janet_to_string_b(buffer, mem[i]);
        stack[D] = janet_wrap_buffer(buffer);
        fiber->stacktop = fiber->stackstart;
        vm_checkgc_pcnext();
    }

    VM_END()
}

/*
 * Execute a single instruction in the fiber. Does this by inspecting
 * the fiber, setting a breakpoint at the next instruction, executing, and
 * reseting breakpoints to how they were prior. Yes, it's a bit hacky.
 */
JanetSignal janet_step(JanetFiber *fiber, Janet in, Janet *out) {
    /* No finished or currently alive fibers. */
    JanetFiberStatus status = janet_fiber_status(fiber);
    if (status == JANET_STATUS_ALIVE ||
            status == JANET_STATUS_DEAD ||
            status == JANET_STATUS_ERROR) {
        janet_panicf("cannot step fiber with status :%s", janet_status_names[status]);
    }

    /* Get PC for setting breakpoints */
    uint32_t *pc = janet_stack_frame(fiber->data + fiber->frame)->pc;

    /* Check current opcode (sans debug flag). This tells us where the next or next two candidate
     * instructions will be. Usually it's the next instruction in memory,
     * but for branching instructions it is also the target of the branch. */
    uint32_t *nexta = NULL, *nextb = NULL, olda = 0, oldb = 0;

    /* Set temporary breakpoints */
    switch (*pc & 0x7F) {
        default:
            nexta = pc + 1;
            break;
        /* These we just ignore for now. Supporting them means
         * we could step into and out of functions (including JOP_CALL). */
        case JOP_RETURN_NIL:
        case JOP_RETURN:
        case JOP_ERROR:
        case JOP_TAILCALL:
            break;
        case JOP_JUMP:
            nexta = pc + DS;
            break;
        case JOP_JUMP_IF:
        case JOP_JUMP_IF_NOT:
            nexta = pc + 1;
            nextb = pc + ES;
            break;
    }
    if (nexta) {
        olda = *nexta;
        *nexta |= 0x80;
    }
    if (nextb) {
        oldb = *nextb;
        *nextb |= 0x80;
    }

    /* Go */
    JanetSignal signal = janet_continue(fiber, in, out);

    /* Restore */
    if (nexta) *nexta = olda;
    if (nextb) *nextb = oldb;

    return signal;
}

Janet janet_call(JanetFunction *fun, int32_t argc, const Janet *argv) {
    /* Check entry conditions */
    if (!janet_vm_fiber)
        janet_panic("janet_call failed because there is no current fiber");
    if (janet_vm_stackn >= JANET_RECURSION_GUARD)
        janet_panic("C stack recursed too deeply");

    /* Tracing */
    if (fun->gc.flags & JANET_FUNCFLAG_TRACE) {
        vm_do_trace(fun, argc, argv);
    }

    /* Push frame */
    janet_fiber_pushn(janet_vm_fiber, argv, argc);
    if (janet_fiber_funcframe(janet_vm_fiber, fun)) {
        janet_panicf("arity mismatch in %v", janet_wrap_function(fun));
    }
    janet_fiber_frame(janet_vm_fiber)->flags |= JANET_STACKFRAME_ENTRANCE;

    /* Set up */
    int32_t oldn = janet_vm_stackn++;
    int handle = janet_gclock();

    /* Run vm */
    janet_vm_fiber->flags |= JANET_FIBER_RESUME_NO_USEVAL | JANET_FIBER_RESUME_NO_SKIP;
    JanetSignal signal = run_vm(janet_vm_fiber, janet_wrap_nil());

    /* Teardown */
    janet_vm_stackn = oldn;
    janet_gcunlock(handle);

    if (signal != JANET_SIGNAL_OK) {
        janet_panicv(*janet_vm_return_reg);
    }

    return *janet_vm_return_reg;
}

static JanetSignal janet_check_can_resume(JanetFiber *fiber, Janet *out) {
    /* Check conditions */
    JanetFiberStatus old_status = janet_fiber_status(fiber);
    if (janet_vm_stackn >= JANET_RECURSION_GUARD) {
        janet_fiber_set_status(fiber, JANET_STATUS_ERROR);
        *out = janet_cstringv("C stack recursed too deeply");
        return JANET_SIGNAL_ERROR;
    }
    if (old_status == JANET_STATUS_ALIVE ||
            old_status == JANET_STATUS_DEAD ||
            (old_status >= JANET_STATUS_USER0 && old_status <= JANET_STATUS_USER4) ||
            old_status == JANET_STATUS_ERROR) {
        const uint8_t *str = janet_formatc("cannot resume fiber with status :%s",
                                           janet_status_names[old_status]);
        *out = janet_wrap_string(str);
        return JANET_SIGNAL_ERROR;
    }
    return JANET_SIGNAL_OK;
}

static JanetSignal janet_continue_no_check(JanetFiber *fiber, Janet in, Janet *out) {
    jmp_buf buf;

    JanetFiberStatus old_status = janet_fiber_status(fiber);

    /* Continue child fiber if it exists */
    if (fiber->child) {
        if (janet_vm_root_fiber == NULL) janet_vm_root_fiber = fiber;
        JanetFiber *child = fiber->child;
        janet_vm_stackn++;
        JanetSignal sig = janet_continue(child, in, &in);
        janet_vm_stackn--;
        if (janet_vm_root_fiber == fiber) janet_vm_root_fiber = NULL;
        if (sig != JANET_SIGNAL_OK && !(child->flags & (1 << sig))) {
            *out = in;
            return sig;
        }
        fiber->child = NULL;
    }

    /* Handle new fibers being resumed with a non-nil value */
    if (old_status == JANET_STATUS_NEW && !janet_checktype(in, JANET_NIL)) {
        Janet *stack = fiber->data + fiber->frame;
        JanetFunction *func = janet_stack_frame(stack)->func;
        if (func) {
            if (func->def->arity > 0) {
                stack[0] = in;
            } else if (func->def->flags & JANET_FUNCDEF_FLAG_VARARG) {
                stack[0] = janet_wrap_tuple(janet_tuple_n(&in, 1));
            }
        }
    }

    /* Save global state */
    int32_t oldn = janet_vm_stackn++;
    int handle = janet_vm_gc_suspend;
    JanetFiber *old_vm_fiber = janet_vm_fiber;
    jmp_buf *old_vm_jmp_buf = janet_vm_jmp_buf;
    Janet *old_vm_return_reg = janet_vm_return_reg;

    /* Setup fiber */
    if (janet_vm_root_fiber == NULL) janet_vm_root_fiber = fiber;
    janet_vm_fiber = fiber;
    janet_gcroot(janet_wrap_fiber(fiber));
    janet_fiber_set_status(fiber, JANET_STATUS_ALIVE);
    janet_vm_return_reg = out;
    janet_vm_jmp_buf = &buf;

    /* Run loop */
    JanetSignal signal;
    int jmpsig;
#if defined(JANET_BSD) || defined(JANET_APPLE)
    jmpsig = _setjmp(buf);
#else
    jmpsig = setjmp(buf);
#endif
    if (jmpsig) {
        signal = (JanetSignal) jmpsig;
    } else {
        signal = run_vm(fiber, in);
    }

    /* Tear down fiber */
    janet_fiber_set_status(fiber, signal);
    janet_gcunroot(janet_wrap_fiber(fiber));

    /* Restore global state */
    if (janet_vm_root_fiber == fiber) janet_vm_root_fiber = NULL;
    janet_vm_gc_suspend = handle;
    janet_vm_fiber = old_vm_fiber;
    janet_vm_stackn = oldn;
    janet_vm_return_reg = old_vm_return_reg;
    janet_vm_jmp_buf = old_vm_jmp_buf;

    return signal;
}

/* Enter the main vm loop */
JanetSignal janet_continue(JanetFiber *fiber, Janet in, Janet *out) {
    /* Check conditions */
    JanetSignal tmp_signal = janet_check_can_resume(fiber, out);
    if (tmp_signal) return tmp_signal;
    return janet_continue_no_check(fiber, in, out);
}

JanetSignal janet_pcall(
    JanetFunction *fun,
    int32_t argc,
    const Janet *argv,
    Janet *out,
    JanetFiber **f) {
    JanetFiber *fiber;
    if (f && *f) {
        fiber = janet_fiber_reset(*f, fun, argc, argv);
    } else {
        fiber = janet_fiber(fun, 64, argc, argv);
    }
    if (f) *f = fiber;
    if (!fiber) {
        *out = janet_cstringv("arity mismatch");
        return JANET_SIGNAL_ERROR;
    }
    return janet_continue(fiber, janet_wrap_nil(), out);
}

Janet janet_mcall(const char *name, int32_t argc, Janet *argv) {
    /* At least 1 argument */
    if (argc < 1) janet_panicf("method :%s expected at least 1 argument");
    /* Find method */
    Janet method = janet_method_lookup(argv[0], name);
    if (janet_checktype(method, JANET_NIL)) {
        janet_panicf("could not find method :%s for %v", name, argv[0]);
    }
    /* Invoke method */
    return janet_method_invoke(method, argc, argv);
}

/* Setup VM */
int janet_init(void) {
    /* Garbage collection */
    janet_vm_blocks = NULL;
    janet_vm_next_collection = 0;
    /* Setting memoryInterval to zero forces
     * a collection pretty much every cycle, which is
     * incredibly horrible for performance, but can help ensure
     * there are no memory bugs during development */
    janet_vm_gc_interval = 0x10000;
    janet_symcache_init();
    /* Initialize gc roots */
    janet_vm_roots = NULL;
    janet_vm_root_count = 0;
    janet_vm_root_capacity = 0;
    /* Scratch memory */
    janet_scratch_mem = NULL;
    janet_scratch_len = 0;
    janet_scratch_cap = 0;
    /* Initialize registry */
    janet_vm_registry = janet_table(0);
    janet_vm_abstract_registry = janet_table(0);
    janet_gcroot(janet_wrap_table(janet_vm_registry));
    janet_gcroot(janet_wrap_table(janet_vm_abstract_registry));
    /* Core env */
    janet_vm_core_env = NULL;
    /* Seed RNG */
    janet_rng_seed(janet_default_rng(), 0);
    /* Fibers */
    janet_vm_fiber = NULL;
    janet_vm_root_fiber = NULL;
    janet_vm_stackn = 0;
    /* Threads */
#ifdef JANET_THREADS
    janet_threads_init();
#endif
    return 0;
}

/* Clear all memory associated with the VM */
void janet_deinit(void) {
    janet_clear_memory();
    janet_symcache_deinit();
    free(janet_vm_roots);
    janet_vm_roots = NULL;
    janet_vm_root_count = 0;
    janet_vm_root_capacity = 0;
    janet_vm_registry = NULL;
    janet_vm_abstract_registry = NULL;
    janet_vm_core_env = NULL;
    janet_vm_fiber = NULL;
    janet_vm_root_fiber = NULL;
#ifdef JANET_THREADS
    janet_threads_deinit();
#endif
#ifdef JANET_NET
    janet_net_deinit();
#endif
}
