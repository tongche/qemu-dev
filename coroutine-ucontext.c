/*
 * ucontext coroutine initialization code
 *
 * Copyright (C) 2006  Anthony Liguori <anthony@codemonkey.ws>
 * Copyright (C) 2011  Kevin Wolf <kwolf@redhat.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.0 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

/* XXX Is there a nicer way to disable glibc's stack check for longjmp? */
#ifdef _FORTIFY_SOURCE
#undef _FORTIFY_SOURCE
#endif
#include <stdlib.h>
#include <setjmp.h>
#include <stdint.h>
#include <pthread.h>
#include <ucontext.h>
#include "qemu-common.h"
#include "block/coroutine_int.h"

#ifdef CONFIG_VALGRIND_H
#include <valgrind/valgrind.h>
#endif

enum {
    /* Maximum free pool size prevents holding too many freed coroutines */
    POOL_MAX_SIZE = 64,
};

/** Free list to speed up creation */
static QSLIST_HEAD(, Coroutine) pool = QSLIST_HEAD_INITIALIZER(pool);
static unsigned int pool_size;

typedef struct {
    Coroutine base;
    void *stack;
    jmp_buf env;

#ifdef CONFIG_VALGRIND_H
    unsigned int valgrind_stack_id;
#endif

} CoroutineUContext;

/**
 * Per-thread coroutine bookkeeping
 */
typedef struct {
    /** Currently executing coroutine */
    Coroutine *current;

    /** The default coroutine */
    CoroutineUContext leader;
} CoroutineThreadState;

static pthread_key_t thread_state_key;

/*
 * va_args to makecontext() must be type 'int', so passing
 * the pointer we need may require several int args. This
 * union is a quick hack to let us do that
 */
union cc_arg {
    void *p;
    int i[2];
};

static CoroutineThreadState *coroutine_get_thread_state(void)
{
    CoroutineThreadState *s = pthread_getspecific(thread_state_key);

    if (!s) {
        s = g_malloc0(sizeof(*s));
        s->current = &s->leader.base;
        pthread_setspecific(thread_state_key, s);
    }
    return s;
}

static void qemu_coroutine_thread_cleanup(void *opaque)
{
    CoroutineThreadState *s = opaque;

    g_free(s);
}

static void __attribute__((destructor)) coroutine_cleanup(void)
{
    Coroutine *co;
    Coroutine *tmp;

    QSLIST_FOREACH_SAFE(co, &pool, pool_next, tmp) {
        g_free(DO_UPCAST(CoroutineUContext, base, co)->stack);
        g_free(co);
    }
}

static void __attribute__((constructor)) coroutine_init(void)
{
    int ret;

    ret = pthread_key_create(&thread_state_key, qemu_coroutine_thread_cleanup);
    if (ret != 0) {
        fprintf(stderr, "unable to create leader key: %s\n", strerror(errno));
        abort();
    }
}

static void coroutine_trampoline(int i0, int i1)
{
    union cc_arg arg;
    CoroutineUContext *self;
    Coroutine *co;

    arg.i[0] = i0;
    arg.i[1] = i1;
    self = arg.p;
    co = &self->base;

    /* Initialize longjmp environment and switch back the caller */
    if (!setjmp(self->env)) {
        longjmp(*(jmp_buf *)co->entry_arg, 1);
    }

    while (true) {
        co->entry(co->entry_arg);
        qemu_coroutine_switch(co, co->caller, COROUTINE_TERMINATE);
    }
}

static Coroutine *coroutine_new(void)
{
    const size_t stack_size = 1 << 20;
    CoroutineUContext *co;
    ucontext_t old_uc, uc;
    jmp_buf old_env;
    union cc_arg arg = {0};

    /* The ucontext functions preserve signal masks which incurs a system call
     * overhead.  setjmp()/longjmp() does not preserve signal masks but only
     * works on the current stack.  Since we need a way to create and switch to
     * a new stack, use the ucontext functions for that but setjmp()/longjmp()
     * for everything else.
     */

    if (getcontext(&uc) == -1) {
        abort();
    }

    co = g_malloc0(sizeof(*co));
    co->stack = g_malloc(stack_size);
    co->base.entry_arg = &old_env; /* stash away our jmp_buf */

    uc.uc_link = &old_uc;
    uc.uc_stack.ss_sp = co->stack;
    uc.uc_stack.ss_size = stack_size;
    uc.uc_stack.ss_flags = 0;

#ifdef CONFIG_VALGRIND_H
    co->valgrind_stack_id =
        VALGRIND_STACK_REGISTER(co->stack, co->stack + stack_size);
#endif

    arg.p = co;

    makecontext(&uc, (void (*)(void))coroutine_trampoline,
                2, arg.i[0], arg.i[1]);

    /* swapcontext() in, longjmp() back out */
    if (!setjmp(old_env)) {
        swapcontext(&old_uc, &uc);
    }
    return &co->base;
}

Coroutine *qemu_coroutine_new(void)
{
    Coroutine *co;

    co = QSLIST_FIRST(&pool);
    if (co) {
        QSLIST_REMOVE_HEAD(&pool, pool_next);
        pool_size--;
    } else {
        co = coroutine_new();
    }
    return co;
}

#ifdef CONFIG_VALGRIND_H
#ifdef CONFIG_PRAGMA_DIAGNOSTIC_AVAILABLE
/* Work around an unused variable in the valgrind.h macro... */
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif
static inline void valgrind_stack_deregister(CoroutineUContext *co)
{
    VALGRIND_STACK_DEREGISTER(co->valgrind_stack_id);
}
#ifdef CONFIG_PRAGMA_DIAGNOSTIC_AVAILABLE
#pragma GCC diagnostic error "-Wunused-but-set-variable"
#endif
#endif

void qemu_coroutine_delete(Coroutine *co_)
{
    CoroutineUContext *co = DO_UPCAST(CoroutineUContext, base, co_);

    if (pool_size < POOL_MAX_SIZE) {
        QSLIST_INSERT_HEAD(&pool, &co->base, pool_next);
        co->base.caller = NULL;
        pool_size++;
        return;
    }

#ifdef CONFIG_VALGRIND_H
    valgrind_stack_deregister(co);
#endif

    g_free(co->stack);
    g_free(co);
}

CoroutineAction qemu_coroutine_switch(Coroutine *from_, Coroutine *to_,
                                      CoroutineAction action)
{
    CoroutineUContext *from = DO_UPCAST(CoroutineUContext, base, from_);
    CoroutineUContext *to = DO_UPCAST(CoroutineUContext, base, to_);
    CoroutineThreadState *s = coroutine_get_thread_state();
    int ret;

    s->current = to_;

    ret = setjmp(from->env);
    if (ret == 0) {
        longjmp(to->env, action);
    }
    return ret;
}

Coroutine *qemu_coroutine_self(void)
{
    CoroutineThreadState *s = coroutine_get_thread_state();

    return s->current;
}

bool qemu_in_coroutine(void)
{
    CoroutineThreadState *s = pthread_getspecific(thread_state_key);

    return s && s->current->caller;
}
