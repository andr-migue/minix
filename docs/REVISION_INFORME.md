# Revisión del Informe Técnico — THE MATCOM MINIX

> Revisión generada el 18 de mayo de 2026.  
> Se revisaron: redacción y consistencia interna, exactitud técnica contra el código fuente, y coherencia con los requisitos por hito.

---

## Problemas críticos (corregir antes de entregar)

### 1. VirtualBox vs VMware — contradicción directa

- **Línea 66** (Introducción): *"preparar el entorno de trabajo en MINIX sobre **VirtualBox**"*
- **Línea 75** (Subsección): *"instalando **VMware Workstation**"*

El párrafo de introducción quedó desactualizado cuando se corrigió el cuerpo del documento. Hay que cambiar "VirtualBox" por "VMware" en la línea 66.

---

### 2. MIN_USER_Q = 14 pero el código dice 15

- **Informe línea 292**: declara `MIN_USER_Q = 14`
- **Código real** (`minix/include/minix/config.h`): `#define MIN_USER_Q (NR_SCHED_QUEUES - 1)` con `NR_SCHED_QUEUES = 16` → **MIN_USER_Q = 15**
- **Screenshots** (figura 4): muestran `PRI=15`, no 14

El valor está mal en toda la sección del scheduler: definición, tabla de comportamiento esperado, y flujo completo. Cambiar 14 → 15 en todos los lugares donde aparece.

---

### 3. Tabla teórica predice prioridad 9, las capturas muestran 15

- **Informe líneas 426–432**: la tabla del "Flujo completo" dice que `cpu_bound` llega a prioridad máxima **9** (`base=7 + MAX_PENALTY_LEVEL=2 = 9`)
- **Screenshots (figura 4)**: muestran `PRI=15`

**Causa de la discrepancia:** `do_noquantum()` degrada la prioridad en **+1 por cada quantum agotado** durante la ventana (comportamiento original conservado), además de la penalización que aplica `balance_queues()` al final de la ventana. La tabla solo documenta la penalización de `balance_queues` pero no la degradación acumulada intra-ventana. En 5 segundos con uso intensivo de CPU se pueden agotar muchos quantums, llevando la prioridad efectiva hasta `MIN_USER_Q = 15`.

La tabla necesita una nota explicando que el valor observado puede ser mayor que `base + penalty_level` por la degradación acumulada del mecanismo original de `do_noquantum()`.

---

### 4. Falta el listing del "bloque de inicialización temprana"

En la sección **C. `do_start_scheduling()`** (línea 374), se anuncia el bloque de inicialización antes del `switch` y se explica con texto, pero **el listing de código fue eliminado**. Hay un párrafo vacío donde debería estar el código.

El código que falta es:

```c
/* Initialize penalty mechanism fields */
rmp->base_priority = USER_Q;
rmp->quantum_count = 0;
rmp->penalty_level = 0;
```

Debe agregarse como `lstlisting` entre la introducción del bloque y la explicación en prosa.

---

### 5. Falta el listing completo de `balance_queues()`

En la sección **D. `balance_queues()`** (línea 409), el informe dice *"La implementación se encuentra en el repositorio"* y pasa directamente a describir las decisiones de implementación. La función más importante del proyecto no tiene ningún `lstlisting` con su código.

El código completo de la función modificada está en `minix/servers/sched/schedule.c` (aproximadamente líneas 380–428) y debe incluirse en el informe, igual que se incluyó el código de `do_noquantum()` y las ramas del `switch`.

---

## Problemas importantes

### 6. El bug de `pthread` no tiene evidencia de validación real

La subsección "Comportamiento esperado tras la corrección" (líneas 174–184) describe *teóricamente* qué debería ocurrir, pero **no hay ningún output real** del programa de prueba ejecutado después de aplicar el fix. La última oración dice "Para verificar la corrección dentro de la VM basta recompilar..." — son instrucciones futuras, no evidencia de que la prueba se realizó.

Se debe agregar: captura de pantalla o texto copiado de la terminal mostrando las dos llamadas a `pthread_mutex_trylock` con sus retornos (0 y EDEADLK).

---

### 7. Solo un ejemplo de ejecución de `tree`

El título de la subsección es **"Ejemplos de ejecución"** (línea 236, plural), pero solo aparece **un ejemplo**: `tree ../` desde `/root`. Faltan al menos:

- Ejecución sin argumentos (ruta actual)
- Ejecución con ruta absoluta
- Comportamiento ante un directorio sin permisos de lectura

---

### 8. "Bloqueado indefinidamente" es técnicamente impreciso

**Línea 134**: *"el programa quedaba bloqueado indefinidamente"*

Una recursión infinita no produce un bloqueo; produce un crecimiento ilimitado de la pila hasta que el proceso falla. El término correcto sería algo como: *"la pila crece sin límite hasta que el proceso falla o el sistema lo interrumpe"*. La palabra "bloqueo" implica espera pasiva (como en un mutex bloqueado), que no es lo que ocurre aquí.

---

### 9. El bug del clamp no está documentado en la sección de cambios

El informe menciona en "Resultados globales" (línea 496) que *"durante las pruebas se detectó y corrigió un bug en la condición de clamp de `balance_queues()` (`<` en lugar de `>`)"*, pero este bug **nunca se documenta en la sección de cambios del scheduler**. Es un hallazgo relevante del proceso de desarrollo y merece al menos un párrafo en la subsección de `balance_queues()` explicando qué era, por qué pasó, y cómo se detectó.

---

## Problemas menores

| # | Problema | Ubicación |
|---|---|---|
| 10 | Mezcla de tiempos verbales: algunos párrafos en presente, otros en pasado | Varios párrafos de la introducción |
| 11 | Oraciones muy largas: línea 62 tiene una sola oración de ~8 líneas | Línea 62 |
| 12 | "Manicomio" en motd mencionado sin contexto en sección de resultados | Línea 493 |
| 13 | Las referencias de orientación (líneas 525–527) no tienen URL ni ubicación accesible externamente | Sección de referencias |
| 14 | La declaración de uso de IA es vaga sobre qué partes fueron generadas vs. supervisadas | Sección 6 |

---

## Resumen de prioridad

| Prioridad | Problema | Esfuerzo estimado |
|---|---|---|
| 🔴 | Cambiar "VirtualBox" → "VMware" en introducción | 1 línea |
| 🔴 | Corregir `MIN_USER_Q`: 14 → 15 en definición, tabla y flujo | ~5 cambios |
| 🔴 | Agregar nota explicando discrepancia tabla (9) vs captura (15) | 1 párrafo |
| 🔴 | Restaurar listing del bloque de inicialización temprana | 6 líneas de código |
| 🔴 | Agregar listing completo de `balance_queues()` | ~50 líneas de código |
| 🟡 | Agregar output real del test de `pthread` (captura o texto) | 1 captura o bloque de texto |
| 🟡 | Agregar 1–2 ejemplos más de `tree` | ~15 líneas |
| 🟡 | Precisar "bloqueado indefinidamente" → comportamiento de stack overflow | 1 oración |
| 🟡 | Documentar el bug del clamp en la sección de cambios | 1 párrafo |
