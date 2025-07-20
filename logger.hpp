// logger.hpp
#pragma once

#include <atomic>
#include <array>
#include <cstdint>
#include <cstddef>
#include <thread>
#include <vector>
#include <cstring>
#include <cassert>
#include <utility>
#include <iostream>
#include <fstream>
#include <unordered_map>
#include <sstream>
#include <iomanip>
#include <type_traits>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#endif

// Settings
constexpr size_t LOGGER_RING_SIZE = 2048; // Must be power of two for mask wrap
constexpr size_t LOGGER_MAX_ARGS = 8;
constexpr size_t LOGGER_MAX_THREADS = 64; // Max concurrent producer threads
constexpr size_t CACHE_LINE_SIZE = 64;

#define CACHE_ALIGN alignas(CACHE_LINE_SIZE)

inline uint64_t read_tsc()
{
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    return __rdtsc();
#elif defined(__x86_64__) || defined(__i386__)
    uint32_t hi, lo;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
#else
    return std::chrono::high_resolution_clock::now().time_since_epoch().count();
#endif
}

enum class LogArgType : uint8_t
{
    I64,
    U64,
    F64,
    CString,
    Pointer
};

struct CACHE_ALIGN LogArg
{
    LogArgType type;
    union
    {
        int64_t i64;
        uint64_t u64;
        double f64;
        const char *cstr; // User guarantees lifetime valid until consumption
        const void *ptr;
    };
    size_t cstr_len; // only valid for CString
};

struct CACHE_ALIGN LogMsg
{
    uint64_t timestamp;
    const char *fmt;
    uint32_t thread_id;
    uint32_t log_id; // <-- log routing ID
    uint8_t arg_count;
    LogArg args[LOGGER_MAX_ARGS];
};

class ThreadRingBuffer
{
public:
    explicit ThreadRingBuffer(uint32_t tid) : _write_idx(0), _read_idx(0), _thread_id(tid),
                                              _data{}
    {
        static_assert((LOGGER_RING_SIZE & (LOGGER_RING_SIZE - 1)) == 0, "Ring size must be power of 2");
    }

    template <typename... Args>
    void emplace(uint32_t log_id, const char *fmt, Args &&...args)
    {
        static_assert(sizeof...(Args) <= LOGGER_MAX_ARGS, "Too many arguments for logger");
        uint64_t idx = _write_idx.fetch_add(1, std::memory_order_relaxed);
        LogMsg &msg = _data[idx & (LOGGER_RING_SIZE - 1)];
        msg.timestamp = read_tsc();
        msg.fmt = fmt;
        msg.thread_id = _thread_id;
        msg.log_id = log_id;
        msg.arg_count = static_cast<uint8_t>(sizeof...(Args));
        fill_args(msg, std::forward<Args>(args)...);
    }

    bool try_consume(LogMsg &out)
    {
        uint64_t read_idx = _read_idx.load(std::memory_order_relaxed);
        uint64_t write_idx = _write_idx.load(std::memory_order_acquire);
        if (read_idx == write_idx)
            return false;

        out = _data[read_idx & (LOGGER_RING_SIZE - 1)];
        _read_idx.fetch_add(1, std::memory_order_release);
        return true;
    }

private:
    template <typename T, typename... Rest>
    void fill_args(LogMsg &msg, T &&first, Rest &&...rest)
    {
        set_arg(msg, msg.arg_count - sizeof...(Rest) - 1, std::forward<T>(first));
        fill_args(msg, std::forward<Rest>(rest)...);
    }
    void fill_args(LogMsg &) {}

    // Explicit overloads for supported argument types:
    void set_arg(LogMsg &msg, size_t idx, uint8_t val)
    {
        msg.args[idx].type = LogArgType::U64;
        msg.args[idx].u64 = static_cast<uint64_t>(val);
    }

    void set_arg(LogMsg &msg, size_t idx, int8_t val)
    {
        msg.args[idx].type = LogArgType::I64;
        msg.args[idx].i64 = static_cast<int64_t>(val);
    }

    void set_arg(LogMsg &msg, size_t idx, uint16_t val)
    {
        msg.args[idx].type = LogArgType::U64;
        msg.args[idx].u64 = static_cast<uint64_t>(val);
    }

    void set_arg(LogMsg &msg, size_t idx, int16_t val)
    {
        msg.args[idx].type = LogArgType::I64;
        msg.args[idx].i64 = static_cast<int64_t>(val);
    }

    void set_arg(LogMsg &msg, size_t idx, uint32_t val)
    {
        msg.args[idx].type = LogArgType::U64;
        msg.args[idx].u64 = static_cast<uint64_t>(val);
    }

    void set_arg(LogMsg &msg, size_t idx, int32_t val)
    {
        msg.args[idx].type = LogArgType::I64;
        msg.args[idx].i64 = static_cast<int64_t>(val);
    }
    void set_arg(LogMsg &msg, size_t idx, int64_t val)
    {
        msg.args[idx].type = LogArgType::I64;
        msg.args[idx].i64 = val;
    }
    void set_arg(LogMsg &msg, size_t idx, uint64_t val)
    {
        msg.args[idx].type = LogArgType::U64;
        msg.args[idx].u64 = val;
    }
    void set_arg(LogMsg &msg, size_t idx, double val)
    {
        msg.args[idx].type = LogArgType::F64;
        msg.args[idx].f64 = val;
    }
    void set_arg(LogMsg &msg, size_t idx, const char *val)
    {
        msg.args[idx].type = LogArgType::CString;
        msg.args[idx].cstr = val;
        msg.args[idx].cstr_len = std::strlen(val);
    }
    void set_arg(LogMsg &msg, size_t idx, void *val)
    {
        msg.args[idx].type = LogArgType::Pointer;
        msg.args[idx].ptr = val;
    }

    template <typename T>
    typename std::enable_if<std::is_enum<T>::value, void>::type
    set_arg(LogMsg &msg, size_t idx, T val)
    {
        using Underlying = typename std::underlying_type<T>::type;
        set_arg(msg, idx, static_cast<Underlying>(val));
    }
    // Catch-all integral to int64_t conversion, avoiding ambiguity
    template <typename T>
    typename std::enable_if<
        std::is_integral<T>::value &&
            !std::is_same<T, int64_t>::value &&
            !std::is_same<T, uint64_t>::value &&
            !std::is_same<T, int32_t>::value &&
            !std::is_same<T, uint32_t>::value &&
            !std::is_same<T, int16_t>::value &&
            !std::is_same<T, uint16_t>::value &&
            !std::is_same<T, int8_t>::value &&
            !std::is_same<T, uint8_t>::value,
        void>::type
    set_arg(LogMsg &msg, size_t idx, T val)
    {
        msg.args[idx].type = LogArgType::I64;
        msg.args[idx].i64 = static_cast<int64_t>(val);
    }

    CACHE_ALIGN std::atomic<uint64_t> _write_idx;
    CACHE_ALIGN std::atomic<uint64_t> _read_idx;
    uint32_t _thread_id;
    std::array<LogMsg, LOGGER_RING_SIZE> _data;
};

class Logger
{
public:
    static Logger &instance()
    {
        static Logger inst;
        return inst;
    }

    ThreadRingBuffer *get_buffer()
    {
        thread_local uint32_t tid = _register_thread();
        thread_local ThreadRingBuffer *buf = _get_or_create_buffer(tid);
        return buf;
    }

    void for_each_buffer(const std::function<void(ThreadRingBuffer *)> &func)
    {
        for (size_t i = 0; i < LOGGER_MAX_THREADS; ++i)
        {
            ThreadRingBuffer *buf = _buffers[i];
            if (buf)
                func(buf);
        }
    }

private:
    Logger() : _tid_counter(0)
    {
        for (auto &b : _buffers)
            b = nullptr;
    }

    uint32_t _register_thread()
    {
        static thread_local uint32_t myid = _tid_counter.fetch_add(1, std::memory_order_relaxed);
        assert(myid < LOGGER_MAX_THREADS);
        return myid;
    }

    ThreadRingBuffer *_get_or_create_buffer(uint32_t tid)
    {
        ThreadRingBuffer *ptr = _buffers[tid];
        if (!ptr)
        {
            ptr = new ThreadRingBuffer(tid);
            _buffers[tid] = ptr;
        }
        return ptr;
    }

    std::atomic<uint32_t> _tid_counter;
    ThreadRingBuffer *_buffers[LOGGER_MAX_THREADS];
};

template <typename... Args>
inline void log(uint32_t log_id, const char *fmt, Args &&...args)
{
    Logger::instance().get_buffer()->emplace(log_id, fmt, std::forward<Args>(args)...);
}
