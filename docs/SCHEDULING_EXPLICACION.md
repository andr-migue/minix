# Scheduling en MINIX: Explicación Completa

## 1. Arquitectura base: ¿Qué es el scheduler en MINIX?

MINIX es un **microkernel**, lo que significa que el kernel hace lo mínimo posible. El scheduling tiene **dos capas**:

```
┌──────────────────────────────────────────────────────┐
│  ESPACIO USUARIO                                     │
│  ┌─────────────────────────────────────────────────┐ │
│  │  Servidor SCHED  (servers/sched/schedule.c)     │ │
│  │  → Decide prioridades, penalizaciones, política │ │
│  └─────────────────────────────────────────────────┘ │
├──────────────────────────────────────────────────────┤
│  KERNEL                                              │
│  ┌─────────────────────────────────────────────────┐ │
│  │  kernel/proc.c                                  │ │
│  │  → pick_proc(), enqueue(), dequeue()            │ │
│  │  → Ejecuta el proceso que SCHED le dice         │ │
│  └─────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────┘
```

El **kernel** ejecuta mecánicamente: "dame el proceso de mayor prioridad en la cola". El **servidor SCHED** es quien decide las prioridades y responde cuando un proceso agota su quantum.

---

## 2. Colas de prioridad (Run Queues)

Hay **16 colas** numeradas del 0 al 15:

```
Cola  0  ← TASK_Q     → Tareas del kernel (máxima prioridad)
Cola  1
Cola  2
Cola  3
Cola  4
Cola  5
Cola  6
Cola  7  ← USER_Q     → Prioridad por defecto de procesos usuario
Cola  8
...
Cola 15  ← MIN_USER_Q → Mínima prioridad (menor = más degradado)
```

**Regla fundamental**: número más bajo = mayor prioridad. Un proceso en cola 0 siempre corre antes que uno en cola 7.

```c
// include/minix/config.h
#define NR_SCHED_QUEUES  16
#define TASK_Q            0   // Tareas kernel, máxima prioridad
#define MAX_USER_Q        0   // Máxima alcanzable por usuario
#define USER_Q            7   // Default para procesos normales
#define MIN_USER_Q       15   // Mínima (la peor)
```

Cada cola es una **lista enlazada** de procesos. `pick_proc()` recorre de 0 a 15 y devuelve el primero que encuentre listo.

---

## 3. ¿Qué es un Quantum?

Un **quantum** (o time slice) es el **tiempo máximo que un proceso puede correr sin ser interrumpido**.

- Por defecto: `USER_QUANTUM = 200 ms`
- Está almacenado en `struct proc.p_cpu_time_left` (en ciclos de CPU, en el kernel)
- Y en `struct schedproc.time_slice` (en milisegundos, en el servidor SCHED)

```
Proceso A corre...
   ├────────────────────────────────────────┤
   0ms                                   200ms
                                           ↑
                               QUANTUM AGOTADO → kernel interrumpe
```

Cuando el quantum se agota, el kernel pone el flag `RTS_NO_QUANTUM` en el proceso y manda un mensaje IPC al servidor SCHED: `SCHEDULING_NO_QUANTUM`. El proceso queda **bloqueado** hasta que SCHED le asigne nuevo quantum.

---

## 4. ¿Qué es `p_priority`?

Es el campo del `struct proc` que indica **en qué cola está el proceso**:

```c
// kernel/proc.h
struct proc {
    int p_priority;           // Cola actual (0-15)
    int p_cpu_time_left;      // Quantum restante (ciclos)
    int p_quantum_size_ms;    // Tamaño del quantum en ms
    ...
};
```

Cuando el kernel llama a `enqueue(proc)`, mete al proceso en `run_q[proc->p_priority]`.

---

## 5. `struct schedproc`: El proceso visto por el servidor SCHED

El servidor SCHED mantiene su propia tabla de procesos:

```c
// servers/sched/schedproc.h
struct schedproc {
    endpoint_t endpoint;     // ID del proceso
    unsigned flags;          // Estado (en uso, etc.)
    unsigned max_priority;   // Techo de prioridad (no puede subir de aquí)
    unsigned priority;       // Prioridad actual en el kernel
    unsigned time_slice;     // Quantum en ms

    // Campos NUEVOS añadidos en este proyecto:
    unsigned base_priority;  // Prioridad de referencia (sin penalización)
    unsigned quantum_count;  // Quantums agotados en la ventana actual
    unsigned penalty_level;  // Nivel de penalización acumulado (0-2)
};
```

---

## 6. El flujo normal del scheduler (antes del cambio)

```
1. Proceso empieza con priority = USER_Q = 7
2. Corre 200ms y agota quantum
3. Kernel → SCHED: "este proceso agotó su quantum"
4. SCHED (do_noquantum()):
       priority = min(priority + 1, MIN_USER_Q)  → degrada a cola 8
       le da nuevo quantum
5. Proceso corre en cola 8 (peor que antes)
6. Agota otro quantum → degrada a cola 9...
7. ...hasta llegar a cola 15
```

El problema de este sistema original: **no había recuperación**. Un proceso CPU-intensivo se quedaba en cola 15 para siempre. Tampoco distinguía bien entre procesos interactivos (que duermen frecuentemente) y procesos CPU-bound (que nunca duermen).

---

## 7. El Problema que había

El sistema original tenía una penalización **demasiado agresiva e irreversible**:

- Cada quantum agotado → degradación inmediata e irrecuperable
- Un proceso interactivo que de vez en cuando necesitaba CPU también se degradaba
- No había mecanismo para "recompensar" el buen comportamiento
- No había distinción entre "este proceso es inherentemente CPU-bound" vs "este proceso tuvo una ráfaga ocasional de CPU"

---

## 8. La solución implementada: Mecanismo de Penalización por Ventanas

### Constantes nuevas

```c
// servers/sched/schedule.c
#define CPU_BOUND_QUANTUM_THRESHOLD  3   // Quantums agotados para considerar CPU-bound
#define MAX_PENALTY_LEVEL            2   // Penalización máxima acumulable
#define BALANCE_TIMEOUT              5   // Segundos entre rebalanceos
```

### Nuevos campos en `schedproc`

| Campo | Significado |
|-------|-------------|
| `base_priority` | Prioridad "real" sin penalizaciones, fija |
| `quantum_count` | Contador de quantums agotados en esta ventana de 5s |
| `penalty_level` | Penalización acumulada: 0, 1 ó 2 |

### La fórmula de prioridad efectiva

```
prioridad_efectiva = base_priority + penalty_level
```

Recuerda: número mayor = peor prioridad.

| penalty_level | prioridad efectiva (si base=7) |
|:---:|:---:|
| 0 | 7 (USER_Q normal) |
| 1 | 8 (un poco peor) |
| 2 | 9 (bastante peor) |

---

## 9. `do_noquantum()`: Cuando el proceso agota su quantum

```c
// servers/sched/schedule.c:91
int do_noquantum(message *m_ptr) {
    struct schedproc *rmp = ...;

    if (rmp->priority < MIN_USER_Q) {
        rmp->priority++;   // Degrada dentro de la ventana (intra-window)
    }

    rmp->quantum_count++;   // Cuenta cuántos quantums agotó en esta ventana

    schedule_process_local(rmp);  // Aplica cambio al kernel via IPC
}
```

**Ojo importante**: esta degradación intra-ventana es **temporal y local**. No modifica `base_priority` ni `penalty_level`. Solo baja la prioridad inmediata del proceso dentro de la ventana actual de 5 segundos.

---

## 10. `balance_queues()`: El rebalanceo cada 5 segundos

Esta es la función central del mecanismo. Se ejecuta periódicamente:

```c
// servers/sched/schedule.c:380
static void balance_queues(struct timer *tp) {
    for cada proceso activo:

        if (es proceso de sistema):
            // Comportamiento original, sin cambios
            if priority > max_priority:
                priority = max_priority

        else (es proceso de usuario):

            if (quantum_count >= CPU_BOUND_QUANTUM_THRESHOLD):
                // Proceso CPU-bound: merece penalización
                if (penalty_level < MAX_PENALTY_LEVEL):
                    penalty_level++

            else if (quantum_count == 0 && penalty_level > 0):
                // Proceso interactivo con penalización previa: recuperar
                penalty_level--

            // Calcular nueva prioridad
            new_priority = base_priority + penalty_level

            // Clampear para no salir de rango
            if (new_priority > MIN_USER_Q):   // ← BUG FIX aquí
                new_priority = MIN_USER_Q
            if (new_priority < max_priority):
                new_priority = max_priority

            rmp->priority = new_priority
            rmp->quantum_count = 0   // Reset para la próxima ventana

        schedule_process_local(rmp)
}
```

---

## 11. El Bug Crítico que se corrigió

En `balance_queues()`, la línea de clamping tenía la comparación **al revés**:

**Antes (bug):**
```c
if (new_priority < MIN_USER_Q)   // ← INCORRECTO
    new_priority = MIN_USER_Q;
```

Esto significaba: "si la prioridad es *mejor* que el mínimo, límitala al mínimo" → **penalizaba a todos los procesos** llevándolos a cola 15 siempre.

**Después (fix):**
```c
if (new_priority > MIN_USER_Q)   // ← CORRECTO
    new_priority = MIN_USER_Q;
```

Ahora significa: "si la prioridad es *peor* que el mínimo permitido, clampear al mínimo" → comportamiento correcto.

---

## 12. `do_nice()`: Cambiar prioridad manualmente

```c
// servers/sched/schedule.c:275
int do_nice(message *m_ptr) {
    new_priority = m_ptr->m_sched_nice.maxprio;

    // Validar rango
    if (new_priority >= NR_SCHED_QUEUES || new_priority < 0)
        return EINVAL;

    // Guardar valor anterior para rollback si falla
    old_priority = rmp->priority;
    old_max_priority = rmp->max_priority;

    rmp->max_priority = rmp->priority = new_priority;

    if (schedule_process_local(rmp) != OK):
        // Rollback
        rmp->priority = old_priority;
        rmp->max_priority = old_max_priority;
}
```

Cuando el usuario ejecuta `renice`, esto es lo que se llama. Modifica tanto `priority` como `max_priority` (el techo).

---

## 13. Diagrama completo del flujo

```
Proceso nuevo creado
        │
        ▼
do_start_scheduling()
  base_priority = USER_Q (7)
  quantum_count = 0
  penalty_level = 0
  priority = 7
        │
        ▼
Proceso en cola 7, corre...
        │
   ┌────┴─────────────────────────┐
   │ Agota quantum (200ms)        │ Duerme/bloquea
   ▼                              │
do_noquantum()                    │
  quantum_count++                 │
  priority++ (intra-window)       │
  → prioridad temporal 8          │
        │                         │
   [cada 5 seg]                   │
        ▼                         │
balance_queues()                  │
  quantum_count >= 3?             │
  ├─ SÍ: penalty_level++          │
  │   new_priority = base + pen   │
  │   reset quantum_count = 0     │
  └─ NO (quantum_count==0):       │
      si penalty>0: penalty--     │
      (recuperación)              │
        │                         │
        └────────────┬────────────┘
                     ▼
             priority actualizada
             proceso en nueva cola
```

---

## 14. Resumen del antes y después

| Aspecto | Antes | Después |
|---------|-------|---------|
| Degradación | Inmediata por cada quantum | Acumulada en ventanas de 5s |
| Recuperación | Ninguna | Sí, si deja de agotar quantums |
| Profundidad de penalización | Hasta MIN_USER_Q (15) | Máximo base+2 |
| Distinción CPU-bound vs interactivo | No | Sí (threshold=3 quantums) |
| Bug de clamping | Comparación invertida | Corregido |
| Campos en schedproc | Solo priority, max_priority | + base_priority, quantum_count, penalty_level |
