/*
 * Copyright 2026 Toni500
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
 * following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following
 * disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the
 * following disclaimer in the documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote
 * products derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS “AS IS” AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#ifndef _UTIL_RESULT_HPP_
#define _UTIL_RESULT_HPP_

// Result, Ok and Err: errors returned instead of thrown

#include <concepts>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include "fmt/format.h"

// ------------------------------
// Macros
// ------------------------------

/**
 * Return the error of a Result from the current function, if it has one
 * @param expr The Result to check
 **/
#define TRY(expr)                     \
    do                                \
    {                                 \
        auto&& _r = (expr);           \
        if (!_r.ok())                 \
            return Err(_r.error_v()); \
    } while (0)

/**
 * TRY(), but wrap the error in a message
 * @param expr The Result to check
 * @param fmtstr The format string. The error is the last argument, after the ones in `...`
 **/
#define TRY_MSG(expr, fmtstr, ...)                                       \
    do                                                                   \
    {                                                                    \
        auto&& _r = (expr);                                              \
        if (!_r.ok())                                                    \
            return Err(fmtstr __VA_OPT__(, ) __VA_ARGS__, _r.error_v()); \
    } while (0)

/**
 * Run some code if a Result has an error
 * @param expr The Result to check
 * @param on_err The code to run. The Result is available as `_r`
 **/
#define MUST_OK(expr, on_err) \
    do                        \
    {                         \
        auto&& _r = (expr);   \
        if (!_r.ok())         \
        {                     \
            on_err;           \
        }                     \
    } while (0)

// ------------------------------
// Types
// ------------------------------

// shoutout to the Better C++ server for these helper structs

/* The value of a successful Result */
template <typename T>
struct Ok
{
    using value_type = T;
    T value;
};

/* A successful Result with no value */
template <>
struct Ok<void>
{
};

template <typename T>
Ok(T) -> Ok<T>;
Ok() -> Ok<void>;

/* The error of a failed Result. A std::string by default */
template <typename E = std::string>
struct Err
{
    using value_type = E;
    E value;

    Err() = default;

    // Err(42), Err(some_error_code), Err(std::string{...})
    template <typename U>
        requires std::constructible_from<E, U&&> && (!std::same_as<std::remove_cvref_t<U>, Err>)
    Err(U&& v) : value(std::forward<U>(v))
    {}

    // Err("failed: {}", code, ...), formatted with fmt::format()
    template <typename... Args>
        requires(sizeof...(Args) >= 1) && std::constructible_from<E, std::string>
    Err(fmt::format_string<Args...> fmt_str, Args&&... args) : value(fmt::format(fmt_str, std::forward<Args>(args)...))
    {}
};
template <typename E>
Err(E) -> Err<E>;
Err(const char*) -> Err<std::string>;

template <typename... Args>
    requires(sizeof...(Args) >= 1)
Err(fmt::format_string<Args...>, Args&&...) -> Err<std::string>;

/**
 * Either a value (Ok) or an error (Err), returned instead of throwing.
 * Check ok() before get(), or propagate the error with TRY().
 **/
template <typename T = Ok<void>, typename E = Err<std::string>>
class [[nodiscard("Must check if ok")]] Result
{
public:
    template <typename U>
    Result(Ok<U> const& v) : m_value(std::in_place_index<0>, v.value)
    {}
    template <typename U>
    Result(Ok<U>&& v) : m_value(std::in_place_index<0>, std::move(v.value))
    {}

    template <typename U>
    Result(Err<U> const& e) : m_value(std::in_place_index<1>, e.value)
    {}
    template <typename U>
    Result(Err<U>&& e) : m_value(std::in_place_index<1>, std::move(e.value))
    {}

    // The value inside the Err, e.g. the std::string of an Err<std::string>
    template <typename U = E, typename = typename U::value_type>
    typename U::value_type& error_v()
    {
        return std::get<E>(m_value).value;
    }

    template <typename U = E, typename = typename U::value_type>
    [[nodiscard]] const typename U::value_type& error_v() const
    {
        return std::get<E>(m_value).value;
    }

    [[nodiscard]] bool     ok() const { return std::holds_alternative<T>(m_value); }
                           operator bool() const { return ok(); }
    T&                     get() { return std::get<T>(m_value); }
    E&                     error() { return std::get<E>(m_value); }
    [[nodiscard]] const T& get() const { return std::get<T>(m_value); }
    [[nodiscard]] const E& error() const { return std::get<E>(m_value); }

private:
    std::variant<T, E> m_value;
};

/* Result<> of a function that only succeeds or fails */
template <typename E>
class [[nodiscard("Must check if ok")]] Result<Ok<void>, E>
{
public:
    Result() : m_ok(true) {}
    Result(Ok<void>) : m_ok(true) {}

    template <typename U>
    Result(const Err<U>& err) : m_ok(false), m_err{ err.value }
    {}
    template <typename U>
    Result(Err<U>&& err) : m_ok(false), m_err{ std::move(err.value) }
    {}

    // The value inside the Err, e.g. the std::string of an Err<std::string>
    template <typename U = E, typename = typename U::value_type>
    typename U::value_type& error_v()
    {
        return m_err.value;
    }

    template <typename U = E, typename = typename U::value_type>
    [[nodiscard]] const typename U::value_type& error_v() const
    {
        return m_err.value;
    }

    [[nodiscard]] bool     ok() const { return m_ok; }
    E&                     error() { return m_err; }
    [[nodiscard]] const E& error() const { return m_err; }
                           operator bool() const { return ok(); }

private:
    bool m_ok;
    E    m_err;
};

#endif  // _UTIL_RESULT_HPP_
