#pragma once

// Thin wrapper over ScriptHookRDR2's native invocation API
// (nativeInit / nativePush64 / nativeCall). Args are passed as uint64_t
// bit-patterns; floats fit in the low 32 bits. Vector3 results come back
// as a struct via nativeCall's returned pointer.

#include <cstdint>
#include <cstring>
#include "script_hook.h"

struct Vec3
{
    float x, y, z;
};

namespace natives_api {

// Begin a native invocation.
inline void Init(unsigned long long hash)
{
    sh::nativeInit(hash);
}

// Push one argument (bit-pattern).
inline void Push(unsigned long long v)
{
    sh::nativePush64(v);
}

inline void Push(float f)
{
    union { float f; unsigned long long u; } u;
    u.f = f;
    sh::nativePush64(u.u);
}

inline void Push(int i)
{
    sh::nativePush64(static_cast<unsigned long long>(static_cast<unsigned int>(i)));
}

inline void Push(unsigned int u)
{
    sh::nativePush64(static_cast<unsigned long long>(u));
}

inline void Push(bool b)
{
    sh::nativePush64(b ? 1 : 0);
}

inline void Push(const Vec3& v)
{
    Push(v.x);
    Push(v.y);
    Push(v.z);
}

// Execute and return result as uint64_t.
inline unsigned long long Call()
{
    unsigned long long* r = sh::nativeCall();
    return r ? r[0] : 0;
}

// Execute and return result reinterpretted as float.
inline float CallFloat()
{
    unsigned long long r = Call();
    union { float f; unsigned long long u; } u;
    u.u = r;
    return u.f;
}

// Execute a Vector3-returning native (GET_ENTITY_COORDS style): the
// native stores x,y,z into the returned scratch buffer and the buffer is
// persistent across the call, so we copy it out immediately.
inline void CallVec3(Vec3& out)
{
    unsigned long long* r = sh::nativeCall();
    if (r)
    {
        out.x = *reinterpret_cast<float*>(r);
        out.y = *reinterpret_cast<float*>(r + 1);
        out.z = *reinterpret_cast<float*>(r + 2);
    }
}

} // namespace natives_api