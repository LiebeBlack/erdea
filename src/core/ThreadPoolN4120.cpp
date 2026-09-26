// EdgeDock Studio :: core/ThreadPoolN4120.cpp
#include "core/ThreadPoolN4120.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>

namespace edgedock {
namespace {

constexpr std::uint64_t kRingMask = ThreadPoolN4120::kCapacity - 1;
constexpr DWORD kIdleWaitMs = 25;   // red de seguridad: los workers nunca duermen para siempre

} // namespace

ThreadPoolN4120& ThreadPoolN4120::Instance() {
    static ThreadPoolN4120 pool;
    return pool;
}

ThreadPoolN4120::~ThreadPoolN4120() {
    Stop();
}

void ThreadPoolN4120::Start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;

    // Inicialización del anillo: la celda i queda lista para la posición i.
    for (std::uint64_t i = 0; i < kCapacity; ++i) {
        ring_[i].task = Task{};
        ring_[i].sequence.store(i, std::memory_order_relaxed);
    }
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);

    SYSTEM_INFO info{};
    ::GetSystemInfo(&info);
    const DWORD cpuCount = info.dwNumberOfProcessors;

    for (int index = 0; index < kWorkerCount; ++index) {
        auto* context = new int(index);
        HANDLE handle = ::CreateThread(nullptr, 0,
                                       [](LPVOID parameter) -> DWORD {
                                           auto* slot = static_cast<int*>(parameter);
                                           const int workerIndex = *slot;
                                           delete slot;
                                           ThreadPoolN4120::Instance().WorkerLoop(workerIndex);
                                           return 0;
                                       },
                                       context, CREATE_SUSPENDED, nullptr);
        if (handle == nullptr) {
            delete context;
            continue;
        }

        workerIds_[index].store(::GetThreadId(handle), std::memory_order_relaxed);
        // Los workers viven por debajo de la UI en prioridad: el panel nunca pierde frames.
        ::SetThreadPriority(handle, THREAD_PRIORITY_BELOW_NORMAL);
        if (cpuCount >= 4) {
            // Reparto fijo: núcleo 0 para la UI, 1..3 para el trabajo de fondo.
            const DWORD_PTR mask = static_cast<DWORD_PTR>(1) << (index + 1);
            if (::SetThreadAffinityMask(handle, mask) == 0) {
                // Algunos equipos no permiten fijar afinidad: se sigue sin ella.
            }
        }
        workers_[index] = handle;
        ::ResumeThread(handle);
    }
}

void ThreadPoolN4120::Stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) return;

    NotifyWorkers();
    // Drenaje: lo encolado antes de apagar se ejecuta siempre.
    Task task;
    while (Dequeue(task)) {
        task.Run();
        executed_.fetch_add(1, std::memory_order_relaxed);
    }
    NotifyWorkers();

    for (int index = 0; index < kWorkerCount; ++index) {
        if (workers_[index] != nullptr) {
            HANDLE handle = static_cast<HANDLE>(workers_[index]);
            ::WaitForSingleObject(handle, 2000);
            ::CloseHandle(handle);
            workers_[index] = nullptr;
        }
    }
}

bool ThreadPoolN4120::TrySubmit(const Task& task) {
    if (!task.Valid()) return false;

    std::uint64_t position = tail_.load(std::memory_order_relaxed);
    Cell* cell = nullptr;
    for (;;) {
        cell = &ring_[position & kRingMask];
        const std::uint64_t sequence = cell->sequence.load(std::memory_order_acquire);
        const auto difference = static_cast<std::int64_t>(sequence) - static_cast<std::int64_t>(position);
        if (difference == 0) {
            if (tail_.compare_exchange_weak(position, position + 1, std::memory_order_relaxed)) break;
        } else if (difference < 0) {
            rejected_.fetch_add(1, std::memory_order_relaxed);
            return false;   // cola llena: el llamador decide qué hacer
        } else {
            position = tail_.load(std::memory_order_relaxed);
        }
    }

    cell->task = task;
    cell->task.sequence = submitted_.fetch_add(1, std::memory_order_relaxed) + 1;
    cell->sequence.store(position + 1, std::memory_order_release);   // publica la celda
    NotifyWorkers();
    return true;
}

bool ThreadPoolN4120::Submit(const Task& task) {
    if (!running_.load(std::memory_order_relaxed)) {
        task.Run();
        executed_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (TrySubmit(task)) return true;

    // Reintento breve: el consumidor suele liberar celdas en microsegundos.
    for (int attempt = 0; attempt < 64; ++attempt) {
        ::SwitchToThread();
        if (TrySubmit(task)) return true;
    }
    return false;
}

bool ThreadPoolN4120::Dequeue(Task& out) {
    std::uint64_t position = head_.load(std::memory_order_relaxed);
    Cell* cell = nullptr;
    for (;;) {
        cell = &ring_[position & kRingMask];
        const std::uint64_t sequence = cell->sequence.load(std::memory_order_acquire);
        const auto difference = static_cast<std::int64_t>(sequence) - static_cast<std::int64_t>(position + 1);
        if (difference == 0) {
            if (head_.compare_exchange_weak(position, position + 1, std::memory_order_relaxed)) break;
        } else if (difference < 0) {
            return false;   // vacía
        } else {
            position = head_.load(std::memory_order_relaxed);
        }
    }

    out = cell->task;
    cell->task = Task{};
    cell->sequence.store(position + kCapacity, std::memory_order_release);   // devuelve la celda al productor
    return true;
}

void ThreadPoolN4120::NotifyWorkers() {
    notifyStamp_.fetch_add(1, std::memory_order_relaxed);
    ::WakeByAddressAll(&notifyStamp_);
}

void ThreadPoolN4120::WorkerLoop(int index) {
    wchar_t name[32];
    ::swprintf_s(name, L"EdgeDock.Worker%d", index);
    ::SetThreadDescription(::GetCurrentThread(), name);

    Task task;
    for (;;) {
        if (Dequeue(task)) {
            task.Run();
            executed_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (!running_.load(std::memory_order_relaxed)) return;

        // Patrón sin carreras: se lee el testigo ANTES de la última comprobación de cola,
        // de modo que ninguna publicación queda sin notificación.
        const std::uint64_t stamp = notifyStamp_.load(std::memory_order_acquire);
        if (Dequeue(task)) {
            task.Run();
            executed_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (!running_.load(std::memory_order_relaxed)) return;

        idleWorkers_.fetch_add(1, std::memory_order_relaxed);
        if (::WaitOnAddress(&notifyStamp_, &stamp, sizeof(stamp), kIdleWaitMs) == FALSE) {
            // Timeout: se vuelve a comprobar la cola (y la orden de parada).
            steals_.fetch_add(1, std::memory_order_relaxed);
        }
        idleWorkers_.fetch_sub(1, std::memory_order_relaxed);
    }
}

bool ThreadPoolN4120::WaitForIdle(uint32_t timeoutMs) {
    const ULONGLONG deadline = ::GetTickCount64() + timeoutMs;
    for (;;) {
        if (QueueDepth() == 0) return true;
        if (::GetTickCount64() >= deadline) return false;
        ::Sleep(1);
    }
}

std::size_t ThreadPoolN4120::QueueDepth() const {
    const std::uint64_t head = head_.load(std::memory_order_relaxed);
    const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
    const std::uint64_t pending = tail - head;
    return pending <= kCapacity ? static_cast<std::size_t>(pending) : kCapacity;
}

std::size_t ThreadPoolN4120::SubmittedCount() const { return submitted_.load(std::memory_order_relaxed); }
std::size_t ThreadPoolN4120::ExecutedCount() const { return executed_.load(std::memory_order_relaxed); }
std::size_t ThreadPoolN4120::RejectedCount() const { return rejected_.load(std::memory_order_relaxed); }
std::size_t ThreadPoolN4120::StealCount() const { return steals_.load(std::memory_order_relaxed); }

int ThreadPoolN4120::ActiveWorkers() const {
    return kWorkerCount - static_cast<int>(std::min<std::size_t>(kWorkerCount, idleWorkers_.load(std::memory_order_relaxed)));
}

uint64_t ThreadPoolN4120::ThreadIdOf(int index) const {
    if (index < 0 || index >= kWorkerCount) return 0;
    return workerIds_[index].load(std::memory_order_relaxed);
}

} // namespace edgedock
