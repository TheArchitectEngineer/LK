//
// Copyright (c) 2021 Travis Geiselbrecht
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT
//
// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#pragma once

#include <utility>

namespace lk {

// Runs a callable when it goes out of scope, unless cancelled first.
//
//   auto cleanup = lk::make_auto_call([&]() { release(thing); });
//   ...
//   cleanup.cancel();   // keep thing
template <typename T>
class auto_call {
public:
    constexpr explicit auto_call(T c) : call_(std::move(c)) {}
    ~auto_call() { call(); }

    auto_call(auto_call &&c) : call_(std::move(c.call_)), armed_(c.armed_) {
        c.cancel();
    }
    auto_call &operator=(auto_call &&c) {
        call();
        call_ = std::move(c.call_);
        armed_ = c.armed_;
        c.cancel();
        return *this;
    }

    auto_call(const auto_call &) = delete;
    auto_call &operator=(const auto_call &) = delete;

    // run the callable now, once; it will not run again on destruction
    void call() {
        bool armed = armed_;
        cancel();
        if (armed) {
            call_();
        }
    }
    void cancel() {
        armed_ = false;
    }

private:
    T call_;
    bool armed_ = true;
};

template <typename T>
inline auto_call<T> make_auto_call(T c) {
    return auto_call<T>(std::move(c));
}

} // namespace lk
