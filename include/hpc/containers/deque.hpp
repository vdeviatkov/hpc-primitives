#pragma once

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace hpc::containers {

// Double-ended queue with the classic std::deque layout: fixed-size chunks
// indexed by a central map of chunk pointers.
//
// Data layout
//   - Chunk: raw storage for chunk_elems elements (about 512 bytes, at least
//     1 element).
//   - Map: a heap array of T* (map_, map_size_), one entry per chunk, kept
//     with spare entries at both ends. It starts with 8 entries and one chunk
//     in the middle.
//   - start_ / finish_: iterators {cur, first, last, node} for the first
//     element and one past the last. `node` points into the map, and
//     [first, last) is that node's chunk.
//
// How it works
//   - push_back: if finish_.cur_ is not the last slot of its chunk, construct
//     there and ++cur. Otherwise construct in that last slot, allocate the
//     next chunk and step finish_ into it, so finish_.cur_ always points at a
//     valid slot. push_front is the mirror image at start_.
//   - pop_front / pop_back: destroy the element and move cur. A chunk is
//     freed as soon as it empties; there is no spare-chunk cache. Push and pop
//     at one end touch only that end's iterator.
//   - When the map runs out of entries on one side, there are two cases. If
//     the map is more than twice the number of chunks in use, the chunk
//     pointers slide back to its centre. Otherwise a larger map is allocated
//     (size + max(size, needed) + 2) and the pointers are copied into its
//     middle. Elements themselves never move.
//   - Indexing (operator[], iterator +=): add the offset to cur. If the result
//     leaves the chunk, divide by chunk_elems to get the chunk step and the
//     slot within it. That costs more than vector indexing.
//   - Because elements never move, push/pop at either end keeps references to
//     other elements valid. Iterators are invalidated by push.
//   - Default construction and the moved-from state allocate nothing; the map
//     is created on first insertion.
template <class T>
class deque {

public:
    using value_type      = T;
    using size_type       = std::size_t;
    using difference_type = std::ptrdiff_t;
    using reference       = T&;
    using const_reference = const T&;
    using pointer         = T*;
    using const_pointer   = const T*;

    // Elements per chunk: ~512 bytes, minimum 1.
    static constexpr size_type chunk_elems =
        sizeof(T) < 512 ? 512 / sizeof(T) : 1;

    // -- Iterator ---------------------------------------------------------

    template <bool Const>
    class iterator_impl {
        friend class deque;
        friend class iterator_impl<!Const>;

        T*  cur_   = nullptr;
        T*  first_ = nullptr;   // start of current chunk
        T*  last_  = nullptr;   // one-past-end of current chunk
        T** node_  = nullptr;   // pointer into the map array

        void set_node(T** n) noexcept {
            node_  = n;
            first_ = *n;
            last_  = first_ + static_cast<difference_type>(chunk_elems);
        }


    public:
        using iterator_category = std::random_access_iterator_tag;
        using value_type        = T;
        using difference_type   = std::ptrdiff_t;
        using pointer           = std::conditional_t<Const, const T*, T*>;
        using reference         = std::conditional_t<Const, const T&, T&>;

        iterator_impl() noexcept = default;

        // non-const → const conversion
        template <bool C2, class = std::enable_if_t<Const && !C2>>
        iterator_impl(const iterator_impl<C2>& o) noexcept
            : cur_(o.cur_), first_(o.first_), last_(o.last_), node_(o.node_) {}

        reference operator*()  const noexcept { return *cur_; }
        pointer   operator->() const noexcept { return  cur_; }

        iterator_impl& operator++() noexcept {
            ++cur_;
            if (cur_ == last_) { set_node(node_ + 1); cur_ = first_; }
            return *this;
        }
        iterator_impl operator++(int) noexcept { auto t = *this; ++*this; return t; }

        iterator_impl& operator--() noexcept {
            if (cur_ == first_) { set_node(node_ - 1); cur_ = last_; }
            --cur_;
            return *this;
        }
        iterator_impl operator--(int) noexcept { auto t = *this; --*this; return t; }

        iterator_impl& operator+=(difference_type n) noexcept {
            const auto ce = static_cast<difference_type>(chunk_elems);
            difference_type off = n + (cur_ - first_);
            if (off >= 0 && off < ce) {
                cur_ += n;
            } else {
                difference_type node_off =
                    off > 0 ? off / ce
                            : -((-off - 1) / ce) - 1;
                set_node(node_ + node_off);
                cur_ = first_ + (off - node_off * ce);
            }
            return *this;
        }
        iterator_impl& operator-=(difference_type n) noexcept { return *this += -n; }

        reference operator[](difference_type n) const noexcept {
            return *(*this + n);
        }

        friend iterator_impl operator+(iterator_impl i, difference_type n) noexcept { return i += n; }
        friend iterator_impl operator+(difference_type n, iterator_impl i) noexcept { return i += n; }
        friend iterator_impl operator-(iterator_impl i, difference_type n) noexcept { return i -= n; }
        friend difference_type operator-(const iterator_impl& a,
                                         const iterator_impl& b) noexcept {
            if (a.node_ == b.node_) return a.cur_ - b.cur_;
            return static_cast<difference_type>(chunk_elems)
                       * (a.node_ - b.node_ - 1)
                   + (a.cur_ - a.first_)
                   + (b.last_ - b.cur_);
        }

        friend bool operator==(const iterator_impl& a, const iterator_impl& b) noexcept { return a.cur_ == b.cur_; }
        friend bool operator< (const iterator_impl& a, const iterator_impl& b) noexcept {
            return a.node_ == b.node_ ? a.cur_ < b.cur_ : a.node_ < b.node_;
        }
        friend bool operator> (const iterator_impl& a, const iterator_impl& b) noexcept { return b < a; }
        friend bool operator<=(const iterator_impl& a, const iterator_impl& b) noexcept { return !(b < a); }
        friend bool operator>=(const iterator_impl& a, const iterator_impl& b) noexcept { return !(a < b); }
    };

    using iterator               = iterator_impl<false>;
    using const_iterator         = iterator_impl<true>;
    using reverse_iterator       = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    // -- Constructors / destructor ----------------------------------------

    deque() noexcept = default;

    // Filling constructors delegate to the default constructor so that the
    // destructor cleans up if an element constructor throws.
    explicit deque(size_type count) : deque() { resize(count); }
    deque(size_type count, const T& value) : deque() { resize(count, value); }
    deque(std::initializer_list<T> il) : deque(il.begin(), il.end()) {}

    template <std::input_iterator It>
    deque(It first, It last) : deque()
    {
        for (; first != last; ++first) emplace_back(*first);
    }

    deque(const deque& rhs) : deque(rhs.begin(), rhs.end()) {}

    deque(deque&& rhs) noexcept
        : map_(rhs.map_), map_size_(rhs.map_size_),
          start_(rhs.start_), finish_(rhs.finish_)
    {
        rhs.map_ = nullptr; rhs.map_size_ = 0;
        rhs.start_ = {}; rhs.finish_ = {};
    }

    ~deque() { teardown(); }

    deque& operator=(const deque& rhs)
    {
        if (this != &rhs) { deque tmp(rhs); swap(tmp); }
        return *this;
    }

    deque& operator=(deque&& rhs) noexcept
    {
        if (this != &rhs) {
            teardown();
            map_ = rhs.map_; map_size_ = rhs.map_size_;
            start_ = rhs.start_; finish_ = rhs.finish_;
            rhs.map_ = nullptr; rhs.map_size_ = 0;
            rhs.start_ = {}; rhs.finish_ = {};
        }
        return *this;
    }

    deque& operator=(std::initializer_list<T> il)
    {
        deque tmp(il); swap(tmp); return *this;
    }

    // -- Capacity ---------------------------------------------------------

    [[nodiscard]] bool      empty() const noexcept { return start_.cur_ == finish_.cur_; }
    [[nodiscard]] size_type size()  const noexcept { return static_cast<size_type>(finish_ - start_); }

    // -- Element access ---------------------------------------------------

    reference       operator[](size_type i)       noexcept { return start_[static_cast<difference_type>(i)]; }
    const_reference operator[](size_type i) const noexcept { return const_iterator(start_)[static_cast<difference_type>(i)]; }

    reference at(size_type i)
    {
        if (i >= size()) throw std::out_of_range("hpc::containers::deque::at");
        return (*this)[i];
    }
    const_reference at(size_type i) const
    {
        if (i >= size()) throw std::out_of_range("hpc::containers::deque::at");
        return (*this)[i];
    }

    reference       front()       noexcept { return *start_.cur_; }
    const_reference front() const noexcept { return *start_.cur_; }
    reference       back()        noexcept { auto t = finish_; --t; return *t; }
    const_reference back()  const noexcept { const_iterator t = finish_; --t; return *t; }

    // -- Iterators --------------------------------------------------------

    iterator       begin()        noexcept { return start_; }
    const_iterator begin()  const noexcept { return start_; }
    const_iterator cbegin() const noexcept { return start_; }
    iterator       end()          noexcept { return finish_; }
    const_iterator end()    const noexcept { return finish_; }
    const_iterator cend()   const noexcept { return finish_; }

    reverse_iterator       rbegin()        noexcept { return reverse_iterator(end()); }
    const_reverse_iterator rbegin()  const noexcept { return const_reverse_iterator(end()); }
    const_reverse_iterator crbegin() const noexcept { return const_reverse_iterator(end()); }
    reverse_iterator       rend()          noexcept { return reverse_iterator(begin()); }
    const_reverse_iterator rend()    const noexcept { return const_reverse_iterator(begin()); }
    const_reverse_iterator crend()   const noexcept { return const_reverse_iterator(begin()); }

    // -- Modifiers (back) -------------------------------------------------

    template <class... Args>
    reference emplace_back(Args&&... args)
    {
        // Pointer difference (not `last_ - 1`) stays well-defined when unallocated.
        if (finish_.last_ - finish_.cur_ > 1) {
            T* p = std::construct_at(finish_.cur_, std::forward<Args>(args)...);
            ++finish_.cur_;
            return *p;
        }
        return emplace_back_aux(std::forward<Args>(args)...);
    }

    void push_back(const T& value) { emplace_back(value); }
    void push_back(T&& value) { emplace_back(std::move(value)); }

    void pop_back() noexcept
    {
        if (finish_.cur_ != finish_.first_) {
            --finish_.cur_;
            destroy(finish_.cur_);
        } else {
            pop_back_aux();
        }
    }

    // -- Modifiers (front) ------------------------------------------------

    template <class... Args>
    reference emplace_front(Args&&... args)
    {
        if (start_.cur_ != start_.first_) {
            T* p = std::construct_at(start_.cur_ - 1, std::forward<Args>(args)...);
            --start_.cur_;
            return *p;
        }
        return emplace_front_aux(std::forward<Args>(args)...);
    }

    void push_front(const T& value) { emplace_front(value); }
    void push_front(T&& value) { emplace_front(std::move(value)); }

    void pop_front() noexcept
    {
        if (start_.cur_ != start_.last_ - 1) {
            destroy(start_.cur_);
            ++start_.cur_;
        } else {
            pop_front_aux();
        }
    }

    // -- General modifiers ------------------------------------------------

    void clear() noexcept
    {
        if (!map_) return;
        // Destroy elements and free inner chunks (keep first chunk).
        for (T** node = start_.node_ + 1; node < finish_.node_; ++node) {
            destroy_range(*node, *node + chunk_elems);
            free_chunk(*node);
        }
        if (start_.node_ != finish_.node_) {
            destroy_range(start_.cur_, start_.last_);
            destroy_range(finish_.first_, finish_.cur_);
            free_chunk(*finish_.node_);
        } else {
            destroy_range(start_.cur_, finish_.cur_);
        }
        finish_.set_node(start_.node_);
        finish_.cur_ = start_.cur_ = start_.first_;
    }

    void resize(size_type count)
    {
        auto s = size();
        if (count < s) {
            for (size_type i = count; i < s; ++i) pop_back();
        } else {
            for (size_type i = s; i < count; ++i) emplace_back();
        }
    }

    void resize(size_type count, const T& value)
    {
        auto s = size();
        if (count < s) {
            for (size_type i = count; i < s; ++i) pop_back();
        } else {
            for (size_type i = s; i < count; ++i) push_back(value);
        }
    }

    void swap(deque& o) noexcept
    {
        std::swap(map_, o.map_);   std::swap(map_size_, o.map_size_);
        std::swap(start_, o.start_); std::swap(finish_, o.finish_);
    }

    // -- Comparison -------------------------------------------------------

    friend bool operator==(const deque& a, const deque& b)
    {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
    }

private:
    T**       map_      = nullptr;
    size_type map_size_ = 0;
    iterator  start_;
    iterator  finish_;

    // -- Chunk allocation -------------------------------------------------

    static T* alloc_chunk() { return std::allocator<T>{}.allocate(chunk_elems); }
    static void free_chunk(T* p) noexcept { std::allocator<T>{}.deallocate(p, chunk_elems); }

    static T** alloc_map(size_type n) { return std::allocator<T*>{}.allocate(n); }
    void free_map() noexcept { std::allocator<T*>{}.deallocate(map_, map_size_); }

    template <class... Args>
    static void construct(T* p, Args&&... args)
    {
        std::construct_at(p, std::forward<Args>(args)...);
    }

    static void destroy(T* p) noexcept { std::destroy_at(p); }
    static void destroy_range(T* first, T* last) noexcept { std::destroy(first, last); }

    // -- Initialization ---------------------------------------------------

    // One chunk, centred in an 8-entry map for room to grow at both ends.
    void init_map()
    {
        map_size_ = 8;
        map_ = alloc_map(map_size_);
        std::fill(map_, map_ + map_size_, nullptr);
        T** node = map_ + map_size_ / 2;
        try {
            *node = alloc_chunk();
        } catch (...) {
            free_map();
            map_ = nullptr;
            throw;
        }
        start_.set_node(node);
        finish_.set_node(node);
        start_.cur_ = finish_.cur_ = start_.first_;
    }

    // -- Teardown ---------------------------------------------------------

    void teardown() noexcept
    {
        if (!map_) return;
        // Destroy live elements.
        for (T** node = start_.node_; node < finish_.node_; ++node)
            destroy_range(node == start_.node_ ? start_.cur_ : *node,
                          *node + chunk_elems);
        if (start_.node_ == finish_.node_)
            destroy_range(start_.cur_, finish_.cur_);
        else
            destroy_range(finish_.first_, finish_.cur_);
        // Free all allocated chunks.
        for (T** node = start_.node_; node <= finish_.node_; ++node)
            free_chunk(*node);
        free_map();
        map_ = nullptr;
    }

    // -- Push / pop aux ---------------------------------------------------

    // Slow path: the element goes into the last slot of the current chunk and
    // a fresh chunk is linked so that finish_.cur_ stays valid.
    template <class... Args>
    reference emplace_back_aux(Args&&... args)
    {
        if (map_ == nullptr) {
            init_map();
            return emplace_back(std::forward<Args>(args)...);
        }
        reserve_map_at_back();
        T* chunk = alloc_chunk();
        T* p;
        try {
            p = std::construct_at(finish_.cur_, std::forward<Args>(args)...);
        } catch (...) {
            free_chunk(chunk);
            throw;
        }
        *(finish_.node_ + 1) = chunk;
        finish_.set_node(finish_.node_ + 1);
        finish_.cur_ = finish_.first_;
        return *p;
    }

    template <class... Args>
    reference emplace_front_aux(Args&&... args)
    {
        if (map_ == nullptr) init_map();
        reserve_map_at_front();
        T* chunk = alloc_chunk();
        T* p;
        try {
            p = std::construct_at(chunk + (chunk_elems - 1), std::forward<Args>(args)...);
        } catch (...) {
            free_chunk(chunk);
            throw;
        }
        *(start_.node_ - 1) = chunk;
        start_.set_node(start_.node_ - 1);
        start_.cur_ = p;
        return *p;
    }

    void pop_back_aux() noexcept
    {
        free_chunk(*finish_.node_);
        finish_.set_node(finish_.node_ - 1);
        finish_.cur_ = finish_.last_ - 1;
        destroy(finish_.cur_);
    }

    void pop_front_aux() noexcept
    {
        destroy(start_.cur_);
        free_chunk(*start_.node_);
        start_.set_node(start_.node_ + 1);
        start_.cur_ = start_.first_;
    }

    // -- Map management ---------------------------------------------------

    void reserve_map_at_back(size_type nodes_to_add = 1)
    {
        if (nodes_to_add + 1 > map_size_
                - static_cast<size_type>(finish_.node_ - map_))
            reallocate_map(nodes_to_add, false);
    }

    void reserve_map_at_front(size_type nodes_to_add = 1)
    {
        if (nodes_to_add > static_cast<size_type>(start_.node_ - map_))
            reallocate_map(nodes_to_add, true);
    }

    void reallocate_map(size_type nodes_to_add, bool add_at_front)
    {
        size_type old_num = static_cast<size_type>(finish_.node_ - start_.node_) + 1;
        size_type new_num = old_num + nodes_to_add;

        T** new_nstart;
        if (map_size_ > 2 * new_num) {
            // Map is big enough; just shift entries to re-center.
            new_nstart = map_ + (map_size_ - new_num) / 2
                         + (add_at_front ? nodes_to_add : 0);
            if (new_nstart < start_.node_)
                std::copy(start_.node_, finish_.node_ + 1, new_nstart);
            else
                std::copy_backward(start_.node_, finish_.node_ + 1,
                                   new_nstart + old_num);
        } else {
            size_type new_map_size = map_size_
                                     + std::max(map_size_, nodes_to_add) + 2;
            T** new_map = alloc_map(new_map_size);
            new_nstart = new_map + (new_map_size - new_num) / 2
                         + (add_at_front ? nodes_to_add : 0);
            std::copy(start_.node_, finish_.node_ + 1, new_nstart);
            free_map();
            map_ = new_map;
            map_size_ = new_map_size;
        }
        start_.set_node(new_nstart);
        finish_.set_node(new_nstart + old_num - 1);
    }
};

} // namespace hpc::containers

