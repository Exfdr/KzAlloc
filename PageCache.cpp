#include "PageCache.h"
#include <iostream>
#include <cassert>
#include <new> // for placement new

namespace KzAlloc {

// =========================================================================
// PageHeap 实现 (全局路由与自举)
// =========================================================================

PageHeap::PageHeap() {
    // 1. 动态探测硬件性能
    unsigned int cores = std::thread::hardware_concurrency();
    if (cores == 0) cores = 8; // 兜底

    // 2. 设定目标分片数 (Scaling Factor)
    // 针对高核心数机器(>=32)使用 4倍冗余，普通机器 2倍
    size_t target_shards = cores >= 32 ? cores * 4 : cores * 2;

    // 3. 向上取整为 2 的幂 (Next Power of 2) 以便使用位掩码路由
    _shardCount = 1;
    while (_shardCount < target_shards) {
        _shardCount <<= 1;
    }
    
    // 4. 计算路由掩码
    _shardMask = _shardCount - 1;

    // 5. 向 OS 申请裸内存来存放分片数组
    size_t arrayBytes = sizeof(PageCacheShard) * _shardCount;
    size_t kpages = (arrayBytes + PAGE_SIZE - 1) >> PAGE_SHIFT;

    void* ptr = SystemAlloc(kpages);
    _shards = static_cast<PageCacheShard*>(ptr);

    // A. 获取物理内存总字节数
    size_t totalRam = GetSystemPhysicalMemory();

    // B. 设定目标：全局最大缓存物理内存的 25%
    //    或者硬限制：全局最大缓存 4GB (看你业务需求，数据库类应用可以设大点，普通应用设小点)
    size_t maxCacheBytes = totalRam / 4; // 25%
    size_t hardLimit = 4ULL * 1024 * 1024 * 1024; // 4GB
    
    if (maxCacheBytes > hardLimit) {
        maxCacheBytes = hardLimit;
    }

    // C. 转换为全局总页数
    size_t totalThresholdPages = maxCacheBytes >> PAGE_SHIFT;

    // D. 平摊到每个分片
    // 注意：至少保留一定数量 (例如 512 页 / 4MB)，防止抖动太频繁
    size_t shardThreshold = totalThresholdPages / _shardCount;
    if (shardThreshold < 4096) {
        shardThreshold = 4096;
    }

    // E. 通过环境变量配置
    const char* envThreshold = std::getenv("KZALLOC_SHARD_THRESHOLD_PAGES");
    if (envThreshold) {
        size_t val = std::strtoull(envThreshold, nullptr, 10);
        if (val > 0) {
            shardThreshold = val; // 强行覆盖
        }
    }

    // 6. 使用 Placement New 在裸内存上构造对象
    // 这避免了调用全局 new/malloc，解决了递归依赖问题
    for (size_t i = 0; i < _shardCount; ++i) {
        // PageCacheShard 的构造函数会初始化内部的 map 和 mutex
        // 因为 map 使用了 BootstrapAllocator，所以也是安全的
        new (&_shards[i]) PageCacheShard();
        // 注入配置
        _shards[i].SetReleaseThreshold(shardThreshold);
        // 初始化 Shard ID
        _shards[i].InitShard(static_cast<uint8_t>(i));
    }
}

PageHeap::~PageHeap() {
    if (_shards) {
        // 1. 显式调用析构函数
        for (size_t i = 0; i < _shardCount; ++i) {
            _shards[i].~PageCacheShard();
        }
        
        // 2. 归还物理内存
        size_t arrayBytes = sizeof(PageCacheShard) * _shardCount;
        size_t kpages = (arrayBytes + PAGE_ROUND_UP_NUM) >> PAGE_SHIFT;
        SystemFree(_shards, kpages);
        
        _shards = nullptr;
    }
}

size_t PageHeap::GetShardIndex() {
    // Thread Local 缓存 Hash 值，避免重复计算
    // 仅进行一次 Hash 计算，后续全是位运算，极速
    static thread_local size_t tidHash = std::hash<std::thread::id>{}(std::this_thread::get_id());
    return tidHash & _shardMask;
}

Span* PageHeap::NewSpan(size_t k) {
    size_t idx = GetShardIndex();
    
    // 路由到指定分片
    Span* span = _shards[idx].NewSpan(k);
    
    // 标记出生地
    // 必须在这里标记，因为 Shard 内部不知道自己的 Index
    if (span) {
        span->_shardId = static_cast<uint8_t>(idx);
    }
    
    return span;
}

void PageHeap::ReleaseSpan(Span* span) {
    if (!span) return;

    // 读取出生地，归还给原分片
    // 实现了 Arena Isolation，无需担心跨分片死锁
    size_t idx = span->_shardId;
    
    // 简单的越界检查 (理论上不可能触发)
    assert(idx < _shardCount); 
    
    _shards[idx].ReleaseSpan(span);
}

// =========================================================================
// PageCacheShard 实现 (核心逻辑)
// =========================================================================


Span* PageCacheShard::NewSpan(size_t k) {
    std::lock_guard<std::mutex> lock(_mtx);

    // int safety_ctr = 0; // 安全计数器
    while (true) {
        /*
        if (++safety_ctr > 100000) {
            printf("ERROR: Infinite loop detected in PageCache::NewSpan! Map corruption?\n");
            abort(); // 强制退出，产生 Core Dump
        }
        static bool printed = false;
    if (!printed) {
        printf("[DEBUG] PageCache compiled with PAGE_SHIFT = %zu\n", PAGE_SHIFT);
        printed = true;
    }
        */
    // --------------------------------------------------------
    // Phase 1: 尝试从 Hot Cache (热数据) 获取
    // --------------------------------------------------------
    if (k < NPAGES) [[likely]] {
        // 1.1 小对象 Hot Array (Exact Match)
        if (!_spanLists[k].Empty()) {
            return AllocFromHotList(_spanLists[k], k);
        }
        // 1.2 小对象 Hot Array (Split Match)
        for (size_t i = k + 1; i < NPAGES; ++i) {
            if (!_spanLists[i].Empty()) {
                return AllocFromHotList(_spanLists[i], k); 
            }
        }
    } 
    else {
        // Phase 2: 检查 Map
        // 【埋点 A】
        // if (safety_ctr % 1000 == 0) printf("Retry Phase 2: Map Lookup...\n");
        // 1.3 大对象 Hot Map
        auto it = _largeSpanLists.lower_bound(k);
        if (it != _largeSpanLists.end()) {
            // return AllocFromMap(_largeSpanLists, it, k, false);
            // 【修复】：如果遇到脏数据返回 null，continue 重试，自然会找到下一个或者去 SystemAlloc
            Span* ret = AllocFromMap(_largeSpanLists, it, k, false);
            if (ret == nullptr) continue;
            return ret;
        }
    }

    // --------------------------------------------------------
    // Phase 2: 尝试从 Cold Cache (冷数据) 获取
    // --------------------------------------------------------
    // 既然热的没货，与其 SystemAlloc，不如复用冷的 (省去 alloc_pages 开销)
    
    if (k < NPAGES) {
        // 2.1 小对象 Cold Array (Exact Match)
        if (!_releasedSpanLists[k].Empty()) {
            return AllocFromColdList(_releasedSpanLists[k], k);
        }
        // 2.2 小对象 Cold Array (Split Match)
        for (size_t i = k + 1; i < NPAGES; ++i) {
            if (!_releasedSpanLists[i].Empty()) {
                return AllocFromColdList(_releasedSpanLists[i], k);
            }
        }
    }
    
    
    // 2.3 大对象 Cold Map (覆盖了 k >= NPAGES 的情况，
    //      也覆盖了 k < NPAGES 但 Cold Array 没货只能切分大块的情况)
    if (!_releasedLargeSpanLists.empty()) {
        // Phase 2: 检查 Map
        // 【埋点 A】
        // if (safety_ctr % 1000 == 0) printf("Retry Phase 2: Map Lookup...\n");
        auto it = _releasedLargeSpanLists.lower_bound(k);
        if (it != _releasedLargeSpanLists.end()) {
            // ：如果遇到脏数据返回 null，continue 重试，自然会找到下一个或者去 SystemAlloc
            Span* ret = AllocFromMap(_releasedLargeSpanLists, it, k, true);
            if (ret == nullptr) continue;
            return ret;
            // return AllocFromMap(_releasedLargeSpanLists, it, k, true);
        }
    }
    

    // --------------------------------------------------------
    // Phase 3: SystemAlloc (兜底)
    // --------------------------------------------------------
    
    // ... Phase 3: SystemAlloc ...
        // 【埋点 C】
        // if (safety_ctr % 1000 == 0) printf("Retry Phase 3: SystemAlloc...\n");
    // 如果是大对象，直接申请 k
    if (k >= NPAGES) {
        void* ptr = SystemAlloc(k); 
        Span* span = _spanPool.New();
        span->_pageId = (PAGE_ID)ptr >> PAGE_SHIFT;
        span->_n = k;
        span->_isUse = true;
        span->_isCold = false; // 新申请的内存是热的
        // 设置 Shard ID
        span->_shardId = _shardId;
        
        for(size_t i = 0; i < k; ++i) PageMap::GetInstance()->set(span->_pageId + i, span);
        return span;
    } 
    
    // 如果是小对象，批发 1MB 大块放入 Hot Array 并递归
    void* ptr = SystemAlloc(NPAGES - 1);
    Span* bigSpan = _spanPool.New();
    bigSpan->_pageId = (PAGE_ID)ptr >> PAGE_SHIFT;
    bigSpan->_n = NPAGES - 1;
    bigSpan->_isCold = false;
    // 设置 Shard ID
    bigSpan->_shardId = _shardId;
    
    PageMap::GetInstance()->set(bigSpan->_pageId, bigSpan);
    PageMap::GetInstance()->set(bigSpan->_pageId + bigSpan->_n - 1, bigSpan);
    // for(size_t i = 0; i < bigSpan->_n; ++i) PageMap::GetInstance()->set(bigSpan->_pageId + i, bigSpan);
    _spanLists[bigSpan->_n].PushFront(bigSpan);
    _totalFreePages += bigSpan->_n; // 入库，增加热计数
    
    
    }
}

void PageCacheShard::ReleaseSpan(Span* span) {
    std::lock_guard<std::mutex> lock(_mtx);

    // ============================================================
    // 合并逻辑 (Coalescing)
    // 注意：我们需要处理 Hot 和 Cold 的混合合并
    // ============================================================
    // 由于 Release 流程保证了 Span 回到原 Shard，
    // 如果 leftSpan 也是空闲的，且 SystemAlloc 具有物理连续性，
    // 那么 leftSpan 必然也属于当前 Shard 管理。
    // 向左合并
    while (true) {
        PAGE_ID leftId = span->_pageId - 1;
        Span* leftSpan = PageMap::GetInstance()->get(leftId);
        
        // 核心修复：禁止跨分片合并！
        // 必须检查 leftSpan->_shardId == _shardId
        if (leftSpan == nullptr || leftSpan->_isUse || leftSpan->_shardId != _shardId) break;

        // 摘除邻居 (无论它在 Hot 还是 Cold 容器中)
        leftSpan->Remove();
        
        // 只有 Hot 的邻居才占用 _totalFreePages
        // 如果邻居是 Cold 的，它没贡献物理内存计数，所以不能减
        if (!leftSpan->_isCold) {
            _totalFreePages -= leftSpan->_n;
        }
        
        span->_pageId = leftSpan->_pageId;
        span->_n += leftSpan->_n;
        _spanPool.Delete(leftSpan);
    }

    // 向右合并
    while (true) {
        PAGE_ID rightId = span->_pageId + span->_n;
        Span* rightSpan = PageMap::GetInstance()->get(rightId);

        // 核心修复：禁止跨分片合并！
        if (rightSpan == nullptr || rightSpan->_isUse || rightSpan->_shardId != _shardId) break;

        rightSpan->Remove();
        
        if (!rightSpan->_isCold) {
            _totalFreePages -= rightSpan->_n;
        }
        
        span->_n += rightSpan->_n;
        _spanPool.Delete(rightSpan);
    }

    // ============================================================
    // 归还逻辑
    // ============================================================
    
    span->_isUse = false;
    
    // 【关键决策】合并后的 Span 算 Hot 还是 Cold？
    // 策略：只要发生合并，或者归还，我们暂时都视为 "Hot"。
    // 理由：虽然它可能包含 Cold 的部分，但我们把它拉回了活动链表。
    // 如果它很大且长时间不用，触发 ReleaseSomeSpansToSystem 时会再次将其变 Cold。
    span->_isCold = false; 
    
    // 闲置 Span 只映射首尾，节省 Radix Tree 压力
    // for(size_t i = 0; i < span->_n; ++i) PageMap::GetInstance()->set(span->_pageId + i, span);
    PageMap::GetInstance()->set(span->_pageId, span);
    PageMap::GetInstance()->set(span->_pageId + span->_n - 1, span);

    if (span->_n < NPAGES) {
        _spanLists[span->_n].PushFront(span);
    } else {
        _largeSpanLists[span->_n].PushFront(span);
    }
    
    _totalFreePages += span->_n; // 视为 Hot，增加计数

    // ============================================================
    // 触发回收
    // ============================================================
    if (_totalFreePages > _releaseThreshold) {
        ReleaseSomeSpansToSystem();
    }
}

void PageCacheShard::ReleaseSomeSpansToSystem() {
    // 1. 优先回收大对象 (Hot Map -> Cold Map)
    while (_totalFreePages > _releaseThreshold && !_largeSpanLists.empty()) {
        // 取出最大的 SpanList
        auto it = _largeSpanLists.rbegin();
        
        // 注意：反向迭代器删除需要技巧，或者转为正向迭代器
        // _largeSpanLists.erase(std::next(it).base()); // C++11 way
        // 或者更简单：保存 key，退出迭代器再删除
        // 检查并清理空的 Map 节点
        if (it->second.Empty()) {
            _largeSpanLists.erase(std::next(it).base()); // 删除key
            continue;
        }

        Span* span = it->second.PopFront();
        // 如果取出来是空的（僵尸节点），直接跳过，继续循环
        // 此时上面的 erase 已经把它清理掉了，下次循环不会再遇到它
        if (span == nullptr) {
            continue; 
        }

        ReleaseSpanToCold(span);
    }

    // 2. 其次回收小对象 (Hot Array -> Cold Array)
    if (_totalFreePages > _releaseThreshold) {
        for (size_t i = NPAGES - 1; i > 0; --i) {
            SpanList& list = _spanLists[i];
            
            while (_totalFreePages > _releaseThreshold && !list.Empty()) {
                Span* span = list.PopFront();
                ReleaseSpanToCold(span);
            }
            
            // 如果水位已经降下来了，提前退出循环，不再清理更小的桶
            // 保护像 1页、2页 这种极度热点的数据不被轻易回收
            if (_totalFreePages <= _releaseThreshold) break;
        }
    }
}

void PageCacheShard::ReleaseSpanToCold(Span* span) {
    // 1. 状态变更
    _totalFreePages -= span->_n; // 从 Hot 计数扣除
    span->_isCold = true;        // 标记为冷

    // 2. Madvise 释放物理内存
    void* ptr = (void*)(span->_pageId << PAGE_SHIFT);
#ifdef _WIN32
    VirtualFree(ptr, span->_n << PAGE_SHIFT, MEM_DECOMMIT);
#else
    madvise(ptr, span->_n << PAGE_SHIFT, MADV_DONTNEED);
    // printf("[DEBUG] Releasing Cold Span: ptr=%p, pages=%zu. Using madvise.\n", ptr, span->_n);
#endif

    // 3. 挂入 Cold 容器
    if (span->_n < NPAGES) {
        _releasedSpanLists[span->_n].PushFront(span);
    } else {
        _releasedLargeSpanLists[span->_n].PushFront(span);
    }
    
    // 注意：Span 在 PageMap 中的映射保持不变！
    // 这样邻居在合并时，依然可以通过 PageMap 找到这个 Cold Span
}

Span* PageCacheShard::AllocFromHotList(SpanList& list, size_t k) {
    Span* span = list.PopFront();
    
    // 出库
    _totalFreePages -= span->_n;

    // 切分逻辑
    if (span->_n > k) {
        Span* split = _spanPool.New();
        split->_pageId = span->_pageId + k;
        split->_n = span->_n - k;
        split->_isCold = false; // 剩下的也是热的

        span->_n = k;

        // 剩下的挂回 Hot List
        _spanLists[split->_n].PushFront(split);
        _totalFreePages += split->_n; // 回库

        // for(size_t i = 0; i < split->_n; ++i) PageMap::GetInstance()->set(split->_pageId + i, split);
        PageMap::GetInstance()->set(split->_pageId, split);
        PageMap::GetInstance()->set(split->_pageId + split->_n - 1, split);
    }

    // 建立映射
    for (size_t i = 0; i < k; ++i) PageMap::GetInstance()->set(span->_pageId + i, span);
    
    span->_isUse = true;
    span->_isCold = false;
    return span;
}

Span* PageCacheShard::AllocFromColdList(SpanList& list, size_t k) {
    Span* span = list.PopFront();
    
    // Cold Span 不占用 _totalFreePages，所以不需要减计数
    // 但当它被分配出去后，用户写入数据，它会变热。
    // 这里我们不需要加 _totalFreePages，因为它直接变成了 _isUse=true

    // 切分逻辑
    if (span->_n > k) {
        Span* split = _spanPool.New();
        split->_pageId = span->_pageId + k;
        split->_n = span->_n - k;
        split->_isCold = true; // 剩下的依然是冷的

        span->_n = k;

        // 剩下的挂回 Cold List
        _releasedSpanLists[split->_n].PushFront(split);
        
        // Cold 的 Split 也需要维护首尾映射，方便合并
        // for (size_t i = 0; i < split->_n; ++i) PageMap::GetInstance()->set(split->_pageId + i, split);
        PageMap::GetInstance()->set(split->_pageId, split);
        PageMap::GetInstance()->set(split->_pageId + split->_n - 1, split);
    }

    // 建立映射
    for (size_t i = 0; i < k; ++i) PageMap::GetInstance()->set(span->_pageId + i, span);
    
    span->_isUse = true;
    span->_isCold = false; // 激活：变热
    return span;
}

template<typename MapType>
Span* PageCacheShard::AllocFromMap(MapType& map, typename MapType::iterator it, size_t k, bool isCold) {
    Span* span = it->second.PopFront();
    // 如果链表意外为空，说明这是一个脏数据（Ghost Entry）
    // 我们应该清理它，并返回 nullptr 通知调用者重试
    if (span == nullptr) {
        map.erase(it);
        return nullptr;
    }

    // 如果是从 Hot Map 拿的，需要减计数
    if (!isCold) {
        _totalFreePages -= span->_n;
    }

    if (span->_n > k) {
        Span* split = _spanPool.New();
        split->_pageId = span->_pageId + k;
        split->_n = span->_n - k;
        split->_isCold = isCold; // 继承来源的冷热属性

        span->_n = k;

        // 剩下的挂回对应的 Map (Hot->Hot, Cold->Cold)
        // 这里为了简单，我们根据 isCold 标志决定挂哪里
        // 注意：AllocFromMap 是模板，但挂回逻辑需要具体对象
        if (isCold) {
            _releasedLargeSpanLists[split->_n].PushFront(split);
        } else {
            _largeSpanLists[split->_n].PushFront(split);
            _totalFreePages += split->_n;
        }

        // for (size_t i = 0; i < split->_n; ++i) PageMap::GetInstance()->set(split->_pageId + i, split);
        PageMap::GetInstance()->set(split->_pageId, split);
        PageMap::GetInstance()->set(split->_pageId + split->_n - 1, split);
    }

    for (size_t i = 0; i < k; ++i) PageMap::GetInstance()->set(span->_pageId + i, span);
    
    span->_isUse = true;
    span->_isCold = false;
    return span;
}


} // namespace KzAlloc