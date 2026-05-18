# Análisis de Cambios: Mecanismo de Penalización CPU-bound

> Commit: `e3e48819b` — `feat: implement CPU-bound process penalty mechanism in scheduler`
> Archivos modificados: `minix/servers/sched/schedproc.h` · `minix/servers/sched/schedule.c`

---

## Idea central

El scheduler original trataba a todos los procesos de usuario de la misma manera: cada vez que un proceso agotaba su quantum, bajaba un nivel de prioridad, y periódicamente `balance_queues` los subía de vuelta. No había memoria del comportamiento pasado del proceso.

La modificación introduce una **política basada en ventanas de tiempo**: el scheduler observa cuántas veces un proceso agota su quantum dentro de una ventana de 5 segundos y castiga progresivamente a los que se comportan como procesos intensivos en CPU, permitiendo además recuperación gradual.

---

## Nota sobre prioridades en MINIX

> En MINIX los números de prioridad son **invertidos**: un número menor significa **mayor** prioridad.
> `USER_Q` es la prioridad normal de usuario. `MIN_USER_Q` es el número más alto permitido, es decir, la **peor** prioridad posible para un proceso de usuario.
> Penalizar = aumentar el número de cola = bajar en el ranking de ejecución.

---

## Archivo 1: `schedproc.h` — La estructura de datos del proceso

### Antes

```c
EXTERN struct schedproc {
    endpoint_t endpoint;    /* process endpoint id */
    endpoint_t parent;      /* parent endpoint id */
    unsigned flags;         /* flag bits */

    /* User space scheduling */
    unsigned max_priority;  /* this process' highest allowed priority */
    unsigned priority;      /* the process' current priority */
    unsigned time_slice;    /* this process's time slice */
    unsigned cpu;           /* what CPU is the process running on */
    bitchunk_t cpu_mask[BITMAP_CHUNKS(CONFIG_MAX_CPUS)];
} schedproc[NR_PROCS];
```

### Despues

```c
EXTERN struct schedproc {
    endpoint_t endpoint;    /* process endpoint id */
    endpoint_t parent;      /* parent endpoint id */
    unsigned flags;         /* flag bits */

    /* User space scheduling */
    unsigned max_priority;  /* this process' highest allowed priority */
    unsigned priority;      /* the process' current priority */
    unsigned time_slice;    /* this process's time slice */
    unsigned cpu;           /* what CPU is the process running on */
    bitchunk_t cpu_mask[BITMAP_CHUNKS(CONFIG_MAX_CPUS)];

    /* CPU-bound process penalty mechanism */          // <-- NUEVO BLOQUE
    unsigned base_priority; /* base priority for gradual recovery */
    unsigned quantum_count; /* number of full quantums consumed in current window */
    unsigned penalty_level; /* current penalty level (0 = no penalty) */
} schedproc[NR_PROCS];
```

### Que se agrego y por que

| Campo nuevo | Tipo | Rol |
|---|---|---|
| `base_priority` | `unsigned` | Guarda la prioridad original del proceso. Es el punto de referencia para calcular la penalización y para saber hasta dónde puede recuperarse. Sin este campo, no habría forma de saber "cuánto castigo acumulado" tiene el proceso. |
| `quantum_count` | `unsigned` | Contador de quantums completos consumidos en la ventana actual (se reinicia cada 5 s). Es la métrica que distingue un proceso CPU-bound de uno interactivo. |
| `penalty_level` | `unsigned` | Nivel de castigo acumulado (0 = sin castigo, máx = `MAX_PENALTY_LEVEL`). La prioridad efectiva es `base_priority + penalty_level`. |

---

## Archivo 2: `schedule.c` — La lógica del scheduler

### Cambio A — Nuevas constantes (líneas 20–22)

#### Antes
*(no existían)*

#### Despues
```c
/* CPU-bound process penalty mechanism */
#define CPU_BOUND_QUANTUM_THRESHOLD 3   /* quantums para activar penalización */
#define MAX_PENALTY_LEVEL           2   /* niveles máximos de penalización */
```

**Por que:** Definen los dos parámetros de la política de manera clara y modificable. Con `THRESHOLD = 3` y `MAX_PENALTY_LEVEL = 2`, un proceso puede ser degradado hasta 2 niveles de prioridad por encima de su `base_priority`.

---

### Cambio B — `do_noquantum()` — Conteo de quantums

Esta función es llamada por el kernel cada vez que un proceso agota completamente su quantum de CPU.

#### Antes

```c
rmp = &schedproc[proc_nr_n];
if (rmp->priority < MIN_USER_Q) {
    rmp->priority += 1; /* lower priority */
}
```

#### Despues

```c
rmp = &schedproc[proc_nr_n];

/* Count full quantum consumed by this process */     // <-- NUEVO
if (!is_system_proc(rmp)) {
    rmp->quantum_count++;
}

if (rmp->priority < MIN_USER_Q) {
    rmp->priority += 1; /* lower priority */
}
```

**Que cambio:** Se añaden 4 líneas antes de la lógica original (que se conserva intacta). Solo se cuentan procesos de usuario (`!is_system_proc`), ignorando procesos del sistema como drivers y servidores.

**Por que:** Este es el punto de observación. Cada vez que un proceso no cede la CPU voluntariamente y el kernel tiene que interrumpirlo, se anota. Al final de la ventana `balance_queues` puede consultar ese contador para decidir si el proceso merece castigo.

---

### Cambio C — `do_start_scheduling()` — Inicialización de los nuevos campos

Esta función se llama cuando el scheduler empieza a gestionar un proceso nuevo.

#### Antes — justo después de validar `max_priority`

```c
rmp->max_priority = m_ptr->m_lsys_sched_scheduling_start.maxprio;
if (rmp->max_priority >= NR_SCHED_QUEUES) {
    return EINVAL;
}

/* Inherit current priority and time slice from parent... */
```

#### Despues

```c
rmp->max_priority = m_ptr->m_lsys_sched_scheduling_start.maxprio;
if (rmp->max_priority >= NR_SCHED_QUEUES) {
    return EINVAL;
}

/* Initialize penalty mechanism fields */             // <-- NUEVO
rmp->base_priority = USER_Q;
rmp->quantum_count = 0;
rmp->penalty_level = 0;

/* Inherit current priority and time slice from parent... */
```

**Por que:** Asegura que ningún proceso nuevo herede un estado de penalización basura de un slot de `schedproc` reutilizado.

---

#### Antes — rama `SCHEDULING_START` (procesos de sistema con prioridad explícita)

```c
case SCHEDULING_START:
    rmp->priority   = rmp->max_priority;
    rmp->time_slice = m_ptr->m_lsys_sched_scheduling_start.quantum;
    break;
```

#### Despues

```c
case SCHEDULING_START:
    rmp->priority      = rmp->max_priority;
    rmp->base_priority = rmp->max_priority;    // <-- NUEVO
    rmp->time_slice    = m_ptr->m_lsys_sched_scheduling_start.quantum;
    rmp->quantum_count = 0;                    // <-- NUEVO
    rmp->penalty_level = 0;                    // <-- NUEVO
    break;
```

**Por que:** `base_priority` debe capturar la prioridad real asignada, no el valor genérico `USER_Q`. Para procesos de sistema, su prioridad inicial es `max_priority`.

---

#### Antes — rama `SCHEDULING_INHERIT` (procesos que heredan del padre)

```c
case SCHEDULING_INHERIT:
    ...
    rmp->priority  = schedproc[parent_nr_n].priority;
    rmp->time_slice = schedproc[parent_nr_n].time_slice;
    break;
```

#### Despues

```c
case SCHEDULING_INHERIT:
    ...
    rmp->priority      = schedproc[parent_nr_n].priority;
    rmp->base_priority = schedproc[parent_nr_n].priority;  // <-- NUEVO
    rmp->time_slice    = schedproc[parent_nr_n].time_slice;
    rmp->quantum_count = 0;                                // <-- NUEVO
    rmp->penalty_level = 0;                                // <-- NUEVO
    break;
```

**Por que:** El hijo hereda la prioridad del padre como punto de partida limpio. Hereda la prioridad actual (no la penalizada), por lo que `base_priority` refleja ese valor heredado. El hijo empieza con contador y penalización en cero, sin importar el historial del padre.

---

### Cambio D — `balance_queues()` — El corazón de la política

Esta es la función más importante modificada. Se ejecuta cada `BALANCE_TIMEOUT = 5` segundos.

#### Antes — lógica completa

```c
void balance_queues(void)
{
    struct schedproc *rmp;
    int r, proc_nr;

    for (proc_nr=0, rmp=schedproc; proc_nr < NR_PROCS; proc_nr++, rmp++) {
        if (rmp->flags & IN_USE) {
            if (rmp->priority > rmp->max_priority) {
                rmp->priority -= 1; /* increase priority */
                schedule_process_local(rmp);
            }
        }
    }

    if ((r = sys_setalarm(balance_timeout, 0)) != OK)
        panic("sys_setalarm failed: %d", r);
}
```

La política original era completamente ciega al historial: si el proceso fue degradado, lo sube un nivel. Sin condiciones adicionales. Se aplicaba igual a procesos de sistema y de usuario.

#### Despues — lógica completa

```c
void balance_queues(void)
{
    struct schedproc *rmp;
    int r, proc_nr;
    unsigned new_priority;                              // <-- NUEVA variable local

    for (proc_nr=0, rmp=schedproc; proc_nr < NR_PROCS; proc_nr++, rmp++) {

        if (rmp->flags & IN_USE && !is_system_proc(rmp)) {
            // ── RAMA: procesos de usuario ──────────────────────────────────

            if (rmp->quantum_count >= CPU_BOUND_QUANTUM_THRESHOLD) {
                // Proceso CPU-bound: penalizar
                if (rmp->penalty_level < MAX_PENALTY_LEVEL)
                    rmp->penalty_level++;

                new_priority = rmp->base_priority + rmp->penalty_level;
                if (new_priority < MIN_USER_Q)           // clamp: no pasar del mínimo
                    new_priority = MIN_USER_Q;
                if (rmp->priority != new_priority) {
                    rmp->priority = new_priority;
                    schedule_process_local(rmp);
                }

            } else if (rmp->quantum_count == 0 && rmp->penalty_level > 0) {
                // Proceso no CPU-bound con penalización activa: recuperar gradualmente
                rmp->penalty_level--;
                new_priority = rmp->base_priority + rmp->penalty_level;
                if (new_priority > rmp->max_priority)    // clamp: no superar max_priority
                    new_priority = rmp->max_priority;
                if (rmp->priority != new_priority) {
                    rmp->priority = new_priority;
                    schedule_process_local(rmp);
                }
            }

            rmp->quantum_count = 0;                     // reiniciar ventana siempre

        } else if (rmp->flags & IN_USE) {
            // ── RAMA: procesos de sistema ─────────────────────────────────
            // Comportamiento original conservado
            if (rmp->priority > rmp->max_priority) {
                rmp->priority -= 1;
                schedule_process_local(rmp);
            }
        }
    }

    if ((r = sys_setalarm(balance_timeout, 0)) != OK)
        panic("sys_setalarm failed: %d", r);
}
```

---

### Tabla de decisión de `balance_queues`

| Tipo de proceso | `quantum_count` | `penalty_level` | Acción |
|---|---|---|---|
| Usuario | `>= 3` | cualquiera | Sube `penalty_level` (máx 2), aplica `base + penalty` |
| Usuario | `== 0` | `> 0` | Baja `penalty_level` en 1, recupera prioridad gradualmente |
| Usuario | `1` o `2` | cualquiera | No cambia prioridad (zona neutral) |
| Usuario | cualquiera | `== 0` | No cambia prioridad |
| Sistema | — | — | Comportamiento original: sube 1 nivel si fue degradado |

> En todos los casos de usuario, `quantum_count` se reinicia a `0` al final de la ventana.

---

## Flujo completo del mecanismo

```
┌─────────────────────────────────────────────────────────────┐
│  Proceso agota su quantum                                    │
│                                                             │
│  do_noquantum()                                             │
│    ├─ !is_system_proc → quantum_count++                     │
│    └─ priority < MIN_USER_Q → priority += 1 (original)     │
└────────────────────────┬────────────────────────────────────┘
                         │
                    (cada 5 segundos)
                         │
┌────────────────────────▼────────────────────────────────────┐
│  balance_queues()                                            │
│                                                             │
│  Para cada proceso de usuario:                              │
│                                                             │
│  quantum_count >= 3 ?                                       │
│    SI  → penalty_level++ (máx 2)                           │
│          priority = base_priority + penalty_level           │
│          [clamp a MIN_USER_Q]                               │
│                                                             │
│  quantum_count == 0 Y penalty_level > 0 ?                   │
│    SI  → penalty_level--                                    │
│          priority = base_priority + penalty_level           │
│          [clamp a max_priority]                             │
│                                                             │
│  Siempre → quantum_count = 0    (reinicia ventana)          │
└─────────────────────────────────────────────────────────────┘
```

---

## Ejemplo de evolución de prioridad

Supongamos `base_priority = USER_Q = 8`, `MIN_USER_Q = 15`, `MAX_PENALTY_LEVEL = 2`.

| Ventana | `quantum_count` | `penalty_level` | `priority` efectiva | Descripción |
|:---:|:---:|:---:|:---:|---|
| 0 | — | 0 | 8 | Proceso recién creado |
| 1 | 5 | 1 | 9 | CPU-bound → primer castigo |
| 2 | 4 | 2 | 10 | CPU-bound → castigo máximo |
| 3 | 0 | 1 | 9 | Se volvió interactivo → recupera 1 nivel |
| 4 | 0 | 0 | 8 | Totalmente recuperado |

---

## Resumen de todos los cambios

| Archivo | Elemento | Tipo de cambio | Descripcion |
|---|---|---|---|
| `schedproc.h` | `base_priority` | Agregado | Prioridad de referencia para calcular penalización |
| `schedproc.h` | `quantum_count` | Agregado | Contador de quantums agotados en la ventana actual |
| `schedproc.h` | `penalty_level` | Agregado | Nivel de castigo acumulado (0–2) |
| `schedule.c` | `CPU_BOUND_QUANTUM_THRESHOLD` | Agregado | Umbral de quantums para activar castigo (valor: 3) |
| `schedule.c` | `MAX_PENALTY_LEVEL` | Agregado | Castigo máximo acumulable (valor: 2) |
| `schedule.c` | `do_noquantum()` | Modificado | Incrementa `quantum_count` en procesos de usuario |
| `schedule.c` | `do_start_scheduling()` | Modificado | Inicializa los 3 nuevos campos en 3 puntos distintos |
| `schedule.c` | `balance_queues()` | Modificado | Implementa la política completa de penalización y recuperación |
