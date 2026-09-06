/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

// lk::function_ref<R(Args...)>: a non-owning reference to a callable. Two words, the
// callable's address and a thunk, trivially copyable, so it passes in registers and costs
// what two pointer arguments cost. It is the parameter type for a callback the callee only
// uses before it returns, a visitor being the usual case:
//
//   status_t for_every_device(lk::function_ref<status_t(device *)> fn);
//
//   for_every_device([&](device *d) { count++; return NO_ERROR; });
//
// Nothing is copied, so there is no size limit on the callable, but the reference is only
// good for as long as the callable lives: a lambda written in the argument list lives to the
// end of that statement. Never keep a function_ref past the call it was passed to; store an
// lk::function instead.

#include <assert.h>
#include <stdint.h>
#include <type_traits>
#include <utility>

namespace lk {

template <typename Signature>
class function_ref;

template <typename R, typename... Args>
class function_ref<R(Args...)> {
public:
    using result_type = R;

    constexpr function_ref() = default;

    // from any callable, including a plain function
    template <typename C, typename D = std::remove_reference_t<C>,
              typename = std::enable_if_t<!std::is_same_v<std::decay_t<C>, function_ref>>,
              typename = std::enable_if_t<
                  std::is_void_v<R> ||
                  std::is_convertible_v<decltype(std::declval<D &>()(std::declval<Args>()...)), R>>>
    function_ref(C &&callable) noexcept
        : callable_(reinterpret_cast<intptr_t>(&callable)), callback_(&invoke<D>) {}

    explicit operator bool() const { return callback_ != nullptr; }

    R operator()(Args... args) const {
        DEBUG_ASSERT(callback_ != nullptr);
        return callback_(callable_, std::forward<Args>(args)...);
    }

private:
    template <typename D>
    static R invoke(intptr_t callable, Args... args) {
        D &c = *reinterpret_cast<D *>(callable);
        if constexpr (std::is_void_v<R>) {
            c(std::forward<Args>(args)...);
        } else {
            return c(std::forward<Args>(args)...);
        }
    }

    intptr_t callable_ = 0;
    R (*callback_)(intptr_t, Args...) = nullptr;
};

} // namespace lk
