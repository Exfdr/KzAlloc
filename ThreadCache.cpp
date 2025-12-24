#include "ThreadCache.h"

namespace KzAlloc {

// 定义 TLS 指针
//thread_local ThreadCache* pTLSThreadCache = nullptr;

void* ThreadCache::Allocate(size_t size) {
    // 1. 计算桶索引
    int index = SizeUtils::Index(size);
    FreeList& list = _freeLists[index];

    // 2. 优先从 FreeList 拿 (Hot Path - 极速)
    if (!list.Empty()) {
        return list.Pop();
    }

    // 3. 没货了，找 CentralCache 进货 (Cold Path)
    return FetchFromCentralCache(index, size);
}

void ThreadCache::Deallocate(void* ptr, size_t size) {
    assert(ptr);

    // 1. 计算桶索引
    int index = SizeUtils::Index(size);

    FreeList& list = _freeLists[index];

    // 2. 归还给 FreeList
    list.Push(ptr);

    // 3. 检测是否囤积了太多内存
    // 如果当前链表长度 > 慢启动阈值，说明该线程可能只是短时间突发分配，
    // 现在不需要这么多了，归还一部分给 CentralCache，避免内存泄露式占用。
    if (list.Size() >= list.MaxSize() + list.MaxNum()) {
        ListTooLong(list, size);
    }
}

void* ThreadCache::FetchFromCentralCache(size_t index, size_t size) {
    FreeList& list = _freeLists[index];

    // 1. 慢启动策略：计算本次应该向 CentralCache 批发多少个
    // 初始 _maxSize 为 1
    // 每次 Fetch，_maxSize + 1，直到达到上限 (例如 512)
    // 这样小对象用得多，批发就多；大对象用得少，批发就少
    size_t batchNum = std::min(list.MaxSize() << 1, list.MaxNum());
    list.SetMaxSize(batchNum); // 更新慢启动阈值

    // 2. 向 CentralCache 申请
    void* start = nullptr;
    void* end = nullptr;
    
    // fetchNum 可能会比 batchNum 少 (CentralCache 也没货了)
    size_t fetchNum = CentralCache::GetInstance()->FetchRangeObj(start, end, batchNum, size);

    assert(fetchNum >= 1);

    // 3. 返回第一个对象给用户
    void* ret = start;

    // 4. 如果批发了多个，剩下的挂到 FreeList 里备用
    if (fetchNum > 1) {
        // start 指向第一个，它要返回给用户，所以 NextObj(start) 才是剩下的链表头
        void* remainStart = NextObj(start);
        
        // 这里的 end 就是 FetchRangeObj 返回的尾节点，可以直接用
        // PushRange 是 O(1) 的
        list.PushRange(remainStart, end, fetchNum - 1);
    }

    return ret;
}

void ThreadCache::ListTooLong(FreeList& list, size_t size) {
    void* start = nullptr;
    void* end = nullptr;

    // 1. 决定归还多少个
    // 策略：归还当前 _maxSize 的数量。
    // 这保证了 ThreadCache 依然保留一部分热数据 (_maxSize 个)，
    // 同时把多出来的部分还回去。
    size_t popNum = list.MaxNum();

    // 2. 从 FreeList 剥离出链表
    list.PopRange(start, end, popNum);

    // 3. 归还给 CentralCache
    // 这里的 start 是链表头，CentralCache 会处理遍历
    CentralCache::GetInstance()->ReleaseListToSpans(start, size);
}

} // namespace KzAlloc