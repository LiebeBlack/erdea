// EdgeDock Studio :: core/PmrPool.cpp
#include "core/PmrPool.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace edgedock::mem {
namespace {

constexpr std::size_t kMinClassShift = 4;     // 16 B
constexpr std::size_t kMaxClassShift = 14;    // 16 KB
constexpr std::size_t kClassCount = kMaxClassShift - kMinClassShift + 1;
constexpr std::size_t kPoolAlignment = 16;
constexpr std::size_t kLargeAlignment = 64;   // lo que garantiza VirtualAlloc
constexpr std::size_t kLargeThreshold = std::size_t(1) << kMaxClassShift;

// Cabecera pegada al bloque grande (justo antes del payload alineado) para poder liberarlo.
struct LargeHeader {
    void* region;      // base devuelta por VirtualAlloc
    std::size_t total; // tamaño total de la región
};

std::size_t ClassIndexFor(std::size_t bytes) {
    std::size_t shift = kMinClassShift;
    while ((std::size_t(1) << shift) < bytes && shift < kMaxClassShift) ++shift;
    return shift - kMinClassShift;
}

std::size_t ClassBlockSize(std::size_t index) {
    return std::size_t(1) << (kMinClassShift + index);
}

std::uintptr_t AlignUp(std::uintptr_t value, std::size_t alignment) {
    return (value + alignment - 1u) & ~static_cast<std::uintptr_t>(alignment - 1u);
}

// Buffer estático de la arena del frame, uno por hilo (la UI trabaja en el hilo principal).
struct FrameBuffer {
    alignas(64) std::byte bytes[FrameArena::kBufferBytes];
    std::size_t cursor = 0;
    std::size_t peak = 0;
    bool exhausted = false;

    void Reset() noexcept {
        if (cursor > peak) peak = cursor;
        cursor = 0;
        exhausted = false;
    }
};

FrameBuffer& FrameState() {
    static thread_local FrameBuffer buffer;
    return buffer;
}

// Scratch reciclado: se usa solo cuando la arena del frame se queda corta.
std::wstring& FrameScratch() {
    static thread_local std::wstring scratch;
    return scratch;
}

class FrameResource final : public std::pmr::memory_resource {
public:
    static FrameResource& Instance() {
        static FrameResource resource;
        return resource;
    }

protected:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        FrameBuffer& buffer = FrameState();
        if (alignment < kPoolAlignment) alignment = kPoolAlignment;
        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(buffer.bytes);
        const std::uintptr_t aligned = AlignUp(base + buffer.cursor, alignment);
        const std::size_t offset = static_cast<std::size_t>(aligned - base);
        if (offset + bytes > FrameArena::kBufferBytes) {
            // Sin memoria en la arena: se marca y se devuelve nullptr. FrameStringV() mide
            // antes de asignar, así que este camino no debería alcanzarse nunca en la UI.
            buffer.exhausted = true;
            return nullptr;
        }
        buffer.cursor = offset + bytes;
        return reinterpret_cast<void*>(aligned);
    }

    void do_deallocate(void*, std::size_t, std::size_t) override {
        // Monotónica: todo se recupera de golpe en BeginFrame().
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

} // namespace

void* RawAllocate(std::size_t bytes) {
    if (bytes == 0) bytes = kLargeAlignment;
    return ::VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

void RawFree(void* block) noexcept {
    if (block != nullptr) ::VirtualFree(block, 0, MEM_RELEASE);
}

// -----------------------------------------------------------------------------
// PoolResource
// -----------------------------------------------------------------------------

struct PoolResource::Impl {
    void* freeLists[kClassCount] = {};
    std::byte* arena = nullptr;
    std::size_t arenaBytes = 0;
    std::size_t cursor = 0;
    SRWLOCK lock = SRWLOCK_INIT;

    std::atomic<std::size_t> inUse{0};
    std::atomic<std::size_t> peakInUse{0};
    std::atomic<std::size_t> allocations{0};
    std::atomic<std::size_t> reuse{0};
    std::atomic<std::size_t> largeBlocks{0};
    std::atomic<std::size_t> exhausted{0};
};

PoolResource& PoolResource::Global() {
    static PoolResource instance;
    return instance;
}

PoolResource::PoolResource(std::size_t totalBytes) : impl_(std::make_unique<Impl>()) {
    if (totalBytes < 1024u * 1024u) totalBytes = 1024u * 1024u;
    impl_->arena = static_cast<std::byte*>(RawAllocate(totalBytes));
    impl_->arenaBytes = impl_->arena != nullptr ? totalBytes : 0;
}

PoolResource::~PoolResource() = default;

std::size_t PoolResource::BytesInUse() const noexcept { return impl_->inUse.load(std::memory_order_relaxed); }
std::size_t PoolResource::PeakBytesInUse() const noexcept { return impl_->peakInUse.load(std::memory_order_relaxed); }
std::size_t PoolResource::ReservedBytes() const noexcept { return impl_->arenaBytes; }
std::size_t PoolResource::AllocationCount() const noexcept { return impl_->allocations.load(std::memory_order_relaxed); }
std::size_t PoolResource::FreeListReuseCount() const noexcept { return impl_->reuse.load(std::memory_order_relaxed); }
std::size_t PoolResource::LargeBlockCount() const noexcept { return impl_->largeBlocks.load(std::memory_order_relaxed); }
std::size_t PoolResource::ExhaustedCount() const noexcept { return impl_->exhausted.load(std::memory_order_relaxed); }

void PoolResource::UpdatePeak() noexcept {
    std::size_t current = impl_->inUse.load(std::memory_order_relaxed);
    std::size_t peak = impl_->peakInUse.load(std::memory_order_relaxed);
    while (current > peak && !impl_->peakInUse.compare_exchange_weak(peak, current, std::memory_order_relaxed)) {
        // Reintento: otro hilo subió el pico mientras tanto.
    }
}

void* PoolResource::do_allocate(std::size_t bytes, std::size_t alignment) {
    if (bytes == 0) bytes = kPoolAlignment;
    impl_->allocations.fetch_add(1, std::memory_order_relaxed);

    if (bytes > kLargeThreshold || alignment > kPoolAlignment) {
        const std::size_t effective = alignment > kLargeAlignment ? kLargeAlignment : alignment;
        const std::size_t total = bytes + effective + sizeof(LargeHeader);
        void* raw = RawAllocate(total);
        if (raw == nullptr) {
            impl_->exhausted.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        auto* payloadBase = reinterpret_cast<std::byte*>(raw) + sizeof(LargeHeader);
        const std::uintptr_t aligned = AlignUp(reinterpret_cast<std::uintptr_t>(payloadBase), effective);
        auto* header = reinterpret_cast<LargeHeader*>(aligned - sizeof(LargeHeader));
        header->region = raw;
        header->total = total;

        impl_->largeBlocks.fetch_add(1, std::memory_order_relaxed);
        impl_->inUse.fetch_add(total, std::memory_order_relaxed);
        UpdatePeak();
        return reinterpret_cast<void*>(aligned);
    }

    const std::size_t index = ClassIndexFor(bytes);
    const std::size_t blockSize = ClassBlockSize(index);

    ::AcquireSRWLockExclusive(&impl_->lock);
    void* block = impl_->freeLists[index];
    if (block != nullptr) {
        impl_->freeLists[index] = *static_cast<void**>(block);
        ::ReleaseSRWLockExclusive(&impl_->lock);
        impl_->reuse.fetch_add(1, std::memory_order_relaxed);
    } else {
        if (impl_->cursor + blockSize > impl_->arenaBytes) {
            ::ReleaseSRWLockExclusive(&impl_->lock);
            impl_->exhausted.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        block = impl_->arena + impl_->cursor;
        impl_->cursor += blockSize;
        ::ReleaseSRWLockExclusive(&impl_->lock);
    }

    impl_->inUse.fetch_add(blockSize, std::memory_order_relaxed);
    UpdatePeak();
    return block;
}

void PoolResource::do_deallocate(void* block, std::size_t bytes, std::size_t alignment) {
    if (block == nullptr) return;

    if (bytes > kLargeThreshold || alignment > kPoolAlignment) {
        auto* header = reinterpret_cast<LargeHeader*>(static_cast<std::byte*>(block) - sizeof(LargeHeader));
        const std::size_t total = header->total;
        void* region = header->region;
        impl_->inUse.fetch_sub(total, std::memory_order_relaxed);
        impl_->largeBlocks.fetch_sub(1, std::memory_order_relaxed);
        RawFree(region);
        return;
    }

    const std::size_t index = ClassIndexFor(bytes);
    const std::size_t blockSize = ClassBlockSize(index);
    ::AcquireSRWLockExclusive(&impl_->lock);
    *static_cast<void**>(block) = impl_->freeLists[index];
    impl_->freeLists[index] = block;
    ::ReleaseSRWLockExclusive(&impl_->lock);
    impl_->inUse.fetch_sub(blockSize, std::memory_order_relaxed);
}

bool PoolResource::do_is_equal(const std::pmr::memory_resource& other) const noexcept {
    return this == &other;
}

// -----------------------------------------------------------------------------
// FrameArena
// -----------------------------------------------------------------------------

void FrameArena::BeginFrame() noexcept {
    FrameState().Reset();
}

void FrameArena::EndFrame() noexcept {
    FrameBuffer& buffer = FrameState();
    if (buffer.cursor > buffer.peak) buffer.peak = buffer.cursor;
}

std::pmr::memory_resource* FrameArena::Resource() noexcept {
    return &FrameResource::Instance();
}

std::size_t FrameArena::UsedBytes() noexcept {
    return FrameState().cursor;
}

std::size_t FrameArena::RemainingBytes() noexcept {
    const FrameBuffer& buffer = FrameState();
    return buffer.cursor < kBufferBytes ? kBufferBytes - buffer.cursor : 0;
}

std::size_t FrameArena::PeakBytes() noexcept {
    return FrameState().peak;
}

bool FrameArena::IsExhausted() noexcept {
    return FrameState().exhausted;
}

FrameString FrameStringV(const wchar_t* format, ...) {
    wchar_t scratch[384];
    va_list args;
    va_start(args, format);
    int written = _vsnwprintf_s(scratch, std::size(scratch), _TRUNCATE, format, args);
    va_end(args);

    const wchar_t* source = scratch;
    std::size_t length = written > 0 ? static_cast<std::size_t>(written) : 0;

    if (length >= std::size(scratch) - 1) {
        // Texto largo: segunda pasada con el scratch reciclado del hilo.
        va_start(args, format);
        const int realLength = _vscwprintf(format, args);
        va_end(args);
        if (realLength > 0) {
            std::wstring& spare = FrameScratch();
            spare.resize(static_cast<std::size_t>(realLength) + 1, L'\0');
            va_start(args, format);
            written = _vsnwprintf_s(spare.data(), spare.size(), _TRUNCATE, format, args);
            va_end(args);
            if (written > 0) {
                spare.resize(static_cast<std::size_t>(written));
                source = spare.c_str();
                length = static_cast<std::size_t>(written);
            }
        }
    }

    FrameString out(FrameArena::Resource());
    if (length == 0) return out;
    if (FrameArena::RemainingBytes() > (length + 1) * sizeof(wchar_t) + 64) {
        out.reserve(length);
        out.assign(source, length);
        return out;
    }

    // La arena del frame no da para más: se reutiliza el scratch del hilo (una sola reserva
    // durante toda la vida del proceso) para no tocar el heap del CRT.
    std::wstring& spare = FrameScratch();
    spare.assign(source, length);
    out.assign(spare.begin(), spare.end());
    return out;
}

FrameString FrameStringOf(std::wstring_view text) {
    FrameString out(FrameArena::Resource());
    if (FrameArena::RemainingBytes() > (text.size() + 1) * sizeof(wchar_t) + 64) {
        out.assign(text.begin(), text.end());
        return out;
    }
    std::wstring& fallback = FrameScratch();
    fallback.assign(text.begin(), text.end());
    out.assign(fallback.begin(), fallback.end());
    return out;
}

} // namespace edgedock::mem
