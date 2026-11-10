// Fuente de reproducción con dos buffers (original / procesado). Se cambia entre los dos
// sin cortar ni perder la posición.
#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <memory>

class ABSource : public juce::PositionableAudioSource
{
public:
    using BufferPtr = std::shared_ptr<const juce::AudioBuffer<float>>;

    void setBuffers (BufferPtr original, BufferPtr processed)
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        a = std::move (original);
        b = std::move (processed);
        const auto len = b ? b->getNumSamples() : (a ? a->getNumSamples() : 0);
        if (pos.load() >= len) pos = 0;
    }
    void setProcessed (BufferPtr processed)
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        b = std::move (processed);
    }
    void setPlayProcessed (bool shouldPlayProcessed) { useB = shouldPlayProcessed; }
    bool isPlayingProcessed() const { return useB; }

    void prepareToPlay (int, double) override {}
    void releaseResources() override {}

    void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override
    {
        info.clearActiveBufferRegion();
        const juce::SpinLock::ScopedTryLockType sl (lock);
        if (! sl.isLocked()) return;
        const auto& src = useB ? b : a;
        if (src == nullptr || src->getNumSamples() == 0) return;

        const int len = src->getNumSamples();
        const int srcCh = src->getNumChannels();
        juce::int64 p = pos.load();
        int done = 0;
        while (done < info.numSamples)
        {
            if (p >= len)
            {
                if (! looping) break;
                p = 0;
            }
            const int n = (int) juce::jmin ((juce::int64) (info.numSamples - done), (juce::int64) len - p);
            for (int c = 0; c < info.buffer->getNumChannels(); ++c)
                info.buffer->copyFrom (c, info.startSample + done, *src, juce::jmin (c, srcCh - 1), (int) p, n); // mono -> a los dos lados
            p += n;
            done += n;
        }
        pos = p;
    }

    void setNextReadPosition (juce::int64 newPosition) override { pos = newPosition; }
    juce::int64 getNextReadPosition() const override { return pos.load(); }
    juce::int64 getTotalLength() const override
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        const auto& src = b ? b : a;
        return src ? src->getNumSamples() : 0;
    }
    bool isLooping() const override { return looping; }
    void setLooping (bool shouldLoop) override { looping = shouldLoop; }

private:
    mutable juce::SpinLock lock;
    BufferPtr a, b;
    std::atomic<juce::int64> pos { 0 };
    std::atomic<bool> useB { true }, looping { false };
};
