# EdgeDock Studio — Omni-Panel de Utilidades

Panel lateral Win32 nativo, anclado al borde derecho, que se contrae a una franja de 2 px y
se despliega con animación suave cuando el cursor choca contra el borde. Interfaz
ultra-minimalista: fondo negro puro OLED (0, 0, 0), texto blanco y acentos en cian neón
(0, 255, 255) y púrpura (160, 32, 240).

## Arquitectura

| Ruta | Responsabilidad |
| --- | --- |
| `src/main.cpp` | DPI por monitor, arranque OLE, bucle de mensajes |
| `src/app/App.*` | Instancia única, icono de bandeja, atajos globales, mantenimiento |
| `src/ui/EdgeDockWindow.*` | Ventana `WS_POPUP`/`WS_EX_TOOLWINDOW`/`WS_EX_TOPMOST`, disparador de borde, animación, módulos |
| `src/ui/Renderer.*` | Direct2D + DirectWrite con brochas y formatos cacheados, recorte por pila |
| `src/ui/Widgets.*` | Estado de interacción immediate-mode, hover animado, menús oscuros |
| `src/ui/Theme.h` | Paleta OLED, acentos y métricas en DIP |
| `src/data/ClipDatabase.*` | SQLite embebido con esquema de clips, índice FTS5 y poda |
| `src/modules/ClipboardHook.*` | `WM_CLIPBOARDUPDATE`, captura de texto/archivos/imágenes/HTML/RTF, búsqueda instantánea |
| `src/modules/TextProcessor.*` | Limpiador de copias de PDF/web, extractor regex, conversores de caja |
| `src/modules/SystemUtils.*` | `ShellExecuteW`, contador en vivo, Focus Assist verificado, suspensión de procesos |
| `src/core/*` | Rutas (`%APPDATA%\EdgeDock`), JSON propio, worker asíncrono, configuración |
| `src/core/PmrPool.*` | Pool PMR sincronizado (acarreo contiguo + listas libres) y arena por frame de 512 KB |
| `src/core/ThreadPoolN4120.*` | Cola MPMC lock-free (anillo de Vyukov) con 3 workers, `WaitOnAddress` y afinidad por núcleo |
| `src/ui/Widgets.*` | Immediate-mode GUI: botones, switches, listas virtualizadas y glifos vectoriales |
| `src/modules/ScratchpadManager.*` | Notas `.md` con pestañas, autoguardado asíncrono por slots PMR y creación por drop |
| `src/modules/DragDrop.*` | `IDataObject` (arrastrar clips fuera) e `IDropTarget` (soltar texto/archivos) |

## Datos locales

- `%APPDATA%\EdgeDock\clips.db` — historial multi-nivel (WAL + FTS5).
- `%APPDATA%\EdgeDock\config.json` — geometría, atajos, lista `cpu_hogs`, tema.
- `%APPDATA%\EdgeDock\Notes` — notas `.md`; `Clips\` — imágenes capturadas en BMP.
- `%APPDATA%\EdgeDock\edgedock.log` — diagnóstico.

## Perfil de rendimiento

- `/O2 /Ob2 /Oi /Ot /Gy /Gw /GL` + `/arch:SSE2` + `/fp:fast` y enlace `/LTCG /OPT:REF /OPT:ICF`.
- Excepciones y RTTI desactivados (`_HAS_EXCEPTIONS=0`, `/EHs-c-`, `/GR-`) en los archivos del
  hot-path: `PmrPool.cpp`, `ThreadPoolN4120.cpp`, `DragDrop.cpp`, `Widgets.cpp`.
- Sin heap del CRT en el bucle de dibujo: las cadenas por frame salen de `mem::FrameArena` y la
  lista de clips sólo recorre las filas visibles.
- Los clips soltados y los buffers de autoguardado viven en `mem::PoolResource::Global()`.

## Terceros

`third_party/sqlite3` contiene la amalgama de SQLite 3.53.4 (dominio público) con FTS5
habilitado. `CMakeLists.txt` la compila dentro del ejecutable; si el archivo no está
presente, lo descarga con `FetchContent` durante la configuración.
