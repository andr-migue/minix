# Hilos en MINIX: mthread y pthreads — Explicación Completa

## 1. Contexto: ¿Qué tipo de hilos son estos?

Existen dos tipos fundamentales de hilos:

| Tipo | Quién los gestiona | Context switch | Ejemplo |
|------|-------------------|---------------|---------|
| **Kernel threads** | El SO (kernel) | El kernel interrumpe | Linux pthreads reales |
| **Green threads** | Una biblioteca de usuario | La propia biblioteca | mthread de MINIX |

**mthread** es la biblioteca de **green threads** de MINIX. Los hilos son completamente gestionados en espacio de usuario: el kernel no sabe que existen. Todo el scheduling, context switching y sincronización ocurre dentro del proceso.

```
┌──────────────────────────────────────────────────────────┐
│  PROCESO (espacio de usuario)                            │
│                                                          │
│   Hilo 1      Hilo 2      Hilo 3                         │
│   ┌─────┐    ┌─────┐    ┌─────┐                          │
│   │stack│    │stack│    │stack│                          │
│   └─────┘    └─────┘    └─────┘                          │
│                                                          │
│   libmthread: scheduler, mutex, cond, queue              │
│   (todo en espacio de usuario)                           │
├──────────────────────────────────────────────────────────┤
│  KERNEL                                                  │
│  Solo ve UN proceso (no sabe nada de los hilos internos) │
└──────────────────────────────────────────────────────────┘
```

---

## 2. Dónde vive el código

```
minix/lib/libmthread/
├── allocate.c      → Creación, exit, join, destrucción de hilos
├── scheduler.c     → Scheduler FIFO, context switching, yield, suspend
├── mutex.c         → Mutex: lock, unlock, trylock
├── condition.c     → Variables de condición: wait, signal, broadcast
├── queue.c         → Cola FIFO de hilos listos
├── event.c         → Objeto evento (mutex + cond combinados)
├── rwlock.c        → Locks de lectura-escritura
├── key.c           → Thread-local storage
├── pthread_compat.c→ Aliases débiles para compatibilidad con pthread API
├── global.h        → Estado global interno, definición del TCB
└── proto.h         → Prototipos internos
```

---

## 3. Thread Control Block (TCB): La estructura central

Cada hilo tiene un **TCB** (`struct __mthread_tcb`) que contiene todo su estado:

```c
// minix/lib/libmthread/global.h
struct __mthread_tcb {
    mthread_thread_t  m_tid;         // ID del hilo (índice en el array)
    mthread_state_t   m_state;       // Estado actual (ver sección 4)
    struct __mthread_attr m_attr;    // Atributos: tamaño de stack, detach state
    struct __mthread_cond *m_cond;   // Condición en la que espera (si MS_CONDITION)
    void *(*m_proc)(void *);         // Puntero a la función del hilo
    void *m_arg;                     // Argumento que recibe esa función
    void *m_result;                  // Valor de retorno (para join)
    mthread_cond_t    m_exited;      // Condición para notificar a quien haga join()
    mthread_mutex_t   m_exitm;       // Mutex asociado al m_exited
    ucontext_t        m_context;     // CONTEXTO DE CPU: registros, stack pointer, PC
    struct __mthread_tcb *m_next;    // Puntero para lista enlazada
};
```

El campo más importante es `m_context`: contiene el estado completo del CPU del hilo (todos los registros, el stack pointer, el program counter). Es lo que permite "congelar" y "descongelar" un hilo.

### Pool de hilos

```c
// Estado global
mthread_tcb_t  threads[MAX_THREAD_POOL];  // Array de todos los TCBs
mthread_queue_t run_queue;                // Cola FIFO de hilos ejecutables
mthread_queue_t free_threads;             // Pool de TCBs libres para reutilizar
mthread_thread_t current_thread;          // ID del hilo que corre ahora
mthread_tcb_t mainthread;                 // TCB especial del hilo principal (ID = -1)
```

- Empieza con `NO_THREADS = 4` TCBs
- Se duplica cuando se agotan (hasta `MAX_THREAD_POOL = 1024`)
- Los TCBs se reutilizan; los stacks se liberan al salir

---

## 4. Estados de un hilo

```c
// global.h
enum mthread_state_t {
    MS_RUNNABLE,    // Listo para ejecutar (está en run_queue)
    MS_MUTEX,       // Bloqueado esperando un mutex
    MS_CONDITION,   // Bloqueado esperando una condición
    MS_EXITING,     // En proceso de terminar
    MS_DEAD,        // No asignado / terminado
    MS_NEEDRESET    // Hilo detached que necesita limpieza
};
```

Diagrama de transiciones:

```
                    mthread_create()
                          │
                          ▼
                     MS_RUNNABLE ◄──────────────────────────────┐
                          │                                      │
            ┌─────────────┼──────────────────┐                  │
            │             │                  │                   │
     mutex bloqueado  cond_wait()       yield()              unlock() /
            │             │                  │              signal() /
            ▼             ▼                  ▼             broadcast()
        MS_MUTEX    MS_CONDITION       MS_RUNNABLE              │
            │             │           (vuelve a la cola)        │
            └─────────────┘                                     │
                   unsuspend() ────────────────────────────────►┘
                          │
                  mthread_exit()
                          │
                          ▼
                    MS_EXITING
                          │
                    MS_DEAD / MS_NEEDRESET
```

---

## 5. Context Switching: La magia de `ucontext`

El mecanismo central que hace posible los green threads es la API POSIX `ucontext`:

| Función | Qué hace |
|---------|---------|
| `getcontext(ucp)` | Guarda el estado actual del CPU en `ucp` |
| `setcontext(ucp)` | Restaura el estado de CPU desde `ucp` (no retorna) |
| `swapcontext(old, new)` | Guarda estado actual en `old` y salta a `new` |
| `makecontext(ucp, fn, ...)` | Configura `ucp` para ejecutar la función `fn` |

Cuando mthread quiere cambiar de hilo:

```
Hilo A ejecutando...
        │
        │ swapcontext(&A->m_context, &B->m_context)
        │   ┌─ Guarda registros de A en A->m_context
        │   └─ Carga registros de B desde B->m_context
        ▼
Hilo B continúa desde donde lo dejó
```

**No hay interrupción del kernel**. El hilo A decide voluntariamente ceder el CPU.

---

## 6. El Scheduler: FIFO no-preemptivo

### `mthread_schedule()`

```c
// scheduler.c
void mthread_schedule(void) {
    mthread_thread_t next = mthread_queue_remove(&run_queue);

    if (next == NO_THREAD) {
        // No hay más hilos → volver al hilo principal
        swapcontext(&current->m_context, &mainthread.m_context);
    } else {
        swapcontext(&current->m_context, &threads[next].m_context);
    }
    current_thread = next;
}
```

La cola `run_queue` es un **FIFO puro**: el primero en entrar es el primero en salir. No hay prioridades dentro de mthread.

### `mthread_yield()`

```c
void mthread_yield(void) {
    mthread_queue_add(&run_queue, current_thread);  // Volvemos al final
    threads[current_thread].m_state = MS_RUNNABLE;
    mthread_schedule();                              // Ceder CPU al siguiente
}
```

### `mthread_suspend(state)`

Cuando un hilo se bloquea (mutex ocupado, esperando condición):

```c
void mthread_suspend(mthread_state_t state) {
    threads[current_thread].m_state = state;  // Marcar como bloqueado
    // NO se añade a run_queue → queda fuera del scheduler
    mthread_schedule();                        // Ceder CPU
}
```

### `mthread_unsuspend(tid)`

Cuando un hilo puede reanudar:

```c
void mthread_unsuspend(mthread_thread_t tid) {
    threads[tid].m_state = MS_RUNNABLE;
    mthread_queue_add(&run_queue, tid);  // Añadir al final del FIFO
}
```

**Clave**: el scheduling es **cooperativo**. Un hilo solo cede el CPU cuando:
- Llama a `mthread_yield()`
- Se bloquea en un mutex (`mthread_suspend(MS_MUTEX)`)
- Se bloquea en una condición (`mthread_suspend(MS_CONDITION)`)
- Termina (`mthread_exit()`)

Un hilo que hace cálculo puro y nunca yield **monopoliza el CPU indefinidamente**.

---

## 7. Ciclo de vida completo de un hilo

### `mthread_create(tid, attr, fn, arg)`

```c
// allocate.c
int mthread_create(mthread_thread_t *tid, mthread_attr_t *attr,
                   void *(*fn)(void *), void *arg) {

    // 1. Obtener TCB libre del pool
    tcb = mthread_get_tcb();

    // 2. Copiar atributos (stack size, detach state)
    tcb->m_attr = attr ? *attr : default_attr;

    // 3. Asignar stack con mmap() + página guard (PROT_NONE)
    tcb->m_attr.ma_stackaddr = mmap(..., PROT_READ|PROT_WRITE, ...);

    // 4. Configurar contexto de ejecución
    getcontext(&tcb->m_context);
    tcb->m_context.uc_stack.ss_sp   = tcb->m_attr.ma_stackaddr;
    tcb->m_context.uc_stack.ss_size = tcb->m_attr.ma_stacksize;
    makecontext(&tcb->m_context, mthread_trampoline, ...);

    // 5. Guardar función y argumento
    tcb->m_proc = fn;
    tcb->m_arg  = arg;

    // 6. Inicializar m_exited (condición para join)
    mthread_cond_init(&tcb->m_exited, NULL);
    mthread_mutex_init(&tcb->m_exitm, NULL);

    // 7. Hacer el hilo ejecutable
    tcb->m_state = MS_RUNNABLE;
    mthread_queue_add(&run_queue, tcb->m_tid);

    *tid = tcb->m_tid;
    return 0;
}
```

### Stack y página guard

```
Stack de un hilo (crece hacia abajo):
┌────────────────────────────────────┐  ← tope del stack
│  Marco de mthread_trampoline       │
│  Marco de la función del usuario   │
│  ...                               │
│  (stack crece aquí hacia abajo)    │
├────────────────────────────────────┤  ← stack pointer actual
│  (espacio libre)                   │
├────────────────────────────────────┤
│  GUARD PAGE (PROT_NONE, 1 página)  │  ← acceder aquí → SIGSEGV
└────────────────────────────────────┘
```

La página guard detecta desbordamiento de stack sin costo en tiempo de ejecución.

### `mthread_exit(retval)`

```c
void mthread_exit(void *retval) {
    tcb->m_result = retval;         // Guardar valor de retorno
    tcb->m_state = MS_EXITING;

    if (joinable) {
        // Despertar al hilo que esté esperando en join()
        mthread_mutex_lock(&tcb->m_exitm);
        mthread_cond_signal(&tcb->m_exited);
        mthread_mutex_unlock(&tcb->m_exitm);
    } else {
        // Detached: limpiar inmediatamente
        munmap(tcb->m_attr.ma_stackaddr, ...);
        tcb->m_state = MS_DEAD;
    }

    mthread_schedule();  // Ceder CPU, ya no volvemos aquí
}
```

### `mthread_join(tid, retval)`

```c
int mthread_join(mthread_thread_t tid, void **retval) {
    tcb = &threads[tid];

    if (tcb->m_state != MS_DEAD && tcb->m_state != MS_EXITING) {
        // El hilo no ha terminado → esperar
        mthread_mutex_lock(&tcb->m_exitm);
        mthread_cond_wait(&tcb->m_exited, &tcb->m_exitm);
        mthread_mutex_unlock(&tcb->m_exitm);
    }

    if (retval) *retval = tcb->m_result;

    // Limpiar TCB y devolver al pool
    munmap(tcb->m_attr.ma_stackaddr, ...);
    tcb->m_state = MS_DEAD;
    mthread_queue_add(&free_threads, tid);
    return 0;
}
```

---

## 8. Mutex

### Estructura

```c
// minix/include/minix/mthread.h
struct __mthread_mutex {
    mthread_queue_t  mm_queue;   // Cola FIFO de hilos esperando este mutex
    mthread_thread_t mm_owner;   // ID del hilo dueño (NO_THREAD si libre)
    unsigned int     mm_magic;   // Número mágico para validación
};
```

### `mthread_mutex_lock(mutex)`

```c
// mutex.c
int mthread_mutex_lock(mthread_mutex_t *mutex) {
    m = *mutex;

    if (m->mm_owner == NO_THREAD) {
        // Mutex libre → tomarlo
        m->mm_owner = current_thread;
        return 0;
    }

    if (m->mm_owner == current_thread) {
        // Ya lo tenemos → deadlock
        return EDEADLK;
    }

    // Ocupado por otro hilo → encolar y bloquear
    mthread_queue_add(&m->mm_queue, current_thread);
    mthread_suspend(MS_MUTEX);   // Ceder CPU hasta que nos despierten

    // Cuando volvemos aquí, somos el nuevo dueño
    return 0;
}
```

### `mthread_mutex_unlock(mutex)`

```c
int mthread_mutex_unlock(mthread_mutex_t *mutex) {
    m = *mutex;

    // Sacar el siguiente hilo de la cola
    next = mthread_queue_remove(&m->mm_queue);

    if (next == NO_THREAD) {
        m->mm_owner = NO_THREAD;  // Nadie esperando → liberar
    } else {
        m->mm_owner = next;        // Transferir ownership
        mthread_unsuspend(next);   // Despertar al siguiente
    }
    return 0;
}
```

### `mthread_mutex_trylock(mutex)`

```c
int mthread_mutex_trylock(mthread_mutex_t *mutex) {
    if ((*mutex)->mm_owner == NO_THREAD) {
        (*mutex)->mm_owner = current_thread;
        return 0;
    }
    return EBUSY;  // No bloquea, retorna inmediatamente
}
```

### Visualización del mutex

```
Estado libre:          Estado ocupado (hilo A tiene, B y C esperan):

mm_owner = NO_THREAD   mm_owner = Hilo_A
mm_queue = []          mm_queue = [Hilo_B → Hilo_C]

unlock(A):
  next = Hilo_B
  mm_owner = Hilo_B
  unsuspend(Hilo_B) → Hilo_B pasa a run_queue
  mm_queue = [Hilo_C]
```

---

## 9. Variables de condición

### Estructura

```c
struct __mthread_cond {
    struct __mthread_mutex *mc_mutex;  // Mutex asociado
    unsigned int mc_magic;             // Validación
};
```

### `mthread_cond_wait(cond, mutex)`

```c
// condition.c
int mthread_cond_wait(mthread_cond_t *cond, mthread_mutex_t *mutex) {
    // 1. Registrar en el TCB que esperamos esta condición
    threads[current_thread].m_cond = *cond;

    // 2. Soltar el mutex (otro hilo puede entrar ahora)
    mthread_mutex_unlock(mutex);

    // 3. Suspenderse hasta ser despertado
    mthread_suspend(MS_CONDITION);

    // 4. Al despertar, volver a tomar el mutex
    mthread_mutex_lock(mutex);

    return 0;
}
```

**Por qué se suelta el mutex**: si no lo soltáramos, ningún otro hilo podría adquirirlo para llamar a `signal()`, y esperaríamos para siempre (deadlock).

### `mthread_cond_signal(cond)`

```c
int mthread_cond_signal(mthread_cond_t *cond) {
    // Buscar el primer hilo esperando esta condición
    for (tid = 0; tid < used_threads; tid++) {
        if (threads[tid].m_state == MS_CONDITION &&
            threads[tid].m_cond == *cond) {
            threads[tid].m_cond = NULL;
            mthread_unsuspend(tid);  // Despertar solo a uno
            return 0;
        }
    }
    return 0;  // Nadie esperaba, no pasa nada (señal perdida)
}
```

### `mthread_cond_broadcast(cond)`

```c
int mthread_cond_broadcast(mthread_cond_t *cond) {
    // Despertar a TODOS los hilos esperando esta condición
    for (tid = 0; tid < used_threads; tid++) {
        if (threads[tid].m_state == MS_CONDITION &&
            threads[tid].m_cond == *cond) {
            threads[tid].m_cond = NULL;
            mthread_unsuspend(tid);
        }
    }
    return 0;
}
```

### Patrón de uso típico

```c
// Productor
mthread_mutex_lock(&mutex);
buffer[i] = dato;
mthread_cond_signal(&hay_dato);
mthread_mutex_unlock(&mutex);

// Consumidor
mthread_mutex_lock(&mutex);
while (buffer_vacio()) {
    mthread_cond_wait(&hay_dato, &mutex);  // Suelta mutex y espera
}
dato = buffer[i];
mthread_mutex_unlock(&mutex);
```

---

## 10. Capa de compatibilidad pthread

MINIX provee `pthread_compat.c` que usa **weak aliases** para que código escrito con la API de pthreads funcione sin cambios:

```c
// pthread_compat.c
__weak_alias(pthread_create,      mthread_create)
__weak_alias(pthread_join,        mthread_join)
__weak_alias(pthread_exit,        mthread_exit)
__weak_alias(pthread_mutex_lock,  mthread_mutex_lock)
__weak_alias(pthread_mutex_unlock,mthread_mutex_unlock)
__weak_alias(pthread_cond_wait,   mthread_cond_wait)
__weak_alias(pthread_cond_signal, mthread_cond_signal)
// ... etc
```

Un **weak alias** significa: "si nadie define `pthread_create`, úsalo como sinónimo de `mthread_create`". Esto permite que código portátil con `#include <pthread.h>` compile y corra en MINIX sin modificaciones.

### Inicialización lazy para inicializadores estáticos

Los inicializadores estáticos de pthread (`PTHREAD_MUTEX_INITIALIZER`) son un valor especial. Como mthread necesita inicialización real, la capa de compatibilidad los detecta:

```c
int mthread_mutex_lock(mthread_mutex_t *mutex) {
    if (*mutex == (mthread_mutex_t) PTHREAD_MUTEX_INITIALIZER) {
        mthread_mutex_init(mutex, NULL);  // Inicializar al primer uso
    }
    // ... resto del lock
}
```

---

## 11. El Bug de Hito 2: Recursión infinita en `pthread_mutex_trylock`

### El problema

En `pthread_compat.c`, la implementación de `pthread_mutex_trylock` tenía una **llamada recursiva a sí misma** en lugar de delegar a `mthread_mutex_trylock`:

**Antes (bug):**
```c
int pthread_mutex_trylock(pthread_mutex_t *mutex) {
    // ...
    return pthread_mutex_trylock(mutex);  // ← LLAMA A SÍ MISMO
}
```

Esto causaba un **stack overflow** inmediato: cualquier llamada a `pthread_mutex_trylock` desencadenaba recursión infinita hasta agotar el stack del hilo.

### La corrección

```c
int pthread_mutex_trylock(pthread_mutex_t *mutex) {
    // ...
    return mthread_mutex_trylock(mutex);  // ← Delegar correctamente
}
```

### Por qué pasó desapercibido

El resto de las funciones usaba `__weak_alias`, pero `trylock` fue escrita manualmente y se coló este error tipográfico. Como el weak alias hace la delegación automáticamente para las demás funciones, el patrón correcto (`mthread_*`) no era obvio al escribir la única función que sí tenía cuerpo manual.

---

## 12. Thread-Local Storage (TLS)

Para datos específicos de cada hilo:

```c
// key.c
mthread_key_t key;
mthread_key_create(&key, destructor_fn);

mthread_setspecific(key, valor_para_este_hilo);
void *v = mthread_getspecific(key);  // Cada hilo obtiene su propio valor

mthread_key_delete(key);
```

Internamente es un array por clave indexado por thread ID. Al hacer `mthread_exit()`, se llaman los destructores de todas las claves con valores no-NULL.

---

## 13. Resumen general

| Concepto | Descripción |
|----------|-------------|
| **Green thread** | Hilo gestionado en espacio de usuario, el kernel no lo ve |
| **TCB** | Estructura que guarda el estado completo de un hilo |
| `m_context` | Estado del CPU del hilo (registros, stack pointer, PC) |
| `swapcontext` | Mecanismo de context switch sin intervención del kernel |
| **run_queue** | Cola FIFO de hilos listos para ejecutar |
| **Scheduling cooperativo** | Los hilos ceden el CPU voluntariamente |
| `mthread_suspend` | Bloquear un hilo y ceder CPU (para mutex/cond) |
| `mthread_unsuspend` | Reactivar un hilo (devolverlo a run_queue) |
| **Mutex** | Garantiza acceso exclusivo; los hilos bloqueados esperan en mm_queue |
| **Cond var** | Permite esperar a que se cumpla una condición; siempre con un mutex |
| **Weak alias** | Hace que `pthread_*` apunte a `mthread_*` sin duplicar código |
| **Bug hito 2** | `pthread_mutex_trylock` se llamaba a sí mismo en lugar de a `mthread_mutex_trylock` |
