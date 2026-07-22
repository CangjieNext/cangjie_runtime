#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#define MRT_USE_MARK_SWEEP 1
#include "src/Heap/MCollector/MSBackgroundConfig.h"

namespace {

using MapleRuntime::GetMSGCBackgroundConfig;
using MapleRuntime::InitializeMSGCBackgroundConfigFromEnv;
using MapleRuntime::LoadMSGCBackgroundConfigFromEnv;
using MapleRuntime::MSGCBackgroundConfig;

void ExpectTrue(bool value, const char* message)
{
    if (!value) {
        std::fprintf(stderr, "%s\n", message);
        std::abort();
    }
}

void ExpectConfig(const MSGCBackgroundConfig& config, bool sweep, bool feed, bool scavenge, bool shrink,
                  const char* message)
{
    if (config.enableSweep != sweep || config.enableFeed != feed || config.enableScavenge != scavenge ||
        config.enableShrink != shrink) {
        std::fprintf(stderr,
                     "%s: expected sweep=%d feed=%d scavenge=%d shrink=%d, actual sweep=%d feed=%d "
                     "scavenge=%d shrink=%d\n",
                     message, sweep, feed, scavenge, shrink, config.enableSweep, config.enableFeed,
                     config.enableScavenge, config.enableShrink);
        std::abort();
    }
}

class ScopedEnvVar {
public:
    ScopedEnvVar(const char* name, const char* value) : name_(name)
    {
        const char* current = std::getenv(name);
        if (current != nullptr) {
            hadOriginal_ = true;
            originalValue_ = current;
        }
        Set(value);
    }

    ~ScopedEnvVar()
    {
        if (hadOriginal_) {
            Set(originalValue_.c_str());
            return;
        }
        Set(nullptr);
    }

private:
    void Set(const char* value)
    {
#ifdef _WIN64
        if (value == nullptr) {
            _putenv_s(name_, "");
            return;
        }
        _putenv_s(name_, value);
#else
        if (value == nullptr) {
            unsetenv(name_);
            return;
        }
        setenv(name_, value, 1);
#endif
    }

    const char* name_;
    bool hadOriginal_ = false;
    std::string originalValue_;
};

void TestDefaultConfiguration()
{
    ScopedEnvVar sweep("CJ_MS_BG_SWEEP", nullptr);
    ScopedEnvVar feed("CJ_MS_BG_FEED", nullptr);
    ScopedEnvVar scavenge("CJ_MS_BG_SCAVENGE", nullptr);
    ScopedEnvVar shrink("CJ_MS_BG_SHRINK", nullptr);

    MSGCBackgroundConfig config = LoadMSGCBackgroundConfigFromEnv();
    ExpectConfig(config, true, false, true, false, "default background config");
}

void TestEnvironmentOverrides()
{
    ScopedEnvVar sweep("CJ_MS_BG_SWEEP", "0");
    ScopedEnvVar feed("CJ_MS_BG_FEED", "off");
    ScopedEnvVar scavenge("CJ_MS_BG_SCAVENGE", "false");
    ScopedEnvVar shrink("CJ_MS_BG_SHRINK", "1");

    MSGCBackgroundConfig config = LoadMSGCBackgroundConfigFromEnv();
    ExpectConfig(config, false, false, false, true, "environment overrides background config");
}

void TestInitializePublishesLoadedConfiguration()
{
    ScopedEnvVar sweep("CJ_MS_BG_SWEEP", "yes");
    ScopedEnvVar feed("CJ_MS_BG_FEED", "0");
    ScopedEnvVar scavenge("CJ_MS_BG_SCAVENGE", "1");
    ScopedEnvVar shrink("CJ_MS_BG_SHRINK", "no");

    InitializeMSGCBackgroundConfigFromEnv();
    ExpectConfig(GetMSGCBackgroundConfig(), true, false, true, false, "initialized background config");
}

} // namespace

int main()
{
    TestDefaultConfiguration();
    TestEnvironmentOverrides();
    TestInitializePublishesLoadedConfiguration();
    return 0;
}
