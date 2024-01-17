#ifndef DEMO_GENERATOR_HPP
#define DEMO_GENERATOR_HPP

#ifndef INCLUDE_BY_GODBOLT
#include <coz/coroutine.hpp>
#endif
#include <optional>

namespace demo {
    template<class T>
    struct generator_promise {
        explicit generator_promise(
            coz::default_init<generator_promise>) noexcept {}

        void finalize() noexcept { m_data.reset(); }

        template<class U = T>
        void yield_value(U&& u) {
            m_data.emplace(std::forward<U>(u));
        }

        void return_void() noexcept {}

        void unhandled_exception() { throw; }

        std::optional<T> m_data;
    };

    template<class T, class Params, class State>
    struct [[nodiscard]] generator_impl {
        using promise = generator_promise<T>;

        explicit generator_impl(Params&& params)
            : m_coro(coz::default_init<promise>{}),
              m_params(std::move(params)) {}

        ~generator_impl() {
            if (!m_coro.done())
                m_coro.destroy();
        }

        struct iterator {
            using value_type = T;
            using difference_type = std::ptrdiff_t;

            coz::coroutine<promise, Params, State>* m_coro = nullptr;

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

        iterator begin() {
            m_coro.done() ? m_coro.start(std::move(m_params)) : m_coro.resume();
            return iterator{&m_coro};
        }

        std::default_sentinel_t end() { return {}; }

    private:
        coz::coroutine<promise, Params, State> m_coro;
        Params m_params;
    };

    template<class T>
    constexpr coz::default_init<generator_promise<T>> generator{};
} // namespace demo

namespace coz {
    template<class T, class Params, class State>
    struct co_result<default_init<demo::generator_promise<T>>,
                                   Params, State> {
        COZ_NO_UNIQUE_ADDRESS default_init<demo::generator_promise<T>> m_init;
        Params m_params;

        demo::generator_impl<T, Params, State> get_return_object() {
            return demo::generator_impl<T, Params, State>(std::move(m_params));
        }
    };
} // namespace coz

#endif
