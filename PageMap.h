#pragma once
#include "Common.h"
#include "Span.h"
#include <cstring>
#include <mutex>
#include <new>     // for placement new (if needed) or std::bad_alloc
#include <algorithm> // for std::max

namespace KzAlloc {

// =========================================================================
// Radix Tree 配置 (根据平台自动调整层级)
// =========================================================================
class PageMap {
private:
    // 64位系统: 3 层基数树
    // 有效虚拟地址 48位 - 页偏移 13位 = 35位有效 PageID
    // 划分: Root(12bit) -> Internal(12bit) -> Leaf(11bit)
#if defined(_WIN64) || defined(__x86_64__) || defined(__aarch64__)
    static constexpr int RADIX_TREE_LEVELS = 3;
    static constexpr int BITS_ROOT = 12;
    static constexpr int BITS_INTERNAL = 12;
    static constexpr int BITS_LEAF = 11;

    // 32位系统: 2 层基数树
    // 有效虚拟地址 32位 - 页偏移 13位 = 19位有效 PageID
    // 划分: Root(5bit) -> Leaf(14bit)
#else
    static constexpr int RADIX_TREE_LEVELS = 2;
    static constexpr int BITS_ROOT = 5;
    static constexpr int BITS_LEAF = 14;
    static constexpr int BITS_INTERNAL = 0;
#endif

    // 计算各层数组长度
    static constexpr size_t LEN_ROOT = 1 << BITS_ROOT;
    static constexpr size_t LEN_INTERNAL = 1 << BITS_INTERNAL;
    static constexpr size_t LEN_LEAF = 1 << BITS_LEAF;

private:
    // Leaf Node: 最底层，直接存储 Span 指针
    struct PageMapLeaf {
        Span* values[LEN_LEAF];
    };

    // Internal Node: 中间层，存储 Leaf 指针
    // 仅在 3 层模式下有效
    struct PageMapInternal {
        PageMapLeaf* leafs[LEN_INTERNAL];
    };

public:
    static PageMap* GetInstance() {
    alignas(PageMap) static char _buffer[sizeof(PageMap)];
    static PageMap* _instance = nullptr;
    
    static const bool _inited = [&]() {
        _instance = new (_buffer) PageMap();
        return true;
    }();
    
    (void)_inited;
    // 返回解引用后的对象
    return _instance;
    }

    // ---------------------------------------------------------------------
    // 查映射 (Read): O(1) 无锁
    // ---------------------------------------------------------------------
    inline Span* get(PAGE_ID id) const {
#if defined(_WIN64) || defined(__x86_64__) || defined(__aarch64__)
        // [Layer 1]
        size_t iRoot = id >> (BITS_INTERNAL + BITS_LEAF);
        if (iRoot >= LEN_ROOT) return nullptr;
        // 因为指针赋值是原子的，所以直接读即可
        // 如果读到 nullptr 说明还没分配，直接返回
        auto rootNode = _root[iRoot];
        if (rootNode == nullptr) return nullptr;

        // [Layer 2]
        size_t iInternal = (id >> BITS_LEAF) & (LEN_INTERNAL - 1);
        auto leafNode = rootNode->leafs[iInternal];
        if (leafNode == nullptr) return nullptr;

        // [Layer 3]
        size_t iLeaf = id & (LEN_LEAF - 1);
        return leafNode->values[iLeaf];
#else
        // 32-bit Logic
        size_t iRoot = id >> BITS_LEAF;
        if (iRoot >= LEN_ROOT) return nullptr;
        
        auto leafNode = _root[iRoot];
        if (leafNode == nullptr) return nullptr;

        size_t iLeaf = id & (LEN_LEAF - 1);
        return leafNode->values[iLeaf];
#endif
    }

    // ---------------------------------------------------------------------
    // 设映射 (Write): 线程安全
    // set 通常在 PageCache 有锁的情况下调用，但为了 RadixTree 自身的稳健性，
    // 我们在节点扩容(Ensure)时使用了内部锁
    // ---------------------------------------------------------------------
    void set(PAGE_ID id, Span* span) {
#if defined(_WIN64) || defined(__x86_64__) || defined(__aarch64__)
        // 1. 确保 Root -> Internal 存在
        size_t iRoot = id >> (BITS_INTERNAL + BITS_LEAF);
        EnsureRoot(iRoot);
        
        // 2. 确保 Internal -> Leaf 存在
        size_t iInternal = (id >> BITS_LEAF) & (LEN_INTERNAL - 1);
        EnsureInternal(iRoot, iInternal);

        // 3. 写入最终值
        size_t iLeaf = id & (LEN_LEAF - 1);
        // 这是原子操作，所以不用锁（按字长(8字节)对齐时，直接汇编优化为单mov操作，而不存在64位撕裂导致前32位和后32位分开mov）
        _root[iRoot]->leafs[iInternal]->values[iLeaf] = span;
#else
        size_t iRoot = id >> BITS_LEAF;
        EnsureRoot(iRoot);
        size_t iLeaf = id & (LEN_LEAF - 1);
        _root[iRoot]->values[iLeaf] = span;
#endif
    }

private:
    explicit PageMap() {
        std::memset(_root, 0, sizeof(_root));
    }

    PageMap(const PageMap&) = delete;
    PageMap& operator=(const PageMap&) = delete;

    // 辅助函数：申请节点内存 (使用 SystemAlloc 绕过 malloc)
    // 自动计算需要申请的页数
    // 这里不用接口allocate()主要是因为对象较大
    template<typename NodeType>
    static NodeType* AllocNode() {
        constexpr size_t size = sizeof(NodeType);
        constexpr size_t kpages = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;
        
        void* ptr = SystemAlloc(kpages);
        // SystemAlloc (mmap/VirtualAlloc) 保证返回的内存是清零的
        // 所以我们不需要手动 memset 0
        return static_cast<NodeType*>(ptr);
    }

    // 确保第一层节点存在
    void EnsureRoot(size_t index) {
        if (index >= LEN_ROOT) return;
        
        // Double-Checked Locking (双重检查锁)
        // 绝大多数情况 _root[index] 已经存在，无锁检查能极大提升性能
        if (_root[index] == nullptr) {
            std::lock_guard<std::mutex> lock(_growMtx);
            if (_root[index] == nullptr) {
#if defined(_WIN64) || defined(__x86_64__) || defined(__aarch64__)
                _root[index] = AllocNode<PageMapInternal>();
#else
                _root[index] = AllocNode<PageMapLeaf>();
#endif
                // 内存屏障：确保节点初始化完成后，指针才对其他线程可见
                // (C++11 std::mutex 隐含了 Release 语义，这里是安全的)
            }
        }
    }

#if defined(_WIN64) || defined(__x86_64__) || defined(__aarch64__)
    // 确保第二层节点存在
    void EnsureInternal(size_t iRoot, size_t iInternal) {
        if (_root[iRoot]->leafs[iInternal] == nullptr) {
            std::lock_guard<std::mutex> lock(_growMtx);
            if (_root[iRoot]->leafs[iInternal] == nullptr) {
                _root[iRoot]->leafs[iInternal] = AllocNode<PageMapLeaf>();
            }
        }
    }
#endif

private:
    // 根数组
#if defined(_WIN64) || defined(__x86_64__) || defined(__aarch64__)
    PageMapInternal* _root[LEN_ROOT];
#else
    PageMapLeaf* _root[LEN_ROOT];
#endif

    // 生长锁：只保护节点分配（AllocNode），不保护 Leaf 值的读写
    std::mutex _growMtx; 
};

} // namespace KzAlloc