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
    // (si se mueve mientras suena, los saltos que provoca llevan fundido: así no hace ruido)
    void setEndSample (juce::int64 e) { if (endSample.exchange (e) != e) edgeMoved = true; }
    // dónde empieza (la línea de Start): lo de antes no suena; el loop de escucha vuelve acá
    void setStartSample (juce::int64 s) { if (startSample.exchange (s) != s) edgeMoved = true; }
    // zona de loop del sample (con Loop activado se repite esta zona; 0/0 = el archivo entero)
    // Si el loop se mueve o cambia de tamaño mientras suena, el cursor se mueve con él y queda en el mismo
    // lugar dentro del loop (así no se escapa del loop al arrastrarlo). Lo aplica el hilo de audio.
    void setLoopRegion (juce::int64 start, juce::int64 end)
    {
        if (loopStart.load() == start && loopEnd.load() == end) return;
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
        lastBlockMs = juce::Time::getMillisecondCounter();
        juce::int64 p = pos.load();
        if (jumpPending.exchange (false) && jumpFrom.load() != p) { jumpOld = jumpFrom.load(); jumpFade = 0.0f; } // (se movió el cursor mientras sonaba)
        const juce::int64 ls = loopStart.load(), le = loopEnd.load();
        const bool region = regionLoop && useB && le > ls && le <= len; // el loop de sección (como en el sampler): sólo en el procesado
        if (remapPending.exchange (false) && region)
        {
            const juce::int64 before = p;
            // el loop "viejo" es el último que usó el audio (no el anterior que llegó del mouse: moviendo
            // rápido llegan varios entre bloque y bloque, y el cursor se escapaba del loop)
            const juce::int64 os = appliedLs, oe = appliedLe;
            if (oe > os && p >= os && p <= oe)         // estaba sonando dentro del loop (o justo en el fin): mismo lugar en el loop nuevo
                p = ls + (p - os) % (le - ls);
            else if (p >= le && (oe <= os || p < oe)) // quedó después del loop nuevo: vuelve adentro
                p = ls + (p - ls) % (le - ls);
            // el cursor saltó (otro loop, o se movió): mini crossfade de lo que venía sonando a lo nuevo
            if (p != before) { jumpOld = before; jumpFade = 0.0f; }
            loopMovedRecently = 13230; // (unos 0,3 s: mientras se arrastra el loop, cada vuelta lleva fundido)
        }
        if (region) { appliedLs = ls; appliedLe = le; } else { appliedLs = appliedLe = 0; }
        if (edgeMoved.exchange (false)) edgeMovedRecently = 13230; // (unos 0,3 s: Start o End se están arrastrando)
        const juce::int64 st = juce::jlimit ((juce::int64) 0, (juce::int64) juce::jmax (0, len - 1), startSample.load());
        if (p < st) // antes de Start no suena nada: arranca desde Start
        {
            if (edgeMovedRecently > 0) { jumpOld = p; jumpFade = 0.0f; } // (Start pasó al cursor mientras sonaba: con fundido)
            p = st;
        }
        int done = 0;
        while (done < info.numSamples)
        {
            if (region && p >= le && p < le + 1) // fin del loop: vuelve al inicio del loop
            {
                // si el loop se está moviendo, el crossfade del audio todavía puede ser el del loop
                // anterior: la vuelta lleva un mini fundido (lo que seguía después del fin -> el inicio)
                if (loopMovedRecently > 0) { jumpOld = p; jumpFade = 0.0f; }
                p = ls;
            }
            if (p >= len)
            {
                if (! region && ! looping) // al final: parar
                {
                    // si End se acaba de mover (pasó al cursor), no se corta de golpe: lo que seguía se
                    // apaga en lo que queda del bloque (hasta 10 ms)
                    if (edgeMovedRecently > 0)
                    {
                        const int tail = juce::jmin (info.numSamples - done, juce::roundToInt (1.0f / fadeStep));
                        const int outCh = info.buffer->getNumChannels();
                        for (int k = 0; k < tail; ++k)
                        {
                            const float gTail = 1.0f - (float) (k + 1) / (float) tail;
                            const int at = (int) p + k;
                            for (int c = 0; c < outCh; ++c)
                            {
                                float v = 0.0f;
                                if (at < src->getNumSamples()) v = src->getSample (juce::jmin (c, srcCh - 1), at);
                                info.buffer->setSample (c, info.startSample + done + k, v * gTail);
                            }
                        }
                        edgeMovedRecently = 0;
                    }
                    break;
                }
                // el loop de sección o el de escucha (desde el principio); si End se movió, con fundido
                if (edgeMovedRecently > 0 && ! region) { jumpOld = p; jumpFade = 0.0f; }
                p = region ? ls : st;
            }
            juce::int64 stop = len;
            if (region && p < le) stop = le;
            const int n = (int) juce::jmin ((juce::int64) (info.numSamples - done), stop - p);
            const bool steady = std::abs (mix - target) < 1e-6f && (swapFade >= 1.0f || ! useB) && jumpFade >= 1.0f;
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
                    const float jf = jumpFade;
                    const int oldIdx = (int) jumpOld + k;
                    if (jumpFade < 1.0f) jumpFade = juce::jmin (1.0f, jumpFade + fadeStep);
                    for (int c = 0; c < outCh; ++c)
                    {
                        auto sampleAt = [&] (const BufferPtr& buf, int at) {
                            if (buf == nullptr || at < 0 || at >= buf->getNumSamples()) return 0.0f;
                            return buf->getSample (juce::jmin (c, buf->getNumChannels() - 1), at);
                        };
                        auto valueAt = [&] (int at) {
                            float vb = sampleAt (b, at);
                            if (sf < 1.0f && prevB != nullptr) vb = sampleAt (prevB, at) * (1.0f - sf) + vb * sf;
                            const float va = sampleAt (a, at);
                            return va * (1.0f - mix) + vb * mix;
                        };
                        float v = valueAt (idx);
                        if (jf < 1.0f) v = valueAt (oldIdx) * (1.0f - jf) + v * jf; // lo de antes del salto, fundiéndose
                        info.buffer->setSample (c, info.startSample + done + k, v);
                    }
                }
            }
            if (swapFade >= 1.0f) swapFade = 1.0f;
            jumpOld += n;
            p += n;
            done += n;
            if (loopMovedRecently > 0) loopMovedRecently = juce::jmax (0, loopMovedRecently - n);
            if (edgeMovedRecently > 0) edgeMovedRecently = juce::jmax (0, edgeMovedRecently - n);
        }
        pos = p;
    }

    // si está sonando (llegó un bloque hace poco), el salto lleva fundido: lo aplica el hilo de audio
    void setNextReadPosition (juce::int64 newPosition) override
    {
        if (juce::Time::getMillisecondCounter() - lastBlockMs.load() < 60u)
        {
            jumpFrom = pos.load();
            jumpPending = true;
        }
        pos = newPosition;
    }
    juce::int64 getNextReadPosition() const override { return pos.load(); }
    juce::int64 getTotalLength() const override
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        const auto& src = b ? b : a;
        const juce::int64 full = src ? src->getNumSamples() : 0, e = endSample.load();
        return (e > 0 && e < full) ? e : full;
    }
    // dos loops distintos: el de escucha (setLooping: al terminar vuelve al principio) y el de sección
    // (setRegionLooping: repite la zona del loop, como el sampler)
    bool isLooping() const override { return looping || (regionLoop && useB); }
    void setLooping (bool shouldLoop) override { looping = shouldLoop; }
    void setRegionLooping (bool on) { regionLoop = on; }

private:
    mutable juce::SpinLock lock;
    BufferPtr a, b, prevB;
    float mix = 1.0f, swapFade = 1.0f, jumpFade = 1.0f, fadeStep = 1.0f / 441.0f;
    juce::int64 jumpOld = 0; // (salto del cursor: de dónde venía) // (solo los toca el hilo de audio, salvo al cambiar buffers)
    std::atomic<juce::int64> pos { 0 }, endSample { 0 }, startSample { 0 }, loopStart { 0 }, loopEnd { 0 };
    std::atomic<bool> remapPending { false };
    std::atomic<bool> useB { true }, looping { false }, regionLoop { false };
    int loopMovedRecently = 0; // (sólo el hilo de audio)
    std::atomic<bool> edgeMoved { false }, jumpPending { false };
    std::atomic<juce::int64> jumpFrom { 0 };
    std::atomic<juce::uint32> lastBlockMs { 0 };
    int edgeMovedRecently = 0; // (sólo el hilo de audio)
    juce::int64 appliedLs = 0, appliedLe = 0; // el loop con el que sonó el último bloque (sólo el hilo de audio)
};
