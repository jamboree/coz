#ifndef DEMO_GENERATOR_HPP
#define DEMO_GENERATOR_HPP

#ifndef COZ_COROUTINE_HPP // skip for compiler-explorer
#include <coz/coroutine.hpp>
#endif
#include <optional>

namespace demo {
    template<class T>
    struct generator_t {
        struct promise_type {
            explicit promise_type(generator_t) noexcept {}

            void finalize() noexcept {}

            template<class U = T>
            void yield_value(U&& u) {
                m_data.emplace(std::forward<U>(u));
            }

            void return_void() noexcept {}

            void unhandled_exception() { throw; }

            std::optional<T> m_data;
        };
    };

    template<class T, class Params, class State>
    struct [[nodiscard]] generator_impl {
        using promise_t = generator_t<T>::promise_type;

        explicit generator_impl(Params&& params) : m_coro(generator_t<T>{}) {
            m_coro.start(std::move(params));
        }

        ~generator_impl() {
            if (!m_coro.done())
                m_coro.destroy();
        }

        struct iterator {
            using value_type = T;
            using difference_type = std::ptrdiff_t;

            coz::coroutine<promise_t, Params, State>* m_coro = nullptr;

            bool operator==(std::default_sentinel_t) const noexcept {
                return m_coro->done();
            }

            iterator& operator++() {
                m_coro->resume();
                return *this;
            }

            void operator++(int) { m_coro->resume(); }

            T& operator*() const noexcept { return *m_coro->promise().m_data; }
        };

        iterator begin() { return iterator{&m_coro}; }

        std::default_sentinel_t end() { return {}; }

    private:
        coz::coroutine<promise_t, Params, State> m_coro;
    };

    template<class T>
    constexpr generator_t<T> generator{};
} // namespace demo

template<class T, class Params, class State>
struct coz::co_result<demo::generator_t<T>, Params, State> {
    COZ_NO_UNIQUE_ADDRESS demo::generator_t<T> m_init;
    Params m_params;

    demo::generator_impl<T, Params, State> get_return_object() {
        return demo::generator_impl<T, Params, State>(std::move(m_params));
    }
};

#endif
