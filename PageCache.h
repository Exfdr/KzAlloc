#pragma once

#include "Common.h"
#include "Span.h"
#include "ObjectPool.h"
#include "PageMap.h"
#include "BootstrapAllocator.h"
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include <algorithm>

namespace KzAlloc {

// 128页（1MB @ 8KB Page）以内的 Span 挂在桶里，超过的挂在 map 里
// 浪费一个位置(实际上内存占用很小)，但是换来了代码可读性和大量的CPU sub指令避免(不用-1来对齐)
static constexpr size_t NPAGES = 129; 

// =========================================================================
// PageCacheShard
// 每个分片独立管理一部分内存，拥有独立的锁、Span池和大对象表
// =========================================================================
class alignas(CACHE_LINE_SIZE) PageCacheShard {
public:
    explicit PageCacheShard() = default;
    ~PageCacheShard() = default;
    
    PageCacheShard(const PageCacheShard&) = delete;
    PageCacheShard& operator=(const PageCacheShard&) = delete;

    Span* NewSpan(size_t k);
    void ReleaseSpan(Span* span);

    // 设置回收阈值接口
    void SetReleaseThreshold(size_t thresholdPages) {
        _releaseThreshold = thresholdPages;
    }

    // 初始化 Shard ID
    void InitShard(uint8_t id) {
        _shardId = id;
    }
    
    std::mutex& GetMutex() { return _mtx; }

private:
    // 将一部分 Hot Span 转化为 Cold Span
    void ReleaseSomeSpansToSystem();
    
    // 具体的单体回收动作
    void ReleaseSpanToCold(Span* span);

    // 辅助函数：从指定的热/冷容器中切分 Span
    Span* AllocFromHotList(SpanList& list, size_t k);
    Span* AllocFromColdList(SpanList& list, size_t k);
    
    // Map 的辅助函数稍微复杂，因为要处理迭代器
    template<typename MapType>
    Span* AllocFromMap(MapType& map, typename MapType::iterator it, size_t k, bool isCold);

private:
    // 大于 128 页的 Span，用 map 管理以支持 Best-Fit 查找
    // 使用 BootstrapAllocator 替换默认分配器，防止递归调用全局 new
    using LargeSpanMap = std::map<
        size_t, 
        SpanList, 
        std::less<size_t>, 
        BootstrapAllocator<std::pair<const size_t, SpanList>>
    >;
    // using LargeSpanMap = std::map<size_t, SpanList>;
    // ==========================================================
    // Hot Data (物理内存存在，可以直接读写)
    // ==========================================================
    SpanList _spanLists[NPAGES];        // 1~128 页
    LargeSpanMap _largeSpanLists;       // >128 页

    // ==========================================================
    // Cold Data (物理内存已 madvise，虚拟地址保留)
    // ==========================================================
    // 小 Span 的冷数据存放处，O(1) 存取
    SpanList _releasedSpanLists[NPAGES];
    // 大 Span 的冷数据存放处
    LargeSpanMap _releasedLargeSpanLists;


    // ==========================================================
    // 元数据管理
    // ==========================================================
    // Span 对象的池化分配器 (每个分片独立，减少元数据分配竞争)
    // 这里使用的是 SpinLock 保护的 ObjectPool
    ObjectPool<Span> _spanPool;
    std::mutex _mtx;

    // 回收阈值控制
    // 记录当前 Shard 缓存了多少页。仅统计 Hot Pages，如果超过阈值，就归还给 OS
    size_t _totalFreePages = 0;
    
    // 阈值设定：例如每个分片最大缓存 512MB 内存 (65536 页)
    // 8个分片就是 4GB。可根据实际需求调整
    // 如果是 32 核 128 分片，这个值应该调小，比如 2048 (16MB)
    size_t _releaseThreshold = 256;

    // 记录当前 Shard 的 ID
    uint8_t _shardId = 0;
};

// =========================================================================
// PageHeap
// 全局单例，负责路由和分片管理
// =========================================================================
class PageHeap {
public:
    static PageHeap* GetInstance() {
    // 1. 预留足够的栈/静态区内存，并保证对齐
    // 编译器看到这只是一个 char 数组，不会产生析构行为
    alignas(PageHeap) static char _buffer[sizeof(PageHeap)];
    
    static PageHeap* _instance = nullptr;
    
    // 2. 利用 C++11 static 局部变量初始化是线程安全的特性
    // 这个 lambda 表达式只会执行一次
    static const bool _inited = [&]() {
        // 3. Placement New: 在 _buffer 上原地调用构造函数
        // 这一步不分配内存，只初始化对象
        _instance = new (_buffer) PageHeap();
        return true;
    }();
    
    (void)_inited; // 防止编译器报“未使用变量”警告
    return _instance;
    }

    Span* NewSpan(size_t k);
    void ReleaseSpan(Span* span);

private:
    // 构造函数中进行自举初始化
    PageHeap();
    
    // 析构函数中归还系统内存
    ~PageHeap();
    
    // 高性能路由函数
    size_t GetShardIndex();

private:
    PageCacheShard* _shards = nullptr; // 动态数组指针 (指向 SystemAlloc 的内存)
    size_t _shardCount = 0;            // 分片数量 (2的幂)
    size_t _shardMask = 0;             // 路由掩码

};

} // namespace KzAlloc