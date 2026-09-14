// Host-level regression for Live rejecting a MIDI-only VST3 with no audio bus.
// Only initializes and negotiates ports; never activates processing or MIDI I/O.
#include <CoreFoundation/CoreFoundation.h>
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include <cstdio>
#include <cstring>

using namespace Steinberg;
using namespace Steinberg::Vst;

struct Host final : IHostApplication {
    uint32 refs = 1;
    tresult PLUGIN_API queryInterface(const TUID iid, void **out) override {
        *out = nullptr;
        if (!std::memcmp(iid, IHostApplication_iid, 16) ||
            !std::memcmp(iid, FUnknown_iid, 16)) {
            *out = static_cast<IHostApplication *>(this);
            addRef();
            return kResultOk;
        }
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++refs; }
    uint32 PLUGIN_API release() override { return --refs; }
    tresult PLUGIN_API getName(String128 name) override {
        const char *text = "LumiPaint Smoke Test";
        unsigned i = 0;
        do { name[i] = text[i]; } while (text[i++]);
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(TUID, TUID, void **out) override {
        *out = nullptr;
        return kNoInterface;
    }
};

int main(int argc, char **argv) {
    if (argc != 2) return 1;
    auto url = CFURLCreateFromFileSystemRepresentation(nullptr,
        reinterpret_cast<const UInt8 *>(argv[1]), std::strlen(argv[1]), true);
    auto bundle = CFBundleCreate(nullptr, url);
    CFRelease(url);
    if (!bundle || !CFBundleLoadExecutable(bundle)) return 2;
    auto entry = reinterpret_cast<bool (*)(CFBundleRef)>(
        CFBundleGetFunctionPointerForName(bundle, CFSTR("bundleEntry")));
    auto exit = reinterpret_cast<bool (*)()>(
        CFBundleGetFunctionPointerForName(bundle, CFSTR("bundleExit")));
    auto factoryFn = reinterpret_cast<IPluginFactory *(*)()>(
        CFBundleGetFunctionPointerForName(bundle, CFSTR("GetPluginFactory")));
    if (!entry || !exit || !factoryFn || !entry(bundle)) return 3;
    auto factory = factoryFn();
    if (!factory) return 4;
    Host host;
    bool passed = false;
    for (int i = 0; i < factory->countClasses(); ++i) {
        PClassInfo info{};
        if (factory->getClassInfo(i, &info) != kResultOk ||
            std::strcmp(info.name, "LumiPaint")) continue;
        IComponent *component = nullptr;
        if (factory->createInstance(info.cid, IComponent_iid,
                reinterpret_cast<void **>(&component)) != kResultOk) return 5;
        if (component->initialize(&host) != kResultOk) return 6;
        IAudioProcessor *processor = nullptr;
        if (component->queryInterface(IAudioProcessor_iid,
                reinterpret_cast<void **>(&processor)) != kResultOk) return 7;
        auto audioIn = component->getBusCount(kAudio, kInput);
        auto audioOut = component->getBusCount(kAudio, kOutput);
        auto midiIn = component->getBusCount(kEvent, kInput);
        auto midiOut = component->getBusCount(kEvent, kOutput);
        SpeakerArrangement stereo = SpeakerArr::kStereo;
        auto arrangement = processor->setBusArrangements(nullptr, 0, &stereo, 1);
        BusInfo output{};
        auto gotOutput = component->getBusInfo(kAudio, kOutput, 0, output);
        std::printf("Audio buses: %d in / %d out; MIDI: %d in / %d out; stereo negotiation: %d\n",
            audioIn, audioOut, midiIn, midiOut, arrangement);
        passed = audioIn == 0 && audioOut == 1 && midiIn == 1 && midiOut == 1 &&
            arrangement == kResultOk && gotOutput == kResultOk &&
            output.channelCount == 2 && output.busType == kMain;
        processor->release();
        component->terminate();
        component->release();
    }
    factory->release();
    exit();
    CFBundleUnloadExecutable(bundle);
    CFRelease(bundle);
    std::puts(passed ? "PASS" : "FAIL");
    return passed ? 0 : 8;
}
