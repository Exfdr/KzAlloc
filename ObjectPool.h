#pragma once
#include "Common.h"
#include "SpinLock.h"
#include <new>

namespace KzAlloc {

// 专门用于分配固定大小对象（如 Span）的定长内存池
// 避免直接调用 new 导致循环依赖 malloc
template<class T>
class ObjectPool {
public:
    ObjectPool() = default;

    // 析构函数：释放所有向系统申请的大块内存
    ~ObjectPool() {
        // 这里不需要加锁，因为析构通常发生在单线程环境或生命周期结束时
        void* cur = _currentBlock;
        while (cur) {
            // 取出头部存储的下一个块地址
            void* next = NextObj(cur); 
            // 归还整块内存
            SystemFree(cur, _blockSize >> PAGE_SHIFT);
            cur = next;
        }
    }

    T* New() {
        T* obj = AllocateMemory();
        return new(obj) T; // 只有这里才调用构造
    }

    void Delete(T* obj) {
        if (obj) {
            obj->~T(); // 只有这里才调用析构
            FreeMemory(obj);
        }
    }

    // ：只申请内存，不调用构造函数
    T* AllocateMemory() {
        std::lock_guard<SpinMutex> lock(_mtx);
        T* obj = nullptr;
        
        // 1. 优先用 FreeList
        if (_freeList) {
            void* next = *((void**)_freeList);
            obj = (T*)_freeList;
            _freeList = next;
            return obj; // 直接返回，不 new
        }
        
        // 2. 自由链表为空，检查大块内存剩余空间
        // 必须保证剩余空间足够一个对象大小
        if (_leftBytes < sizeof(T)) {
            // 剩余空间不足（哪怕只剩几字节也只能浪费掉，这是内部碎片），申请新块
                void* newBlock = SystemAlloc(_blockSize >> PAGE_SHIFT);

                // 构建内存块链表(头插法)
                // 内存布局: [ NextBlockPtr (8B) ] [ ... Data ... ]
                // 1. 将旧的 block 头指针存入新 block 的前8个字节
                NextObj(newBlock) = _currentBlock;
                
                // 2. 更新 _currentBlock 指向新 block
                _currentBlock = newBlock;

                // 3. 更新剩余字节数 (总大小 - 头部指针大小)
                _memory = (char*)newBlock + sizeof(void*);
                
                // 4. 调整 _memory 指针，跳过头部链接指针
                _leftBytes = _blockSize - sizeof(void*);
        }
        obj = (T*)_memory;
        _memory += sizeof(T);
        _leftBytes -= sizeof(T);
        
        return obj; // 直接返回，不 new
    }

    // ：只释放内存，不调用析构函数
    void FreeMemory(T* obj) {
        static_assert(sizeof(T) >= sizeof(void*), "ObjectPool elements must be larger than void*");
        std::lock_guard<SpinMutex> lock(_mtx);
        // 头插法挂回到自由链表
        // 也就是把 obj 当作一个 void* 节点，指向原来的 _freeList
        // 前提：sizeof(T) 必须 >= sizeof(void*)
        NextObj(obj) = _freeList;
        _freeList = obj;
    }

private:
    // 每次向系统申请的内存块大小 (128KB)
    // 这是一个经验值，太大容易浪费，太小频繁系统调用
    static constexpr size_t _blockSize = 128 * 1024; 

    char* _memory = nullptr;       // 指向当前可用内存的游标
    size_t _leftBytes = 0;         // 当前块剩余字节数
    
    void* _freeList = nullptr;     // 回收对象的空闲链表 (T* 形成的链表)
    void* _currentBlock = nullptr; // 大内存块链表的头指针 (用于析构时释放所有系统内存)
    
    SpinMutex _mtx;                // 高性能自旋锁
};

} // namespace KzAlloc