#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <memory>
#include <thread>
#include <vector>
#include "tsfqueue.hpp"


template <typename T, size_t N>
using LockfreeMPMCBounded = tsfqueue::impl::lockfree_mpmc_bounded<T, N>;

template <typename T, size_t N>
using LockfreeMPMCBoundedUniquePtr = tsfqueue::impl::lockfree_mpmc_bounded_unique_ptr<T, N>;


// ---------------------------------------------------------------------------
// 1. Basic FIFO behaviour with a single producer and single consumer.
//    Pushes ascending values and verifies pops come out in the same order.
// ---------------------------------------------------------------------------
TEST(LockfreeMPMCBoundedQueue, BasicFIFO_SingleProducerSingleConsumer) {
    constexpr size_t N = 16;
    LockfreeMPMCBounded<uint64_t, N> q;

    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.size(), 0u);
    EXPECT_EQ(q.capacity(), N);

    for (uint64_t i = 1; i <= 8; ++i) {
        EXPECT_TRUE(q.push(i));
    }
    EXPECT_FALSE(q.empty());
    EXPECT_EQ(q.size(), 8u);

    for (uint64_t i = 1; i <= 8; ++i) {
        uint64_t v = 0;
        EXPECT_TRUE(q.pop(v));
        EXPECT_EQ(v, i);
    }

    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.size(), 0u);
}


// ---------------------------------------------------------------------------
// 2. Edge cases: pop on empty, push on full, and fill-then-drain cycles
//    to exercise the circular buffer's seq-number wrap.
// ---------------------------------------------------------------------------
TEST(LockfreeMPMCBoundedQueue, EdgeCases_EmptyPop_FullPush_WrapAround) {
    constexpr size_t N = 4;
    LockfreeMPMCBounded<uint64_t, N> q;

    // Pop on empty queue should return false and not modify the out parameter.
    uint64_t v = 0xDEADBEEF;
    EXPECT_FALSE(q.pop(v));
    EXPECT_EQ(v, 0xDEADBEEFu);

    // Fill the queue to capacity.
    for (uint64_t i = 0; i < N; ++i) {
        EXPECT_TRUE(q.push(i));
    }
    EXPECT_EQ(q.size(), N);

    // Push on full queue should return false.
    EXPECT_FALSE(q.push(999));
    EXPECT_EQ(q.size(), N);

    // Drain and check ordering.
    for (uint64_t i = 0; i < N; ++i) {
        uint64_t out = 0;
        EXPECT_TRUE(q.pop(out));
        EXPECT_EQ(out, i);
    }
    EXPECT_TRUE(q.empty());

    // Repeat fill/drain several times to wrap the sequence numbers past N.
    for (int cycle = 0; cycle < 100; ++cycle) {
        for (uint64_t i = 0; i < N; ++i) {
            EXPECT_TRUE(q.push(cycle * 1000 + i));
        }
        EXPECT_FALSE(q.push(0));
        for (uint64_t i = 0; i < N; ++i) {
            uint64_t out = 0;
            EXPECT_TRUE(q.pop(out));
            EXPECT_EQ(out, cycle * 1000 + i);
        }
        EXPECT_FALSE(q.pop(v));
    }
}


// ---------------------------------------------------------------------------
// 3. MPMC correctness under contention: every pushed value is popped
//    exactly once (no losses, no duplicates), verified via per-value counts.
//    Under ThreadSanitizer the op count is reduced (TSAN is ~10x slower);
//    TSAN still gets plenty of interleavings to find races.
// ---------------------------------------------------------------------------
TEST(LockfreeMPMCBoundedQueue, MultiProducerMultiConsumer_NoLossNoDuplicates) {
    constexpr size_t N = 1024;
    constexpr int num_producers = 4;
    constexpr int num_consumers = 4;
#if defined(TSFQUEUE_TSAN_BUILD)
    constexpr int ops_per_producer = 2000;
#else
    constexpr int ops_per_producer = 5000;
#endif
    constexpr int total_items = num_producers * ops_per_producer;

    LockfreeMPMCBounded<uint64_t, N> q;

    std::vector<std::atomic<int>> seen(total_items);
    for (auto& c : seen) c.store(0);

    std::atomic<int> popped_count{0};
    std::atomic<bool> producers_done{false};
    

    std::vector<std::thread> producers;
    producers.reserve(num_producers);
    for (int p = 0; p < num_producers; ++p) {
        producers.emplace_back([&, p]() {
            for (int j = 0; j < ops_per_producer; ++j) {
                uint64_t val = static_cast<uint64_t>(p * ops_per_producer + j);
                while (!q.push(val)) {
                    std::this_thread::yield();
                }
            }
        });
    }

    std::vector<std::thread> consumers;
    consumers.reserve(num_consumers);
    for (int c = 0; c < num_consumers; ++c) {
        consumers.emplace_back([&]() {
            uint64_t val;
            while (true) {
                if (q.pop(val)) {
                    ASSERT_LT(val, static_cast<uint64_t>(total_items));
                    int prev = seen[val].fetch_add(1);
                    ASSERT_EQ(prev, 0) << "value " << val << " popped more than once";
                    popped_count.fetch_add(1);
                } else if (producers_done.load() && q.empty()) {
                    break;
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    for (auto& t : producers) t.join();
    producers_done.store(true);
    for (auto& t : consumers) t.join();

    EXPECT_EQ(popped_count.load(), total_items);
    for (int i = 0; i < total_items; ++i) {
        EXPECT_EQ(seen[i].load(), 1) << "value " << i << " was not popped exactly once";
    }
    EXPECT_TRUE(q.empty());
}


// ---------------------------------------------------------------------------
// 4. Linearizability sniff test: per-producer monotonic ordering.
//    Each producer P pushes values tagged (P, seq=0,1,2,...). After the run,
//    for every producer the pop-order subsequence of its values must be
//    strictly increasing. This catches reorderings within a single producer
//    that any valid linearization (FIFO per producer) forbids.
// ---------------------------------------------------------------------------
TEST(LockfreeMPMCBoundedQueue, Linearizability_PerProducerMonotonicOrder) {
    constexpr size_t N = 256;
    constexpr int num_producers = 4;
    constexpr int num_consumers = 4;
#if defined(TSFQUEUE_TSAN_BUILD)
    constexpr int ops_per_producer = 2000;
#else
    constexpr int ops_per_producer = 5000;
#endif

    // Encode (producer_id, seq) into a 64-bit value: high 16 bits = producer,
    // low 48 bits = sequence.
    auto encode = [](uint64_t pid, uint64_t seq) {
        return (pid << 48) | seq;
    };
    auto decode_pid = [](uint64_t v) { return v >> 48; };
    auto decode_seq = [](uint64_t v) { return v & ((1ULL << 48) - 1); };

    LockfreeMPMCBounded<uint64_t, N> q;
    std::atomic<bool> producers_done{false};

    // Per-consumer pop log to avoid cross-consumer contention on a shared vector.
    std::vector<std::vector<uint64_t>> consumer_logs(num_consumers);

    std::vector<std::thread> producers;
    producers.reserve(num_producers);
    for (int p = 0; p < num_producers; ++p) {
        producers.emplace_back([&, p]() {
            for (int j = 0; j < ops_per_producer; ++j) {
                uint64_t v = encode(p, j);
                while (!q.push(v)) std::this_thread::yield();
            }
        });
    }

    std::vector<std::thread> consumers;
    consumers.reserve(num_consumers);
    for (int c = 0; c < num_consumers; ++c) {
        consumers.emplace_back([&, c]() {
            auto& log = consumer_logs[c];
            log.reserve(num_producers * ops_per_producer / num_consumers + 16);
            uint64_t v;
            while (true) {
                if (q.pop(v)) {
                    log.push_back(v);
                } else if (producers_done.load() && q.empty()) {
                    break;
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    for (auto& t : producers) t.join();
    producers_done.store(true);
    for (auto& t : consumers) t.join();

    // For each producer, gather the order in which its values were popped
    // *within a single consumer*. Across consumers a producer's items can
    // appear in any interleaving, but within one consumer they must be
    // strictly increasing (since the consumer popped them in the order the
    // queue handed them out, and the queue is FIFO per producer).
    for (int c = 0; c < num_consumers; ++c) {
        std::vector<int64_t> last_seq_by_producer(num_producers, -1);
        for (uint64_t v : consumer_logs[c]) {
            uint64_t pid = decode_pid(v);
            uint64_t seq = decode_seq(v);
            ASSERT_LT(pid, static_cast<uint64_t>(num_producers));
            int64_t prev = last_seq_by_producer[pid];
            ASSERT_GT(static_cast<int64_t>(seq), prev)
                << "consumer " << c << " popped producer " << pid
                << " seq " << seq << " after seq " << prev
                << " - FIFO ordering violated";
            last_seq_by_producer[pid] = static_cast<int64_t>(seq);
        }
    }
}


// ---------------------------------------------------------------------------
// 5. Memory safety: unique_ptr wrapper drain-on-destroy.
//    Constructs N values, pushes them, destroys the queue without popping.
//    All N destructors must run exactly once. Run this under ASAN to also
//    catch UAF / double-free at the C heap level.
// ---------------------------------------------------------------------------
namespace {
    std::atomic<int> g_counted_alive{0};
    std::atomic<int> g_counted_constructed{0};
    std::atomic<int> g_counted_destroyed{0};

    struct Counted {
        int value;
        explicit Counted(int v) : value(v) {
            g_counted_constructed.fetch_add(1);
            g_counted_alive.fetch_add(1);
        }
        ~Counted() {
            g_counted_destroyed.fetch_add(1);
            g_counted_alive.fetch_sub(1);
        }
        Counted(const Counted&) = delete;
        Counted& operator=(const Counted&) = delete;
    };

    void reset_counters() {
        g_counted_alive.store(0);
        g_counted_constructed.store(0);
        g_counted_destroyed.store(0);
    }
}

TEST(LockfreeMPMCBoundedUniquePtr, MemorySafety_DrainOnDestroy) {
    constexpr size_t N = 16;
    reset_counters();
    {
        LockfreeMPMCBoundedUniquePtr<Counted, N> q;
        for (int i = 0; i < static_cast<int>(N); ++i) {
            std::unique_ptr<Counted> p(new Counted(i));
            ASSERT_TRUE(q.push(p));
            EXPECT_EQ(p.get(), nullptr) << "push must release ownership";
        }
        EXPECT_EQ(g_counted_alive.load(), static_cast<int>(N));
        // Destructor runs here and must drain the remaining N items.
    }
    EXPECT_EQ(g_counted_alive.load(), 0)
        << "destructor leaked owned objects";
    EXPECT_EQ(g_counted_constructed.load(), static_cast<int>(N));
    EXPECT_EQ(g_counted_destroyed.load(), static_cast<int>(N));
}


// ---------------------------------------------------------------------------
// 6. Memory safety: unique_ptr wrapper roundtrip - construct, push, pop,
//    let the popped unique_ptr go out of scope. Every constructed object
//    must be destroyed exactly once.
// ---------------------------------------------------------------------------
TEST(LockfreeMPMCBoundedUniquePtr, MemorySafety_PushPopRoundtrip) {
    constexpr size_t N = 32;
    constexpr int kItems = N;
    reset_counters();
    {
        LockfreeMPMCBoundedUniquePtr<Counted, N> q;
        for (int i = 0; i < kItems; ++i) {
            std::unique_ptr<Counted> p(new Counted(i));
            ASSERT_TRUE(q.push(p));
        }
        for (int i = 0; i < kItems; ++i) {
            std::unique_ptr<Counted> out;
            ASSERT_TRUE(q.pop(out));
            ASSERT_NE(out.get(), nullptr);
            EXPECT_EQ(out->value, i);
            // out goes out of scope at next iteration -> destructor runs.
        }
        EXPECT_EQ(g_counted_alive.load(), 0);
    }
    EXPECT_EQ(g_counted_constructed.load(), kItems);
    EXPECT_EQ(g_counted_destroyed.load(), kItems);
}


// ---------------------------------------------------------------------------
// 7. Memory safety under contention: unique_ptr wrapper with MPMC traffic.
//    All produced objects must eventually be destroyed (either by consumer
//    pop or by destructor drain). Best run under TSAN + (separately) ASAN.
// ---------------------------------------------------------------------------
TEST(LockfreeMPMCBoundedUniquePtr, MemorySafety_MPMC_NoLeak) {
    constexpr size_t N = 256;
    constexpr int num_producers = 4;
    constexpr int num_consumers = 4;
#if defined(TSFQUEUE_TSAN_BUILD)
    constexpr int ops_per_producer = 1000;
#else
    constexpr int ops_per_producer = 3000;
#endif
    constexpr int total_items = num_producers * ops_per_producer;
    reset_counters();
    {
        LockfreeMPMCBoundedUniquePtr<Counted, N> q;
        std::atomic<bool> producers_done{false};
        std::atomic<int> popped{0};

        std::vector<std::thread> producers;
        for (int p = 0; p < num_producers; ++p) {
            producers.emplace_back([&]() {
                for (int j = 0; j < ops_per_producer; ++j) {
                    std::unique_ptr<Counted> ptr(new Counted(j));
                    while (!q.push(ptr)) std::this_thread::yield();
                }
            });
        }

        std::vector<std::thread> consumers;
        for (int c = 0; c < num_consumers; ++c) {
            consumers.emplace_back([&]() {
                std::unique_ptr<Counted> out;
                while (true) {
                    if (q.pop(out)) {
                        popped.fetch_add(1);
                        out.reset();
                    } else if (producers_done.load() && q.empty()) {
                        break;
                    } else {
                        std::this_thread::yield();
                    }
                }
            });
        }

        for (auto& t : producers) t.join();
        producers_done.store(true);
        for (auto& t : consumers) t.join();

        EXPECT_EQ(popped.load(), total_items);
        // No outstanding items -> destructor drain has nothing to do.
    }
    EXPECT_EQ(g_counted_constructed.load(), total_items);
    EXPECT_EQ(g_counted_destroyed.load(), total_items)
        << "objects leaked: alive=" << g_counted_alive.load();
}


// ---------------------------------------------------------------------------
// 8. Lifecycle: repeated construct/destruct cycles must not leak or corrupt
//    state across instances. Run under ASAN to verify no heap mismanagement.
// ---------------------------------------------------------------------------
TEST(LockfreeMPMCBoundedQueue, Lifecycle_RepeatedConstructDestruct) {
    constexpr size_t N = 64;
#if defined(TSFQUEUE_TSAN_BUILD)
    constexpr int cycles = 200;
#else
    constexpr int cycles = 2000;
#endif

    for (int cycle = 0; cycle < cycles; ++cycle) {
        LockfreeMPMCBounded<uint64_t, N> q;
        EXPECT_TRUE(q.empty());
        for (uint64_t i = 0; i < N; ++i) {
            ASSERT_TRUE(q.push(i + cycle));
        }
        for (uint64_t i = 0; i < N; ++i) {
            uint64_t v;
            ASSERT_TRUE(q.pop(v));
            ASSERT_EQ(v, i + cycle);
        }
        EXPECT_TRUE(q.empty());
    }
}


// ---------------------------------------------------------------------------
// 9. Lifecycle: type traits / move/copy disabled at compile time.
//    These are compile-time guarantees expressed as runtime EXPECTs so the
//    test record makes the contract visible.
// ---------------------------------------------------------------------------
TEST(LockfreeMPMCBoundedQueue, Lifecycle_NonCopyableNonMovable) {
    using Q  = LockfreeMPMCBounded<uint64_t, 16>;
    using UQ = LockfreeMPMCBoundedUniquePtr<int, 16>;

    // The base queue holds atomic members and must not be copied or moved.
    EXPECT_FALSE(std::is_copy_constructible_v<Q>);
    EXPECT_FALSE(std::is_copy_assignable_v<Q>);

    // The unique_ptr wrapper explicitly deletes copy/move.
    EXPECT_FALSE(std::is_copy_constructible_v<UQ>);
    EXPECT_FALSE(std::is_copy_assignable_v<UQ>);
    EXPECT_FALSE(std::is_move_constructible_v<UQ>);
    EXPECT_FALSE(std::is_move_assignable_v<UQ>);
}
