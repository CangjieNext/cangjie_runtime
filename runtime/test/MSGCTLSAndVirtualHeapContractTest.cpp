#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "src/Cangjie.h"
#include "src/Mutator/ThreadLocal.h"

namespace {

using MapleRuntime::ThreadLocalData;

template <class T, class = void>
struct HasGCFastState : std::false_type {
};

template <class T>
struct HasGCFastState<T, std::void_t<decltype(&T::gcFastState)>> : std::true_type {
};

static_assert(!HasGCFastState<ThreadLocalData>::value,
              "ThreadLocalData should no longer expose gcFastState");

static_assert(offsetof(ThreadLocalData, encodedGCPhase) == sizeof(void*) * 9 + 6,
              "encodedGCPhase offset should stay ABI-stable for packed TLS phase publishing");

static_assert(std::is_same<decltype(::GCParam {}.msMaxVirtualHeapSize), size_t>::value,
              "GCParam should expose msMaxVirtualHeapSize in KB");

} // namespace

int main()
{
    ::GCParam param {};
    param.msMaxVirtualHeapSize = 16ULL * 1024ULL * 1024ULL;
    return static_cast<int>(param.msMaxVirtualHeapSize == 0);
}
