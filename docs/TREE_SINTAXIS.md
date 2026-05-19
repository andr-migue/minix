# tree.c: Sintaxis y funciones de C explicadas

## Headers incluidos

```c
#include <stdio.h>      // printf, perror, snprintf
#include <string.h>     // strcmp
#include <dirent.h>     // DIR, struct dirent, opendir, readdir, closedir
#include <sys/stat.h>   // struct stat, lstat, S_ISDIR, S_ISLNK
#include <limits.h>     // PATH_MAX
#include <unistd.h>     // getcwd
```

---

## Funciones del sistema de ficheros

### `opendir` — abrir un directorio

```c
#include <dirent.h>

DIR *opendir(const char *path);
```

- Abre el directorio en `path` y devuelve un puntero `DIR *` (el "handle" del directorio)
- Si falla (no existe, sin permisos) devuelve `NULL`
- `DIR` es un tipo opaco: no se accede a sus campos directamente, solo se pasa a `readdir` y `closedir`

```c
DIR *dir = opendir(path);
if (dir == NULL) { perror(path); return; }
```

---

### `readdir` — leer la siguiente entrada del directorio

```c
#include <dirent.h>

struct dirent *readdir(DIR *dirp);
```

- Cada llamada devuelve un puntero a la siguiente entrada del directorio
- Devuelve `NULL` cuando no quedan más entradas (fin del directorio)
- El puntero devuelto apunta a memoria interna que se sobreescribe en la siguiente llamada (no se debe liberar ni guardar)

```c
struct dirent *entry;
while ((entry = readdir(dir)) != NULL) {
    // procesar entry
}
```

#### `struct dirent`

```c
struct dirent {
    ino_t  d_ino;       // Número de inodo
    char   d_name[];    // Nombre del fichero/directorio (sin el path)
};
```

El campo usado en tree.c es solo `d_name`:

```c
printf("%s\n", entry->d_name);
```

Las entradas `.` (directorio actual) y `..` (padre) siempre aparecen y hay que filtrarlas manualmente:

```c
if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
    continue;
```

---

### `closedir` — cerrar el directorio

```c
#include <dirent.h>

int closedir(DIR *dirp);
```

- Libera los recursos asociados al handle `DIR *`
- Siempre debe llamarse cuando ya no se necesita el directorio
- Devuelve 0 si OK, -1 si error

```c
closedir(dir);
```

---

### `lstat` — obtener metadatos de un fichero (sin seguir symlinks)

```c
#include <sys/stat.h>

int lstat(const char *path, struct stat *buf);
```

- Rellena la estructura `struct stat` con metadatos del fichero: tipo, tamaño, permisos, fechas, etc.
- A diferencia de `stat()`, si `path` es un **symlink**, devuelve información del propio symlink, no del fichero al que apunta
- Devuelve 0 si OK, -1 si error

```c
struct stat entry_stats;
int check = lstat(fullpath, &entry_stats);
```

#### `struct stat` (campos relevantes)

```c
struct stat {
    mode_t  st_mode;   // Tipo de fichero y permisos
    off_t   st_size;   // Tamaño en bytes
    // ... más campos (uid, gid, tiempos, etc.)
};
```

En tree.c solo se usa `st_mode` para determinar si la entrada es directorio o symlink.

---

### Macros de `st_mode`: `S_ISDIR` y `S_ISLNK`

```c
#include <sys/stat.h>

S_ISDIR(mode_t m)   // ¿Es un directorio?
S_ISLNK(mode_t m)   // ¿Es un enlace simbólico?
```

- Son macros que reciben el campo `st_mode` y devuelven un valor verdadero/falso
- Internamente aplican una máscara de bits sobre `st_mode`

```c
if (S_ISDIR(entry_stats.st_mode) && !S_ISLNK(entry_stats.st_mode))
    print_tree(fullpath, depth + 1);
```

Se comprueba que sea directorio Y que no sea un symlink para evitar seguir enlaces (lo que podría causar bucles infinitos).

Otras macros del mismo grupo (no usadas en tree.c pero parte de la misma familia):

| Macro | Tipo |
|-------|------|
| `S_ISREG(m)` | Fichero regular |
| `S_ISDIR(m)` | Directorio |
| `S_ISLNK(m)` | Enlace simbólico |
| `S_ISCHR(m)` | Dispositivo de caracteres |
| `S_ISBLK(m)` | Dispositivo de bloques |
| `S_ISFIFO(m)` | FIFO / pipe |
| `S_ISSOCK(m)` | Socket |

---

### `getcwd` — obtener el directorio de trabajo actual

```c
#include <unistd.h>

char *getcwd(char *buf, size_t size);
```

- Escribe el path absoluto del directorio actual en `buf`
- `size` debe ser al menos la longitud del path + 1 (para el `\0`)
- Devuelve `buf` si OK, `NULL` si error (p.ej. buffer demasiado pequeño)

```c
char cwd[PATH_MAX];
getcwd(cwd, sizeof(cwd));
path = cwd;
```

---

## Constante `PATH_MAX`

```c
#include <limits.h>

PATH_MAX  // Longitud máxima de un path en el sistema (típicamente 4096 en Linux)
```

Se usa para declarar buffers de tamaño suficiente para cualquier path válido:

```c
char fullpath[PATH_MAX];
char cwd[PATH_MAX];
```

---

## Construcción del path completo con `snprintf`

```c
#include <stdio.h>

int snprintf(char *buf, size_t size, const char *fmt, ...);
```

- Como `sprintf` pero con límite de tamaño: nunca escribe más de `size` bytes (incluido el `\0`)
- Evita desbordamiento de buffer

```c
snprintf(fullpath, sizeof(fullpath), "%s/%s", path, entry->d_name);
```

Construye: `"/ruta/actual"` + `"/"` + `"nombre_entrada"` → `"/ruta/actual/nombre_entrada"`

---

## `perror` — imprimir error del sistema

```c
#include <stdio.h>

void perror(const char *s);
```

- Imprime `s` seguido de `: ` y el mensaje de error correspondiente a `errno` (variable global que las syscalls setean al fallar)
- Útil para dar contexto al error: qué path causó el fallo

```c
if (dir == NULL) {
    perror(path);  // Ej: "/ruta/sin/permisos: Permission denied"
    return;
}
```

---

## `strcmp` — comparar cadenas

```c
#include <string.h>

int strcmp(const char *s1, const char *s2);
```

- Devuelve 0 si las cadenas son iguales, negativo si s1 < s2, positivo si s1 > s2
- Se usa para filtrar las entradas especiales `.` y `..`

```c
if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
    continue;
```

---

## Argumentos de `main`

```c
int main(int argc, char *argv[])
```

| Parámetro | Descripción |
|-----------|-------------|
| `argc` | Número de argumentos (incluye el nombre del programa, siempre >= 1) |
| `argv` | Array de strings; `argv[0]` es el nombre del programa, `argv[1]` el primer argumento |

```c
if (argc > 1)
    path = argv[1];   // Se pasó un path como argumento
else
    getcwd(cwd, sizeof(cwd));  // Usar directorio actual
```

---

## Flujo de las llamadas al sistema

```
main()
  │
  ├─ argc > 1 ?  →  usar argv[1]
  └─ no         →  getcwd() → obtener directorio actual
                                    │
                              print_tree(path, 0)
                                    │
                              opendir(path)     ← abre el DIR*
                                    │
                              readdir(dir)      ← itera entradas
                                    │
                              lstat(fullpath)   ← obtiene st_mode
                                    │
                              S_ISDIR() ?       ← ¿es directorio?
                                    │  sí
                              print_tree(fullpath, depth+1)  ← recursión
                                    │
                              closedir(dir)     ← cierra el DIR*
```
