#pragma once
#include "Common.h"
#include "ObjectPool.h"
#include <limits>
#include <type_traits>

namespace KzAlloc {

// 这是一个专门给内存池内部容器（如 PageCache 中的 std::map）使用的分配器
// 它保证不调用 global operator new，也不经过 ThreadCache，从而避免递归死锁
template <typename T>
class BootstrapAllocator {

public:
    using value_type = T;
    using pointer = T*;
    using const_pointer = const T*;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;

    // 标准 Rebind 结构，兼容 std::list / std::map 等节点型容器
    template <typename U>
    struct rebind {
        using other = BootstrapAllocator<U>;
    };

    explicit BootstrapAllocator() noexcept = default;
    
    template <typename U>
    BootstrapAllocator(const BootstrapAllocator<U>&) noexcept {}

    ~BootstrapAllocator() = default;

    // 核心分配接口
    T* allocate(size_t n) {
        // 场景 1: 单个对象分配 (std::map/std::list 节点的典型场景)
        // 走 ObjectPool，速度最快，且完全自举
        if (n == 1) {
            return GetPool().AllocateMemory();
        }

        // 场景 2: 数组分配 (std::vector 扩容场景)
        // 走 SystemAlloc 直接向 OS 批发，避免 ObjectPool 处理不了大块连续内存（ObjectPool是单块链表式，块与块之间不保证连续）
        if (n > std::numeric_limits<size_t>::max() / sizeof(T)) {
            throw std::bad_alloc();
        }
        
        size_t bytes = n * sizeof(T);
        // 计算需要的页数 (向上取整)
        size_t kpages = (bytes + PAGE_SIZE - 1) >> PAGE_SHIFT;
        
        void* ptr = SystemAlloc(kpages);
        return static_cast<T*>(ptr);
    }

    // 核心释放接口
    void deallocate(T* p, size_t n) {
        if (p == nullptr) return;

        // 场景 1: 单个对象回收
        if (n == 1) {
            GetPool().FreeMemory(p);
            return;
        }

        // 场景 2: 数组回收
        size_t bytes = n * sizeof(T);
        size_t kpages = (bytes + PAGE_SIZE - 1) >> PAGE_SHIFT;
        SystemFree(p, kpages);
    }

    // 传统Alloctor标准（C++11以下）要求实现construct()和destroy()
    // 高版本可以省略，此时std::allocator_traits 会自动替你的construct()调用 placement new
    // destroy()则是std::allocator_traits 会自动替你调用 p->~T()
/*
    // std::allocator_traits 内部逻辑伪代码
static void construct(Alloc& a, T* p, Args&&... args) {
    // 1. 检查 Alloc 是否有自定义的 construct 成员函数
    if constexpr (has_custom_construct_v<Alloc>) {
        a.construct(p, std::forward<Args>(args)...);
    } 
    // 2. 如果没有，使用默认的 Placement New
    else {
        new (static_cast<void*>(p)) T(std::forward<Args>(args)...);
    }
}
*/
    // 对象构造 (Placement New)
    template <typename U, typename... Args>
    void construct(U* p, Args&&... args) {
        new(p) U(std::forward<Args>(args)...);
    }

    // 对象析构
    template <typename U>
    void destroy(U* p) {
        p->~U();
    }

private:
    // 静态单例 ObjectPool
    // 利用 C++ 模板特性，BootstrapAllocator<T> 和 BootstrapAllocator<U> 
    // 会拥有各自独立的静态 ObjectPool<T> 和 ObjectPool<U>
    static ObjectPool<T>& GetPool() {
        static ObjectPool<T> pool;
        return pool;
    }
};

// 所有的 BootstrapAllocator 都是无状态的，因此被视为相等
template <typename T, typename U>
bool operator==(const BootstrapAllocator<T>&, const BootstrapAllocator<U>&) { return true; }

template <typename T, typename U>
bool operator!=(const BootstrapAllocator<T>&, const BootstrapAllocator<U>&) { return false; }

} // namespace KzAlloc