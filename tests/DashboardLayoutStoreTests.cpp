// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/DashboardLayoutStore.h"

#include <juce_core/juce_core.h>

#include <array>

namespace audio_insight {
namespace {
class TemporaryDirectory final {
public:
    TemporaryDirectory()
        : directory(juce::File::getSpecialLocation(juce::File::tempDirectory)
                  .getNonexistentChildFile(
                      "audio-insight-dashboard-layout-store-tests", { }, false))
    {
        created = directory.createDirectory().wasOk();
    }

    ~TemporaryDirectory()
    {
        if (created)
            directory.deleteRecursively(false);
    }

    [[nodiscard]] bool wasCreated() const noexcept
    {
        return created;
    }

    [[nodiscard]] juce::File child(const juce::String& name) const
    {
        return directory.getChildFile(name);
    }

private:
    juce::File directory;
    bool created = false;
};

class DashboardLayoutStoreTests final : public juce::UnitTest {
public:
    DashboardLayoutStoreTests() : UnitTest("Dashboard layout store", "audio-insight")
    {
    }

    void runTest() override
    {
        beginTest("The default preference location is per-user and wrapper-independent");
        {
            const DashboardLayoutStore first;
            const DashboardLayoutStore second;
            const auto userData
                = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);

            expect(first.getStorageFile() == second.getStorageFile());
            expect(first.getStorageFile() == DashboardLayoutStore::defaultStorageFile());
            expect(first.getStorageFile().isAChildOf(userData));
            expectEquals(
                first.getStorageFile().getFileName(), juce::String("dashboard-layout.json"));
        }

        beginTest("A missing preference loads the complete compiled default");
        {
            TemporaryDirectory temporary;
            expect(temporary.wasCreated());
            const auto file = temporary.child("missing.json");
            const DashboardLayoutStore store(file);

            expect(store.getStorageFile() == file);
            expect(store.load() == DashboardLayout::defaultSplits);
            expect(!file.exists());
        }

        beginTest("An explicit commit round-trips exactly one version and four integers");
        {
            TemporaryDirectory temporary;
            expect(temporary.wasCreated());
            const auto file = temporary.child("layout.json");
            const DashboardLayoutStore store(file);
            constexpr DashboardLayoutSplits saved { 18, 30, 20, 38 };

            expect(store.commit(saved));
            expect(DashboardLayoutStore(file).load() == saved);

            const auto parsed = juce::JSON::parse(file);
            const auto* object = parsed.getDynamicObject();
            expect(object != nullptr);
            if (object != nullptr) {
                expectEquals(object->getProperties().size(), 5);
                expect(object->getProperty("version").isInt());
                expect(object->getProperty("horizontal").isInt());
                expect(object->getProperty("upper").isInt());
                expect(object->getProperty("lowerLeft").isInt());
                expect(object->getProperty("lowerRight").isInt());
                expectEquals(static_cast<int>(object->getProperty("version")),
                    DashboardLayoutStore::schemaVersion);
            }
        }

        beginTest("The last successfully completed explicit commit wins");
        {
            TemporaryDirectory temporary;
            expect(temporary.wasCreated());
            const auto file = temporary.child("layout.json");
            const DashboardLayoutStore first(file);
            const DashboardLayoutStore second(file);
            constexpr DashboardLayoutSplits earlier { 16, 24, 16, 36 };
            constexpr DashboardLayoutSplits later { 26, 40, 30, 42 };

            expect(first.commit(earlier));
            expect(second.commit(later));
            expect(first.load() == later);

            const auto contentsBeforeRejectedCommit = file.loadFileAsString();
            expect(!first.commit({ 13, 36, 28, 40 }));
            expect(file.loadFileAsString() == contentsBeforeRejectedCommit);
            expect(second.load() == later);
        }

        beginTest("Malformed input never partially applies otherwise valid split fields");
        {
            TemporaryDirectory temporary;
            expect(temporary.wasCreated());
            const auto file = temporary.child("layout.json");
            const DashboardLayoutStore store(file);

            const std::array malformedPreferences {
                juce::String { },
                juce::String { "{" },
                juce::String { "[]" },
                juce::String {
                    R"json({"horizontal":18,"upper":30,"lowerLeft":20,"lowerRight":38})json" },
                juce::String {
                    R"json({"version":2,"horizontal":18,"upper":30,"lowerLeft":20,"lowerRight":38})json" },
                juce::String {
                    R"json({"version":"1","horizontal":18,"upper":30,"lowerLeft":20,"lowerRight":38})json" },
                juce::String {
                    R"json({"version":1,"horizontal":18,"upper":30,"lowerLeft":20})json" },
                juce::String {
                    R"json({"version":1,"horizontal":18.0,"upper":30,"lowerLeft":20,"lowerRight":38})json" },
                juce::String {
                    R"json({"version":1,"horizontal":13,"upper":30,"lowerLeft":20,"lowerRight":38})json" },
                juce::String {
                    R"json({"version":1,"horizontal":18,"upper":30,"lowerLeft":20,"lowerRight":38,"extra":1})json" },
            };

            for (const auto& malformed : malformedPreferences) {
                expect(file.replaceWithText(malformed));
                expect(store.load() == DashboardLayout::defaultSplits);
            }

            expect(file.deleteFile());
            expect(file.createDirectory().wasOk());
            expect(store.load() == DashboardLayout::defaultSplits);
        }

        beginTest("Injected preference paths remain independent");
        {
            TemporaryDirectory temporary;
            expect(temporary.wasCreated());
            const DashboardLayoutStore first(temporary.child("first/layout.json"));
            const DashboardLayoutStore second(temporary.child("second/layout.json"));
            constexpr DashboardLayoutSplits firstLayout { 14, 24, 16, 36 };
            constexpr DashboardLayoutSplits secondLayout { 26, 40, 30, 42 };

            expect(first.commit(firstLayout));
            expect(second.commit(secondLayout));
            expect(first.load() == firstLayout);
            expect(second.load() == secondLayout);
        }
    }
};

DashboardLayoutStoreTests dashboardLayoutStoreTests;
} // namespace
} // namespace audio_insight
