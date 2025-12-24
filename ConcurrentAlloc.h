#pragma once

#include "Common.h"
#include "ThreadCache.h"
#include "PageCache.h"
#include "ObjectPool.h"

namespace KzAlloc {

// 声明 ThreadCache 创建器，避免在头文件中包含过多实现细节
// 这里的 ObjectPool 必须使用 SystemAlloc，绝对不能依赖 malloc
static void* CreateThreadCache() {
    static ObjectPool<ThreadCache> tcPool;
    return tcPool.New();
}

static void DestroyThreadCache(void* ptr) {
    static ObjectPool<ThreadCache> tcPool;
    tcPool.Delete(static_cast<ThreadCache*>(ptr));
}

// 定义一个 RAII 管理类
class ThreadCacheManager {
public:
    ~ThreadCacheManager() {
        // 线程退出时自动调用
        if (_tlsCache) {
            DestroyThreadCache(_tlsCache);
            _tlsCache = nullptr;
        }
    }

    ThreadCache* Get() {
        if (_tlsCache == nullptr) {
            _tlsCache = static_cast<ThreadCache*>(CreateThreadCache());
        }
        return _tlsCache;
    }

private:
    ThreadCache* _tlsCache = nullptr;
};

// 使用 thread_local 管理这个对象，而不是直接管理指针
static thread_local ThreadCacheManager tls_manager;

// ==========================================================
// 核心申请接口
// ==========================================================
static inline void* malloc(size_t size) {
    // 1. 处理超大内存 (> 256KB)
    // PageCache 的对齐单位是页 (8KB)
    if (size > MAX_BYTES) [[unlikely]] {
        // 向上对齐到页大小
        size_t alignSize = SizeUtils::RoundUp(size);
        size_t kPages = alignSize >> PAGE_SHIFT;

        // 向 PageHeap 申请
        Span* span = PageHeap::GetInstance()->NewSpan(kPages);
        span->_objSize = alignSize;
        span->_isUse = true;

        // 计算返回地址
        void* ptr = (void*)(span->_pageId << PAGE_SHIFT);
        return ptr;
    }

    // 2. 处理普通小内存 (<= 256KB)
    // 走 ThreadCache (TLS)
    /*
    if (tls_manager.Get() == nullptr) {
        // 惰性初始化: 第一次分配时才创建
        pTLSThreadCache = static_cast<ThreadCache*>(CreateThreadCache());
    }
        */

    return tls_manager.Get()->Allocate(size);
}

// ==========================================================
// 1. 优化版 Realloc (Sized Realloc)
// 场景：STL 容器扩容，或者用户知道原始大小
// 优势：完全跳过 PageMap 查找，O(1) 完成
// ==========================================================
static inline void* realloc(void* ptr, size_t old_size, size_t new_size) {
    // 1. 处理特殊情况
    if (ptr == nullptr) [[unlikely]] {
        return KzAlloc::malloc(new_size);
    }
    if (new_size == 0) [[uhnlikely]] {
        KzAlloc::free(ptr, old_size);
        return nullptr;
    }

    // 2. 计算对齐后的大小 (Size Class)
    // 注意：这里即使 old_size 是未对齐的 (如 13)，RoundUp 后也会变成 16，与 ThreadCache 逻辑一致
    size_t old_aligned = SizeUtils::RoundUp(old_size);
    size_t new_aligned = SizeUtils::RoundUp(new_size);

    // 3. 原地复用策略 (In-Place)
    // 情况 A: 规格相同 (例如从 17 扩容到 25，都属于 32 字节桶) -> 直接返回
    if (new_aligned == old_aligned) {
        return ptr;
    }
    
    // 情况 B: 缩容 (Shrink) -> 懒惰策略
    // 如果新大小比旧大小小，为了避免频繁抖动，我们通常选择不搬迁
    // (除非差异巨大，但在通用内存池中，保留原块通常是更优解)
    if (new_aligned < old_aligned) [[unlikely]] {
        return ptr;
    }

    // 4. 异地扩容 (Grow)
    void* new_ptr = KzAlloc::malloc(new_size);
    if (new_ptr != nullptr) {
        // 核心优化：只拷贝用户声称的有效数据长度
        // 这里 old_size 可能小于 old_aligned (例如用户实际只用了 13 字节)
        // 拷贝 13 字节即可，虽然拷贝 16 字节也安全，但精确拷贝指令更少
        std::memcpy(new_ptr, ptr, old_size);
        
        // 释放旧内存 (调用 Sized Free，不查 PageMap)
        KzAlloc::free(ptr, old_size);
    }

    return new_ptr;
}

// ==========================================================
// 2. 标准版 Realloc (Standard C Interface)
// 场景：兼容标准 C 接口，或者不知道旧大小时使用
// 代价：需要查 PageMap，多一次内存访问
// ==========================================================
static inline void* realloc(void* ptr, size_t new_size) {
    if (ptr == nullptr) [[unlikely]] return KzAlloc::malloc(new_size);
    if (new_size == 0) [[unlikely]] {
        KzAlloc:free(ptr);
        return nullptr;
    }

    // 核心差异：必须查 PageMap 找到 Span 才能知道旧大小
    // 这是一个相对较重的操作 (虽然是基数树 O(1)，但涉及 Cache Miss)
    PAGE_ID id = (PAGE_ID)ptr >> PAGE_SHIFT;
    Span* span = PageMap::GetInstance()->get(id);
    
    // 获取 Span 记录的对象大小 (这是对齐后的大小，例如 16)
    size_t old_aligned_size = span->_objSize;

    // 复用优化版逻辑
    // 注意：这里传入 old_aligned_size 作为 old_size
    // memcpy 会拷贝整个对齐块 (包括 padding)，这是安全的
    return KzAlloc::realloc(ptr, old_aligned_size, new_size);
}

// ==========================================================
// 核心释放接口
// ==========================================================
static inline void free(void* ptr) {
    if (ptr == nullptr) return;

    // 1. 通过地址反查 Span
    // 我们的 PageMap 记录了每个页对应的 Span 指针
    PAGE_ID id = (PAGE_ID)ptr >> PAGE_SHIFT;
    Span* span = PageMap::GetInstance()->get(id);

    // 这里的 span 不应该为空，除非用户释放了野指针
    if (span != nullptr) {
        size_t size = span->_objSize;

        // 2. 判断是大内存还是小内存
        if (size > MAX_BYTES) [[unlikely]] {
            // 大内存：直接还给 PageHeap
            PageHeap::GetInstance()->ReleaseSpan(span);
        } else {
            // 小内存：还给 ThreadCache
            // 注意：这里需要再次检查 TLS，虽然理论上释放时 TLS 肯定存在，
            // 但如果线程在极端情况下（如线程退出时析构顺序问题）可能需要防御性处理
            /*
            if (pTLSThreadCache == nullptr) {
                 pTLSThreadCache = static_cast<ThreadCache*>(CreateThreadCache());
            }
                 */
            tls_manager.Get()->Deallocate(ptr, size);
        }
    } else {
        assert(false); // 提醒用户释放了非法地址
    }
}

static inline void free(void* ptr, size_t size) {
    if (size > MAX_BYTES) [[unlikely]]{
        // 大对象还是走 PageHeap，这里可以复用之前的逻辑，或者直接走 PageMap 查 Span
        // 因为大对象不常见，这里稍微慢点没关系，为了安全可以回退到 ConcurrentFree
            KzAlloc::free(ptr); 
            return;
        }
    
        // 小对象直接走 TLS，跳过 PageMap 查找！
        tls_manager.Get()->Deallocate(ptr, size);
}

} // namespace KzAlloc