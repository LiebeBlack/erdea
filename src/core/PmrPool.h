#pragma once
// EdgeDock Studio :: core/PmrPool.h
// Memoria del hot-path: pool sincronizado de un único acarreo contiguo (clases de tamaño en
// potencias de dos + listas libres intrusivas) y arena monotónica por frame sobre buffer
// estático del hilo. Ninguna ruta de dibujo toca el heap del CRT.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>

namespace edgedock::mem {

void* RawAllocate(std::size_t bytes);   // VirtualAlloc, alineado a 64 bytes
void RawFree(void* block) noexcept;     // VirtualFree(MEM_RELEASE)

class PoolResource final : public std::pmr::memory_resource {
public:
    static PoolResource& Global();

    explicit PoolResource(std::size_t totalBytes = 32u * 1024u * 1024u);
    ~PoolResource() override;
    PoolResource(const PoolResource&) = delete;
    PoolResource& operator=(const PoolResource&) = delete;

    std::size_t BytesInUse() const noexcept;
    std::size_t PeakBytesInUse() const noexcept;
    std::size_t ReservedBytes() const noexcept;
    std::size_t AllocationCount() const noexcept;
    std::size_t FreeListReuseCount() const noexcept;
    std::size_t LargeBlockCount() const noexcept;
    std::size_t ExhaustedCount() const noexcept;   // peticiones que no cupieron en el acarreo

protected:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override;
    void do_deallocate(void* block, std::size_t bytes, std::size_t alignment) override;
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override;

private:
    struct Impl;
    void UpdatePeak() noexcept;
    std::unique_ptr<Impl> impl_;
};

// Arena del hilo de UI: bump pointer sobre un buffer estático de 512 KB, reiniciada en cada
// frame. Si una cadena no cabe, FrameStringV() cae a un scratch reciclado del hilo en lugar
// de pedir memoria nueva (el texto nunca se pierde y nunca se llama al heap).
class FrameArena {
public:
    static constexpr std::size_t kBufferBytes = 512u * 1024u;

    static void BeginFrame() noexcept;
    static void EndFrame() noexcept;
    static std::pmr::memory_resource* Resource() noexcept;
    static std::size_t UsedBytes() noexcept;
    static std::size_t RemainingBytes() noexcept;
    static std::size_t PeakBytes() noexcept;
    static bool IsExhausted() noexcept;
};

template <typename T>
using FrameAllocator = std::pmr::polymorphic_allocator<T>;

using FrameString = std::pmr::basic_string<wchar_t, std::char_traits<wchar_t>, FrameAllocator<wchar_t>>;

FrameString FrameStringV(const wchar_t* format, ...);
FrameString FrameStringOf(std::wstring_view text);

} // namespace edgedock::mem
