#pragma once

#include "Common.h"      //
#include "SpinLock.h"    // 
#include "PageCache.h"  //
#include "PageMap.h"    //
#include <mutex>

namespace KzAlloc {

// 策略模式：将锁的类型泛型化
// 默认使用 SpinMutex，如果想测试 std::mutex 也可以直接换
// 综合来说自旋锁性能会好些，因为大部分都是高速链表操作。
// 但是在申请内存的时候可能会导致CPU大量空转，这时互斥锁会好些，不过，我们会在申请内存前手动释放自旋锁，所以也差不了
// 并且，由于我们尽力降低了并发冲突，所以自旋锁就是最优解
template <class LockType>
struct alignas(CACHE_LINE_SIZE) SpanListBucket : public SpanList {
    LockType _mtx;
};

// 单例模式：中心缓存
class CentralCache {
public:
    static CentralCache* GetInstance() {
    alignas (CentralCache) static char _buffer[sizeof(CentralCache)];
    static CentralCache* _instance = nullptr;
    
    static const bool _inited = [&]() {
        _instance = new (_buffer) CentralCache();
        return true;
    }();
    
    (void)_inited;
    return _instance;
    }

    // 从中心缓存获取一定数量的对象给 ThreadCache
    size_t FetchRangeObj(void*& start, void*& end, size_t n, size_t size);

    // 将 ThreadCache 归还的一串对象释放回对应的 Span
    void ReleaseListToSpans(void* start, size_t size);

    

private:
    CentralCache() = default;
    CentralCache(const CentralCache&) = delete;
    CentralCache& operator=(const CentralCache&) = delete;

    // 获取一个非空的 Span
    // 为了解耦，这里传入具体的 Bucket 类型
    Span* GetOneSpan(SpanListBucket<SpinMutex>& bucket, size_t size);

private:
    // 这里显式指定使用 SpinMutex
    // 如果未来想对比性能，改成 SpanListBucket<std::mutex> 即可
    SpanListBucket<SpinMutex> _spanLists[MAX_NFREELISTS]; 
};

} // namespace KzAlloc