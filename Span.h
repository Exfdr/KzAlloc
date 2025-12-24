#pragma once
#include "Common.h"
#include "ObjectPool.h"

namespace KzAlloc {
// 定义链表节点基类 (只包含指针)
struct SpanLink {
    SpanLink* _next = nullptr; // 双向链表结构
    SpanLink* _prev = nullptr;
};

// 管理 Span 的核心结构体 (也是双向链表节点)
struct Span : public SpanLink {

    PAGE_ID _pageId = 0;   // 页号
    size_t  _n = 0;        // 页数
    
    size_t  _objSize = 0;    // 切分的小对象大小 (CentralCache 使用)
    size_t  _useCount = 0;   // 分配出去的小对象数量
    void* _freeList = nullptr; // 切好小对象的空闲链表
    
    bool    _isUse = false;  // true: 在 CentralCache/用户手中; false: 在 PageCache 中
    bool   _isCold = false;   // 标记是否为冷数据 (物理内存已释放，但虚拟地址保留)
    
    // 记录该 Span 属于哪个 PageCacheShard，防止跨分片死锁
    uint8_t _shardId = 0;

    void Remove() {
        _prev->_next = _next;
        _next->_prev = _prev;
        _prev = nullptr;
        _next = nullptr;
    }
};

// 双向链表容器 (带哨兵位)
class SpanList {
public:

    SpanList() {
        _head = GetSentinelPool().New();
        _head->_next = reinterpret_cast<Span*>(_head);
        _head->_prev = reinterpret_cast<Span*>(_head);
    } 

    ~SpanList() {
        if (_head) {
            GetSentinelPool().Delete(_head); // 替代 delete _head
            _head = nullptr;
        }
    }
    
    // 禁用拷贝
    SpanList(const SpanList&) = delete;
    SpanList& operator=(const SpanList&) = delete;

    // B. 允许移动构造 (Move Constructor)
    // std::map 在旋转平衡树时需要移动节点，这是必须实现的！
    SpanList(SpanList&& other) noexcept {
        // 1. 接管对方的哨兵
        _head = other._head;
        // 2. 把对方置空，防止对方析构时销毁哨兵
        other._head = nullptr;
    }

    // C. 允许移动赋值
    SpanList& operator=(SpanList&& other) noexcept {
        if (this != &other) {
            // 1. 先释放自己的旧哨兵
            if (_head) GetSentinelPool().Delete(_head);
            
            // 2. 接管对方
            _head = other._head;
            other._head = nullptr;
        }
        return *this;
    }

    Span* Begin() { 
        return static_cast<Span*>(_head->_next); 
    }
    Span* End() { 
        return static_cast<Span*>(_head); 
    }
    bool Empty() const { 
        return _head->_next == _head; 
    }

    void PushFront(Span* span) {
        Insert(Begin(), span);
    }
    
    // 弹出并返回首个节点 (如果空则返回 nullptr)
    Span* PopFront() {
        Span* front = Begin();
        if (front == End()) {
            return nullptr;
        }
        Erase(front);
        return front;
    }
    
    // 在 pos 之前插入 span
    void Insert(Span* pos, Span* span) {
        assert(pos);
        assert(span);
        
        SpanLink* prev = pos->_prev;
        
        prev->_next = span;
        span->_prev = prev;
        
        span->_next = pos;
        pos->_prev = span;
    }
    
    // 移除指定节点 (并不释放内存，只是从链表解绑)
    void Erase(Span* span) {
        assert(span);
        assert(span != End());
        
        SpanLink* prev = span->_prev;
        SpanLink* next = span->_next;
        
        prev->_next = next;
        next->_prev = prev;
        
        // 断开连接，防野指针
        span->_prev = nullptr;
        span->_next = nullptr;
    }

private:
    SpanLink* _head = nullptr; // 哨兵节点

    // 【私有静态单例池】
    // 专门用于分配 SpanList 的哨兵节点
    // 使用 static 局部变量保证线程安全的惰性初始化
    static ObjectPool<SpanLink>& GetSentinelPool() {
        static ObjectPool<SpanLink> pool;
        return pool;
    }
};

}