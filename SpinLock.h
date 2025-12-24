#pragma once
#include <atomic>
#include <thread>

// 引入 PAUSE 指令
#if defined(_MSC_VER)
    #include <intrin.h> // Windows MSVC
#endif

namespace KzAlloc {

class SpinMutex {
public:
    explicit SpinMutex() = default;
    
    SpinMutex(const SpinMutex&) = delete;
    SpinMutex& operator=(const SpinMutex&) = delete;

    void lock() {
        // 1. 快速路径：直接尝试 CAS
        if (!_flag.test_and_set(std::memory_order_acquire)) {
            return;
        }

        // 2. 慢速路径：自旋等待
        // spin_count 用于防止死循环导致的优先级反转
        int spin_count = 0;
        while (true) {
            // 2.1 读操作循环 (Test-Test-and-Set 模式)
            // 先只读不写，减少缓存行失效导致的锁总线风暴
            while (_flag.test(std::memory_order_relaxed)) {
                // CPU 指令级优化
#if defined(_MSC_VER)
                _mm_pause(); 
#elif defined(__GNUC__) || defined(__clang__)
                __builtin_ia32_pause();
#else
                std::this_thread::yield(); // 兜底
#endif
            }
            
            // 2.2 再次尝试抢锁
            if (!_flag.test_and_set(std::memory_order_acquire)) {
                return;
            }
            
            // 2.3 让步策略 (Backoff)
            if (++spin_count > 1024) {
                std::this_thread::yield();
                spin_count = 0;
            }
        }
    }

    void unlock() {
        _flag.clear(std::memory_order_release);
    }

private:
    std::atomic_flag _flag = ATOMIC_FLAG_INIT;
};

} // namespace KzAlloc