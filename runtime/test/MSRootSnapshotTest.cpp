#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "src/Heap/MCollector/MSRootSnapshot.h"

namespace {
using MapleRuntime::BaseObject;
using MapleRuntime::MSRootSnapshot;

void Expect(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::abort();
    }
}

void TestDeduplicateAndPublishSharesOnePublicationBoundary()
{
    MSRootSnapshot snapshot;
    BaseObject* first = reinterpret_cast<BaseObject*>(static_cast<uintptr_t>(0x1000));
    BaseObject* second = reinterpret_cast<BaseObject*>(static_cast<uintptr_t>(0x2000));
    snapshot.Append(first);
    snapshot.Append(first);
    snapshot.Append(reinterpret_cast<BaseObject*>(static_cast<uintptr_t>(0x3000)));
    snapshot.Append(second);
    size_t published = 0;
    size_t unique = snapshot.DeduplicateAndPublish(
        [](BaseObject* root) {
            return root != reinterpret_cast<BaseObject*>(0x3000);
        },
        [&published, first, second](const std::vector<BaseObject*>& roots) {
            published = roots.size();
        Expect(roots[0] == first && roots[1] == second, "snapshot must preserve first-seen order");
        });
    Expect(unique == 2 && published == 2,
           "snapshot must publish every deduplicated root exactly once");

    snapshot.Append(first);
    size_t nextPublished = 0;
    Expect(snapshot.DeduplicateAndPublish(
               [](BaseObject* root) { return root != nullptr; },
               [&nextPublished](const std::vector<BaseObject*>& roots) {
                   nextPublished = roots.size();
               }) == 1 && nextPublished == 1,
           "publishing must clear the snapshot for the next root set");
}
}

int main()
{
    TestDeduplicateAndPublishSharesOnePublicationBoundary();
    return 0;
}
