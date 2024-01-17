COZ - Coroutine ZERO
====================
Stackless coroutine for C++, but zero-allocation. Rebirth of [CO2](https://github.com/jamboree/co2).

### Dependencies
* Boost.Config
* Boost.Preprocessor

## Why
C++20 introduced coroutine into the language, however, in many cases (especially in async scenario), it incurs memory allocation, as HALO is not guaranteed. This creates resistance to its usage, as the convenience it offers may not worth the overhead it brings. People will tend to write monolithic coroutine function instead of splitting it into small, reusable coroutines in fear of introducing too many allocations, this is contrary to the discipline of programming.

## What it is
COZ is a single-header library that utilizes preprocessor & compiler magic to emulate the C++ coroutine, while requires zero allocation, it also doesn't require type erasure. With COZ, the entire coroutine is under your control, unlike standard coroutine, which can only be accessed indirectly via the `coroutine_handle`.

> __NOTE__ \
> COZ uses stateful metaprogramming technique, which may not be blessed by the standard committee.

## Overview
This library is modeled after the standard coroutine. It offers several macros to replace the language counterparts.

To use it, `#include <coz/coroutine.hpp>`

A coroutine written in this library looks like below:
```c++
auto function(Args... args) COZ_BEG(promise-initializer, (captured-args...),
    local-vars...;
) {
    // for generator-like coroutine
    COZ_YIELD(...);
    // for task-like coroutine
    COZ_AWAIT(...);
    COZ_RETURN(...);
} COZ_END
```

The coroutine body has to be surrounded with 2 macros: `COZ_BEG` and `COZ_END`.

The macro `COZ_BEG` takes some parameters:
* _promise-initializer_ - expression to initialize the promise, e.g. `async<int>(exe)`
* _captured-args_ (optional) - comma separated args to be captured, e.g. `(a, b)`
* _local-vars_ (optional) - local-variable definitions, e.g. `int a = 42;`

If there's no _captured-args_ and _locals_, it looks like:
```c++
COZ_BEG(init, ())
```
### promise-initializer
The _promise-initializer_ is an expression, whose type must define a `promise_type`, which will be constructed with the expression.
It can take args from the function params. For example, you can take an executor to be used for the promise.
```c++
template<class Exe>
auto f(Exe exe) COZ_BEG(async<int>(exe), ())
```
#### Remarks
* the args (e.g. `exe` in above example) don't have to be in the _captured-args_.
* if the expression contains comma that is not in parentheses, you must surround the it with parentheses (e.g. `(task<T, E>)`).

### local-vars
You can intialize the local variables as below:
```c++
auto f(int i) COZ_BEG(init, (i),
    int i2 = i * 2; // can refer to the arg
    std::string msg{"hello"};
) ...
```
#### Remarks
* `()` initializer cannot be used.
* `auto` deduced variable cannot be used.

### coroutine-body
Inside the coroutine body, there are some restrictions:
* local variables with automatic storage cannot cross suspension points - you should specify them in local variables section of `COZ_BEG` as described above
* `switch` body cannot contain suspension points.
* identifiers starting with `_coz_` are reserved for this library
* Some language constructs should use their marcro replacements (see below).

After defining the coroutine body, remember to close it with `COZ_END`.

## Replacements for language constructs
### `co_await`
It has 4 variants: `COZ_AWAIT`, `COZ_AWAIT_SET`, `COZ_AWAIT_APPLY` and `COZ_AWAIT_LET`.
| MACRO | Core Language |
|---|---|
| `COZ_AWAIT(expr)` | `co_await expr` |
| `COZ_AWAIT_SET(var, expr)` | `var = co_await expr` |
| `COZ_AWAIT_APPLY(f, expr, args...)` | `f(co_await expr, args...)` |
| `COZ_AWAIT_LET(var-decl, expr) {...}` | `{var-decl = co_await expr; ...}` |

#### Remarks
* The `expr` is either used directly or transformed. `operator co_await` is not used.
* If your compiler supports _Statement Expression_ extension (e.g. GCC & Clang), you can use `COZ_AWAIT` as an expression.
However, don't use more than one `COZ_AWAIT` in a single statement, and don't use it as an argument of a function in company with other arguments.
* `f` in `COZ_AWAIT_APPLY` can also be a marco (e.g. `COZ_RETURN`)
* `COZ_AWAIT_LET` allows you to declare a local variable that binds to the `co_await` result, then you can process it in the brace scope.

### `co_yield`
| MACRO | `expr` Lifetime |
|---|---|
| `COZ_YIELD(expr)` | transient |
| `COZ_YIELD_KEEP(expr)` | cross suspension point |

#### Semantic
```c++
promise.yield_value(expr);
<suspend>
```
It differs from the standard semantic, which is equivalent to `co_await promise.yield_value(expr)`. Instead, we ignore the result of `yield_value` and just suspend afterward.

### `co_return`
| MACRO | Core Language |
|---|---|
| `COZ_RETURN()` | `co_return` |
| `COZ_RETURN(expr)` | `co_return expr` |

### `try`/`catch`
Needed only if the try-block contains suspension points.

```c++
COZ_TRY {
    ...
} COZ_CATCH (const std::runtime_error& e) {
    ...
} catch (const std::exception& e) {
    ...
}
```
#### Remarks
Only the first `catch` clause needs to be written as `COZ_CATCH`, the subsequent ones should use the plain `catch`.

## Coroutine API
`coz::coroutine` has interface defined as below:
```c++
template<class Promise, class Params, class State>
struct coroutine {
    template<class Init>
    explicit coroutine(Init&& init);

    // No copy.
    coroutine(const coroutine&) = delete;
    coroutine& operator=(const coroutine&) = delete;

    coroutine_handle<Promise> handle() noexcept;

    Promise& promise() noexcept;
    const Promise& promise() const noexcept;

    bool done() const noexcept;
    void start(Params&& params);
    void resume();
    void destroy();
};
```
#### Remarks
* The `init` constructor param is the _promise-initializer_.
* The lifetime of `Promise` is tied to the coroutine.
* Non-started coroutine is considered to be `done`.
* Don't call `destroy` if it's already `done`.

`coz::coroutine_handle` has the same interface as the standard one.

## Customization points
### `coz::co_result`
This defines what is returned from the coroutine.
The prototype is:
```c++
template<class Init, class Params, class State>
struct co_result;
```
The first template param (i.e. `Init`) is the type of _promise-initializer_.
`Params` and `State` are the template params that you should pass to `coz::coroutine<Promise, Params, State>`, the `Promise` should be the same as `Init::promise_type`.

Users could customize it like below:
```c++
template<class Params, class State>
struct [[nodiscard]] coz::co_result<MyCoroInit, Params, State> {
    MyCoroInit m_init;
    Params m_params;

    // optional
    auto get_return_object();
    ...
};
```
#### Remarks
* `co_result` will be constructed the with the _promise-initializer_ and the _captured-args_.
* if `get_return_object` is defined, its result is returned; otherwise, the `co_result` itself is returned.

### *Promise*
The interface for *Promise* looks like below:
```c++
struct Promise {
    void finalize();

    // either
    void return_void();
    // or
    void return_value();

    void unhandled_exception();

    // optional
    auto await_transform(auto expr);
};
```
#### Remarks
* There's no `initial_suspend` and `final_suspend`.
The user should call `coroutine::start` to start the coroutine.
* Once the coroutine stops (either normally or via `destroy`) the `Promise::finalize` will be called.
* `await_transform` is not greedy (i.e. could be filtered by SFINAE).

### *Awaiter*
The interface for *Awaiter* looks like below:
```c++
struct Awaiter {
    bool await_ready();

    // either
    void await_suspend(coroutine_handle<Promise> coro);
    // or
    bool await_suspend(coroutine_handle<Promise> coro);

    T await_resume();
};
```
#### Remarks
* Unlike standard coroutine, `await_suspend` cannot return `coroutine_handle`.

## License

    Copyright (c) 2024 Jamboree

    Distributed under the Boost Software License, Version 1.0. (See accompanying
    file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
