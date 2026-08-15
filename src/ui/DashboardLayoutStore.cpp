// SPDX-License-Identifier: AGPL-3.0-or-later

#include "DashboardLayoutStore.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>

namespace audio_insight {
namespace {
constexpr auto preferenceDirectoryName = "Audio Insight";
constexpr auto preferenceFileName = "dashboard-layout.json";
constexpr auto processLockName = "audio-insight-dashboard-layout";
constexpr auto lockTimeout = std::chrono::milliseconds { 1000 };
constexpr juce::int64 maximumPreferenceBytes = 4096;

constexpr auto versionProperty = "version";
constexpr auto horizontalProperty = "horizontal";
constexpr auto upperProperty = "upper";
constexpr auto lowerLeftProperty = "lowerLeft";
constexpr auto lowerRightProperty = "lowerRight";
constexpr int propertyCount = 5;

std::timed_mutex& localProcessMutex()
{
    static std::timed_mutex mutex;
    return mutex;
}

class PreferenceLock final {
public:
    PreferenceLock()
        : localLock(localProcessMutex(), std::defer_lock), interProcessLock(processLockName)
    {
        const auto deadline = std::chrono::steady_clock::now() + lockTimeout;
        if (!localLock.try_lock_until(deadline))
            return;

        const auto remaining = std::max(std::chrono::milliseconds::zero(),
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()));
        crossProcessLocked = interProcessLock.enter(static_cast<int>(remaining.count()));
    }

    ~PreferenceLock()
    {
        if (crossProcessLocked)
            interProcessLock.exit();
    }

    [[nodiscard]] bool isLocked() const noexcept
    {
        return crossProcessLocked;
    }

private:
    std::unique_lock<std::timed_mutex> localLock;
    juce::InterProcessLock interProcessLock;
    bool crossProcessLocked = false;
};

std::optional<int> readIntegerProperty(
    const juce::DynamicObject& object, const juce::Identifier& property)
{
    if (!object.hasProperty(property))
        return std::nullopt;

    const auto& value = object.getProperty(property);
    if (!value.isInt() && !value.isInt64())
        return std::nullopt;

    const auto integer = static_cast<juce::int64>(value);
    if (integer < std::numeric_limits<int>::min() || integer > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }

    return static_cast<int>(integer);
}

std::optional<DashboardLayoutSplits> parsePreference(const juce::String& text)
{
    juce::var parsed;
    if (juce::JSON::parse(text, parsed).failed())
        return std::nullopt;

    const auto* object = parsed.getDynamicObject();
    if (object == nullptr || object->getProperties().size() != propertyCount)
        return std::nullopt;

    const auto version = readIntegerProperty(*object, versionProperty);
    const auto horizontal = readIntegerProperty(*object, horizontalProperty);
    const auto upper = readIntegerProperty(*object, upperProperty);
    const auto lowerLeft = readIntegerProperty(*object, lowerLeftProperty);
    const auto lowerRight = readIntegerProperty(*object, lowerRightProperty);

    if (!version.has_value() || *version != DashboardLayoutStore::schemaVersion
        || !horizontal.has_value() || !upper.has_value() || !lowerLeft.has_value()
        || !lowerRight.has_value()) {
        return std::nullopt;
    }

    DashboardLayoutSplits splits { *horizontal, *upper, *lowerLeft, *lowerRight };
    if (!DashboardLayout::isValid(splits))
        return std::nullopt;

    return splits;
}

juce::String serializePreference(const DashboardLayoutSplits& splits)
{
    juce::DynamicObject::Ptr object { new juce::DynamicObject };
    object->setProperty(versionProperty, DashboardLayoutStore::schemaVersion);
    object->setProperty(horizontalProperty, splits.horizontal);
    object->setProperty(upperProperty, splits.upper);
    object->setProperty(lowerLeftProperty, splits.lowerLeft);
    object->setProperty(lowerRightProperty, splits.lowerRight);

    return juce::JSON::toString(juce::var { object.get() },
               juce::JSON::FormatOptions { }.withSpacing(juce::JSON::Spacing::multiLine))
        + "\n";
}
} // namespace

DashboardLayoutStore::DashboardLayoutStore() : DashboardLayoutStore(defaultStorageFile())
{
}

DashboardLayoutStore::DashboardLayoutStore(juce::File storageFileToUse)
    : storageFile(std::move(storageFileToUse))
{
}

juce::File DashboardLayoutStore::defaultStorageFile()
{
    auto root = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);

#if JUCE_MAC
    root = root.getChildFile("Application Support");
#endif

    return root.getChildFile(preferenceDirectoryName).getChildFile(preferenceFileName);
}

const juce::File& DashboardLayoutStore::getStorageFile() const noexcept
{
    return storageFile;
}

DashboardLayoutSplits DashboardLayoutStore::load() const
{
    PreferenceLock lock;
    if (!lock.isLocked() || !storageFile.existsAsFile())
        return DashboardLayout::defaultSplits;

    const auto size = storageFile.getSize();
    if (size <= 0 || size > maximumPreferenceBytes)
        return DashboardLayout::defaultSplits;

    if (const auto parsed = parsePreference(storageFile.loadFileAsString()); parsed.has_value())
        return *parsed;

    return DashboardLayout::defaultSplits;
}

bool DashboardLayoutStore::commit(const DashboardLayoutSplits& splits) const
{
    if (!DashboardLayout::isValid(splits) || storageFile == juce::File())
        return false;

    const auto encoded = serializePreference(splits);
    if (encoded.getNumBytesAsUTF8() > maximumPreferenceBytes)
        return false;

    PreferenceLock lock;
    if (!lock.isLocked())
        return false;

    const auto parent = storageFile.getParentDirectory();
    if (parent.createDirectory().failed())
        return false;

    juce::TemporaryFile temporary(storageFile, juce::TemporaryFile::useHiddenFile);
    {
        juce::FileOutputStream output(temporary.getFile());
        if (!output.openedOk() || !output.writeText(encoded, false, false, "\n"))
            return false;

        output.flush();
        if (output.getStatus().failed())
            return false;
    }

    return temporary.overwriteTargetFileWithTemporary();
}
} // namespace audio_insight
