#ifndef LOCKFREE_MPMC_BOUNDED_IMPL
#define LOCKFREE_MPMC_BOUNDED_IMPL

#include "defs.hpp"


namespace tsfqueue::impl{

    template<typename dataT, size_t N, typename indexT>
    [[using gnu: hot, flatten]] bool lockfree_mpmc_bounded<dataT, N, indexT>::push(value_type data_to_push) noexcept {
        while (true){
            // A: Got the current position at which write_index is pointing.
            // I all goes good we might push here only, in this iteration.
            index_type wr_index = _write_index.load();

            // B: Got the seq part of the cell we saved in wr_index. 
            // [at this time it might not be empty, or emptied and again occupied anything can occur]
            index_type seq = _array[wr_index].get_seq();

            if (seq == static_cast<index_type>((wr_index << 1))){
                // Confirms that A and B occurred at same time !

                // Expected entry at cell wr_index
                entry e{static_cast<index_type>(wr_index << 1)};

                // Entry to be placed at cell wr_index if everything goes right.
                entry new_e{static_cast<index_type>((wr_index << 1) | 1U), data_to_push};

                // Try to push to wr_index if everything is as expected
                if (_array[wr_index].compare_exchange(e, new_e)){

                    // ***[POSITION - X]***

                    // Till now its possible that after we added our data, someone else already 'helped us' and incremented 'wr_index'
                    // If NOT, we try to increment 'wr_index'.
                    _write_index.compare_exchange_strong(wr_index, wr_index + 1);

                    return true;
                }
            }else if ((seq == static_cast<index_type>((wr_index << 1) | 1U)) || 
                     (seq == static_cast<index_type>((wr_index + _array.size()) << 1))){
                    // If at 'B', someone else pushed in the current cell, but no one else poped.
                    // OR, If at 'B', someone else pushed and someone else poped but not pushed after that.
                    // OR, someone pushed but is currently at position 'X', in that case we can help them by incrementing 'wr_index'
                    // In the first two cases also, we might have empty cell where we can push after wr_index.
                    _write_index.compare_exchange_strong(wr_index, wr_index + 1);
                    
            }else if (static_cast<index_type>(seq+(_array.size() << 1)) == static_cast<index_type>((wr_index << 1) | 1U)){
                // Queue is full !
                return false;
            }
        }        
    }

    template<typename dataT, size_t N, typename indexT>
    [[using gnu: hot, flatten]] bool lockfree_mpmc_bounded<dataT, N, indexT>::enqueue(value_type data_to_enqueue) noexcept {
        return push(data_to_enqueue);
    }

    template<typename dataT, size_t N, typename indexT>
    [[using gnu: hot, flatten]] bool lockfree_mpmc_bounded<dataT, N, indexT>::pop(value_type & popped_data) noexcept {

        while (1){
            // A: choosed a 'rd_index' where we try to read from in this iteration.
            index_type rd_index = _read_index.load();

            // B: Here, we read the data at that position after some time.
            entry e{_array[rd_index].load()};

            if (e.get_seq() == static_cast<index_type>((rd_index << 1) | 1U)){
                // No one poped between 'A' and 'B'

                // Creating an empty entry to place there
                entry empty_entry{static_cast<index_type>((rd_index + _array.size()) << 1)};

                if (_array[rd_index].compare_exchange(e, empty_entry)){
                    // Successfully popped !
                    popped_data = e.get_data();

                    index_type tmp_index = rd_index;
                    _read_index.compare_exchange_strong(tmp_index, rd_index + 1);

                    return true;
                }
            }else if (static_cast<index_type>(e.get_seq() | 1U) == static_cast<index_type>(((rd_index + _array.size())<<1) | 1U)){
                // someone else poped and it again got filled. OR someone else popoed (incremented read index ? -> Not sure, so we try to do so).
                // Try to read from some higher index...
                _read_index.compare_exchange_strong(rd_index, rd_index + 1);
            }else if (e.get_seq() == static_cast<index_type>(rd_index << 1)){
                // Queue is empty ! -> return false...sure that we will not find place to pop no higher index
                return false;
            }
        }
    }

    template<typename dataT, size_t N, typename indexT>
    [[using gnu: hot, flatten]] bool lockfree_mpmc_bounded<dataT, N, indexT>::dequeue(value_type & popped_data) noexcept {
        return pop(popped_data);
    }

    // Now Methods where what the exact index where we pushed or popped from.
    
    template<typename dataT, size_t N, typename indexT>
    [[using gnu: hot, flatten]] bool lockfree_mpmc_bounded<dataT, N, indexT>::push(value_type data_to_push, index_type & pushed_where) noexcept {
        while (true){
            // A: Got the current position at which write_index is pointing.
            // I all goes good we might push here only, in this iteration.
            index_type wr_index = _write_index.load();

            // B: Got the seq part of the cell we saved in wr_index. 
            // [at this time it might not be empty, or emptied and again occupied anything can occur]
            index_type seq = _array[wr_index].get_seq();

            if (seq == static_cast<index_type>((wr_index << 1))){
                // Confirms that A and B occurred at same time !

                // Expected entry at cell wr_index
                entry e{static_cast<index_type>(wr_index << 1)};

                // Entry to be placed at cell wr_index if everything goes right.
                entry new_e{static_cast<index_type>((wr_index << 1) | 1U), data_to_push};

                // Try to push to wr_index if everything is as expected
                if (_array[wr_index].compare_exchange(e, new_e)){

                    pushed_where = wr_index;
                    
                    // ***[POSITION - X]***

                    // Till now its possible that after we added our data, someone else already 'helped us' and incremented 'wr_index'
                    // If NOT, we try to increment 'wr_index'.
                    _write_index.compare_exchange_strong(wr_index, wr_index + 1);

                    return true;
                }
            }else if ((seq == static_cast<index_type>((wr_index << 1) | 1U)) || 
                     (seq == static_cast<index_type>((wr_index + _array.size()) << 1))){
                    // If at 'B', someone else pushed in the current cell, but no one else poped.
                    // OR, If at 'B', someone else pushed and someone else poped but not pushed after that.
                    // OR, someone pushed but is currently at position 'X', in that case we can help them by incrementing 'wr_index'
                    // In the first two cases also, we might have empty cell where we can push after wr_index.

                    _write_index.compare_exchange_strong(wr_index, wr_index + 1);
            }else if (static_cast<index_type>(seq+(_array.size() << 1)) == static_cast<index_type>((wr_index << 1) | 1U)){
                // Queue is full !
                return false;
            }
        }        
    }

    template<typename dataT, size_t N, typename indexT>
    [[using gnu: hot, flatten]] bool lockfree_mpmc_bounded<dataT, N, indexT>::enqueue(value_type data_to_enqueue, index_type & enqueued_where) noexcept {
        return push(data_to_enqueue, enqueued_where);
    }

    template<typename dataT, size_t N, typename indexT>
    [[using gnu: hot, flatten]] bool lockfree_mpmc_bounded<dataT, N, indexT>::pop(value_type & popped_data, index_type & popped_from) noexcept {

        while (1){
            // A: choosed a 'rd_index' where we try to read from in this iteration.
            index_type rd_index = _read_index.load();

            // B: Here, we read the data at that position after some time.
            entry e{_array[rd_index].load()};

            if (e.get_seq() == static_cast<index_type>((rd_index << 1) | 1U)){
                // No one poped between 'A' and 'B'

                // Creating an empty entry to place there
                entry empty_entry{static_cast<index_type>((rd_index + _array.size()) << 1)};


                if (_array[rd_index].compare_exchange(e, empty_entry)){
                    // Successfully popped !
                    popped_data = e.get_data();
                    popped_from = rd_index;

                    index_type tmp_index = rd_index;
                    _read_index.compare_exchange_strong(tmp_index, rd_index + 1);

                    return true;
                }
            }else if (static_cast<index_type>(e.get_seq() | 1U) == static_cast<index_type>(((rd_index + _array.size())<<1) | 1U)){
                // someone else poped and it again got filled. OR someone else popoed (incremented read index ? -> Not sure, so we try to do so).
                // Try to read from some higher index...
                _read_index.compare_exchange_strong(rd_index, rd_index + 1);
            }else if (e.get_seq() == static_cast<index_type>(rd_index << 1)){
                // Queue is empty ! -> return false...sure that we will not find place to pop no higher index
                return false;
            }
        }
    }

    template<typename dataT, size_t N, typename indexT>
    [[using gnu: hot, flatten]] bool lockfree_mpmc_bounded<dataT, N, indexT>::dequeue(value_type & popped_data, index_type & dequeued_from) noexcept {
        return pop(popped_data, dequeued_from);
    }

    template<typename dataT, size_t N, typename indexT>
    template <typename F>
    [[using gnu: hot, flatten]] bool lockfree_mpmc_bounded<dataT, N, indexT>::pop_if(F& f, value_type & popped_data) noexcept {

        while (1){
            // A: choosed a 'rd_index' where we try to read from in this iteration.
            index_type rd_index = _read_index.load();

            // B: Here, we read the data at that position after some time.
            entry e{_array[rd_index].load()};

            if (e.get_seq() == static_cast<index_type>((rd_index << 1) | 1U)){
                // No one poped between 'A' and 'B'

                // Checking the condition
                if (!f(e.get_data(), e.get_seq())){return false;}

                // Creating an empty entry to place there
                entry empty_entry{static_cast<index_type>((rd_index + _array.size()) << 1)};

                if (_array[rd_index].compare_exchange(e, empty_entry)){
                    // Successfully popped !
                    popped_data = e.get_data();

                    index_type tmp_index = rd_index;
                    _read_index.compare_exchange_strong(tmp_index, rd_index + 1);

                    return true;
                }
            }else if (static_cast<index_type>(e.get_seq() | 1U) == static_cast<index_type>(((rd_index + _array.size())<<1) | 1U)){
                // someone else poped and it again got filled. OR someone else popoed (incremented read index ? -> Not sure, so we try to do so).
                // Try to read from some higher index...
                _read_index.compare_exchange_strong(rd_index, rd_index + 1);
            }else if (e.get_seq() == static_cast<index_type>(rd_index << 1)){
                // Queue is empty ! -> return false...sure that we will not find place to pop no higher index
                return false;
            }
        }
    }


    template<typename dataT, size_t N, typename indexT>
    template <typename F>
    [[using gnu: hot, flatten]] bool lockfree_mpmc_bounded<dataT, N, indexT>::pop_if(F& f, value_type & popped_data, index_type & popped_from) noexcept {

        while (1){
            // A: choosed a 'rd_index' where we try to read from in this iteration.
            index_type rd_index = _read_index.load();

            // B: Here, we read the data at that position after some time.
            entry e{_array[rd_index].load()};

            if (e.get_seq() == static_cast<index_type>((rd_index << 1) | 1U)){
                // No one poped between 'A' and 'B'

                // Checking the condition
                if (!f(e.get_data(), e.get_seq())){return false;}

                // Creating an empty entry to place there
                entry empty_entry{static_cast<index_type>((rd_index + _array.size()) << 1)};

                if (_array[rd_index].compare_exchange(e, empty_entry)){
                    // Successfully popped !
                    popped_data = e.get_data();
                    popped_from = rd_index;

                    index_type tmp_index = rd_index;
                    _read_index.compare_exchange_strong(tmp_index, rd_index + 1);

                    return true;
                }
            }else if (static_cast<index_type>(e.get_seq() | 1U) == static_cast<index_type>(((rd_index + _array.size())<<1) | 1U)){
                // someone else poped and it again got filled. OR someone else popoed (incremented read index ? -> Not sure, so we try to do so).
                // Try to read from some higher index...
                _read_index.compare_exchange_strong(rd_index, rd_index + 1);
            }else if (e.get_seq() == static_cast<index_type>(rd_index << 1)){
                // Queue is empty ! -> return false...sure that we will not find place to pop no higher index
                return false;
            }
        }
    }

    template<typename dataT, size_t N, typename indexT>
    [[using gnu: hot, flatten]] bool lockfree_mpmc_bounded<dataT, N, indexT>::exchange(index_type i, value_type expected_value, value_type new_value) noexcept {
        entry e_old{static_cast<index_type>((i << 1) | 1U), expected_value};
        entry e_new{static_cast<index_type>((i << 1) | 1U), new_value};
        
        return _array[i].compare_exchange(e_old, e_new);
    }

    template<typename dataT, size_t N, typename indexT>
    [[using gnu: hot, flatten]] bool lockfree_mpmc_bounded<dataT, N, indexT>::empty() noexcept {
        index_type rd_index = _read_index.load();
        entry e{_array[rd_index].load()};
        if (e.get_seq() == static_cast<index_type>(rd_index << 1)) return true;
        else return false;
    }

    template<typename dataT, size_t N, typename indexT>
    [[using gnu: hot, flatten]] bool lockfree_mpmc_bounded<dataT, N, indexT>::empty() const noexcept {
        index_type rd_index = _read_index.load();
        entry e{_array[rd_index].load()};
        if (e.get_seq() == static_cast<index_type>(rd_index << 1)) return true;
        else return false;
    }

    // Improved this function's logic, i think this is better that the Erez Strauss's queue function.
    template<typename dataT, size_t N, typename indexT>
    [[using gnu: hot, flatten]] [[nodiscard]] size_t lockfree_mpmc_bounded<dataT, N, indexT>::size() const noexcept {
        index_type wr_index = _write_index.load();
        index_type rd_index = _read_index.load();
        if (wr_index >= rd_index) return (wr_index - rd_index);
        return (_array.size() - ((rd_index - wr_index)&(_array.size()-1)));
    }

    template<typename dataT, size_t N, typename indexT>
    [[using gnu: hot, flatten]] [[nodiscard]] size_t lockfree_mpmc_bounded<dataT, N, indexT>::capacity() const noexcept {
        return _array.size();
    }

    template<typename dataT, size_t N, typename indexT>
    [[nodiscard]] constexpr size_t lockfree_mpmc_bounded<dataT, N, indexT>::entry_size() const noexcept {
        return sizeof(entry);
    }

    template<typename dataT, size_t N, typename indexT>
    [[nodiscard]] constexpr size_t lockfree_mpmc_bounded<dataT, N, indexT>::size_n() noexcept {
        return N;
    }

    // ------------------------------------------------------------------------
    // Unique pointer wrapper for lockfree_mpmc_bounded
    // ------------------------------------------------------------------------

    template<typename T, size_t N>
    [[using gnu: hot, flatten]] bool lockfree_mpmc_bounded_unique_ptr<T, N>::push(value_type & p) noexcept {
        uint64_t raw = reinterpret_cast<uint64_t>(p.get());
        if (_q.push(raw)) {(void)p.release(); return true;}
        return false;
    }

    template<typename T, size_t N>
    [[using gnu: hot, flatten]] bool lockfree_mpmc_bounded_unique_ptr<T, N>::push(value_type & p, index_type & i) noexcept {
        uint64_t raw = reinterpret_cast<uint64_t>(p.get());
        if (_q.push(raw, i)) {(void)p.release(); return true;}
        return false;
    }

    template<typename T, size_t N>
    [[using gnu: hot, flatten]] bool lockfree_mpmc_bounded_unique_ptr<T, N>::enqueue(value_type & p) noexcept {
        return push(p);
    }

    template<typename T, size_t N>
    [[using gnu: hot, flatten]] bool lockfree_mpmc_bounded_unique_ptr<T, N>::enqueue(value_type & p, index_type & i) noexcept {
        return push(p, i);
    }

    template<typename T, size_t N>
    [[using gnu: hot, flatten]] bool lockfree_mpmc_bounded_unique_ptr<T, N>::pop(value_type & out) noexcept {
        uint64_t raw;
        if (_q.pop(raw)) {out.reset(reinterpret_cast<T*>(raw)); return true;}
        return false;
    }

    template<typename T, size_t N>
    [[using gnu: hot, flatten]] bool lockfree_mpmc_bounded_unique_ptr<T, N>::pop(value_type & out, index_type & i) noexcept {
        uint64_t raw;
        if (_q.pop(raw, i)) {out.reset(reinterpret_cast<T*>(raw)); return true;}
        return false;
    }

    template<typename T, size_t N>
    [[using gnu: hot, flatten]] bool lockfree_mpmc_bounded_unique_ptr<T, N>::dequeue(value_type & out) noexcept {
        return pop(out);
    }

    template<typename T, size_t N>
    [[using gnu: hot, flatten]] bool lockfree_mpmc_bounded_unique_ptr<T, N>::dequeue(value_type & out, index_type & i) noexcept {
        return pop(out, i);
    }

    template<typename T, size_t N>
    bool lockfree_mpmc_bounded_unique_ptr<T, N>::evict_until_push(value_type & p) noexcept {
        while (true){
            if (push(p)) return true;
            value_type evicted;
            pop(evicted);
        }
    }

    template<typename T, size_t N>
    bool lockfree_mpmc_bounded_unique_ptr<T, N>::evict_until_push(value_type & p, index_type & i) noexcept {
        while (true){
            if (push(p, i)) return true;
            value_type evicted;
            pop(evicted);
        }
    }

    template<typename T, size_t N>
    [[using gnu: hot, flatten]] bool lockfree_mpmc_bounded_unique_ptr<T, N>::empty() noexcept {
        return _q.empty();
    }

    template<typename T, size_t N>
    [[using gnu: hot, flatten]] [[nodiscard]] bool lockfree_mpmc_bounded_unique_ptr<T, N>::empty() const noexcept {
        return _q.empty();
    }

    template<typename T, size_t N>
    [[using gnu: hot, flatten]] [[nodiscard]] size_t lockfree_mpmc_bounded_unique_ptr<T, N>::size() const noexcept {
        return _q.size();
    }

    template<typename T, size_t N>
    [[nodiscard]] size_t lockfree_mpmc_bounded_unique_ptr<T, N>::capacity() const noexcept {
        return _q.capacity();
    }

    template<typename T, size_t N>
    [[nodiscard]] constexpr size_t lockfree_mpmc_bounded_unique_ptr<T, N>::entry_size() const noexcept {
        return _q.entry_size();
    }

    template<typename T, size_t N>
    [[nodiscard]] constexpr size_t lockfree_mpmc_bounded_unique_ptr<T, N>::size_n() noexcept {
        return underlying_type::size_n();
    }

} // namespace tsfqueue::impl

#endif
