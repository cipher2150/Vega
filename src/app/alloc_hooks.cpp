// Global operator new/delete replacement for the service binary: forwards to malloc/free
// and counts allocations per registered thread (see core/alloc_counter.hpp).
// Note what this does NOT see: malloc called directly by C libraries (OpenSSL, zlib).

#include "core/alloc_counter.hpp"

#include <cstdlib>
#include <new>

namespace {

void* allocate(std::size_t size) {
  cb::alloc::note_allocation();
  if (void* p = std::malloc(size == 0 ? 1 : size)) {
    return p;
  }
  throw std::bad_alloc();
}

void* allocate_aligned(std::size_t size, std::align_val_t align) {
  cb::alloc::note_allocation();
  const auto a = static_cast<std::size_t>(align);
  // aligned_alloc requires size to be a multiple of the alignment.
  const std::size_t rounded = (size + a - 1) / a * a;
  if (void* p = std::aligned_alloc(a, rounded == 0 ? a : rounded)) {
    return p;
  }
  throw std::bad_alloc();
}

}  // namespace

void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void* operator new(std::size_t size, std::align_val_t align) { return allocate_aligned(size, align); }
void* operator new[](std::size_t size, std::align_val_t align) { return allocate_aligned(size, align); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  cb::alloc::note_allocation();
  return std::malloc(size == 0 ? 1 : size);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  cb::alloc::note_allocation();
  return std::malloc(size == 0 ? 1 : size);
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
