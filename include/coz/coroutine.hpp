/*//////////////////////////////////////////////////////////////////////////////
    Copyright (c) 2024 Jamboree

    Distributed under the Boost Software License, Version 1.0. (See accompanying
    file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//////////////////////////////////////////////////////////////////////////////*/
#ifndef COZ_COROUTINE_HPP
#define COZ_COROUTINE_HPP

#include <utility>
#include <cstdint>
#include <cassert>
#include <exception>
#include <algorithm>
#include <boost/config.hpp>
#include <boost/preprocessor/seq/for_each.hpp>
#include <boost/preprocessor/tuple/to_seq.hpp>
#include <boost/preprocessor/facilities/is_empty_variadic.hpp>

#if defined(BOOST_MSVC) // MSVC
#define COZ_NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
#else
#define COZ_NO_UNIQUE_ADDRESS [[no_unique_address]]
#endif

namespace coz::detail {
    enum : unsigned { SENTINEL = ~0u };

    struct void_t {};

    struct devoider {
        template<class T>
        struct wrap {
            T&& m_Value;
            T&& operator,(void_t) { return static_cast<T&&>(m_Value); }
        };

        template<class T>
        wrap<T> operator,(T&& val) const {
            return {static_cast<T&&>(val)};
        }
    };

    template<class T>
    struct lvref_wrapper {
        T* m_ptr;
    };

    struct lvrefer {
        template<class Expr>
        lvref_wrapper<Expr> operator,(Expr& expr) const {
            return {&expr};
        }
    };

    template<class T>
    T norvref(T&&);

    template<class T>
    inline T* unwrap_ptr(T* p) {
        return p;
    }

    template<class T>
    inline T* unwrap_ptr(lvref_wrapper<T>* p) {
        return p->m_ptr;
    }

    template<class T>
    inline T&& deref(T* p) {
        return static_cast<T&&>(*p);
    }

    template<class T>
    inline T& deref(lvref_wrapper<T>* p) {
        return *p->m_ptr;
    }

    template<class T>
    struct auto_reset {
        T* m_ptr;
        ~auto_reset() { m_ptr->~T(); }
        auto operator->() const { return unwrap_ptr(m_ptr); }
    };

    template<class T>
    struct manual_lifetime {
        alignas(T) std::uint8_t m_data[sizeof(T)];

        T& get() noexcept { return *reinterpret_cast<T*>(&m_data); }

        const T& get() const noexcept {
            return *reinterpret_cast<const T*>(&m_data);
        }

        template<class... A>
        void emplace(A&&... a) {
            new (&m_data) T(std::forward<A>(a)...);
        }

        T release() {
            T ret(std::move(get()));
            destroy();
            return ret;
        }

        void destroy() noexcept { get().~T(); }
    };

    struct size_align {
        std::size_t size;
        std::size_t align;

        friend constexpr size_align unite(size_align a, size_align b) {
            return {(std::max)(a.size, b.size), (std::max)(a.align, b.align)};
        }
    };

    template<class T>
    inline constexpr size_align size_align_of{sizeof(T), alignof(T)};

    // Stateful Metaprogramming trick described below:
    // https://mc-deltat.github.io/articles/stateful-metaprogramming-cpp20
    // -------------------------------------------------------------------------
    template<unsigned N, auto C>
    struct smp_state {
        static constexpr unsigned index = N;
        static constexpr auto value = C;
    };

    template<class Domain, unsigned N>
    struct smp_bump {
        // Suppress nasty GCC warning.
#if defined(BOOST_GCC)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-template-friend"
#endif
        friend auto observe(smp_bump);
#if defined(BOOST_GCC)
#pragma GCC diagnostic pop
#endif
    };

    template<auto C>
    struct smp_const {
        static constexpr auto value = C;
    };

    template<class Domain, unsigned N, auto C>
    struct smp_store_state {
        friend auto observe(smp_bump<Domain, N>) { return smp_const<C>{}; }

        static constexpr smp_state<N, C> state{};
    };

    template<class Domain, auto Tick, unsigned N = 0>
    [[nodiscard]] consteval auto smp_load_state() {
        constexpr bool observed = requires { observe(smp_bump<Domain, N>{}); };
        if constexpr (observed) {
            return smp_load_state<Domain, Tick, N + 1>();
        }
        else {
            return smp_state<N - 1, decltype(observe(
                                        smp_bump<Domain, N - 1>{}))::value>{};
        }
    }

    template<class Domain, auto C>
    constexpr auto smp_init_state() {
        return smp_store_state<Domain, 0, C>::state;
    }
    // -------------------------------------------------------------------------

    // Program counters.
    struct coro_state {
        unsigned m_next = SENTINEL;
        unsigned m_eh = SENTINEL;
    };

    // Common ABI as described at:
    // https://devblogs.microsoft.com/oldnewthing/20220103-00/?p=106109
    struct coro_proto {
        void (&m_resume)(coro_proto*);
        void (&m_destroy)(coro_proto*);
    };

    // We place 'state' before 'proto' to optimize the access, as 'state' is
    // accessed more directly, while 'proto' is for indirect access.
    struct coro_base : coro_state, coro_proto {};

    template<class Promise, class Expr>
    inline auto awt_trans(Promise* p, Expr&& expr)
        -> decltype(p->await_transform(std::forward<Expr>(expr))) {
        return p->await_transform(std::forward<Expr>(expr));
    }

    template<class Expr>
    inline lvref_wrapper<Expr> awt_trans(const void*, Expr& expr) {
        return {&expr};
    }

    template<class Promise>
    struct coro_ctx : coro_base, Promise {
        // Promote these names to avoid confliction with Promise.
        using coro_base::m_eh;
        using coro_base::m_next;

        // Use comma to transform the satisfied expr while leaving the
        // unsatisfied expr untouched.
        template<class Expr>
        auto operator,(Expr&& expr)
                          -> decltype(awt_trans(this,
                                                std::forward<Expr>(expr))) {
            return awt_trans(this, std::forward<Expr>(expr));
        }
    };

    template<class State, class Promise>
    struct finalizer {
        State* m_state;
        Promise* m_promise;

        ~finalizer() {
            m_state->~State();
            m_promise->finalize();
        }
    };

    template<size_align mem, class State>
    struct body {
        alignas(mem.align) char m_mem_tmp[mem.size];
        manual_lifetime<State> m_state;

        template<class Promise>
        void invoke(coro_ctx<Promise>* ctx) {
            m_state.get()(ctx, m_mem_tmp);
        }
    };

    template<class State>
    struct body<size_align{}, State> {
        manual_lifetime<State> m_state;

        template<class Promise>
        void invoke(coro_ctx<Promise>* ctx) {
            m_state.get()(ctx, nullptr);
        }
    };
} // namespace coz::detail

namespace coz {
    template<class Promise = void>
    struct coroutine_handle;

    template<>
    struct coroutine_handle<void> {
        constexpr coroutine_handle() noexcept = default;
        constexpr coroutine_handle(std::nullptr_t) noexcept {}

        constexpr void* address() const noexcept { return m_ptr; }

        static constexpr coroutine_handle from_address(void* addr) noexcept {
            return static_cast<detail::coro_proto*>(addr);
        }

        constexpr explicit operator bool() const noexcept {
            return m_ptr != nullptr;
        }

        bool done() const noexcept {
            return static_cast<detail::coro_base*>(m_ptr)->m_next ==
                   detail::SENTINEL;
        }

        void operator()() const { m_ptr->m_resume(m_ptr); }

        void resume() const { m_ptr->m_resume(m_ptr); }

        void destroy() const noexcept { m_ptr->m_destroy(m_ptr); }

    protected:
        constexpr coroutine_handle(detail::coro_proto* p) noexcept : m_ptr(p) {}

        detail::coro_proto* m_ptr = nullptr;
    };

    template<class Promise>
    struct coroutine_handle : coroutine_handle<void> {
        using coroutine_handle<void>::coroutine_handle;

        static constexpr coroutine_handle from_address(void* addr) noexcept {
            return static_cast<detail::coro_proto*>(addr);
        }

        Promise& promise() const noexcept {
            return *static_cast<detail::coro_ctx<Promise>*>(m_ptr);
        }

        static coroutine_handle from_promise(Promise& promise) noexcept {
            return static_cast<detail::coro_ctx<Promise>*>(&promise);
        }
    };

    template<class Promise, class Params, class State>
    struct coroutine : private detail::coro_ctx<Promise> {
        template<class Init>
        explicit coroutine(Init&& init)
            : detail::coro_ctx<Promise>{{{}, {resume_impl, destroy_impl}},
                                        Promise(std::forward<Init>(init))} {}

        coroutine(const coroutine&) = delete;
        coroutine& operator=(const coroutine&) = delete;

        coroutine_handle<Promise> handle() noexcept { return this; }

        Promise& promise() noexcept { return *this; }

        const Promise& promise() const noexcept { return *this; }

        bool done() const noexcept { return this->m_next == detail::SENTINEL; }

        void start(Params&& params) {
            m_body.m_state.emplace(std::move(params));
            this->m_next = 0;
            m_body.invoke(this);
        }

        void resume() { m_body.invoke(this); }

        void destroy() {
            assert(this->m_next != detail::SENTINEL);
            ++this->m_next;
            m_body.invoke(this);
        }

    private:
        static void resume_impl(detail::coro_proto* base) {
            static_cast<coroutine*>(base)->resume();
        }

        static void destroy_impl(detail::coro_proto* base) {
            static_cast<coroutine*>(base)->destroy();
        }

        detail::body<decltype(std::declval<State>()(nullptr, nullptr))::value,
                     State>
            m_body;
    };

    template<class Init, class Params, class State>
    struct co_result;

    template<class Promise>
    struct default_init {
        using promise_type = Promise;

        constexpr default_init operator()() const { return *this; }
    };
} // namespace coz

namespace coz::detail {
    template<class Promise>
    inline auto implicit_return(Promise* p) -> decltype(p->return_void()) {
        p->return_void();
    }

    [[noreturn]] inline void implicit_return(void*) {
        assert(!"missing return statement");
    }

    template<class Promise>
    inline auto explicit_return(Promise* p, void_t)
        -> decltype(p->return_void()) {
        p->return_void();
    }

    template<class Promise, class T>
    inline void explicit_return(Promise* p, T&& value) {
        p->return_value(std::forward<T>(value));
    }

    template<class Expr, class Promise>
    inline bool suspend(Expr* p, coroutine_handle<Promise> coro) {
        using R = decltype(p->await_suspend(coro));
        if constexpr (std::is_same_v<R, bool>) {
            return p->await_suspend(coro);
        } else {
            static_assert(std::is_same_v<R, void>);
            p->await_suspend(coro);
            return true;
        }
    }

    template<class Expr, class Promise>
    inline bool try_suspend(Expr* p_, coro_ctx<Promise>* ctx, unsigned ip) {
        const auto p = unwrap_ptr(p_);
        if (p->await_ready())
            return false;
        ctx->m_next = ip;
        return suspend(p, coroutine_handle<Promise>::from_address(
                              static_cast<coro_proto*>(ctx)));
    }

    template<class Domain, auto Tick>
    static consteval size_align get_size_align() {
        return smp_load_state<Domain, Tick>().value;
    }

    template<size_align Val, class Domain, auto Tick>
    static consteval auto update_size_align_impl() {
        using curr = decltype(smp_load_state<Domain, Tick>());
        return smp_store_state<Domain, curr::index + 1,
                               unite(Val, curr::value)>::state;
    }

    template<class Domain, class T, auto Tick>
    constexpr auto update_size_align() {
        return update_size_align_impl<size_align_of<T>, Domain, Tick>();
    };

    template<class T>
    concept HasReturnObject = requires(T* p) {
        p->get_return_object();
    };

    template<HasReturnObject T>
    inline auto get_return_object(T* p) {
        return p->get_return_object();
    }

    void get_return_object(const void*); // undefined
} // namespace coz::detail

#define z_COZ_DEVOID(...) (_coz_::devoider{}, __VA_ARGS__, _coz_::void_t{})

#define z_COZ_TUPLE_FOR_EACH_IMPL(macro, t)                                    \
    BOOST_PP_SEQ_FOR_EACH(macro, ~, BOOST_PP_TUPLE_TO_SEQ(t))

#define z_COZ_TUPLE_FOR_EACH_EMPTY(macro, t)

#define z_COZ_TUPLE_FOR_EACH(t, macro)                                         \
    BOOST_PP_IIF(BOOST_PP_IS_EMPTY t, z_COZ_TUPLE_FOR_EACH_EMPTY,              \
                 z_COZ_TUPLE_FOR_EACH_IMPL)                                    \
    (macro, t)

#define z_COZ_DECL_PARAM_T(r, _, e) using BOOST_PP_CAT(e, _t) = decltype(e);
#define z_COZ_DECL_PARAM(r, _, e) typename _coz_params_t::BOOST_PP_CAT(e, _t) e;
#define z_COZ_FWD_PARAM(r, _, e) std::forward<decltype(e)>(e),

#define z_COZ_NEW_IP (__COUNTER__ - _coz_start)
#define z_COZ_NEW_EH [[unlikely]] case z_COZ_NEW_IP

// clang-format off
// Begin of the coroutine body.
#define COZ_BEG(init, args, ...)                                               \
    {                                                                          \
        namespace _coz_ = ::coz::detail;                                       \
        using _coz_init = std::decay_t<decltype(init)>;                        \
        using _coz_promise = _coz_init::promise_type;                          \
        struct _coz_params_t {                                                 \
            z_COZ_TUPLE_FOR_EACH(args, z_COZ_DECL_PARAM_T)                     \
        };                                                                     \
        struct _coz_params {                                                   \
            z_COZ_TUPLE_FOR_EACH(args, z_COZ_DECL_PARAM)                       \
        };                                                                     \
        struct _coz_state;                                                     \
        ::coz::co_result<_coz_init, _coz_params, _coz_state> _coz_result{      \
            init, _coz_params{z_COZ_TUPLE_FOR_EACH(args, z_COZ_FWD_PARAM)}};   \
        struct _coz_state : _coz_params {                                      \
            __VA_ARGS__                                                        \
            _coz_state(_coz_params&& params)                                   \
                : _coz_params(std::move(params)) {}                            \
            auto operator()(_coz_::coro_ctx<_coz_promise>* _coz_ctx,           \
                            void* _coz_mem_tmp) {                              \
                _coz_::smp_init_state<_coz_state, _coz_::size_align{}>();      \
                _coz_::manual_lifetime<std::exception_ptr> _coz_ex;            \
                enum : unsigned {                                              \
                    _coz_start = __COUNTER__,                                  \
                    _coz_curr_eh = _coz_::SENTINEL                             \
                };                                                             \
            _coz_retry:                                                        \
                try {                                                          \
                    switch (_coz_ctx->m_next) {                                \
                    case 0:

// End of the async body.
#define COZ_END                                                                \
                        _coz_ctx->m_next = _coz_::SENTINEL;                    \
                        _coz_::implicit_return(_coz_ctx);                      \
                    _coz_finalize:                                             \
                        _coz_::finalizer{this, _coz_ctx};                      \
                    }                                                          \
                } catch (...) {                                                \
                    _coz_ctx->m_next = _coz_ctx->m_eh;                         \
                    if (_coz_ctx->m_next != _coz_::SENTINEL) {                 \
                        _coz_ex.emplace(std::current_exception());             \
                        goto _coz_retry;                                       \
                    }                                                          \
                    _coz_::finalizer fin{this, _coz_ctx};                      \
                    _coz_ctx->unhandled_exception();                           \
                }                                                              \
            _coz_suspend:                                                      \
                return z_COZ_HIDE_MAGIC(                                       \
                    _coz_::smp_const<_coz_::get_size_align<                    \
                        _coz_state, _coz_::SENTINEL>()>{});                    \
            }                                                                  \
        };                                                                     \
        if constexpr (_coz_::HasReturnObject<decltype(_coz_result)>) {         \
            return _coz_::get_return_object(&_coz_result);                     \
        } else {                                                               \
            return _coz_result;                                                \
        }                                                                      \
    }

// TODO: Adopt https://wg21.link/p2806 when available.
#if defined(BOOST_GCC) | defined(BOOST_CLANG)
#define z_COZ_AWAIT_EXPR_BEG ({
#define z_COZ_AWAIT_EXPR_END                                                   \
    })
#else
#define z_COZ_AWAIT_EXPR_BEG do {
#define z_COZ_AWAIT_EXPR_END                                                   \
    }                                                                          \
    while (false)
#endif
#define z_COZ_AWAIT_EXPR_RET(e) e
// clang-format on

// Hide code that's problematic for Intellisense.
#ifdef __INTELLISENSE__
#define z_COZ_HIDE_MAGIC(...)
#else
#define z_COZ_HIDE_MAGIC(...) __VA_ARGS__
#endif

#define z_COZ_AWT(expr) (*_coz_ctx, expr)
#define z_COZ_TMP(expr) (_coz_::lvrefer{}, expr)

#define z_COZ_AWAIT_SUSPEND(expr)                                              \
    enum : unsigned { _coz_ip = z_COZ_NEW_IP };                                \
    z_COZ_HIDE_MAGIC(                                                          \
        _coz_::update_size_align<_coz_state, _coz_awt_t, _coz_ip>());          \
    if (_coz_::try_suspend(new (_coz_mem_tmp) _coz_awt_t{z_COZ_AWT(expr)},     \
                           _coz_ctx, _coz_ip)) {                               \
        goto _coz_suspend;                                                     \
    z_COZ_NEW_EH:                                                              \
        static_cast<_coz_awt_t*>(_coz_mem_tmp)->~_coz_awt_t();                 \
        goto _coz_finalize;                                                    \
    }                                                                          \
    case _coz_ip:

#define z_COZ_APPEND_ARGS0()
#define z_COZ_APPEND_ARGS1(...) , __VA_ARGS__
#define z_COZ_APPEND_ARGS(t)                                                   \
    BOOST_PP_IIF(BOOST_PP_IS_EMPTY t, z_COZ_APPEND_ARGS0, z_COZ_APPEND_ARGS1) t

#define z_COZ_AWAIT_STMT(ret, expr, args)                                      \
    do {                                                                       \
        using _coz_awt_t = decltype(_coz_::norvref(z_COZ_AWT(expr)));          \
        z_COZ_AWAIT_SUSPEND(expr) ret(_coz_::auto_reset {                      \
            static_cast<_coz_awt_t*>(_coz_mem_tmp)                             \
        } -> await_resume() z_COZ_APPEND_ARGS(args));                          \
    } while (false)

#define z_COZ_AWAIT_LET(init, expr, label)                                     \
    if (typedef decltype(z_COZ_AWT(expr)) _coz_awt_t; true) {                  \
        z_COZ_AWAIT_SUSPEND(expr) goto label;                                  \
    } else                                                                     \
    label:                                                                     \
        if (init =                                                             \
                _coz_::auto_reset { static_cast<_coz_awt_t*>(_coz_mem_tmp) }   \
                -> await_resume();                                             \
            false) {                                                           \
        } else

#define COZ_AWAIT(expr)                                                        \
    z_COZ_AWAIT_EXPR_BEG using _coz_awt_t =                                    \
        decltype(_coz_::norvref(z_COZ_AWT(expr)));                             \
    z_COZ_AWAIT_SUSPEND(expr) z_COZ_AWAIT_EXPR_RET(_coz_::auto_reset {         \
        static_cast<_coz_awt_t*>(_coz_mem_tmp)                                 \
    } -> await_resume());                                                      \
    z_COZ_AWAIT_EXPR_END

#define COZ_AWAIT_SET(var, expr) z_COZ_AWAIT_STMT(var =, expr, ())
#define COZ_AWAIT_APPLY(f, expr, ...) z_COZ_AWAIT_STMT(f, expr, (__VA_ARGS__))

#define COZ_AWAIT_LET(init, expr)                                              \
    z_COZ_AWAIT_LET(init, expr, BOOST_PP_CAT(_coz_L, __LINE__))

#define COZ_YIELD_LITE(expr)                                                   \
    do {                                                                       \
        enum : unsigned { _coz_ip = z_COZ_NEW_IP };                            \
        _coz_ctx->yield_value(expr);                                           \
        _coz_ctx->m_next = _coz_ip;                                            \
        goto _coz_suspend;                                                     \
    case _coz_ip:                                                              \
        break;                                                                 \
    z_COZ_NEW_EH:                                                              \
        goto _coz_finalize;                                                    \
    } while (false)

#define COZ_YIELD(expr)                                                        \
    do {                                                                       \
        using _coz_tmp_t = decltype(_coz_::norvref(z_COZ_TMP(expr)));          \
        enum : unsigned { _coz_ip = z_COZ_NEW_IP };                            \
        z_COZ_HIDE_MAGIC(                                                      \
            _coz_::update_size_align<_coz_state, _coz_tmp_t, _coz_ip>());      \
        _coz_ctx->yield_value(                                                 \
            _coz_::deref(new (_coz_mem_tmp) _coz_tmp_t{z_COZ_TMP(expr)}));     \
        _coz_ctx->m_next = _coz_ip;                                            \
        goto _coz_suspend;                                                     \
    case _coz_ip:                                                              \
        static_cast<_coz_tmp_t*>(_coz_mem_tmp)->~_coz_tmp_t();                 \
        break;                                                                 \
    z_COZ_NEW_EH:                                                              \
        static_cast<_coz_tmp_t*>(_coz_mem_tmp)->~_coz_tmp_t();                 \
        goto _coz_finalize;                                                    \
    } while (false)

#define z_COZ_RETURN0(t) _coz_ctx->return_void()
#define z_COZ_RETURN1(t) _coz_::explicit_return(_coz_ctx, z_COZ_DEVOID t)
#define z_COZ_RETURN(t)                                                        \
    BOOST_PP_IIF(BOOST_PP_IS_EMPTY t, z_COZ_RETURN0, z_COZ_RETURN1)(t)

#define COZ_RETURN(...)                                                        \
    do {                                                                       \
        _coz_ctx->m_next = _coz_::SENTINEL;                                    \
        z_COZ_RETURN((__VA_ARGS__));                                           \
        goto _coz_finalize;                                                    \
    } while (false)

#define COZ_TRY                                                                \
    if (enum                                                                   \
        : unsigned{_coz_prev_eh = _coz_curr_eh, _coz_curr_eh = z_COZ_NEW_IP};  \
        _coz_ctx->m_eh = _coz_curr_eh, true)

#define COZ_CATCH                                                              \
    else case _coz_curr_eh : try {                                             \
        _coz_ctx->m_eh = _coz_prev_eh;                                         \
        std::rethrow_exception(_coz_ex.release());                             \
    } catch

#endif