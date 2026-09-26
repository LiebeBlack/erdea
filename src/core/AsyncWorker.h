#pragma once
// EdgeDock Studio :: core/AsyncWorker.h
// Hilo trabajador único con cola de tareas + marshal de vuelta al hilo de UI.
// Se usa para el autoguardado de notas, el Poda de la base de datos y el IO de disco.

#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace edgedock {

// Mensaje que transporta un std::function<void()> desde el worker al hilo de UI.
inline constexpr UINT kMsgWorkerCallback = WM_APP + 0x51;

class AsyncWorker {
public:
    AsyncWorker() = default;
    ~AsyncWorker();
    AsyncWorker(const AsyncWorker&) = delete;
    AsyncWorker& operator=(const AsyncWorker&) = delete;

    void Start();
    void Stop();
    bool Running() const;

    // Encola trabajo inmediato en el hilo trabajador.
    void Post(std::function<void()> job);
    // Encola trabajo con retardo mínimo (se ejecuta en el worker cuando vence).
    void PostDelayed(std::function<void()> job, std::chrono::milliseconds delay);
    // Ejecuta `job` en el hilo de UI: vía PostMessage si `target` es válido, o en la
    // siguiente llamada a DrainMainQueue() si no lo es.
    void PostToMain(HWND target, std::function<void()> job);
    // Debe llamarse desde WndProc al recibir kMsgWorkerCallback.
    static void DispatchMainCallback(LPARAM lParam);
    // Ejecuta los callbacks pendientes cuando no hay HWND disponible.
    void DrainMainQueue();

    size_t PendingJobs() const;
    size_t CompletedJobs() const;

private:
    struct DelayedJob {
        std::chrono::steady_clock::time_point when;
        std::function<void()> job;
    };

    void Loop();
    void RunOne(std::function<void()> job);

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> jobs_;
    std::vector<DelayedJob> delayed_;
    std::thread thread_;
    bool running_ = false;
    bool stopping_ = false;
    size_t completed_ = 0;

    std::mutex mainMutex_;
    std::deque<std::function<void()>> mainQueue_;
};

} // namespace edgedock
