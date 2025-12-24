#pragma once

#include "Common.h"
#include "CentralCache.h"

namespace KzAlloc {

// 专门为 ThreadCache 设计的轻量级单向自由链表
// 记录了 tail 和 size，支持 O(1) 的区间插入和删除
class FreeList {
public:
    void Push(void* obj) {
        assert(obj);
        // 头插法
        NextObj(obj) = _head;
        _head = obj;
        
        // 如果是第一个节点，tail 也是它
        if (_tail == nullptr) {
            _tail = obj;
        }
        _size++;
    }

    void* Pop() {
        assert(_head);
        void* obj = _head;
        _head = NextObj(_head);
        
        if (_head == nullptr) {
            _tail = nullptr;
        }
        _size--;
        
        return obj;
    }

    // O(1) 插入一段链表
    // start: 链表头, end: 链表尾, n: 数量
    void PushRange(void* start, void* end, size_t n) {
        assert(start && end);
        
        // 把新链表的尾巴接到旧链表的头
        NextObj(end) = _head;
        _head = start;

        // 如果旧链表为空，更新 tail
        if (_tail == nullptr) {
            _tail = end;
        }
        
        _size += n;
    }

    // O(1) 弹出一批对象
    // n: 期望弹出的数量
    // start, end: 输出参数
    void PopRange(void*& start, void*& end, size_t n) {
        assert(n <= _size);
        
        // 这里的实现需要稍微遍历一下找到第 n 个节点的前驱，或者只移除头部的 n 个
        // 由于是单链表，如果要截取前 n 个，我们需要找到第 n 个节点作为 end
        // 但为了性能，ThreadCache 回收时通常不需要极度精确，
        // 或者我们可以在这里做一个循环 (n 通常不大，且是在回收路径，可接受)
        
        start = _head;
        // 走 n-1 步找到 end
        end = _head;
        for (size_t i = 0; i < n - 1; ++i) {
            end = NextObj(end);
        }
        
        // 记录新的 head
        _head = NextObj(end);
        
        // 断开 end 的连接
        NextObj(end) = nullptr;
        
        // 如果取空了，更新 tail
        if (_head == nullptr) {
            _tail = nullptr;
        }
        
        _size -= n;
    }

    bool Empty() const { return _head == nullptr; }
    size_t Size() const { return _size; }
    size_t MaxSize() const { return _maxSize; }
    size_t MaxNum() const { return _maxNum; }
    void SetMaxSize(size_t maxSize) { _maxSize = maxSize; }
    void SetMaxNum(size_t maxNum) { _maxNum = maxNum; }


private:
    void* _head = nullptr;
    void* _tail = nullptr; // 记录尾指针，方便 PushRange O(1)
    size_t _size = 0;      // 当前链表长度
    size_t _maxSize = 1;   // 慢启动阈值 (限制该链表最大能挂多少个)
    size_t _maxNum;        // _maxSize上限，由构造函数赋值
};

class ThreadCache {
public:

    explicit ThreadCache() {
       for (int i = 0; i < MAX_NFREELISTS; ++i) {
        _freeLists[i].SetMaxNum(SizeUtils::NumMoveSize(i));
       }
    }
    // 申请内存
    void* Allocate(size_t size);

    // 释放内存
    void Deallocate(void* ptr, size_t size);

    // 从 CentralCache 获取对象
    void* FetchFromCentralCache(size_t index, size_t size);

    // 释放过多内存给 CentralCache
    void ListTooLong(FreeList& list, size_t size);

private:
    // 哈希桶，对应 SizeUtils 的映射规则
    FreeList _freeLists[MAX_NFREELISTS];
};

// TLS 全局指针
// static 保证只在当前编译单元可见（如果有多个cpp包含这个h可能会有问题，建议放cpp里定义，这里声明）
// 为了头文件整洁，我们在 cpp 里定义 pTLSThreadCache
// extern thread_local ThreadCache* pTLSThreadCache;

} // namespace KzAlloc