// CompleteTest.cpp
#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <cassert>
#include <algorithm>
#include <random>
#include <chrono>
#include <list>
#include <mutex>
#include <condition_variable>

// 引入内存池头文件
#include "ConcurrentAlloc.h"
#include "KzAllocator.h"

using namespace KzAlloc;

// ============================================================================
// 工具类：计时器
// ============================================================================
class ScopedTimer {
public:
    ScopedTimer(const char* name) : _name(name), _start(std::chrono::high_resolution_clock::now()) {}
    ~ScopedTimer() {
        auto end = std::chrono::high_resolution_clock::now();
        long long duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - _start).count();
        std::cout << "[ " << _name << " ] Cost: " << duration << " ms" << std::endl;
    }
private:
    const char* _name;
    std::chrono::high_resolution_clock::time_point _start;
};

// ============================================================================
// 第一部分：正确性单元测试 (Unit Tests)
// ============================================================================
void TestAlignment() {
    std::cout << "=> Running Alignment Test..." << std::endl;
    for (size_t size = 1; size <= 4096; ++size) {
        void* ptr = KzAlloc::malloc(size);
        // 检查地址是否按 8 字节对齐
        assert(((uintptr_t)ptr & 7) == 0);
        KzAlloc::free(ptr);
    }
    std::cout << "   Pass." << std::endl;
}

void TestLargeAlloc() {
    std::cout << "=> Running Large Allocation Test (>256KB)..." << std::endl;
    // 测试 PageHeap 直接分配
    size_t size = 1024 * 1024; // 1MB
    void* ptr = KzAlloc::malloc(size);
    assert(ptr != nullptr);
    
    // 写入测试
    char* data = (char*)ptr;
    data[0] = 'A';
    data[size - 1] = 'Z';
    
    KzAlloc::free(ptr);
    std::cout << "   Pass." << std::endl;
}

// ============================================================================
// 第二部分：STL 兼容性测试 (STL Adapter Tests)
// ============================================================================
void TestSTLAdapter() {
    std::cout << "=> Running STL Adapter Test..." << std::endl;
    
    // 1. 测试 Vector
    {
        std::vector<int, KzAllocator<int>> v;
        for (int i = 0; i < 10000; ++i) {
            v.push_back(i);
        }
        for (int i = 0; i < 10000; ++i) {
            assert(v[i] == i);
        }
    } // v 析构时会自动调用 ConcurrentFree

    // 2. 测试 List (测试节点频繁分配)
    {
        std::list<int, KzAllocator<int>> l;
        for (int i = 0; i < 10000; ++i) {
            l.push_back(i);
        }
        auto it = l.begin();
        for (int i = 0; i < 10000; ++i) {
            assert(*it == i);
            ++it;
        }
    }
    std::cout << "   Pass." << std::endl;
}

// ============================================================================
// 第三部分：并发健壮性测试 (Robustness Tests)
// ============================================================================

// 场景：跨线程释放 (Producer-Consumer)
// Thread A 分配内存 -> 放入队列 -> Thread B 取出 -> 释放内存
// 这测试了 CentralCache 的锁机制和 PageMap 的查找准确性
void TestCrossThreadFree() {
    std::cout << "=> Running Cross-Thread Free Test (Producer-Consumer)..." << std::endl;
    
    const int ITEM_COUNT = 100000;
    std::vector<void*> queue;
    std::mutex mtx;
    std::condition_variable cv;
    bool finished = false;

    // 消费者线程
    std::thread consumer([&]() {
        int count = 0;
        while (true) {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, [&]() { return !queue.empty() || finished; });
            
            if (queue.empty() && finished) break;

            void* ptr = queue.back();
            queue.pop_back();
            lock.unlock();

            // 异地释放
            KzAlloc::free(ptr);
            count++;
        }
    });

    // 生产者线程
    std::thread producer([&]() {
        for (int i = 0; i < ITEM_COUNT; ++i) {
            void* ptr = KzAlloc::malloc(rand() % 1024 + 1);
            
            std::unique_lock<std::mutex> lock(mtx);
            queue.push_back(ptr);
            lock.unlock();
            cv.notify_one();
        }
        
        std::unique_lock<std::mutex> lock(mtx);
        finished = true;
        cv.notify_all();
    });

    producer.join();
    consumer.join();
    std::cout << "   Pass." << std::endl;
}

// 场景：高并发竞争
void TestMultiThreadContention() {
    std::cout << "=> Running High Contention Test (4 Threads)..." << std::endl;
    
    auto thread_routine = [](int thread_id) {
        std::vector<void*> ptrs;
        ptrs.reserve(10000);
        
        // 疯狂申请
        for (int i = 0; i < 10000; ++i) {
            ptrs.push_back(KzAlloc::malloc(8)); // 大家都申请 8 字节，故意制造冲突
        }
        
        // 疯狂释放
        for (auto ptr : ptrs) {
            KzAlloc::free(ptr);
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back(thread_routine, i);
    }

    for (auto& t : threads) t.join();
    std::cout << "   Pass." << std::endl;
}

// ============================================================================
// 第四部分：基准性能对比 (Benchmark)
// ============================================================================
void Benchmark(size_t n_times, size_t alloc_size) {
    std::cout << "\n--------------------------------------------------" << std::endl;
    std::cout << " Benchmark: " << n_times << " ops, Size: " << alloc_size << " bytes" << std::endl;
    std::cout << "--------------------------------------------------" << std::endl;

    size_t batch_size = 100000;
    std::vector<void*> ptrs(batch_size);
    // 1. 测试 KzAlloc
    {
        auto start = std::chrono::high_resolution_clock::now();
        for (size_t k = 0; k < n_times; k += batch_size) {
            for (size_t i = 0; i < batch_size; ++i) {
                ptrs[i] = KzAlloc::malloc(alloc_size);
            }
            for (size_t i = 0; i < batch_size; ++i) {
                KzAlloc::free(ptrs[i]);
            }
        }
        auto end = std::chrono::high_resolution_clock::now();
        
        auto cost = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        std::cout << "KzAlloc:      " << cost << " ms  |  " 
                  << (n_times * 2 / 1000.0 / cost) * 1000 << " Kops/sec" << std::endl;
    }

    // 2. 测试 malloc/free
    {
        auto start = std::chrono::high_resolution_clock::now();
        for (size_t k = 0; k < n_times; k += batch_size) {
            for (size_t i = 0; i < batch_size; ++i) {
                ptrs[i] = ::malloc(alloc_size);
            }
            for (size_t i = 0; i < batch_size; ++i) {
                ::free(ptrs[i]);
            }
        }
        auto end = std::chrono::high_resolution_clock::now();
        
        auto cost = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        std::cout << "System Malloc: " << cost << " ms  |  " 
                  << (n_times * 2 / 1000.0 / cost) * 1000 << " Kops/sec" << std::endl;
    }
}

// 多线程 Benchmark
void MultiThreadBenchmark(size_t n_threads, size_t n_times_per_thread, size_t alloc_size) {
    std::cout << "\n--------------------------------------------------" << std::endl;
    std::cout << " Multi-Thread Benchmark: " << n_threads << " threads, Size: " << alloc_size << " bytes" << std::endl;
    std::cout << "--------------------------------------------------" << std::endl;

    size_t batch_size = 100000;

    // KzAlloc
    {
        auto start = std::chrono::high_resolution_clock::now();
        std::vector<std::thread> vthread;
        for (size_t k = 0; k < n_threads; ++k) {
            vthread.emplace_back([&]() {
                // 【修正】初始化 vector 大小，后续只修改值，不扩容
                std::vector<void*> ptrs(batch_size); 
                
                for (size_t k = 0; k < n_times_per_thread; k += batch_size) {
                    // 【修正】使用下标赋值，而不是 push_back
                    for (size_t i = 0; i < batch_size; ++i) {
                        ptrs[i] = KzAlloc::malloc(alloc_size); 
                    }
                    // 【修正】释放对应的下标
                    for (size_t i = 0; i < batch_size; ++i) {
                        KzAlloc::free(ptrs[i]); 
                    }
                }
            });
        }
        for (auto& t : vthread) t.join();
        auto end = std::chrono::high_resolution_clock::now();
        
        auto cost = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        std::cout << "KzAlloc:       " << cost << " ms  |  " 
                  << (n_times_per_thread * n_threads * 2 / 1000.0 / cost) * 1000 << " Kops/sec" << std::endl;
    }

    // Malloc (同样的修正)
    {
        auto start = std::chrono::high_resolution_clock::now();
        std::vector<std::thread> vthread;
        for (size_t k = 0; k < n_threads; ++k) {
            vthread.emplace_back([&]() {
                std::vector<void*> ptrs(batch_size);
                for (size_t k = 0; k < n_times_per_thread; k += batch_size) {
                    for (size_t i = 0; i < batch_size; ++i) {
                        ptrs[i] = ::malloc(alloc_size);
                    }
                    for (size_t i = 0; i < batch_size; ++i) {
                        ::free(ptrs[i]);
                    }
                }
            });
        }
        for (auto& t : vthread) t.join();
        auto end = std::chrono::high_resolution_clock::now();
        
        auto cost = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        std::cout << "System Malloc: " << cost << " ms  |  " 
                  << (n_times_per_thread * n_threads * 2 / 1000.0 / cost) * 1000 << " Kops/sec" << std::endl;
    }
}

class RealisticBenchmark {
public:
    struct Config {
        size_t thread_count = 4;
        size_t iterations_per_thread = 200000; // 每个线程的操作次数
        size_t max_working_set = 1000;         // 模拟工作集大小（同时存活的对象数）
    };

    // 随机大小生成器 (模拟真实分布)
    static size_t GetRealisticSize(std::mt19937& gen) {
        // 定义不同大小区间的概率分布
        // 0: [8, 64]      -> 50% 概率 (极小对象)
        // 1: [64, 512]    -> 30% 概率 (小对象)
        // 2: [512, 8KB]   -> 15% 概率 (中对象)
        // 3: [8KB, 256KB] -> 5%  概率 (大对象)
        static std::discrete_distribution<> d({50, 30, 15, 5});
        
        int category = d(gen);
        
        if (category == 0) {
            std::uniform_int_distribution<> dis(8, 64);
            return dis(gen);
        } else if (category == 1) {
            std::uniform_int_distribution<> dis(64, 512);
            return dis(gen);
        } else if (category == 2) {
            std::uniform_int_distribution<> dis(512, 8 * 1024);
            return dis(gen);
        } else {
            std::uniform_int_distribution<> dis(8 * 1024, 256 * 1024);
            return dis(gen);
        }
    }

    static void Run(const Config& cfg) {
        std::cout << "\n========================================================" << std::endl;
        std::cout << "  Realistic Workload Benchmark (Mixed Sizes & Threads)  " << std::endl;
        std::cout << "========================================================" << std::endl;
        std::cout << "Threads: " << cfg.thread_count 
                  << " | Iterations: " << cfg.iterations_per_thread 
                  << " | Working Set: " << cfg.max_working_set << std::endl;

        // ----------------------------------------------------------------
        // 1. 测试 KzAlloc
        // ----------------------------------------------------------------
        {
            auto start = std::chrono::high_resolution_clock::now();
            
            std::vector<std::thread> threads;
            for (size_t i = 0; i < cfg.thread_count; ++i) {
                threads.emplace_back([&cfg, i]() {
                    // 每个线程独立的随机数生成器，保证无锁竞争生成随机数
                    std::mt19937 gen(1234 + i); 
                    
                    // 模拟工作集：保存分配的指针，稍后释放
                    std::vector<void*> ptrs;
                    ptrs.reserve(cfg.max_working_set);
                    
                    for (size_t k = 0; k < cfg.iterations_per_thread; ++k) {
                        // 随机决定是 Alloc 还是 Free
                        // 如果工作集满了，强制 Free；如果空了，强制 Alloc
                        // 否则 70% 概率 Alloc，30% 概率 Free (模拟内存增长)
                        // 或者 50/50 模拟稳定状态。这里用 50/50 保持平衡
                        
                        bool do_alloc = true;
                        if (ptrs.empty()) do_alloc = true;
                        else if (ptrs.size() >= cfg.max_working_set) do_alloc = false;
                        else do_alloc = (gen() % 2 == 0);

                        if (do_alloc) {
                            size_t sz = GetRealisticSize(gen);
                            void* p = KzAlloc::malloc(sz);
                            ptrs.push_back(p);
                        } else {
                            // 随机挑一个释放 (模拟无序释放)
                            std::uniform_int_distribution<> dis_idx(0, ptrs.size() - 1);
                            size_t idx = dis_idx(gen);
                            
                            // Swap remove (O(1) 删除)
                            void* p = ptrs[idx];
                            ptrs[idx] = ptrs.back();
                            ptrs.pop_back();
                            
                            KzAlloc::free(p);
                        }
                    }
                    
                    // 清理剩余内存
                    for (void* p : ptrs) KzAlloc::free(p);
                });
            }

            for (auto& t : threads) t.join();
            
            auto end = std::chrono::high_resolution_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
            
            size_t total_ops = cfg.thread_count * cfg.iterations_per_thread;
            std::cout << "KzAlloc:       " << ms << " ms | " 
                      << (total_ops * 1000.0 / ms / 1000.0) << " Kops/sec" << std::endl;
        }

        // ----------------------------------------------------------------
        // 2. 测试 System Malloc
        // ----------------------------------------------------------------
        {
            auto start = std::chrono::high_resolution_clock::now();
            
            std::vector<std::thread> threads;
            for (size_t i = 0; i < cfg.thread_count; ++i) {
                threads.emplace_back([&cfg, i]() {
                    std::mt19937 gen(1234 + i); // 相同的种子，保证同样的分配序列
                    
                    std::vector<void*> ptrs;
                    ptrs.reserve(cfg.max_working_set);
                    
                    for (size_t k = 0; k < cfg.iterations_per_thread; ++k) {
                        bool do_alloc = true;
                        if (ptrs.empty()) do_alloc = true;
                        else if (ptrs.size() >= cfg.max_working_set) do_alloc = false;
                        else do_alloc = (gen() % 2 == 0);

                        if (do_alloc) {
                            size_t sz = GetRealisticSize(gen);
                            void* p = ::malloc(sz);
                            ptrs.push_back(p);
                        } else {
                            std::uniform_int_distribution<> dis_idx(0, ptrs.size() - 1);
                            size_t idx = dis_idx(gen);
                            
                            void* p = ptrs[idx];
                            ptrs[idx] = ptrs.back();
                            ptrs.pop_back();
                            
                            ::free(p);
                        }
                    }
                    for (void* p : ptrs) ::free(p);
                });
            }

            for (auto& t : threads) t.join();
            
            auto end = std::chrono::high_resolution_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
            
            size_t total_ops = cfg.thread_count * cfg.iterations_per_thread;
            std::cout << "System Malloc: " << ms << " ms | " 
                      << (total_ops * 1000.0 / ms / 1000.0) << " Kops/sec" << std::endl;
        }
    }
};

int main() {
    SizeUtils::Init();
    std::cout << "========================================" << std::endl;
    std::cout << "      KzMemoryPool Full Test Suite      " << std::endl;
    std::cout << "========================================" << std::endl;

    // 1. 基础正确性测试
    TestAlignment();
    TestLargeAlloc();

    // 2. STL 适配测试
    TestSTLAdapter();

    // 3. 并发健壮性测试
    TestCrossThreadFree();
    TestMultiThreadContention();

    // 4. 性能对比测试
    std::cout << "\n========================================================" << std::endl;
    puts("固定内存申请测试：除512KB为两百万op,其他总数均为两千万op,工作集锁定为十万");
    std::cout << "========================================================" << std::endl;
    // 小内存 (ThreadCache 优势区)
    MultiThreadBenchmark(1, 10000000, 16); 
    // 中等内存
    MultiThreadBenchmark(1, 10000000, 1024);
    // 超大内存
    MultiThreadBenchmark(1, 1000000, 512 * 1024);
    
    // 多线程高频竞争 (重点关注 ThreadCache 无锁优势)
    MultiThreadBenchmark(5, 2000000, 16);
    

    std::cout << "\n\n========================================================" << std::endl;
    puts("真实内存申请测试：每线程均为一千万op");
    std::cout << "========================================================" << std::endl;
    RealisticBenchmark::Config cfg;
    cfg.thread_count = 1;
    cfg.iterations_per_thread = 10000000; 
    cfg.max_working_set = 5000;
    
    RealisticBenchmark::Run(cfg);

    cfg.thread_count = 4;
    RealisticBenchmark::Run(cfg);

    cfg.thread_count = 8;
    RealisticBenchmark::Run(cfg);

    cfg.max_working_set = 50000;
    cfg.thread_count = 1;
    RealisticBenchmark::Run(cfg);

    cfg.thread_count = 4;
    RealisticBenchmark::Run(cfg);

    cfg.thread_count = 8;
    RealisticBenchmark::Run(cfg);

    cfg.max_working_set = 100000;
    cfg.thread_count = 1;
    RealisticBenchmark::Run(cfg);

    cfg.thread_count = 4;
    RealisticBenchmark::Run(cfg);

    cfg.thread_count = 8;
    RealisticBenchmark::Run(cfg);


    std::cout << "\nAll tests passed successfully!" << std::endl;
    return 0;
}