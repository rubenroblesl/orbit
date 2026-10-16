// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Microsoft::WRL::ComPtr over the compat IUnknown.

#include <windows.h>

#include <utility>

namespace Microsoft::WRL {

template <typename T>
class ComPtr;

namespace Details {
// What &ComPtr yields, as in WRL: converts to ComPtr<T>*, T** (releasing first) and void**.
template <typename T>
class ComPtrRef {
public:
    explicit ComPtrRef(ComPtr<T>* pointer) noexcept : pointer_(pointer) {}
    operator ComPtr<T>*() const noexcept { return pointer_; }
    operator T**() const noexcept { return pointer_->ReleaseAndGetAddressOf(); }
    operator void**() const noexcept { return reinterpret_cast<void**>(pointer_->ReleaseAndGetAddressOf()); }
    T* operator*() const noexcept { return pointer_->Get(); }
    T** GetAddressOf() const noexcept { return pointer_->GetAddressOf(); }
    T** ReleaseAndGetAddressOf() const noexcept { return pointer_->ReleaseAndGetAddressOf(); }

private:
    ComPtr<T>* pointer_;
};
}  // namespace Details

template <typename T>
class ComPtr {
public:
    using InterfaceType = T;

    ComPtr() noexcept = default;
    ComPtr(std::nullptr_t) noexcept {}
    template <typename U>
    ComPtr(U* pointer) noexcept : pointer_(pointer) { add_ref(); }
    ComPtr(const ComPtr& other) noexcept : pointer_(other.pointer_) { add_ref(); }
    template <typename U>
    ComPtr(const ComPtr<U>& other) noexcept : pointer_(other.Get()) { add_ref(); }
    ComPtr(ComPtr&& other) noexcept : pointer_(std::exchange(other.pointer_, nullptr)) {}
    ~ComPtr() { release(); }

    ComPtr& operator=(const ComPtr& other) noexcept {
        if (pointer_ != other.pointer_) {
            ComPtr(other).Swap(*this);
        }
        return *this;
    }
    ComPtr& operator=(ComPtr&& other) noexcept {
        ComPtr(std::move(other)).Swap(*this);
        return *this;
    }
    ComPtr& operator=(T* pointer) noexcept {
        if (pointer_ != pointer) {
            ComPtr(pointer).Swap(*this);
        }
        return *this;
    }
    ComPtr& operator=(std::nullptr_t) noexcept {
        Reset();
        return *this;
    }

    T* Get() const noexcept { return pointer_; }
    T* operator->() const noexcept { return pointer_; }
    explicit operator bool() const noexcept { return pointer_ != nullptr; }
    T** GetAddressOf() noexcept { return &pointer_; }
    T* const* GetAddressOf() const noexcept { return &pointer_; }
    T** ReleaseAndGetAddressOf() noexcept {
        release();
        return &pointer_;
    }
    Details::ComPtrRef<T> operator&() noexcept { return Details::ComPtrRef<T>(this); }
    T* Detach() noexcept { return std::exchange(pointer_, nullptr); }
    void Attach(T* pointer) noexcept {
        release();
        pointer_ = pointer;
    }
    ULONG Reset() noexcept { return release(); }
    void Swap(ComPtr& other) noexcept { std::swap(pointer_, other.pointer_); }
    template <typename U>
    HRESULT As(Details::ComPtrRef<U> other) const noexcept {
        return As(static_cast<ComPtr<U>*>(other));
    }
    template <typename U>
    HRESULT As(ComPtr<U>* other) const noexcept {
        return pointer_->QueryInterface(__uuidof(U),
            reinterpret_cast<void**>(other->ReleaseAndGetAddressOf()));
    }
    HRESULT CopyTo(T** other) const noexcept {
        add_ref();
        *other = pointer_;
        return S_OK;
    }

    friend bool operator==(const ComPtr& pointer, std::nullptr_t) noexcept { return pointer.pointer_ == nullptr; }
    friend bool operator!=(const ComPtr& pointer, std::nullptr_t) noexcept { return pointer.pointer_ != nullptr; }

private:
    void add_ref() const noexcept {
        if (pointer_ != nullptr) pointer_->AddRef();
    }
    ULONG release() noexcept {
        ULONG count = 0;
        if (T* const pointer = std::exchange(pointer_, nullptr)) {
            count = pointer->Release();
        }
        return count;
    }

    T* pointer_ = nullptr;
};

}  // namespace Microsoft::WRL

template <typename T>
void** IID_PPV_ARGS_Helper(Microsoft::WRL::Details::ComPtrRef<T> pointer) {
    return static_cast<void**>(pointer);
}
