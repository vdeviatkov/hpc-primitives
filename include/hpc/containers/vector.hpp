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

// Contiguous growable array with the std::vector interface, minus allocator
// support and the bool specialisation.
//
// Data layout: data_ (heap buffer from std::allocator), size_ (constructed
// elements) and cap_ (allocated slots). Slots [size_, cap_) are raw memory.
// Iterators are raw pointers.
//
// How it works
//   - push_back / emplace_back: if size_ < cap_, construct at data_[size_]
//     and ++size_. Otherwise grow; capacity doubles, starting at 4 elements.
//   - Growth constructs the new element in the *new* buffer first, then
//     relocates the old elements and destroys and frees the old buffer.
//     Building the new element first makes v.push_back(v[0]) safe, because
//     the argument is still alive while it is read.
//   - Relocation uses std::uninitialized_move, which becomes memmove for
//     trivially copyable T. If T's move constructor can throw and T is
//     copyable, elements are copied instead (std::move_if_noexcept), so a
//     failed growth leaves the vector unchanged (the strong guarantee).
//   - insert in the middle: build the value in a temporary first, since it may
//     alias an element, and grow if needed. Move-construct the last element
//     one slot to the right, std::move_backward the rest, then move-assign the
//     temporary into the gap. erase shifts the tail left with std::move and
//     destroys the leftover end.
//   - Compared with libc++: libc++ relocates types it marks trivially
//     relocatable (e.g. std::string) with memcpy. This vector moves them one
//     by one, so vector<std::string> grows about 2x slower than libc++'s.
template <class T>
class vector {
public:
    using value_type             = T;
    using size_type              = std::size_t;
    using difference_type        = std::ptrdiff_t;
    using reference              = T&;
    using const_reference        = const T&;
    using pointer                = T*;
    using const_pointer          = const T*;
    using iterator               = T*;
    using const_iterator         = const T*;
    using reverse_iterator       = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    vector() noexcept = default;

    explicit vector(size_type count) { resize(count); }
    vector(size_type count, const T& value) { resize(count, value); }
    vector(std::initializer_list<T> il) { assign(il.begin(), il.end()); }

    template <std::input_iterator It>
    vector(It first, It last) { assign(first, last); }

    vector(const vector& rhs) { assign(rhs.begin(), rhs.end()); }

    vector(vector&& rhs) noexcept
        : data_(std::exchange(rhs.data_, nullptr))
        , size_(std::exchange(rhs.size_, 0))
        , cap_(std::exchange(rhs.cap_, 0))
    {}

    ~vector()
    {
        std::destroy_n(data_, size_);
        deallocate(data_, cap_);
    }

    vector& operator=(const vector& rhs)
    {
        if (this != &rhs) assign(rhs.begin(), rhs.end());
        return *this;
    }

    vector& operator=(vector&& rhs) noexcept
    {
        vector tmp(std::move(rhs));
        swap(tmp);
        return *this;
    }

    vector& operator=(std::initializer_list<T> il)
    {
        assign(il.begin(), il.end());
        return *this;
    }

    // -- Element access ------------------------------------------------------

    reference       operator[](size_type i) noexcept { return data_[i]; }
    const_reference operator[](size_type i) const noexcept { return data_[i]; }

    reference at(size_type i)
    {
        if (i >= size_) throw std::out_of_range("hpc::containers::vector::at");
        return data_[i];
    }

    const_reference at(size_type i) const
    {
        if (i >= size_) throw std::out_of_range("hpc::containers::vector::at");
        return data_[i];
    }

    reference       front() noexcept { return data_[0]; }
    const_reference front() const noexcept { return data_[0]; }
    reference       back() noexcept { return data_[size_ - 1]; }
    const_reference back() const noexcept { return data_[size_ - 1]; }
    pointer         data() noexcept { return data_; }
    const_pointer   data() const noexcept { return data_; }

    // -- Iterators -----------------------------------------------------------

    iterator               begin() noexcept { return data_; }
    const_iterator         begin() const noexcept { return data_; }
    const_iterator         cbegin() const noexcept { return data_; }
    iterator               end() noexcept { return data_ + size_; }
    const_iterator         end() const noexcept { return data_ + size_; }
    const_iterator         cend() const noexcept { return data_ + size_; }
    reverse_iterator       rbegin() noexcept { return reverse_iterator(end()); }
    const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator(end()); }
    const_reverse_iterator crbegin() const noexcept { return const_reverse_iterator(end()); }
    reverse_iterator       rend() noexcept { return reverse_iterator(begin()); }
    const_reverse_iterator rend() const noexcept { return const_reverse_iterator(begin()); }
    const_reverse_iterator crend() const noexcept { return const_reverse_iterator(begin()); }

    // -- Capacity ------------------------------------------------------------

    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    size_type          size() const noexcept { return size_; }
    size_type          capacity() const noexcept { return cap_; }

    void reserve(size_type n)
    {
        if (n > cap_) reallocate(n);
    }

    void shrink_to_fit()
    {
        if (size_ < cap_) reallocate(size_);
    }

    // -- Modifiers -----------------------------------------------------------

    void clear() noexcept
    {
        std::destroy_n(data_, size_);
        size_ = 0;
    }

    template <class... Args>
    reference emplace_back(Args&&... args)
    {
        if (size_ == cap_) return emplace_back_grow(std::forward<Args>(args)...);
        T* p = std::construct_at(data_ + size_, std::forward<Args>(args)...);
        ++size_;
        return *p;
    }

    void push_back(const T& v) { emplace_back(v); }
    void push_back(T&& v) { emplace_back(std::move(v)); }

    void pop_back() noexcept { std::destroy_at(data_ + --size_); }

    template <class... Args>
    iterator emplace(const_iterator pos, Args&&... args)
    {
        const auto off = pos - data_;
        if (pos == end()) {
            emplace_back(std::forward<Args>(args)...);
        } else {
            T tmp(std::forward<Args>(args)...); // args may alias an element
            if (size_ == cap_) reallocate(grown_capacity());
            T* p = data_ + off;
            std::construct_at(data_ + size_, std::move(data_[size_ - 1]));
            ++size_;
            std::move_backward(p, data_ + size_ - 2, data_ + size_ - 1);
            *p = std::move(tmp);
        }
        return data_ + off;
    }

    iterator insert(const_iterator pos, const T& v) { return emplace(pos, v); }
    iterator insert(const_iterator pos, T&& v) { return emplace(pos, std::move(v)); }

    iterator erase(const_iterator pos) { return erase(pos, pos + 1); }

    iterator erase(const_iterator first, const_iterator last)
    {
        T* f = data_ + (first - data_);
        T* l = data_ + (last - data_);
        if (f != l) {
            T* new_end = std::move(l, end(), f);
            std::destroy(new_end, end());
            size_ = static_cast<size_type>(new_end - data_);
        }
        return f;
    }

    void resize(size_type count)
    {
        if (count < size_) {
            std::destroy(data_ + count, end());
        } else if (count > size_) {
            reserve(count);
            std::uninitialized_value_construct(end(), data_ + count);
        }
        size_ = count;
    }

    void resize(size_type count, const T& value)
    {
        if (count < size_) {
            std::destroy(data_ + count, end());
        } else if (count > size_) {
            if (count > cap_) {
                vector tmp;
                tmp.reserve(count);
                tmp.assign(begin(), end());
                tmp.resize(count, value); // value may alias an element of *this
                swap(tmp);
                return;
            }
            std::uninitialized_fill(end(), data_ + count, value);
        }
        size_ = count;
    }

    void assign(size_type count, const T& value)
    {
        vector tmp;
        tmp.resize(count, value);
        swap(tmp);
    }

    template <std::input_iterator It>
    void assign(It first, It last)
    {
        clear();
        if constexpr (std::forward_iterator<It>) {
            reserve(static_cast<size_type>(std::distance(first, last)));
            std::uninitialized_copy(first, last, data_);
            size_ = static_cast<size_type>(std::distance(first, last));
        } else {
            for (; first != last; ++first) emplace_back(*first);
        }
    }

    void assign(std::initializer_list<T> il) { assign(il.begin(), il.end()); }

    void swap(vector& o) noexcept
    {
        std::swap(data_, o.data_);
        std::swap(size_, o.size_);
        std::swap(cap_, o.cap_);
    }

    // -- Comparison ----------------------------------------------------------

    friend bool operator==(const vector& a, const vector& b)
    {
        return std::equal(a.begin(), a.end(), b.begin(), b.end());
    }

    friend bool operator<(const vector& a, const vector& b)
    {
        return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end());
    }

    friend bool operator>(const vector& a, const vector& b) { return b < a; }
    friend bool operator<=(const vector& a, const vector& b) { return !(b < a); }
    friend bool operator>=(const vector& a, const vector& b) { return !(a < b); }

private:
    static T* allocate(size_type n) { return n ? std::allocator<T>{}.allocate(n) : nullptr; }

    static void deallocate(T* p, size_type n) noexcept
    {
        if (p != nullptr) std::allocator<T>{}.deallocate(p, n);
    }

    size_type grown_capacity() const noexcept { return cap_ ? cap_ * 2 : 4; }

    // Moves (or copies, if moving could throw) [src, src + n) into raw `dst`.
    static void relocate(T* src, size_type n, T* dst)
    {
        if constexpr (std::is_nothrow_move_constructible_v<T> || !std::is_copy_constructible_v<T>)
            std::uninitialized_move_n(src, n, dst);
        else
            std::uninitialized_copy_n(src, n, dst);
    }

    void reallocate(size_type new_cap)
    {
        T* buf = allocate(new_cap);
        try {
            relocate(data_, size_, buf);
        } catch (...) {
            deallocate(buf, new_cap);
            throw;
        }
        std::destroy_n(data_, size_);
        deallocate(data_, cap_);
        data_ = buf;
        cap_  = new_cap;
    }

    // Constructs the new element in the new buffer before relocating, so
    // `args` may refer to an existing element.
    template <class... Args>
    reference emplace_back_grow(Args&&... args)
    {
        const size_type new_cap = grown_capacity();
        T* buf = allocate(new_cap);
        T* p   = buf + size_;
        try {
            std::construct_at(p, std::forward<Args>(args)...);
        } catch (...) {
            deallocate(buf, new_cap);
            throw;
        }
        try {
            relocate(data_, size_, buf);
        } catch (...) {
            std::destroy_at(p);
            deallocate(buf, new_cap);
            throw;
        }
        std::destroy_n(data_, size_);
        deallocate(data_, cap_);
        data_ = buf;
        cap_  = new_cap;
        ++size_;
        return *p;
    }

    T*        data_{nullptr};
    size_type size_{0};
    size_type cap_{0};
};

} // namespace hpc::containers
