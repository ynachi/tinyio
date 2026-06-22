#include "tinyio/detail/stack.hpp"

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>

#include <sys/mman.h>

#include "error.hpp"

#if __has_include(<valgrind/valgrind.h>)
    #include <valgrind/valgrind.h>
    #define TINYIO_HAVE_VALGRIND 1
#else
    #define TINYIO_HAVE_VALGRIND                0
    #define VALGRIND_STACK_REGISTER(start, end) (0U)
    #define VALGRIND_STACK_DEREGISTER(id)       ((void)0)
#endif

namespace tinyio::detail::stack
{

namespace
{

[[nodiscard]] std::size_t align_up(const std::size_t value, const std::size_t alignment) noexcept
{
    return (value + alignment - 1U) & ~(alignment - 1U);
}

}  // namespace

Result<Stack> allocate_stack(std::size_t usable_size, std::size_t guard_size) noexcept
{
    const long page_long = ::sysconf(_SC_PAGESIZE);
    if (page_long <= 0)
    {
        return fail(EINVAL, "allocate_stack");
    }
    const auto page = static_cast<std::size_t>(page_long);
    usable_size = std::max<std::size_t>(usable_size, page * 4U);
    usable_size = align_up(usable_size, page);
    guard_size = align_up(guard_size, page);

    if (usable_size > static_cast<std::size_t>(PTRDIFF_MAX) ||
        guard_size > static_cast<std::size_t>(PTRDIFF_MAX) - usable_size)
    {
        return fail(ENOMEM, "allocate_stack");
    }

    const std::size_t mapping_size = guard_size + usable_size;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_STACK
    flags |= MAP_STACK;
#endif

    void* mapping = ::mmap(nullptr, mapping_size, PROT_NONE, flags, -1, 0);
    if (mapping == MAP_FAILED)
    {
        return fail(errno, "mmap_stack");
    }

    auto* base = static_cast<std::byte*>(mapping);
    auto* low = base + guard_size;
    if (::mprotect(low, usable_size, PROT_READ | PROT_WRITE) != 0)
    {
        const int err = errno;
        (void)::munmap(mapping, mapping_size);
        return fail(err, "mprotect_stack");
    }

    Stack stack;
    stack.mapping = mapping;
    stack.mapping_size = mapping_size;
    stack.low = low;
    stack.high = low + usable_size;
    stack.usable_size = usable_size;
    stack.valgrind_id = VALGRIND_STACK_REGISTER(stack.low, stack.high);
    return stack;
}

void release_stack(Stack& stack) noexcept
{
    if (stack.mapping == nullptr)
    {
        return;
    }
    if (stack.valgrind_id != 0U)
    {
        VALGRIND_STACK_DEREGISTER(stack.valgrind_id);
        stack.valgrind_id = 0;
    }
    (void)::munmap(stack.mapping, stack.mapping_size);
    stack = Stack{};
}

}  // namespace tinyio::detail::stack
