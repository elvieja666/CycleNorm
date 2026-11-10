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
    // dónde termina la reproducción (la línea de Fin); 0 = hasta el final del archivo
    void setEndSample (juce::int64 e) { endSample = e; }
    // zona de loop del sample (con Loop activado se repite esta zona; 0/0 = el archivo entero)
    void setLoopRegion (juce::int64 start, juce::int64 end) { loopStart = start; loopEnd = end; }
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

        const int full = src->getNumSamples();
        const juce::int64 e = endSample.load();
        const int len = (e > 0 && e < full) ? (int) e : full;
        const int srcCh = src->getNumChannels();
        juce::int64 p = pos.load();
        const juce::int64 ls = loopStart.load(), le = loopEnd.load();
        const bool region = looping && le > ls && le <= len;
        int done = 0;
        while (done < info.numSamples)
        {
            if (region && p >= le && p < le + 1) p = ls; // fin del loop: vuelve al inicio del loop
            if (p >= len)
            {
                if (! looping) break;
                p = region ? ls : 0;
            }
            juce::int64 stop = len;
            if (region && p < le) stop = le;
            const int n = (int) juce::jmin ((juce::int64) (info.numSamples - done), stop - p);
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
        const juce::int64 full = src ? src->getNumSamples() : 0, e = endSample.load();
        return (e > 0 && e < full) ? e : full;
    }
    bool isLooping() const override { return looping; }
    void setLooping (bool shouldLoop) override { looping = shouldLoop; }

private:
    mutable juce::SpinLock lock;
    BufferPtr a, b;
    std::atomic<juce::int64> pos { 0 }, endSample { 0 }, loopStart { 0 }, loopEnd { 0 };
    std::atomic<bool> useB { true }, looping { false };
};
