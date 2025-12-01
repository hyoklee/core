@CLAUDE.md

Implement a shared-memory vector. 
We will implement in two parts:
1. The shared memory (hshm::ipc) class
2. The view (hshm::ipc::view) class

The shared-memory class must be consistent across all processes.
It does not assume thread-local storage.
It should not implement fancy C++ features like operator overloads.
For operator overloads, ensure there is a corresponding method that mimics that behavior.
Each function that allocates / frees data should take as input a CtxAllocator
This should be nearly every function except get.

The view should implement inline wrappers around every function in the original vector class,
provide iterators, and relevant operator overloads.

Below describes some of the methods to implement.

```
namespace hshm::ipc {

template<typename T, typename AllocT>
class vector {
    size_t size_;
    size_t capacity_;
    OffsetPtr<T> data_;

    // Destructor does nothing. Explicit destruction using destroy();

    // Emplace at bakc
    emplace_back(CtxAllocator<AllocT> alloc, const T &value);
    // Emplace at index
    emplace(CtxAllocator<AllocT> alloc, T& value, int idx);
    // Replace the values in t his range with value
    replace(CtxAllocator<AllocT> alloc, T& value, int off, int count);
    // Get at index
    get(size_t idx);
    // Set index to value
    set(CtxAllocator<AllocT> alloc, size_t idx, T& value)
    // Erase this range of elements
    erase(CtxAllocator<AllocT> alloc, int off, int count);
    // Erase all elements
    clear(CtxAllocator<AllocT> alloc);
    // Destroy this vector
    destroy();
}

namespace view {

template<typename T, typename AllocT>
class vector {
    CtxAllocator ctx_alloc_;
    ::vector<T> &shm_vec_;
    bool owner_;  // Does this view own the vector?

    // Allocate new vector. Set owner to true internally
    template<typename ...Args>
    vector(CtxAllocator<AllocT> ctx_alloc, Args&& ...args);

    // Wrap around the existing vector
    vector(CtxAllocator<AllocT> ctx_alloc, ::vector<T> &shm_vec, bool owner = false); 

    // Destroy vector if this view owns the vector
    ~vector();

    // Wrap around emplace back
    emplace_back(const T &value) {
        shm_vec_.emplace_back();
    }
}
}

}
```

Concerns:
1. ``view::vector<vector<int>> x``, what will ``x[0][0]`` do?
Will it automatically give the view?
```
if HAS_VIEW(T) {
    return T::View();
}
```