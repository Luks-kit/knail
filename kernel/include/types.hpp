#pragma once
// kernel/types.hpp - Knail shared type definitions

#include <stdint.h>
#include <stddef.h>

// ─────────────────────────────────────────────
//  Primitive aliases
// ─────────────────────────────────────────────

using u8    = uint8_t;
using u16   = uint16_t;
using u32   = uint32_t;
using u64   = uint64_t;
using i8    = int8_t;
using i16   = int16_t;
using i32   = int32_t;
using i64   = int64_t;
using usize = size_t;
using isize = ptrdiff_t;

// stdint names remain available via <stdint.h>

// ─────────────────────────────────────────────
//  Address types
// ─────────────────────────────────────────────

using paddr_t = u64;   // physical address
using vaddr_t = u64;   // virtual address

// ─────────────────────────────────────────────
//  Error codes
// ─────────────────────────────────────────────

enum class kError : u32 {
    None = 0,

    // Generic
    InvalidArgument,
    NullPointer,
    OutOfMemory,
    NotImplemented,
    Overflow,

    // I/O / drivers
    IOError,
    Timeout,
    DeviceNotReady,
    DeviceError,

    // Filesystem
    NotFound,
    NotADirectory,
    NotAFile,
    EndOfFile,
    FilesystemCorrupt,
    ReadOnly,
    AlreadyExists,
    NoSpace,
    IsDirectory,
    BadFileDescriptor,
    OutOfFileDescriptors,
    PermissionDenied,
    Busy,
};

// ─────────────────────────────────────────────
//  kResult<T, E>
// ─────────────────────────────────────────────

template<typename T, typename E = kError>
class kResult {
public:
    static kResult ok(T value) {
        kResult r;
        r.m_ok    = true;
        r.m_value = value;
        return r;
    }

    static kResult err(E error) {
        kResult r;
        r.m_ok    = false;
        r.m_error = error;
        return r;
    }

    bool is_ok()  const { return m_ok; }
    bool is_err() const { return !m_ok; }

    T value()    const { return m_value; }
    E error()    const { return m_error; }

    T value_or(T fallback) const { return m_ok ? m_value : fallback; }

private:
    bool m_ok = false;
    union {
        T m_value;
        E m_error;
    };
};

// Void specialisation — operations that succeed or fail with no return value
template<typename E>
class kResult<void, E> {
public:
    static kResult ok() {
        kResult r;
        r.m_ok = true;
        return r;
    }

    static kResult err(E error) {
        kResult r;
        r.m_ok    = false;
        r.m_error = error;
        return r;
    }

    bool is_ok()  const { return m_ok; }
    bool is_err() const { return !m_ok; }
    E    error()  const { return m_error; }

private:
    bool m_ok    = false;
    E    m_error = E{};
};

// Convenience alias — the common "did this succeed?" return type
using kStatus = kResult<void, kError>;

// ─────────────────────────────────────────────
//  Date / Time
// ─────────────────────────────────────────────

struct kDateTime {
    u16 year;    // e.g. 2025
    u8  month;   // 1–12
    u8  day;     // 1–31
    u8  hour;    // 0–23
    u8  minute;  // 0–59
    u8  second;  // 0–59
};
