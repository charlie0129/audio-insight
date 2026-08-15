// SPDX-License-Identifier: AGPL-3.0-or-later

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <iostream>

namespace
{
class ConsoleUnitTestRunner final : public juce::UnitTestRunner
{
protected:
    void logMessage(const juce::String& message) override
    {
        std::cout << message << '\n';
    }
};
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI initialiseJuce;
    ConsoleUnitTestRunner runner;
    runner.setAssertOnFailure(false);
    runner.setPassesAreLogged(false);
    runner.runTestsInCategory("audio-insight", 0x415544494F494E53LL);

    auto failures = 0;

    for (auto index = 0; index < runner.getNumResults(); ++index)
    {
        if (const auto* result = runner.getResult(index))
            failures += result->failures;
    }

    return failures == 0 ? 0 : 1;
}
