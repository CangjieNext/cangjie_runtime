#include <cstdio>
#include <cstdlib>

#include "src/Heap/MCollector/MSGCConfig.h"

namespace {
using namespace MapleRuntime;
void Expect(bool condition, const char* message) { if (!condition) { std::fprintf(stderr, "%s\n", message); std::abort(); } }
void TestAPIUsesMSGCDefaults()
{
    MSGCConfigInput input {};
    MSGCResolvedConfig config = ResolveMSGCConfig(input, MSGCConfigSource::API);
    Expect(config.backgroundSweepMaxTimeNs == kMSGCDefaultBackgroundSweepMaxTimeNs, "API sweep budget default");
    Expect(config.markTimeBudgetNs == kMSGCDefaultMarkTimeBudgetNs, "API mark budget default");
    Expect(config.backgroundSweep == 1 && config.backgroundScavenge == 1, "API background defaults");
}
void TestEnvironmentPreservesExplicitFalse()
{
    MSGCConfigInput input {};
    input.backgroundSweep = 0;
    input.backgroundScavenge = 0;
    input.parallelSweep = 0;
    MSGCResolvedConfig config = ResolveMSGCConfig(input, MSGCConfigSource::ENVIRONMENT);
    Expect(config.backgroundSweep == 0 && config.backgroundScavenge == 0 && config.parallelSweep == 0,
           "environment false values must not be treated as API defaults");
}

void TestAPINormalizesBooleanFields()
{
    MSGCConfigInput input {};
    input.backgroundShrink = 2;
    input.parallelSweep = 7;
    input.adaptiveGrowth = 3;
    MSGCResolvedConfig config = ResolveMSGCConfig(input, MSGCConfigSource::API);
    Expect(config.backgroundShrink == 1 && config.parallelSweep == 1 && config.adaptiveGrowth == 1,
           "API boolean fields must be normalized");
}

void TestResolverNormalizesAssistDisableOverride()
{
    MSGCConfigInput input {};
    input.enumAssistEnabled = 7;
    input.assistDisabled = 7;
    MSGCResolvedConfig config = ResolveMSGCConfig(input, MSGCConfigSource::ENVIRONMENT);
    Expect(config.enumAssistEnabled == 1,
           "enum assist must be normalized by the shared resolver");
    Expect(config.assistDisabled == 1,
           "assist diagnostic override must be normalized by the shared resolver");
}
}
int main() { TestAPIUsesMSGCDefaults(); TestEnvironmentPreservesExplicitFalse(); TestAPINormalizesBooleanFields(); TestResolverNormalizesAssistDisableOverride(); return 0; }
