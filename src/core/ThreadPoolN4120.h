#pragma once
// EdgeDock Studio :: core/ThreadPoolN4120.h
// Pool de 3 hilos trabajadores con cola MPMC sin cerrojos (bounded ring de Vyukov:
// secuencia por celda + head/tail atómicos) y espera eficiente con WaitOnAddress.
// Diseñado para un Celeron N4120: cero asignaciones por tarea, cero contención en el
// camino rápido y afinidad opcional para repartir los 4 núcleos con el hilo de UI.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace edgedock {

// Tarea sin heap: función + 4 argumentos de tamaño puntero. Cabe en 48 bytes y se copia
// por valor en la celda del anillo.
struct alignas(16) Task {
    void (*fn)(void* a, void* b, void* c, void* d) = nullptr;
    void* argA = nullptr;
    void* argB = nullptr;
    void* argC = nullptr;
    void* argD = nullptr;
    uint64_t sequence = 0;

    bool Valid() const { return fn != nullptr; }
    void Run() const {
        if (fn != nullptr) fn(argA, argB, argC, argD);
    }
};

class ThreadPoolN4120 {
public:
    static constexpr std::size_t kCapacity = 1024;   // potencia de dos obligatoria
    static constexpr int kWorkerCount = 3;           // 3 workers + 1 hilo de UI = 4 núcleos

    static ThreadPoolN4120& Instance();

    ThreadPoolN4120() = default;
    ~ThreadPoolN4120();
    ThreadPoolN4120(const ThreadPoolN4120&) = delete;
    ThreadPoolN4120& operator=(const ThreadPoolN4120&) = delete;

    void Start();
    void Stop();

    // Envío sin bloqueo. Devuelve false si la cola está llena (el llamador decide: reintentar,
    // degradar a ejecución en línea o descartar). Nunca asigna memoria.
    bool TrySubmit(const Task& task);
    // Igual que TrySubmit pero con reintento acotado (útil en el hilo de UI).
    bool Submit(const Task& task);
    // Ejecuta la tarea en el hilo que llama (camino de emergencia cuando la cola está llena).
    static void RunInline(const Task& task) { task.Run(); }

    bool WaitForIdle(uint32_t timeoutMs);

    std::size_t QueueDepth() const;
    std::size_t SubmittedCount() const;
    std::size_t ExecutedCount() const;
    std::size_t RejectedCount() const;
    std::size_t StealCount() const;
    int ActiveWorkers() const;
    uint64_t ThreadIdOf(int index) const;

private:
    struct Cell {
        std::atomic<uint64_t> sequence{0};
        Task task{};
    };

    bool Dequeue(Task& out);
    void WorkerLoop(int index);
    void NotifyWorkers();

    // Padding a líneas de caché: evita el falso compartir entre productores y consumidores.
    alignas(64) Cell ring_[kCapacity]{};
    alignas(64) std::atomic<uint64_t> head_{0};
    alignas(64) std::atomic<uint64_t> tail_{0};
    alignas(64) std::atomic<uint64_t> notifyStamp_{0};
    alignas(64) std::atomic<std::size_t> submitted_{0};
    alignas(64) std::atomic<std::size_t> executed_{0};
    alignas(64) std::atomic<std::size_t> rejected_{0};
    alignas(64) std::atomic<std::size_t> steals_{0};
    alignas(64) std::atomic<std::size_t> idleWorkers_{0};

    void* workers_[kWorkerCount] = {};          // HANDLE, para no incluir windows.h aquí
    std::atomic<uint64_t> workerIds_[kWorkerCount] = {};
    std::atomic<bool> running_{false};
};

// Crea una tarea sin asignar memoria. La firma es exacta a propósito: reinterpretar
// punteros a función de otro tipo sería comportamiento indefinido y el compilador no
// podría aplicar /Ob2 con seguridad.
inline Task MakeTask(void (*fn)(void*, void*, void*, void*), void* a = nullptr, void* b = nullptr,
                     void* c = nullptr, void* d = nullptr) {
    Task task;
    task.fn = fn;
    task.argA = a;
    task.argB = b;
    task.argC = c;
    task.argD = d;
    return task;
}

// Trampolines para funciones sin argumentos o con uno: cero asignaciones, cero lambdas
// capturadoras y tipado estricto.
template <auto Fn>
struct TaskTrampoline0 {
    static void Call(void*, void*, void*, void*) { Fn(); }
};

template <auto Fn>
struct TaskTrampoline1 {
    static void Call(void* a, void*, void*, void*) { Fn(a); }
};

} // namespace edgedock
