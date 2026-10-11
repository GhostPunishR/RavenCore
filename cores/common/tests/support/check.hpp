#pragma once

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace ravenemu::testing {

void check(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string{message});
}

template <typename Exception, typename Function>
void expect_failure(Function&& function, std::string_view message) {
    try {
        std::forward<Function>(function)();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error(std::string{message});
}

} // namespace ravenemu::testing
