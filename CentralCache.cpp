#include "CentralCache.h"


namespace KzAlloc {

// 慢启动算法 (根据对齐后的大小计算应该一次性申请多少个aligned_size大小内存)
static size_t CalculateFetchBatchSize(size_t aligned_size) {
    size_t num = MAX_BYTES / aligned_size; 
    if (num == 0) num = 1;
    if (num > 512) num = 512;
    return num;
}

// 计算申请页数
// 此函数现在假设传入的已经是 aligned_size
static size_t CalculatePageNeed(size_t aligned_size) {
    size_t num = CalculateFetchBatchSize(aligned_size);
    size_t npage = (num * aligned_size) >> PAGE_SHIFT; //
    if (npage == 0) npage = 1;
    return npage;
}

size_t CentralCache::FetchRangeObj(void*& start, void*& end, size_t n, size_t size) {
    // 【优化1 - Hot Path】：直接使用 raw_size 查表
    // SizeUtils 保证了 Index(raw_size) == Index(aligned_size)
    // 避免了 RoundUp 的开销，因为如果桶里有货，我们根本不需要知道 aligned_size
    int index = SizeUtils::Index(size); //
    auto& bucket = _spanLists[index];

    // 加自旋锁
    bucket._mtx.lock();

    // 尝试获取 Span
    // 注意：这里传入的是 raw_size，因为 GetOneSpan 只有在真要申请内存时才需要对齐
    Span* span = GetOneSpan(bucket, size);
    assert(span);
    assert(span->_freeList);

    // 从链表提取对象
    // 这里的遍历不可避免，因为 span->_freeList 可能是乱序归还的，物理上不连续
    start = span->_freeList;
    end = start;
    size_t actualNum = 1;
    
    while (actualNum < n && NextObj(end) != nullptr) {
        void* next = NextObj(end);
        __builtin_prefetch(NextObj(next), 0, 3);
        end = next;
        actualNum++;
    }

    span->_freeList = NextObj(end);
    NextObj(end) = nullptr; 
    span->_useCount += actualNum;

    bucket._mtx.unlock();

    return actualNum; 
}

// 入参是 raw_size
Span* CentralCache::GetOneSpan(SpanListBucket<SpinMutex>& bucket, size_t size) {
    // 1. 尝试从桶中查找现成的 Span
    Span* it = bucket.Begin();
    while (it != bucket.End()) {
        if (it->_freeList != nullptr) {
            return it;
        }
        it = static_cast<Span*>(it->_next);
    }

    // 2. 桶空了，进入真正的 Cold Path -> 也就是这里才需要对齐
    // 先解锁
    bucket._mtx.unlock();

    // 在此处进行对齐
    // 这是整个分配路径中唯一一次调用 RoundUp
    size_t aligned_size = SizeUtils::RoundUp(size); 
    size_t kPages = CalculatePageNeed(aligned_size);
    
    PageHeap* ph = PageHeap::GetInstance();
    Span* span = ph->NewSpan(kPages); //
    span->_isUse = true;
    span->_objSize = aligned_size; // 记录对齐后的大小

    // 3. 切分内存 (Linking)
    // 使用 aligned_size 进行切分，保证无碎片
    char* start = (char*)(span->_pageId << PAGE_SHIFT); 
    size_t bytes = span->_n << PAGE_SHIFT; 
    char* end = start + bytes - aligned_size; // 减一个size避免剩余10字节(内存碎片)但却要分配16字节这种情况

    // 初始化链表结构
    // 虽然物理连续，但必须写入 Next 指针，ThreadCache 才能识别
    span->_freeList = start;
    void* tail = start;
    char* cur = start + aligned_size;
    
    // 这里依然需要循环写入指针
    // 虽然我们知道总量是 bytes / aligned_size (乘除法关系)
    // 但我们需要把每一个节点的头部写上 data，这个 O(N) 的内存写入无法省略
    while (cur <= end) {
        NextObj(tail) = cur; 
        tail = cur;          
        cur += aligned_size;         
    }
    NextObj(tail) = nullptr;

    // 重新加锁
    bucket._mtx.lock();
    bucket.PushFront(span);

    return span;
}

void CentralCache::ReleaseListToSpans(void* start, size_t size) {
    // 这里使用 raw_size 查表也是安全的
    int index = SizeUtils::Index(size);
    auto& bucket = _spanLists[index];

    bucket._mtx.lock();

    // int safety_ctr = 0;
    while (start) {
        /*
        if (++safety_ctr > 100000) {
            printf("ERROR: Infinite loop in ReleaseListToSpans! Linked list cycle detected (Double Free)?\n");
            abort();
        }
            */
        void* next = NextObj(start);

        PAGE_ID id = (PAGE_ID)start >> PAGE_SHIFT;
        Span* span = PageMap::GetInstance()->get(id);

#ifdef _DEBUG
        // 校验时最好 RoundUp 一下，或者只校验 Index 是否一致
        // assert(span->_objSize == SizeUtils::RoundUp(size));
#endif
        
        NextObj(start) = span->_freeList;
        span->_freeList = start;
        span->_useCount--;

        if (span->_useCount == 0) {
            bucket.Erase(span);
            span->_freeList = nullptr; 
            span->_next = nullptr;
            span->_prev = nullptr;

            bucket._mtx.unlock();
            PageHeap::GetInstance()->ReleaseSpan(span);
            bucket._mtx.lock();
        }
        
        if (next) [[likely]] {
            __builtin_prefetch(NextObj(next), 1, 3);
        }
            
        start = next;
    }
    bucket._mtx.unlock();
}

} // namespace KzAlloc