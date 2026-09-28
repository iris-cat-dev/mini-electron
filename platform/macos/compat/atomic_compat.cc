
#include <windows.h>
#include <stdio.h>

#if defined(_MSC_VER)

extern "C" uintptr_t MbTlsAlloc()
{
    return (uintptr_t)TlsAlloc();
}

extern "C" LPVOID MbTlsGetValue(uintptr_t dwTlsIndex)
{
    return TlsGetValue((DWORD)dwTlsIndex);
}

extern "C" BOOL MbTlsSetValue(uintptr_t dwTlsIndex, LPVOID lpTlsValue)
{
    return TlsSetValue((DWORD)dwTlsIndex, lpTlsValue);
}

extern "C" BOOL MbTlsFree(uintptr_t dwTlsIndex)
{
    return TlsFree((DWORD)dwTlsIndex);
}

extern "C" long MB_InterlockedIncrement(long volatile* _Target)
{
    return _InterlockedIncrement((long volatile*)_Target);
}

extern "C" long MB_InterlockedExchange(long volatile* _Target, long _Value)
{
    return _InterlockedExchange((long volatile*)_Target, _Value);
}

extern "C" long MB_InterlockedExchangeAdd(long volatile* _Addend, long _Value)
{
    return _InterlockedExchangeAdd((long volatile*)_Addend, _Value);
}

extern "C" long MB_InterlockedDecrement(long volatile* _Target)
{
    return _InterlockedDecrement((long volatile*)_Target);
}

extern "C" long MB_InterlockedCompareExchange(long volatile* _Destination, long _Exchange, long _Comparand)
{
    return _InterlockedCompareExchange((long volatile*)_Destination, _Exchange, _Comparand);
}

#else

uintptr_t MbTlsAlloc()
{
    pthread_key_t key;
    pthread_key_create(&key, nullptr);
    return reinterpret_cast<uintptr_t>(key);
}

LPVOID MbTlsGetValue(uintptr_t dwTlsIndex)
{
    return pthread_getspecific(static_cast<pthread_key_t>(dwTlsIndex));
}

BOOL MbTlsSetValue(uintptr_t dwTlsIndex, LPVOID lpTlsValue)
{
    return pthread_setspecific(static_cast<pthread_key_t>(dwTlsIndex), lpTlsValue) == 0;
}

BOOL MbTlsFree(uintptr_t dwTlsIndex)
{
    return pthread_key_delete(static_cast<pthread_key_t>(dwTlsIndex)) == 0;
}

extern "C" long MB_InterlockedIncrement(long volatile* target)
{
    return __atomic_add_fetch(reinterpret_cast<volatile int32_t*>(target), 1, __ATOMIC_SEQ_CST);
}

extern "C" long MB_InterlockedExchange(long volatile* target, long value)
{
    return __atomic_exchange_n(reinterpret_cast<volatile int32_t*>(target), static_cast<int32_t>(value), __ATOMIC_SEQ_CST);
}

extern "C" long MB_InterlockedExchangeAdd(long volatile* target, long value)
{
    return __atomic_fetch_add(reinterpret_cast<volatile int32_t*>(target), static_cast<int32_t>(value), __ATOMIC_SEQ_CST);
}

extern "C" long MB_InterlockedDecrement(long volatile* target)
{
    return __atomic_sub_fetch(reinterpret_cast<volatile int32_t*>(target), 1, __ATOMIC_SEQ_CST);
}

extern "C" long MB_InterlockedCompareExchange(long volatile* target, long exchange, long comparand)
{
    int32_t expected = static_cast<int32_t>(comparand);
    __atomic_compare_exchange_n(reinterpret_cast<volatile int32_t*>(target), &expected,
        static_cast<int32_t>(exchange), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
}

template <typename T>
static void AtomicLoad(void* ptr, void* ret, int memorder)
{
    *static_cast<T*>(ret) = __atomic_load_n(static_cast<T*>(ptr), memorder);
}

template <typename T>
static void AtomicStore(void* ptr, const void* value, int memorder)
{
    __atomic_store_n(static_cast<T*>(ptr), *static_cast<const T*>(value), memorder);
}

template <typename T>
static void AtomicExchange(void* ptr, void* value, void* ret, int memorder)
{
    *static_cast<T*>(ret) = __atomic_exchange_n(static_cast<T*>(ptr), *static_cast<T*>(value), memorder);
}

extern "C" void MB__atomic_load(size_t size, void* ptr, void* ret, int memorder)
{
    switch (size) {
    case 1: AtomicLoad<uint8_t>(ptr, ret, memorder); return;
    case 2: AtomicLoad<uint16_t>(ptr, ret, memorder); return;
    case 4: AtomicLoad<uint32_t>(ptr, ret, memorder); return;
    case 8: AtomicLoad<uint64_t>(ptr, ret, memorder); return;
    default: __builtin_trap();
    }
}

extern "C" void MB__atomic_store(size_t size, void* ptr, const void* value, int memorder)
{
    switch (size) {
    case 1: AtomicStore<uint8_t>(ptr, value, memorder); return;
    case 2: AtomicStore<uint16_t>(ptr, value, memorder); return;
    case 4: AtomicStore<uint32_t>(ptr, value, memorder); return;
    case 8: AtomicStore<uint64_t>(ptr, value, memorder); return;
    default: __builtin_trap();
    }
}

extern "C" void MB__atomic_exchange(size_t size, void* ptr, void* value, void* ret, int memorder)
{
    switch (size) {
    case 1: AtomicExchange<uint8_t>(ptr, value, ret, memorder); return;
    case 2: AtomicExchange<uint16_t>(ptr, value, ret, memorder); return;
    case 4: AtomicExchange<uint32_t>(ptr, value, ret, memorder); return;
    case 8: AtomicExchange<uint64_t>(ptr, value, ret, memorder); return;
    default: __builtin_trap();
    }
}

#endif

#if (defined(_M_X64) || defined(__x86_64__)) && defined(__clang__)
// for BN_mod_word in div.c
extern "C" unsigned __int64 MB__umodti3(unsigned __int64 a, unsigned __int64 b)
{
    return a % b;
}
extern "C" unsigned __int64 MB__udivti3(unsigned __int64 a, unsigned __int64 b)
{
    return a / b;
}
#endif