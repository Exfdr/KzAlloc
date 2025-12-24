#pragma once

#include <cstddef>
#include <cstdint>
#include <cassert>
#include <new> // for std::bad_alloc
#include <mutex>


// 平台宏判断
#ifdef _WIN32
    #include <windows.h>
#else
    #include <sys/mman.h>
    #include <unistd.h>
    #include <sys/sysinfo.h> // Linux sysinfo
    #include <sys/syscall.h>
    
#endif
#include <assert.h>

#define CACHE_LINE_SIZE 64 // 缓存行对齐

namespace KzAlloc {

// 辅助函数
static inline void*& NextObj(void* obj) {
    return *(void**)obj;
}

// =========================================================================
// 基础配置
// =========================================================================

#ifdef _WIN64
    typedef unsigned long long PAGE_ID;
#elif _WIN32
    typedef unsigned int PAGE_ID;
#else
    typedef unsigned long long PAGE_ID;
#endif

// 页大小配置: 8KB
static constexpr size_t PAGE_SHIFT = 13;
static constexpr size_t PAGE_SIZE = 1 << PAGE_SHIFT;
static constexpr size_t PAGE_ROUND_UP_NUM = PAGE_SIZE - 1;
static constexpr size_t PAGE_ROUND_UP_NUM_NEGATE = ~PAGE_ROUND_UP_NUM;

// =========================================================================
// 系统内存接口 (封装 OS 系统调用)
// =========================================================================

// 获取系统物理内存总大小 (字节)
inline size_t GetSystemPhysicalMemory() {
#ifdef _WIN32
    MEMORYSTATUSEX status;
    status.dwLength = sizeof(status);
    GlobalMemoryStatusEx(&status);
    return static_cast<size_t>(status.ullTotalPhys);
#else
    // Linux 获取物理内存
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && page_size > 0) {
        return static_cast<size_t>(pages) * page_size;
    }
#endif
    // 兜底：如果获取失败，默认假设 8GB (生产环境应处理 error)
    return 8ULL * 1024 * 1024 * 1024; 
}

// 2MB 阈值，超过这个值尝试申请大页 (Linux 默认大页通常是 2MB)
static constexpr size_t HUGE_PAGE_THRESHOLD = 2 * 1024 * 1024;
// 向系统申请 kpage 页的内存
inline void* SystemAlloc(size_t kpage) {
    size_t size = kpage << PAGE_SHIFT;
    void* ptr = nullptr;
    
#ifdef _WIN32
    ptr = VirtualAlloc(0, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (ptr == nullptr) throw std::bad_alloc();
#else
    // Linux mmap
    
    // 1. 尝试大页 (2MB)
    if (size >= HUGE_PAGE_THRESHOLD) [[unlikely]] {
        ptr = mmap(0, size, PROT_READ | PROT_WRITE, 
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_POPULATE, -1, 0);
        if (ptr != MAP_FAILED) {
            // 【监控点 1】大页申请成功
            /* printf("[SystemAlloc Huge] start=%p, end=%p, size=%zu\n", 
                   ptr, (char*)ptr + size, size);
                   */
            return ptr;
        }
    }
    // 编译器会自动优化掉不执行的分支，没有运行时开销
    if (PAGE_SIZE <= 4096) {
        // -----------------------------------------------------------
        // 4KB 页逻辑 (直接申请)
        // -----------------------------------------------------------
        ptr = mmap(0, size, PROT_READ | PROT_WRITE, 
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        /*
        if (ptr != MAP_FAILED) {
             printf("[SystemAlloc 4KB]  start=%p, end=%p, size=%zu\n", ptr, (char*)ptr + size, size);
        }
             */
    }
    else {
    // 2. Fallback 逻辑 (4KB页 -> 手动对齐)
    // 多申请一页用于调整
    size_t allocSize = size + PAGE_SIZE;
    
    void* raw_ptr = mmap(0, allocSize, PROT_READ | PROT_WRITE, 
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                   
    if (raw_ptr == MAP_FAILED) throw std::bad_alloc();
    
    // 必须转为 uintptr_t 才能做位运算，不能用 char* 直接 &
    uintptr_t raw_addr = (uintptr_t)raw_ptr;
    // 向上对齐算法
    uintptr_t aligned_addr = (raw_addr + PAGE_ROUND_UP_NUM) & PAGE_ROUND_UP_NUM_NEGATE;
    
    // 1. 切除头部
    size_t prefix_len = aligned_addr - raw_addr;
    if (prefix_len > 0) {
        munmap((void*)raw_addr, prefix_len);
    }
    
    // 2. 切除尾部
    size_t suffix_len = allocSize - size - prefix_len;
    if (suffix_len > 0) {
        munmap((void*)(aligned_addr + size), suffix_len);
    }
    
    ptr = (void*)aligned_addr;

    // 【监控点 2】普通页申请成功
    // 注意：这里打印的是最终给用户的 ptr，不是 raw_ptr
    /*
    printf("[SystemAlloc]      start=%p, end=%p, size=%zu\n", 
           ptr, (char*)ptr + size, size);
           */
    }
    if (ptr == MAP_FAILED || ptr == nullptr) throw std::bad_alloc();
#endif

    return ptr;
}

// 释放内存归还给系统
inline void SystemFree(void* ptr, size_t kpage) {
    if (ptr == nullptr) return;

#ifdef _WIN32
    VirtualFree(ptr, 0, MEM_RELEASE);
#else
    // Linux munmap
    size_t size = kpage << PAGE_SHIFT;
    
    // 【修正 B】修复了原代码中的括号错误和指针加法类型错误
    // 指针加法必须是：(char*)ptr + 整数大小
    /*
    printf("[SystemFree]       start=%p, end=%p, size=%zu\n", 
           ptr, (char*)ptr + size, size);
    */
           
    munmap(ptr, size);
#endif
}

// =========================================================================
// 核心数据结构
// =========================================================================

// 桶的总数量 (根据分段对齐策略计算得出: 16+56+56+112+24)
// [1, 128B]: 8B 对齐 (8, 16, ... 128) -> 浪费率低
// [129B, 1024B]: 16B 对齐
// [1KB, 8KB]: 128B 对齐
// [8KB, 64KB]: 512B 对齐
// [64KB, 256KB]: 8KB 对齐
static constexpr int MAX_NFREELISTS = 264;
static constexpr size_t MAX_BYTES = 256 * 1024;
// =========================================================================
// SizeUtils
// 管理内存对齐和桶映射的静态工具集
// =========================================================================
inline uint16_t _size_lookup_table[MAX_BYTES + 1] = {0};
inline size_t _class_to_size[MAX_NFREELISTS] = {0};
namespace SizeUtils {

namespace detail {
    // 辅助：超大内存对齐
    inline static size_t RoundUpToPage(size_t size) {
        return (size + PAGE_ROUND_UP_NUM) & PAGE_ROUND_UP_NUM_NEGATE;
    }

    // 根据当前桶的大小，决定下一个桶应该大多少
    // 只有在 Init 时调用，稍微多几个 if-else 没关系
    inline static size_t _CalculateNextBlockSize(size_t current_size) {
        // [1, 128] -> 8B 对齐
        // 注意：这里判断的是 current_size，即“上一个桶的大小”
        if (current_size < 128) {
            return current_size + 8;
        }
        // [129, 1024] -> 16B 对齐
        else if (current_size < 1024) {
            return current_size + 16;
        }
        // [1K, 8K] -> 128B 对齐，这个乘法会编译优化为位运算，为了可读性保留乘法
        else if (current_size < 8 * 1024) {
            return current_size + 128;
        }
        // [8K, 64K] -> 512B 对齐
        else if (current_size < 64 * 1024) {
            return current_size + 512;
        }
        // [64K, 256K] -> 8KB 对齐
        else {
            return current_size + 8 * 1024;
        }
    }
}
    // ---------------------------------------------------------
    // 核心热点函数 (Hot Path) - 全部 O(1)
    // ---------------------------------------------------------

    // 1. 输入 size，返回对应的桶编号 (0 ~ 263)
    inline static int Index(size_t size) {
        assert(size <= MAX_BYTES);
        return _size_lookup_table[size];
    }

    // 2. 输入 size，返回对齐后的大小 (例如输入 13 返回 16)
    inline static size_t RoundUp(size_t size) {
        if (size > MAX_BYTES) return detail::RoundUpToPage(size); // 超大内存通常按页对齐

        int idx = Index(size);
        return _class_to_size[idx];
    }

    // 3. (可选) 获取某个桶里的块有多大
    inline static size_t Size(size_t index) {
        assert(index < MAX_NFREELISTS);
        return _class_to_size[index];
    }

    inline static size_t NumMoveSize(size_t index) {
        assert(index >= 0);
        // 计算上限：256KB / size
        size_t num = MAX_BYTES / _class_to_size[index];
        // 限制范围 [2, 512]
        if (num < 2) num = 2;
        if (num > 32768) num = 32768;
        return num;
    }

    // ---------------------------------------------------------
    // 初始化 (Cold Path)
    // ---------------------------------------------------------
    inline static void Init() {
        static std::once_flag flag;
        std::call_once(flag, []() {
            // 初始化映射表
            int index = 0;
            // block_size: 当前桶的大小，初始为第一个对齐数 8
            size_t block_size = 8;

            for (size_t i = 1; i <= MAX_BYTES; ++i) {
                // 如果当前申请的大小 i 超过了当前桶的大小 block_size
                // 说明必须要升级到下一个桶了
                if (i > block_size) {
                    index++;
                    // 计算下一个桶的大小应该是多少
                    block_size = detail::_CalculateNextBlockSize(block_size);
                }
                
                // 填充：用户申请 i 字节 -> 对应第 index 个桶
                _size_lookup_table[i] = static_cast<uint16_t>(index);
                
                // 填充：第 index 个桶 -> 实际大小是 block_size
                // 只要 index 没变，这里会重复赋值，没关系
                if (index < MAX_NFREELISTS) {
                    _class_to_size[index] = block_size;
                }
            }
            // 简单处理 size=0 的情况
             _size_lookup_table[0] = 0;
        });
    }

};



} // namespace KzAlloc