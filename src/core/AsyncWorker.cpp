// EdgeDock Studio :: core/AsyncWorker.cpp
#include "core/AsyncWorker.h"

#include "core/AppPaths.h"

#include <memory>
#include <utility>

namespace edgedock {

AsyncWorker::~AsyncWorker() {
    Stop();
}

void AsyncWorker::Start() {
    std::lock_guard<std::mutex> guard(mutex_);
    if (running_) return;
    stopping_ = false;
    running_ = true;
    thread_ = std::thread([this]() { Loop(); });
}

void AsyncWorker::Stop() {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (!running_) return;
        stopping_ = true;
        running_ = false;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();

    std::deque<std::function<void()>> leftovers;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        leftovers.swap(jobs_);
        delayed_.clear();
    }
    for (auto& job : leftovers) {
        if (job) {
            // Drenaje de los trabajos no ejecutados para no perder escrituras pendientes.
            job();
        }
    }
    DrainMainQueue();
}

bool AsyncWorker::Running() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return running_;
}

void AsyncWorker::Post(std::function<void()> job) {
    if (!job) return;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (!running_) {
            // Sin worker activo se ejecuta en línea: el trabajo no se descarta nunca.
            job();
            ++completed_;
            return;
        }
        jobs_.push_back(std::move(job));
    }
    cv_.notify_one();
}

void AsyncWorker::PostDelayed(std::function<void()> job, std::chrono::milliseconds delay) {
    if (!job) return;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (!running_) {
            job();
            ++completed_;
            return;
        }
        delayed_.push_back(DelayedJob{std::chrono::steady_clock::now() + delay, std::move(job)});
    }
    cv_.notify_one();
}

void AsyncWorker::PostToMain(HWND target, std::function<void()> job) {
    if (!job) return;
    if (target != nullptr) {
        auto* holder = new std::function<void()>(std::move(job));
        if (!::PostMessageW(target, kMsgWorkerCallback, 0, reinterpret_cast<LPARAM>(holder))) {
            // Ventana destruida: se ejecuta el trabajo aquí para no filtrar memoria.
            (*holder)();
            delete holder;
        }
        return;
    }
    std::lock_guard<std::mutex> guard(mainMutex_);
    mainQueue_.push_back(std::move(job));
}

void AsyncWorker::DispatchMainCallback(LPARAM lParam) {
    std::unique_ptr<std::function<void()>> holder(reinterpret_cast<std::function<void()>*>(lParam));
    if (holder && *holder) {
        (*holder)();
    }
}

void AsyncWorker::DrainMainQueue() {
    std::deque<std::function<void()>> pending;
    {
        std::lock_guard<std::mutex> guard(mainMutex_);
        pending.swap(mainQueue_);
    }
    for (auto& job : pending) {
        if (job) job();
    }
}

size_t AsyncWorker::PendingJobs() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return jobs_.size() + delayed_.size();
}

size_t AsyncWorker::CompletedJobs() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return completed_;
}

void AsyncWorker::RunOne(std::function<void()> job) {
    try {
        job();
    } catch (...) {
        // Ninguna tarea puede tumbar el bucle del worker.
        paths::AppendLog(L"AsyncWorker: tarea terminada con excepción desconocida");
    }
    std::lock_guard<std::mutex> guard(mutex_);
    ++completed_;
}

void AsyncWorker::Loop() {
    ::SetThreadDescription(::GetCurrentThread(), L"EdgeDock.Worker");
    // Una tarea jamás debe degradar la latencia de la UI: prioridad por debajo de normal.
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (stopping_ && jobs_.empty() && delayed_.empty()) return;

            // Al apagar, los retardos pendientes se ejecutan de inmediato para no
            // perder la última escritura (autoguardado de notas, poda de la base...).
            if (stopping_ && !delayed_.empty()) {
                for (auto& entry : delayed_) jobs_.push_back(std::move(entry.job));
                delayed_.clear();
            }

            // Promover los retardos vencidos a la cola inmediata.
            const auto now = std::chrono::steady_clock::now();
            for (size_t i = 0; i < delayed_.size();) {
                if (delayed_[i].when <= now) {
                    jobs_.push_back(std::move(delayed_[i].job));
                    delayed_.erase(delayed_.begin() + static_cast<ptrdiff_t>(i));
                } else {
                    ++i;
                }
            }

            if (!jobs_.empty()) {
                job = std::move(jobs_.front());
                jobs_.pop_front();
            } else {
                if (delayed_.empty()) {
                    cv_.wait(lock, [this]() { return stopping_ || !jobs_.empty() || !delayed_.empty(); });
                } else {
                    auto next = delayed_.front().when;
                    for (const auto& entry : delayed_) {
                        if (entry.when < next) next = entry.when;
                    }
                    cv_.wait_until(lock, next, [this]() {
                        return stopping_ || !jobs_.empty() || !delayed_.empty();
                    });
                }
                continue;
            }
        }
        RunOne(std::move(job));
    }
}

} // namespace edgedock
