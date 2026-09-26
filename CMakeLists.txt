cmake_minimum_required(VERSION 3.24)
project(EdgeDockStudio LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

# Perfil "extremo" para hardware de bajos recursos (Celeron N4120, 4 núcleos): SSE2 fijo,
# optimización agresiva, metadatos RTTI fuera y excepciones desactivadas en el hot-path.
option(EDGEDOCK_XTREME "Optimizaciones extremas y sin excepciones en el hot-path" ON)

# --------------------------------------------------------------------------------------
# SQLite 3.53.4 embebido (amalgama en third_party/sqlite3). Si el archivo no está en el
# árbol, se descarga con FetchContent durante la configuración.
# --------------------------------------------------------------------------------------
set(SQLITE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite3")
if(NOT EXISTS "${SQLITE_DIR}/sqlite3.c")
    include(FetchContent)
    FetchContent_Declare(sqlite_amalgamation
        URL https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(sqlite_amalgamation)
    set(SQLITE_DIR "${sqlite_amalgamation_SOURCE_DIR}")
endif()

add_library(sqlite3_edgedock STATIC "${SQLITE_DIR}/sqlite3.c")
target_include_directories(sqlite3_edgedock PUBLIC "${SQLITE_DIR}")
target_compile_definitions(sqlite3_edgedock PRIVATE
    SQLITE_ENABLE_FTS5
    SQLITE_ENABLE_JSON1
    SQLITE_ENABLE_COLUMN_METADATA
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_OMIT_DEPRECATED
    SQLITE_THREADSAFE=1
    SQLITE_USE_URI=1
    SQLITE_DQS=0
    _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(sqlite3_edgedock PRIVATE /O2 /w)
else()
    target_compile_options(sqlite3_edgedock PRIVATE -O2 -w)
endif()

# --------------------------------------------------------------------------------------
# EdgeDock Studio
# --------------------------------------------------------------------------------------
set(EDGEDOCK_SOURCES
    src/main.cpp
    src/app/App.cpp
    src/core/AppPaths.cpp
    src/core/AsyncWorker.cpp
    src/core/Config.cpp
    src/core/Json.cpp
    src/core/PmrPool.cpp
    src/core/TextConv.cpp
    src/core/ThreadPoolN4120.cpp
    src/data/ClipDatabase.cpp
    src/modules/ClipboardHook.cpp
    src/modules/DragDrop.cpp
    src/modules/ScratchpadManager.cpp
    src/modules/SystemUtils.cpp
    src/modules/TextProcessor.cpp
    src/ui/EdgeDockWindow.cpp
    src/ui/Renderer.cpp
    src/ui/Widgets.cpp)

set(EDGEDOCK_HEADERS
    src/app/App.h
    src/core/AppPaths.h
    src/core/AsyncWorker.h
    src/core/Config.h
    src/core/Json.h
    src/core/PmrPool.h
    src/core/TextConv.h
    src/core/ThreadPoolN4120.h
    src/data/ClipDatabase.h
    src/modules/ClipboardHook.h
    src/modules/DragDrop.h
    src/modules/ScratchpadManager.h
    src/modules/SystemUtils.h
    src/modules/TextProcessor.h
    src/ui/EdgeDockWindow.h
    src/ui/Renderer.h
    src/ui/Theme.h
    src/ui/Widgets.h)

# Rutas 100% libres de excepciones y de heap del CRT: son las que corren en el bucle de
# dibujo, en los workers y en el camino del drag & drop.
set(EDGEDOCK_NOEXCEPT_SOURCES
    src/core/PmrPool.cpp
    src/core/ThreadPoolN4120.cpp
    src/modules/DragDrop.cpp
    src/ui/Widgets.cpp)

set_source_files_properties(${EDGEDOCK_NOEXCEPT_SOURCES} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

add_executable(EdgeDockStudio WIN32 ${EDGEDOCK_SOURCES} ${EDGEDOCK_HEADERS})
target_include_directories(EdgeDockStudio PRIVATE src)
target_compile_definitions(EdgeDockStudio PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN)

if(MSVC)
    # Base de rendimiento: inlining agresivo, intrínsecos, códigos rápidos y SSE2 garantizado.
    target_compile_options(EdgeDockStudio PRIVATE
        /utf-8 /W4 /permissive- /Zc:__cplusplus /EHsc /MP
        /O2 /Ob2 /Oi /Ot /GT /Gy /Gw /GL
        /fp:fast
        /arch:SSE2
        /wd4100   # parámetros sin usar en los thunks de Win32
        /wd4324)  # padding intencional de alignas (líneas de caché)
    target_link_options(EdgeDockStudio PRIVATE /LTCG /OPT:REF /OPT:ICF)
    set_property(TARGET EdgeDockStudio PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    if(EDGEDOCK_XTREME)
        # Excepciones desactivadas en el hot-path: con _HAS_EXCEPTIONS=0 el CRT aborta en
        # lugar de propagar, y /EHs-c- evita generar tablas de desenrollado en esos TUs.
        set_source_files_properties(${EDGEDOCK_NOEXCEPT_SOURCES} PROPERTIES
            COMPILE_DEFINITIONS "_HAS_EXCEPTIONS=0"
            COMPILE_OPTIONS "/EHs-c-;/GR-;/O2;/Ob2;/Oi")
    endif()
else()
    target_compile_options(EdgeDockStudio PRIVATE -O2 -Wall -Wextra -fno-rtti -fno-exceptions -msse2 -municode)
    target_link_options(EdgeDockStudio PRIVATE -static)
endif()

target_link_libraries(EdgeDockStudio PRIVATE
    sqlite3_edgedock
    d2d1
    dwrite
    user32
    gdi32
    kernel32
    shell32
    ole32
    oleaut32
    shlwapi
    comctl32
    psapi
    advapi32)
