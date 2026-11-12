// Fuente de reproducción con dos buffers (original / procesado). Se cambia entre los dos
// sin cortar ni perder la posición, con un fundido corto (10 ms) para que no haga click. También
// hay fundido cuando el procesado cambia mientras suena (al mover un control, el loop, etc.).
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
        prevB = nullptr;
        swapFade = 1.0f;
        mix = useB ? 1.0f : 0.0f;
        const auto len = b ? b->getNumSamples() : (a ? a->getNumSamples() : 0);
        if (pos.load() >= len) pos = 0;
    }
    void setProcessed (BufferPtr processed)
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        prevB = b;             // el que venía sonando, para fundirlo con el nuevo
        b = std::move (processed);
        swapFade = prevB != nullptr ? 0.0f : 1.0f;
    }
    // largo de los fundidos, en muestras del archivo (se llama al elegir un archivo: 10 ms)
    void setFadeLength (int samples) { fadeStep = 1.0f / (float) juce::jmax (1, samples); }
    void setPlayProcessed (bool shouldPlayProcessed) { useB = shouldPlayProcessed; }
    // dónde termina la reproducción (la línea de Fin); 0 = hasta el final del archivo
    void setEndSample (juce::int64 e) { endSample = e; }
    // zona de loop del sample (con Loop activado se repite esta zona; 0/0 = el archivo entero)
    // Si el loop se mueve o cambia de tamaño mientras suena, el cursor se mueve con él y queda en el mismo
    // lugar dentro del loop (así no se escapa del loop al arrastrarlo). Lo aplica el hilo de audio.
    void setLoopRegion (juce::int64 start, juce::int64 end)
    {
        const juce::int64 os = loopStart.load(), oe = loopEnd.load();
        if (os == start && oe == end) return;
        prevLoopStart = os; prevLoopEnd = oe;
        loopStart = start; loopEnd = end;
        remapPending = true;
    }
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
        if (a == nullptr) mix = 1.0f;
        if (b == nullptr) mix = 0.0f;
        const float target = useB ? 1.0f : 0.0f;

        const int full = src->getNumSamples();
        const juce::int64 e = endSample.load();
        const int len = (e > 0 && e < full) ? (int) e : full;
        const int srcCh = src->getNumChannels();
        juce::int64 p = pos.load();
        const juce::int64 ls = loopStart.load(), le = loopEnd.load();
        const bool region = looping && le > ls && le <= len;
        if (remapPending.exchange (false) && region)
        {
            const juce::int64 os = prevLoopStart.load(), oe = prevLoopEnd.load();
            if (oe > os && p >= os && p < oe)          // estaba sonando dentro del loop: mismo lugar en el loop nuevo
                p = ls + (p - os) % (le - ls);
            else if (p >= le && (oe <= os || p < oe)) // quedó después del loop nuevo: vuelve adentro
                p = ls + (p - ls) % (le - ls);
        }
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
            const bool steady = std::abs (mix - target) < 1e-6f && (swapFade >= 1.0f || ! useB);
            if (steady)
            {
                for (int c = 0; c < info.buffer->getNumChannels(); ++c)
                    info.buffer->copyFrom (c, info.startSample + done, *src, juce::jmin (c, srcCh - 1), (int) p, n); // mono -> a los dos lados
            }
            else
            {
                // fundido: original <-> procesado (mix) y procesado viejo -> nuevo (swapFade)
                const int outCh = info.buffer->getNumChannels();
                for (int k = 0; k < n; ++k)
                {
                    const int idx = (int) p + k;
                    mix = target > mix ? juce::jmin (target, mix + fadeStep) : juce::jmax (target, mix - fadeStep);
                    const float sf = swapFade;
                    if (swapFade < 1.0f) swapFade = juce::jmin (1.0f, swapFade + fadeStep);
                    for (int c = 0; c < outCh; ++c)
                    {
                        auto sampleOf = [&] (const BufferPtr& buf) {
                            if (buf == nullptr || idx >= buf->getNumSamples()) return 0.0f;
                            return buf->getSample (juce::jmin (c, buf->getNumChannels() - 1), idx);
                        };
                        float vb = sampleOf (b);
                        if (sf < 1.0f && prevB != nullptr) vb = sampleOf (prevB) * (1.0f - sf) + vb * sf;
                        const float va = sampleOf (a);
                        info.buffer->setSample (c, info.startSample + done + k, va * (1.0f - mix) + vb * mix);
                    }
                }
            }
            if (swapFade >= 1.0f) swapFade = 1.0f;
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
    BufferPtr a, b, prevB;
    float mix = 1.0f, swapFade = 1.0f, fadeStep = 1.0f / 441.0f; // (solo los toca el hilo de audio, salvo al cambiar buffers)
    std::atomic<juce::int64> pos { 0 }, endSample { 0 }, loopStart { 0 }, loopEnd { 0 };
    std::atomic<juce::int64> prevLoopStart { 0 }, prevLoopEnd { 0 };
    std::atomic<bool> remapPending { false };
    std::atomic<bool> useB { true }, looping { false };
};
