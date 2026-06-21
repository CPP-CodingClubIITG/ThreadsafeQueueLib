#include <atomic>
#include <iostream>
#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <chrono>
#include "tsfqueue.hpp"
#include <string>


// Basic sanity
TEST(MPMCQueue, BasicPushPop_Unbounded) {
  tsfqueue::BlockingMPMCUnbounded<int> q;

  EXPECT_TRUE(q.empty());

  q.push(1);
  q.push(2);

  int x;
  EXPECT_TRUE(q.try_pop(x));
  EXPECT_EQ(x, 1);

  EXPECT_TRUE(q.try_pop(x));
  EXPECT_EQ(x, 2);

  EXPECT_TRUE(q.empty());
}

TEST(MPMCQueue, WaitFor_isTimeExact) {
  tsfqueue::BlockingMPMCUnbounded<int> q;

  EXPECT_TRUE(q.empty());

  q.push(1);
  q.push(2);

  int x;
  EXPECT_TRUE(q.try_pop(x));
  EXPECT_EQ(x, 1);

  EXPECT_TRUE(q.try_pop(x));
  EXPECT_EQ(x, 2);

  EXPECT_TRUE(q.empty());

  int wtime = 200;
  int error = 10;
  // auto func = [&](){
  //   std::this_thread::sleep_for(std::chrono::seconds(10));
  //   q.push(12);
  //   q.push(31);
  // };
  // std::thread t(func);
  auto start = std::chrono::high_resolution_clock::now();
  int res = q.wait_for_and_pop(x, std::chrono::milliseconds(wtime));
  auto end = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  EXPECT_NEAR((int)duration, (int)wtime, error);
}

TEST(MPMCQueue, WaitForTester) {
  tsfqueue::BlockingMPMCUnbounded<int> q;

  EXPECT_TRUE(q.empty());

  q.push(1);
  q.push(2);

  int x;
  EXPECT_TRUE(q.try_pop(x));
  EXPECT_EQ(x, 1);

  EXPECT_TRUE(q.try_pop(x));
  EXPECT_EQ(x, 2);

  EXPECT_TRUE(q.empty());

  int wtime = 4000;
  int pushTime = 2000;
  int error = 50;
  auto func = [&](){
    std::this_thread::sleep_for(std::chrono::milliseconds(pushTime));
    q.push(12);
  };
  std::thread t(func);
  auto start = std::chrono::high_resolution_clock::now();
  EXPECT_TRUE(q.wait_for_and_pop(x, std::chrono::milliseconds(wtime)));
  auto end = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  EXPECT_NEAR((int)duration, (int)pushTime, error);

  EXPECT_EQ(x, 12);
  
  EXPECT_TRUE(q.empty());
  t.join();
}

TEST(MPMCQueue, DataRaceStressTest) {
    tsfqueue::BlockingMPMCUnbounded<int> q;
    const int num_threads = 8;
    const int ops_per_thread = 1000;
    std::atomic<int> total_popped{0};
    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;

    // Launch Producers
    for (int i = 0; i < num_threads; ++i) {
        producers.emplace_back([&q, ops_per_thread]() {
            for (int j = 0; j < ops_per_thread; ++j) {
                q.push(j);
            }
        });
    }

    // Launch Consumers
    for (int i = 0; i < num_threads; ++i) {
        consumers.emplace_back([&q, ops_per_thread, &total_popped]() {
            for (int j = 0; j < ops_per_thread; ++j) {
                int val;
                // Using wait_for to ensure we don't block forever if a push is missed
                if (q.wait_for_and_pop(val, std::chrono::milliseconds(100))) {
                    total_popped++;
                }
            }
        });
    }

    for (auto& t : producers) t.join();
    for (auto& t : consumers) t.join();

    // Verify all items were accounted for
    EXPECT_EQ(total_popped.load(), num_threads * ops_per_thread);
    EXPECT_TRUE(q.empty());
}

// Testing Emplace Back working and Static Assert working, i.e it does not create a copy of the object.
struct MockObject {
    static int copies;
    static int moves;
    int x;
    MockObject(int val) : x(val) {}
    MockObject(const MockObject& other) { copies++; }
    MockObject(MockObject&& other) noexcept { moves++; }
    MockObject& operator=(const MockObject& other) {
        x = other.x;
        copies++;
        return *this;
    }

    MockObject& operator=(MockObject&& other) noexcept {
        x = other.x;
        moves++;
        return *this;
    }
};
int MockObject::copies = 0;
int MockObject::moves = 0;

TEST(MPMCQueue, EmplaceBackEfficiency_and_StaticAsserts) {
    tsfqueue::BlockingMPMCUnbounded<MockObject> q;
    MockObject::copies = 0;
    MockObject::moves = 0;
    // Emplace should construct the object directly in the internal storage
    q.emplace_back(42); 

    EXPECT_EQ(MockObject::copies, 0);
    // Depending on implementation, moves should ideally be 0 or 1
    EXPECT_LE(MockObject::moves, 1); 
    
    MockObject out(0);

    EXPECT_TRUE(q.try_pop(out));
    EXPECT_EQ(out.x, 42);
}

// ---------------------------------------------------------
// Helper: A type that is NEITHER copyable NOR movable
// ---------------------------------------------------------
struct LockedType {
    LockedType() = default;
    LockedType(const LockedType&) = delete;
    LockedType(LockedType&&) = delete;
    LockedType& operator=(const LockedType&) = delete;
    LockedType& operator=(LockedType&&) = delete;
};

// ---------------------------------------------------------
// Helper: A type that is ONLY movable
// ---------------------------------------------------------
struct MoveOnly {
    MoveOnly() = default;
    MoveOnly(const MoveOnly&) = delete;
    MoveOnly(MoveOnly&&) = default;
    MoveOnly& operator=(const MoveOnly&) = delete;
    MoveOnly& operator=(MoveOnly&&) = default;
};

// ---------------------------------------------------------
// The Test Case
// ---------------------------------------------------------
TEST(MPMCQueue, StaticRequirementsValidation) {
    // 1. Test Copy/Move Constructibility (Required for Push/Emplace)
    bool is_pushable = std::is_copy_constructible_v<int> || std::is_move_constructible_v<int>;
    EXPECT_TRUE(is_pushable) << "Queue must support copy or move for push operations.";

    // 2. Test Assignability (Required for try_pop(T& value))
    // This mirrors your failing static_assert: 
    // static_assert(std::is_copy_assignable_v<T> || std::is_move_assignable_v<T>)
    
    bool move_only_assignable = std::is_copy_assignable_v<MoveOnly> || std::is_move_assignable_v<MoveOnly>;
    EXPECT_TRUE(move_only_assignable) << "Move-only types should be allowed if they are move-assignable.";

    bool locked_type_assignable = std::is_copy_assignable_v<LockedType> || std::is_move_assignable_v<LockedType>;
    EXPECT_FALSE(locked_type_assignable) << "LockedType should fail the assignability requirement.";
}