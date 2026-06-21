#ifndef LOCKFREE_MPMC_BOUNDED_DEFS
#define LOCKFREE_MPMC_BOUNDED_DEFS

#include "../utils.hpp"
#include <array>
#include <atomic>
#include <climits>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <type_traits>

namespace tsfqueue::impl{

    template <size_t N>
    class unit_value{
        static_assert(N == 8 || N == 16,
                      "unit_value only supports 8 or 16 byte entries; "
                      "choose value_type/index_type so sizeof({data,seq}) is 8 or 16");
    };
    template <>
    class unit_value<8>{public: using type = __int64_t;};
    template <>
    class unit_value<16>{public: using type = __int128_t;};
    // ---------------------------------------------------

    // For creating aligned custom types
    template <typename T, unsigned A>
    class alignas(A) aligned_type : public T{};

    // ---------------------------------------------------

    // Helper classes for Runtime and CompileTime Arrays
    // Compile-time array initialization helper
    template <typename T, size_t N>
    class array_compileTime{
        std::array<T, N> _data;
        static_assert(N > 0 && ((N & (N-1)) == 0), "The size of compile-time array for queue must be positive and power of 2 !");
        
        public:
            explicit array_compileTime(size_t = N) noexcept : _data() {} // initialize elements as '0'
            ~array_compileTime() noexcept = default;
            T& operator[](size_t index) noexcept { return _data[index & (N - 1)]; }
            const T& operator[](size_t index) const noexcept { return _data[index & (N - 1)]; }
            [[nodiscard]] constexpr size_t size() const noexcept {return N;}
            [[nodiscard]] constexpr size_t index_mask() const noexcept {return N-1;}
            [[nodiscard]] constexpr size_t capacity() const noexcept {return N;}
    };

    // Runtime array initialization helper 
    template <typename T, size_t N>
    class array_runtime{
        std::unique_ptr<T[]> _datap alignas(2 * cache_line_size);
        const size_t _n;
        const size_t _index_mask;
        static_assert(N == 0, "Runtime array is formed only when N == 0 !");
        public:
            explicit array_runtime(size_t n = 0) noexcept : _datap(nullptr), _n(n), _index_mask(n-1){
                if (n > 0){
                    assert(((n & (n-1)) == 0) && "Array size should be power of 2.");
                    _datap = std::unique_ptr<T[]>{new T[n]};
                }
            }

            ~array_runtime() noexcept = default;

            T& operator[](size_t _index) noexcept {return _datap[_index & _index_mask];}
            const T& operator[](size_t _index) const noexcept {return _datap[_index & _index_mask];}
            [[nodiscard]] size_t size() const noexcept {return _n;}
            [[nodiscard]] size_t index_mask() const noexcept {return _index_mask;}
            [[nodiscard]] size_t capacity() const noexcept {return _n;}
    };
    // ------------------------------------------------------------------------------------------------- 

    template <typename dataT,
              size_t N = 0,
              typename indexT = uint64_t
              >
    // Used alignas(...) so that members like write and read index (which are 128bit aligned) works well as whole class is aligned.
    class alignas(2*cache_line_size) lockfree_mpmc_bounded{
        /*
        For the implementation we start with a bounded array of size N of atomic<__int128_t> integers.
        Here, the 128 bits of each cell is divided into three sections: 

        ** [64bits(DATA), 64bits(sequence Number with the least significan bit for full/empty flag)] **

        This bounded array, say 'queue' is our Main queue data structure, its a circular buffer of queue entries.
        We have two atomic integers 'read_index' and 'write_index'
        'read_index' ->  1: read_index & (N-1) is the index from which we read.
                        2: We read the value only when the seq. no. == read_index and cell not empty.
        'write_index' -> 1: write_index & (N-1) is the index where which we write.
                        2: We write only when seq. no. == write_index and cell is empty.
                
        When is:
        [Empty queue] -> When read_index == write_index.
        [Full queue] -> When write_index == N + read_index.

        For this queue, to store any dataType of large size, we need CAS16, i.e Hardware Atomic Compare and Swap for 16 bytes (128 bit) integers.
        This allow us to atomically add full 128 bits to the queue cell, we add sequence number, Flag and DATA at the same time.
        -> We can store any size data by storing the 64bit pointer to the data. (Created a wrapper which make this queue maintain ownership at terminals using unique_ptr).

        PUSH: (see the code, all these points can occur at very different time, as queue is MPMC supporting)
            - A: Got the current position at which write_index is pointing. If all goes good we might push here only, in this iteration.
            - B: Got the seq part of the cell we saved in wr_index. [at this time it might not be empty, or emptied and again occupied, anything can occur]
            - If seq confirms A and B occurred at same time: try to compare_exchange the cell with our new entry. On success, try to increment 'wr_index' (someone else might have already 'helped us').
            - Else if someone else pushed in the current cell (with or without a subsequent pop), or someone pushed but is still at position X: help by incrementing 'wr_index'. We might still find an empty cell after wr_index.
            - Else if queue is full: return false.

        POP: (see the code, all these points can occur at very different time, as queue is MPMC supporting)
            - A: chose a 'rd_index' where we try to read from in this iteration.
            - B: read the data (whole cell) at that position after some time.
            - If seq says no one popped between A and B: try to compare_exchange the cell with an empty entry. On success, return the data and try to increment 'rd_index'.
            - Else if someone else popped and it got filled again, OR someone else popped (incremented read_index? - not sure, so we try to do so): help by incrementing 'rd_index' and retry from a higher index.
            - Else if queue is empty: return false (no higher index will have data either).
        
        */

        public: // Descriptive names for data and index types.
            using value_type = dataT;
            using index_type = indexT;

            static constexpr unsigned bits_in_index() noexcept { return sizeof(index_type) * CHAR_BIT; }
            static constexpr unsigned bits_for_value(unsigned n) noexcept {
                unsigned b{0};
                while (n != 0) { ++b; n >>= 1U; }
                return b;
            }

            static_assert(std::is_trivial_v<value_type>,
                          "value_type must be trivial (queue stores entries via raw memory copy)");
            static_assert(std::is_unsigned_v<index_type>,
                          "index_type must be unsigned (algorithm relies on defined wrap-around)");
            static_assert(sizeof(index_type) >= 4,
                          "index_type should be 4 bytes or wider; 1/2 byte indices are for experiments only");
            static_assert(sizeof(index_type) == 1 || sizeof(index_type) == 2 ||
                          sizeof(index_type) == 4 || sizeof(index_type) == 8,
                          "index_type size must be one of: 1, 2, 4, 8");
            static_assert(N == 0 || (bits_in_index() > bits_for_value(N)),
                          "index_type must be wide enough to address N slots without wrap-around");

        private:

            // For measuring size, so that we can align our main entry class with its size.
            struct alignas(8) helper_entry{
                value_type _data;
                index_type _index;
            };

            using entry_as_value = typename unit_value<sizeof(helper_entry)>::type;
            
            constexpr static inline bool is_always_lock_free = std::atomic<entry_as_value>::is_always_lock_free;

            static constexpr bool USE_BUILTIN_16B{
                #if defined(__GNUC__) && defined(__clang__)
                    false // clang: std::atomic<__int128> is already hardware lock-free
                #elif defined(__GNUC__)
                    true  // gcc: std::atomic<__int128> falls back to libatomic (locked) -> use __sync_* builtin
                #else
                    false // unknown compiler: trust std::atomic
                #endif
            };

            // main 'entry' class (making class because by default members are private)
            class alignas(sizeof(helper_entry)) entry{
                // Made this as Union, so that the same memory location stores both members and we can change any one based on need.
                union entry_union{
                    mutable entry_as_value _value;
                    struct entry_struct{
                        value_type _data;
                        index_type _index;
                    } _x;
                    // Data, Sequence and Flag all '0' at the start.
                    entry_union(){_value = 0;}
                } _u;

                public:
                    // We are clearing in each constructor because 
                    entry() noexcept {clear();}

                    explicit entry(index_type s) noexcept {
                        clear();
                        _u._x._index = s;
                    }

                    explicit entry(index_type s, value_type d) noexcept {
                        clear();
                        _u._x._index = s;
                        _u._x._data = d;
                    }

                    // Here no clearing because, it already changing the full entry.
                    explicit entry(entry_as_value ev) noexcept {
                        _u._value = ev;
                    }

                    void clear(){_u._value = 0;}

                    ~entry() noexcept = default;

                    // For setting values later
                    void set_seq(index_type s){
                        _u._x._index = s;
                    }

                    void set(index_type s, value_type v){
                        clear();
                        _u._x._index = s;
                        _u._x._data = v;
                    }

                    void set_full(entry_as_value ev){_u._value = ev;}

                    index_type get_seq(){
                        return _u._x._index;
                    }

                    value_type get_data(){
                        return _u._x._data;
                    }

                    bool is_empty() const { return !(_u._x._index & 1U); }
                    bool is_full() const {return !is_empty();}

                    // Needed because we might do, entry e = _array[index].load() -> this return a raw __int128_t or __int64_t...
                    // so need to overload '=' so that convert raw int to entry object.
                    entry & operator=(entry_as_value ev) noexcept {
                        _u._value = ev;
                        return *(this);
                    }

                    [[using gnu: hot]] entry_as_value load() noexcept {
                        if constexpr (sizeof(entry_as_value)==16 && USE_BUILTIN_16B){
                            return __sync_val_compare_and_swap(&this->_u._value, 0, 0);
                        }else{
                            return reinterpret_cast<std::atomic<entry_as_value>*>(this)->load();
                        }
                    }

                    [[using gnu: hot]] entry_as_value load() const noexcept {
                        if constexpr (sizeof(entry_as_value)==16 && USE_BUILTIN_16B){
                            return __sync_val_compare_and_swap(&this->_u._value, 0, 0);
                        }else{
                            return reinterpret_cast<std::atomic<entry_as_value>*>(this)->load();
                        }
                    }

                    [[using gnu: hot]] bool compare_exchange(entry expected, entry new_to_put) noexcept {
                        if constexpr (sizeof(entry_as_value)==16 && USE_BUILTIN_16B){
                            return __sync_bool_compare_and_swap(&this->_u._value, expected._u._value, new_to_put._u._value);
                        }else{
                            return reinterpret_cast<std::atomic<entry_as_value>*>(this)->compare_exchange_strong(expected._u._value, new_to_put._u._value);
                        }
                    }
            };

            static_assert(sizeof(entry) == 2 || sizeof(entry) == 4 || sizeof(entry) == 8 || sizeof(entry) == 16,
                          "entry size not supported (must be 2, 4, 8 or 16 bytes for atomic CAS)");
            static_assert(sizeof(entry) == sizeof(helper_entry),
                          "entry and helper_entry must be of the same size");
            static_assert(sizeof(entry) == sizeof(entry_as_value),
                          "entry and entry_as_value must be of the same size");

            // Type of _array, Based on condition whether size of array is given before or not, it assigns its type.
            using array_t = typename std::conditional<
                N == 0, array_runtime<aligned_type<entry, 2 * cache_line_size>, 0>,
                array_compileTime<aligned_type<entry, 2 * cache_line_size>, N>
            >::type;

        public:
            lockfree_mpmc_bounded(uint64_t n = N): _write_index(0), _read_index(0), _array(n) // Here the constructor of _array got 'n' and Memory got allocated
            {
                if (N > 0){
                    if (n != N){
                        throw(std::invalid_argument{"The Compile time size given as a template argument should be same as constructor argument to mpmc_queue when deciding size at compile-time size (N > 0) !"});
                    }
                }else{
                    if ((n & (n-1)) != 0){
                        throw(std::invalid_argument{
                            std::string{"The Runtime size provided to mpmc_queue::constructor should be power of 2 !"}
                        });
                    }else if (bits_in_index() <= bits_for_value(n)){
                        throw(std::invalid_argument{
                            std::string{"The size given ["}+std::to_string(n)+std::string{"] is too large for index_type !"}
                        });
                    }
                }
                // Initializing the _array with 0, 2, 4, ... -> (here LSB = 0) => Empty. 
                // and Index in incremental order so (seq of _array[i]) >> 1 == INDEX in array.
                for (index_type i = 0;i < _array.size();i++){
                    _array[i].set_seq(i << 1);
                }
            }

            // Best practice to remove all our data from the queue.
            ~lockfree_mpmc_bounded(){
                value_type v;
                while (pop(v));
            }

            // Removing Copy Constructor and Copy Assignment operator.
            lockfree_mpmc_bounded(const lockfree_mpmc_bounded &) = delete;
            lockfree_mpmc_bounded& operator=(const lockfree_mpmc_bounded &) = delete;

            // Removing Move Constructor and Move Assignment operator.
            lockfree_mpmc_bounded(lockfree_mpmc_bounded &&) = delete;
            lockfree_mpmc_bounded& operator=(lockfree_mpmc_bounded &&) = delete;

            [[using gnu: hot, flatten]] bool push(value_type) noexcept;
            [[using gnu: hot, flatten]] bool enqueue(value_type) noexcept;

            [[using gnu: hot, flatten]] bool pop(value_type&) noexcept;
            [[using gnu: hot, flatten]] bool dequeue(value_type&) noexcept;

            // If we want the index of the cell where we put it. 
            // (NOT QUEUE INDEX, BUT THE SEQUENCE NUMBER OF CELL BEFORE WE PUT THE VALUE)
            [[using gnu: hot, flatten]] bool enqueue(value_type, index_type&) noexcept;
            [[using gnu: hot, flatten]] bool push(value_type, index_type&) noexcept;
            [[using gnu: hot, flatten]] bool dequeue(value_type&, index_type&) noexcept;
            [[using gnu: hot, flatten]] bool pop(value_type&, index_type&) noexcept;

            // Takes a callable which returns a boolean and takes two argumets (value_type, index_type)
            template<typename F>
            [[using gnu: hot, flatten]] bool pop_if(F&, value_type&) noexcept;
            template<typename F>
            [[using gnu: hot, flatten]] bool pop_if(F&, value_type&, index_type&) noexcept;

            // Keep evicting the oldest value from the queue, until you are able to push successfully.
            bool evict_until_push(value_type v){
                while (true){
                    if (push(v)){return true;}
                    value_type being_lost;
                    pop(being_lost);
                }
            }

            bool evict_until_push(value_type v, index_type & i){
                while (true){
                    if (push(v, i)){return true;}
                    value_type being_lost;
                    pop(being_lost);
                }
            }

            [[using gnu: hot, flatten]] bool exchange(index_type, value_type, value_type) noexcept;

            [[using gnu: hot, flatten]] bool empty() noexcept;

            // Using [[nodiscard]] because calling them and not using the return value is certainly a bug !
            [[using gnu: hot, flatten]] [[nodiscard]] bool empty() const noexcept;

            [[using gnu: hot, flatten]] [[nodiscard]] size_t size() const noexcept;
            [[nodiscard]] size_t capacity() const noexcept;

            [[nodiscard]] constexpr size_t entry_size() const noexcept;
            [[nodiscard]] static constexpr size_t size_n() noexcept; // 'const' does not makes sense in 'static' method, as relation with '*this'.
            
        private:
            alignas(2 * cache_line_size) std::atomic<index_type> _write_index;
            alignas(2 * cache_line_size) std::atomic<index_type> _read_index;
            array_t _array;
    };

    template <typename T, size_t N>
    class alignas(2*cache_line_size) lockfree_mpmc_bounded_unique_ptr{
        /*
            This Class is a wrapper which uses 'lockfree_mpmc_bounded' queue and forms most of its function
            but with a slight modification to make it work for 'unique_ptr'.

            PUSH - For pushing we take unique_ptr (ownership) as argument -> Retrive its underlying pointer
                   and RELEASE the unique_ptr [Now the ownership is GONE] -> Reinterpreat_Cast the pointer to 'uint64_t'
                   -> push it to the queue.
            
            POP - For popping we first pop the data from queue and get 'uint64_t' -> reinterpreat_cast it to Normal Pointer (T*)
                   -> Create a unique_ptr with that pointer [Ownership regained].
        */
        public:
            using value_type      = std::unique_ptr<T>;
            using underlying_type = lockfree_mpmc_bounded<uint64_t, N>;
            using index_type      = typename underlying_type::index_type;

            static_assert(sizeof(T*) <= sizeof(uint64_t), "T* must fit in uint64_t");

            lockfree_mpmc_bounded_unique_ptr() noexcept = default;

            lockfree_mpmc_bounded_unique_ptr(const lockfree_mpmc_bounded_unique_ptr&) = delete;
            lockfree_mpmc_bounded_unique_ptr& operator=(const lockfree_mpmc_bounded_unique_ptr&) = delete;
            lockfree_mpmc_bounded_unique_ptr(lockfree_mpmc_bounded_unique_ptr&&) = delete;
            lockfree_mpmc_bounded_unique_ptr& operator=(lockfree_mpmc_bounded_unique_ptr&&) = delete;

            // Destructor: removes remaining entries so owned objects are freed.
            // Not thread-safe: must not race with push/pop on other threads.
            ~lockfree_mpmc_bounded_unique_ptr() noexcept {
                value_type tmp;
                while (pop(tmp)) {}
            }

            [[using gnu: hot, flatten]] bool push(value_type &) noexcept;
            [[using gnu: hot, flatten]] bool push(value_type &, index_type &) noexcept;

            [[using gnu: hot, flatten]] bool enqueue(value_type &) noexcept;
            [[using gnu: hot, flatten]] bool enqueue(value_type &, index_type &) noexcept;

            [[using gnu: hot, flatten]] bool pop(value_type &) noexcept;
            [[using gnu: hot, flatten]] bool pop(value_type &, index_type &) noexcept;

            [[using gnu: hot, flatten]] bool dequeue(value_type &) noexcept;
            [[using gnu: hot, flatten]] bool dequeue(value_type &, index_type &) noexcept;

            // Keep evicting (and destroying) the oldest owned pointer until push succeeds.
            bool evict_until_push(value_type &) noexcept;
            bool evict_until_push(value_type &, index_type &) noexcept;

            [[using gnu: hot, flatten]] bool empty() noexcept;
            [[using gnu: hot, flatten]] [[nodiscard]] bool empty() const noexcept;

            [[using gnu: hot, flatten]] [[nodiscard]] size_t size() const noexcept;
            [[nodiscard]] size_t capacity() const noexcept;

            [[nodiscard]] constexpr size_t entry_size() const noexcept;
            [[nodiscard]] static constexpr size_t size_n() noexcept;

        private:
            underlying_type _q;
    };

}

#endif