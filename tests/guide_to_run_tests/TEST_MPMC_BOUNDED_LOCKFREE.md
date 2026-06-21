# Running `test_mpmc_bounded_lockfree`

All commands run from the repo root.

## 1. First-time setup

```bash
rm -rf build
mkdir build && cd build
cmake ..
```

## 2. Normal build & run (everything)

```bash
cmake --build . --target test_mpmc_bounded_lockfree
./test_mpmc_bounded_lockfree
```

## 3. Run a single test

```bash
./test_mpmc_bounded_lockfree --gtest_filter=LockfreeMPMCBoundedQueue.BasicFIFO_SingleProducerSingleConsumer
./test_mpmc_bounded_lockfree --gtest_filter=*EdgeCases*
./test_mpmc_bounded_lockfree --gtest_list_tests
```

## 4. Run via ctest

```bash
ctest -R MPMC_BOUNDED_LOCKFREE --output-on-failure
```

## 5. ThreadSanitizer build (data races)

```bash
cd build && rm -rf *
cmake -DENABLE_TSAN=ON ..
cmake --build . --target test_mpmc_bounded_lockfree
./test_mpmc_bounded_lockfree
```

## 6. AddressSanitizer build (UAF, leaks, OOB)

```bash
cd build && rm -rf *
cmake -DENABLE_ASAN=ON ..
cmake --build . --target test_mpmc_bounded_lockfree
./test_mpmc_bounded_lockfree
```

## 7. UndefinedBehaviorSanitizer build

```bash
cd build && rm -rf *
cmake -DENABLE_UBSAN=ON ..
cmake --build . --target test_mpmc_bounded_lockfree
./test_mpmc_bounded_lockfree
```

## Test list

| # | Test | Category |
|---|---|---|
| 1 | `LockfreeMPMCBoundedQueue.BasicFIFO_SingleProducerSingleConsumer`       | Basic SPSC FIFO |
| 2 | `LockfreeMPMCBoundedQueue.EdgeCases_EmptyPop_FullPush_WrapAround`       | Edge cases |
| 3 | `LockfreeMPMCBoundedQueue.MultiProducerMultiConsumer_NoLossNoDuplicates`| MPMC correctness |
| 4 | `LockfreeMPMCBoundedQueue.Linearizability_PerProducerMonotonicOrder`    | Linearizability |
| 5 | `LockfreeMPMCBoundedUniquePtr.MemorySafety_DrainOnDestroy`              | Memory safety |
| 6 | `LockfreeMPMCBoundedUniquePtr.MemorySafety_PushPopRoundtrip`            | Memory safety |
| 7 | `LockfreeMPMCBoundedUniquePtr.MemorySafety_MPMC_NoLeak`                 | Memory safety (concurrent) |
| 8 | `LockfreeMPMCBoundedQueue.Lifecycle_RepeatedConstructDestruct`          | Lifecycle |
| 9 | `LockfreeMPMCBoundedQueue.Lifecycle_NonCopyableNonMovable`              | Lifecycle (type traits) |
