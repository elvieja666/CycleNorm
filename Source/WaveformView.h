// Visor: arriba el original, abajo el procesado, con las ondas detectadas marcadas.
// Rueda / dos dedos vertical = zoom, horizontal = desplazar, doble clic = ver todo, clic = mover el cursor.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <functional>
#include <memory>
#include <vector>

class WaveformView : public juce::Component, private juce::Timer
{
public:
    using BufferPtr = std::shared_ptr<const juce::AudioBuffer<float>>;

    std::function<double()> getPlayheadSeconds;  // posición actual (segundos)
    std::function<bool()> isPlaying;
    std::function<void (double)> onSeek;          // clic: mover a esta posición (segundos)
    std::function<void (int)> onSkipDragged;      // arrastrando la línea de Skip: nueva cantidad de semiciclos
    std::function<void (size_t)> onEndDragged;    // arrastrando la línea de Fin: nueva muestra final
    std::function<void (size_t, size_t)> onLoopDragged; // arrastrando una punta del loop: nuevo inicio y fin
    std::function<void (size_t)> onXfadeDragged;  // arrastrando la manija del crossfade: dónde empieza el fundido
    int maxSkip = 64;
    std::function<bool()> isPlayingProcessed;     // para resaltar el carril que suena

    WaveformView() { startTimerHz (30); }

    void setData (BufferPtr original, BufferPtr processed, std::vector<size_t> cycleBounds, double sampleRate, bool resetView)
    {
        orig = std::move (original);
        proc = std::move (processed);
        bounds = std::move (cycleBounds);
        sr = sampleRate;
        if (resetView || viewLen <= 0) { viewStart = 0; viewLen = (double) totalLen(); }
        clampView();
        dirty = true;
        repaint();
    }
    void setProcessed (BufferPtr processed) { proc = std::move (processed); dirty = true; repaint(); }
    void clear() { setData (nullptr, nullptr, {}, 44100.0, true); }
    void refresh() { dirty = true; repaint(); }
    void setMarkers (std::vector<size_t> cycleBounds) { bounds = std::move (cycleBounds); dirty = true; repaint(); }
    // hasta qué muestra llegan las ondas salteadas con Skip (en el procesado se dibujan en gris)
    void setSkipEnd (size_t sample) { if (sample != skipEnd) { skipEnd = sample; dirty = true; repaint(); } }
    // dónde se corta el final (la línea de Fin); el largo total = sin cortar
    void setEndSample (size_t sample) { if (sample != endSample) { endSample = sample; dirty = true; repaint(); } }
    // zona de loop (0/0 = sin loop) y los cruces ascendentes (ondas enteras) donde pueden caer sus puntas
    void setLoop (size_t start, size_t end) { if (start != loopStart || end != loopEnd) { loopStart = start; loopEnd = end; dirty = true; repaint(); } }
    void setLoopBounds (std::vector<size_t> b) { loopBounds = std::move (b); }
    // largo del crossfade del loop en muestras (0 = Off)
    void setLoopXfade (size_t len) { if (len != xfadeLen) { xfadeLen = len; dirty = true; repaint(); } }

    void paint (juce::Graphics& g) override
    {
        if (dirty || cache.getWidth() != getWidth() || cache.getHeight() != getHeight()) renderCache();
        g.drawImageAt (cache, 0, 0);

        if (totalLen() > 0 && getPlayheadSeconds)
        {
            const double x = sampleToX (getPlayheadSeconds() * sr);
            if (x >= 0 && x <= getWidth())
            {
                g.setColour (juce::Colours::white.withAlpha (0.85f));
                g.fillRect ((float) x, 0.0f, 1.5f, (float) getHeight());
            }
        }
    }

    void resized() override { dirty = true; }

    void mouseMove (const juce::MouseEvent& e) override
    {
        if (whichLine (e.position) != 0) setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
        else if (insideLoop (e.position)) setMouseCursor (juce::MouseCursor::DraggingHandCursor);
        else setMouseCursor (juce::MouseCursor::NormalCursor);
    }
    void mouseDown (const juce::MouseEvent& e) override
    {
        const int line = whichLine (e.position);
        // adentro de la zona del loop (no sobre una punta): se prepara para mover el loop entero.
        // Si se suelta sin arrastrar, es un clic normal (mueve el cursor de reproducción).
        movingLoop = false;
        pendingLoopMove = line == 0 && insideLoop (e.position);
        if (pendingLoopMove)
        {
            moveDownX = e.position.x;
            moveStartIdx = indexOfLoopBound (loopStart);
            moveCycles = (int) indexOfLoopBound (loopEnd) - (int) moveStartIdx;
            return;
        }
        draggingSkip = line == 1;
        draggingEnd = line == 2;
        draggingLoop = line >= 3 ? line : 0;
        if (draggingSkip) { dragSkipTo (e.position.x); return; }
        if (draggingEnd) { dragEndTo (e.position.x); return; }
        if (draggingLoop) { dragLoopTo (e.position.x); return; }
        if (totalLen() > 0 && onSeek) onSeek (juce::jlimit (0.0, (double) endOrLen(), xToSample (e.position.x)) / sr);
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (pendingLoopMove)
        {
            if (! movingLoop && e.getDistanceFromDragStartX() != 0 && std::abs (e.position.x - moveDownX) >= 3.0f) movingLoop = true;
            if (movingLoop) moveLoopBy (e.position.x - moveDownX);
            return;
        }
        if (draggingSkip) { dragSkipTo (e.position.x); return; }
        if (draggingEnd) { dragEndTo (e.position.x); return; }
        if (draggingLoop) { dragLoopTo (e.position.x); return; }
        mouseDown (e);
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (pendingLoopMove && ! movingLoop && totalLen() > 0 && onSeek) // clic sin arrastrar: mover el cursor
            onSeek (juce::jlimit (0.0, (double) endOrLen(), xToSample (e.position.x)) / sr);
        pendingLoopMove = movingLoop = false;
        draggingSkip = draggingEnd = false;
        draggingLoop = 0;
        dirty = true; repaint();
        mouseMove (e);
    }
    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        viewStart = 0; viewLen = (double) totalLen(); dirty = true; repaint();
    }
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (totalLen() == 0) return;
        if (std::abs (w.deltaX) > std::abs (w.deltaY))
        {
            viewStart -= w.deltaX * viewLen * 0.5;
        }
        else
        {
            zoomAround (e.position.x, std::pow (2.0, -w.deltaY * 3.0));
        }
        clampView(); dirty = true; repaint();
    }
    void mouseMagnify (const juce::MouseEvent& e, float scale) override
    {
        zoomAround (e.position.x, 1.0 / juce::jmax (0.01f, scale));
        clampView(); dirty = true; repaint();
    }

private:
    BufferPtr orig, proc;
    std::vector<size_t> bounds;
    double sr = 44100.0, viewStart = 0, viewLen = 0;
    juce::Image cache;
    bool dirty = true;
    size_t skipEnd = 0, endSample = 0, loopStart = 0, loopEnd = 0;
    bool draggingSkip = false, draggingEnd = false;
    int draggingLoop = 0; // 3 = inicio del loop, 4 = fin del loop, 5 = manija del crossfade
    size_t xfadeLen = 0;
    std::vector<size_t> loopBounds;
    bool pendingLoopMove = false, movingLoop = false; // arrastrando la zona del loop entera
    float moveDownX = 0;
    size_t moveStartIdx = 0;
    int moveCycles = 0;

    bool insideLoop (juce::Point<float> p) const
    {
        if (! hasLoop() || p.y < getHeight() / 2.0f) return false; // solo en el carril de abajo (procesado)
        const float x0 = (float) sampleToX ((double) loopStart), x1 = (float) sampleToX ((double) loopEnd);
        return p.x > x0 && p.x < x1;
    }
    size_t indexOfLoopBound (size_t sample) const
    {
        auto it = std::lower_bound (loopBounds.begin(), loopBounds.end(), sample);
        return (size_t) std::distance (loopBounds.begin(), it);
    }
    // Mover el loop entero: misma cantidad de ondas, las dos puntas en cruces ascendentes (ondas
    // enteras), entre la línea de Skip y la de Fin.
    void moveLoopBy (float dx)
    {
        if (moveCycles <= 0 || loopBounds.empty() || ! onLoopDragged) return;
        const double from = (double) loopBounds[std::min (moveStartIdx, loopBounds.size() - 1)];
        const double target = from + (double) dx / juce::jmax (1, getWidth()) * viewLen;
        size_t best = moveStartIdx; double bestD = 1e300;
        for (size_t k = 0; k + (size_t) moveCycles < loopBounds.size(); ++k)
        {
            const size_t s0 = loopBounds[k], s1 = loopBounds[k + (size_t) moveCycles];
            if (s0 < skipEnd || s1 > endOrLen()) continue;
            const double d = std::abs ((double) s0 - target);
            if (d < bestD) { bestD = d; best = k; }
        }
        if (best + (size_t) moveCycles >= loopBounds.size()) return;
        const size_t s0 = loopBounds[best], s1 = loopBounds[best + (size_t) moveCycles];
        if (s0 != loopStart || s1 != loopEnd) onLoopDragged (s0, s1);
    }

    bool hasLoop() const { return loopEnd > loopStart && onLoopDragged != nullptr; }
    bool xVisible (double sample) const { const double x = sampleToX (sample); return x >= -1.0 && x <= getWidth() + 1.0; }
    // las puntas del loop saltan de onda entera en onda entera (cruces ascendentes de la fundamental)
    void dragLoopTo (float x)
    {
        if (draggingLoop == 5) // manija del crossfade: se engancha en las ondas enteras adentro del loop
        {
            const double target = xToSample (x);
            size_t best = loopEnd; double bestD = std::abs ((double) loopEnd - target);
            for (size_t v : loopBounds)
            {
                if (v + 16 < loopStart || v > loopEnd) continue;
                const double d = std::abs ((double) v - target);
                if (d < bestD) { bestD = d; best = v; }
            }
            if (onXfadeDragged) onXfadeDragged (best);
            return;
        }
        const double target = xToSample (x);
        const bool isStart = draggingLoop == 3;
        size_t best = isStart ? loopStart : loopEnd; double bestD = 1e300;
        for (size_t v : loopBounds)
        {
            if (isStart && (v < skipEnd || v >= loopEnd)) continue;      // el inicio: después de Skip y antes del fin
            if (! isStart && (v <= loopStart || v > endOrLen())) continue; // el fin: después del inicio y antes de Fin
            const double d = std::abs ((double) v - target);
            if (d < bestD) { bestD = d; best = v; }
        }
        if (onLoopDragged) onLoopDragged (isStart ? best : loopStart, isStart ? loopEnd : best);
    }

    size_t endOrLen() const { return (endSample > 0 && endSample < (size_t) totalLen()) ? endSample : (size_t) totalLen(); }
    bool hasEndLine() const { return totalLen() > 0 && onEndDragged != nullptr; }
    // x de la línea de Fin; si con el zoom quedó fuera de la vista, no se ve (como la de Skip).
    // Pegada al borde derecho (el final del archivo con la vista completa) se corre 2 píxeles para que se vea.
    float endLineRawX() const { return (float) sampleToX ((double) endOrLen()); }
    bool endLineVisible() const { const float x = endLineRawX(); return x >= -1.0f && x <= (float) getWidth() + 1.0f; }
    float endLineX() const { return juce::jlimit (1.0f, (float) getWidth() - 2.0f, endLineRawX()); }
    // 0 = ninguna, 1 = Skip, 2 = Fin (si las dos están cerca, la más cercana)
    float xfadeHandleX() const { return (float) sampleToX ((double) (loopEnd - std::min (xfadeLen, loopEnd))); }
    int whichLine (juce::Point<float> p) const
    {
        if (p.y < getHeight() / 2.0f) return 0; // solo en el carril de abajo (procesado)
        // la manija del crossfade: el cuadradito de abajo de todo
        if (hasLoop() && onXfadeDragged != nullptr && p.y >= (float) getHeight() - 26.0f
            && std::abs (p.x - xfadeHandleX()) <= 9.0f && xVisible ((double) (loopEnd - std::min (xfadeLen, loopEnd))))
            return 5;
        const float dS = hasSkipLine() ? std::abs (p.x - std::max (1.0f, (float) sampleToX (skipLineSample()))) : 1e9f;
        const float dE = (hasEndLine() && endLineVisible()) ? std::abs (p.x - endLineX()) : 1e9f;
        const float dLs = (hasLoop() && xVisible ((double) loopStart)) ? std::abs (p.x - (float) sampleToX ((double) loopStart)) : 1e9f;
        const float dLe = (hasLoop() && xVisible ((double) loopEnd)) ? std::abs (p.x - (float) sampleToX ((double) loopEnd)) : 1e9f;
        const float d[4] = { dS, dE, dLs, dLe };
        int best = 0;
        for (int k = 1; k < 4; ++k) if (d[k] < d[best]) best = k;
        return d[best] > 6.0f ? 0 : best + 1;
    }
    // la línea de Fin salta de semiciclo en semiciclo (corta en un cruce por cero), o al final del archivo
    void dragEndTo (float x)
    {
        const double target = xToSample (x);
        size_t best = (size_t) totalLen(); double bestD = std::abs ((double) totalLen() - target);
        for (size_t v : bounds)
        {
            if (v <= skipEnd) continue; // no antes de donde empieza a normalizar
            const double d = std::abs ((double) v - target);
            if (d < bestD) { bestD = d; best = v; }
        }
        if (onEndDragged) onEndDragged (best);
    }

    // La línea de Skip está donde empieza el semiciclo número Skip. Con Skip en 0, bien a la izquierda.
    double skipLineSample() const { return (double) skipEnd; }
    bool hasSkipLine() const { return totalLen() > 0 && ! bounds.empty() && onSkipDragged != nullptr; }
    void dragSkipTo (float x)
    {
        // salta de corte en corte: 0 = el principio del archivo, N = el corte número N (bounds[N-1]).
        // Así puede pararse en todos los cortes, incluido el primero.
        const double target = xToSample (x);
        int best = 0; double bestD = std::abs (target);
        for (int k = 1; k <= maxSkip && (size_t) (k - 1) < bounds.size(); ++k)
        {
            const double d = std::abs ((double) bounds[(size_t) (k - 1)] - target);
            if (d < bestD) { bestD = d; best = k; }
        }
        if (onSkipDragged) onSkipDragged (best);
    }

    juce::int64 totalLen() const { return orig ? orig->getNumSamples() : 0; }
    double sampleToX (double s) const { return viewLen > 0 ? (s - viewStart) / viewLen * getWidth() : 0; }
    double xToSample (double x) const { return viewStart + x / juce::jmax (1, getWidth()) * viewLen; }

    void zoomAround (float x, double factor)
    {
        const double anchor = xToSample (x);
        const double minLen = juce::jmin ((double) totalLen(), juce::jmax (32.0, (double) getWidth() / 8.0));
        viewLen = juce::jlimit (minLen, (double) totalLen(), viewLen * factor);
        viewStart = anchor - (x / juce::jmax (1, getWidth())) * viewLen;
    }
    void clampView()
    {
        const double len = (double) totalLen();
        viewLen = juce::jlimit (0.0, len, viewLen);
        viewStart = juce::jlimit (0.0, juce::jmax (0.0, len - viewLen), viewStart);
        if (viewStart < viewLen * 0.005) viewStart = 0.0; // a unos píxeles del principio: pegado al principio
        else if (len - (viewStart + viewLen) < viewLen * 0.005) viewStart = juce::jmax (0.0, len - viewLen); // idem al final
    }

    void timerCallback() override
    {
        if (isPlaying && isPlaying()) repaint();
    }

    void drawLane (juce::Graphics& g, const juce::AudioBuffer<float>* buf, juce::Rectangle<int> r,
                   juce::Colour col, const juce::String& title, bool active, size_t greyUntil = 0, bool showSkipLine = false)
    {
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (r); // que un archivo que pasa de 0 dBFS no invada el otro carril
        g.setColour (juce::Colour (0xff15171b));
        g.fillRect (r);
        const float mid = (float) r.getCentreY(), half = r.getHeight() * 0.46f;

        // líneas de -6 dB y centro
        g.setColour (juce::Colour (0xff22262c));
        g.drawHorizontalLine ((int) (mid - half * 0.5f), (float) r.getX(), (float) r.getRight());
        g.drawHorizontalLine ((int) (mid + half * 0.5f), (float) r.getX(), (float) r.getRight());
        g.setColour (juce::Colour (0xff2c3138));
        g.drawHorizontalLine ((int) mid, (float) r.getX(), (float) r.getRight());

        // marcas de cada tramo (onda o semiciclo), si entran (al menos 5 píxeles por tramo)
        if (! bounds.empty() && viewLen > 0)
        {
            auto first = std::lower_bound (bounds.begin(), bounds.end(), (size_t) juce::jmax (0.0, viewStart));
            auto last  = std::upper_bound (bounds.begin(), bounds.end(), (size_t) (viewStart + viewLen));
            const auto inView = (double) std::distance (first, last);
            if (inView > 0 && getWidth() / inView >= 5.0)
            {
                g.setColour (juce::Colour (0xff3a3f2a));
                for (auto it = first; it != last; ++it)
                    g.drawVerticalLine ((int) sampleToX ((double) *it), (float) r.getY(), (float) r.getBottom());
            }
        }

        // zona salteada con Skip: fondo apenas distinto y una línea donde empieza a normalizar
        const float skipX = greyUntil > 0 ? (float) r.getX() + (float) sampleToX ((double) greyUntil) : (float) r.getX();
        if (greyUntil > 0 && skipX > (float) r.getX())
        {
            g.setColour (juce::Colour (0xff1d2025));
            g.fillRect (juce::Rectangle<float> ((float) r.getX(), (float) r.getY(), skipX - (float) r.getX(), (float) r.getHeight()));
        }

        auto drawWave = [&] (juce::Colour waveColour)
        {
            const int w = r.getWidth();
            const double spp = viewLen / juce::jmax (1, w);
            const int nCh = buf->getNumChannels(), n = buf->getNumSamples();
            g.setColour (waveColour);
            if (spp < 1.0)
            {
                juce::Path p;
                const int s0 = juce::jmax (0, (int) std::floor (viewStart)), s1 = juce::jmin (n - 1, (int) std::ceil (viewStart + viewLen) + 1);
                for (int s = s0; s <= s1; ++s)
                {
                    float v = 0; // el canal con mayor valor absoluto
                    for (int c = 0; c < nCh; ++c) { const float x = buf->getSample (c, s); if (std::abs (x) > std::abs (v)) v = x; }
                    const float px = (float) sampleToX (s) + (float) r.getX(), py = mid - juce::jlimit (-1.2f, 1.2f, v) * half;
                    if (s == s0) p.startNewSubPath (px, py); else p.lineTo (px, py);
                }
                g.strokePath (p, juce::PathStrokeType (1.3f));
            }
            else
            {
                for (int x = 0; x < w; ++x)
                {
                    const int s0 = (int) (viewStart + x * spp), s1 = juce::jmin (n, (int) (viewStart + (x + 1) * spp) + 1);
                    float lo = 0, hi = 0;
                    for (int c = 0; c < nCh; ++c)
                    {
                        const float* d = buf->getReadPointer (c);
                        for (int s = juce::jmax (0, s0); s < s1; ++s) { lo = juce::jmin (lo, d[s]); hi = juce::jmax (hi, d[s]); }
                    }
                    const float y0 = mid - juce::jlimit (-1.2f, 1.2f, hi) * half, y1 = mid - juce::jlimit (-1.2f, 1.2f, lo) * half;
                    g.fillRect ((float) (r.getX() + x), y0, 1.0f, juce::jmax (1.0f, y1 - y0));
                }
            }
        };

        if (buf != nullptr && buf->getNumSamples() > 0 && viewLen > 0)
        {
            if (greyUntil > 0 && skipX > (float) r.getX())
            {
                {   // las ondas salteadas, en el gris del original
                    juce::Graphics::ScopedSaveState ss (g);
                    g.reduceClipRegion (juce::Rectangle<float> ((float) r.getX(), (float) r.getY(), skipX - (float) r.getX(), (float) r.getHeight()).getSmallestIntegerContainer());
                    drawWave (juce::Colour (0xff8f98a6));
                }
                {
                    juce::Graphics::ScopedSaveState ss (g);
                    g.reduceClipRegion (juce::Rectangle<float> (skipX, (float) r.getY(), (float) r.getRight() - skipX, (float) r.getHeight()).toNearestInt());
                    drawWave (col);
                }
            }
            else
                drawWave (col);

            if (showSkipLine)
            {
                // línea de Skip (se arrastra): punteada, con una manija arriba
                // (en 0 queda pegada al borde izquierdo; se corre 1 píxel para que se vea entera)
                const float rawX = (float) r.getX() + (float) sampleToX (skipLineSample());
                const float lx = juce::jmax ((float) r.getX() + 1.0f, rawX);
                if (rawX >= (float) r.getX() - 1.0f && lx <= (float) r.getRight())
                {
                    g.setColour (juce::Colour (0xffd0a040).withAlpha (draggingSkip ? 1.0f : 0.85f));
                    const float dashes[] = { 4.0f, 3.0f };
                    g.drawDashedLine (juce::Line<float> (lx, (float) r.getY(), lx, (float) r.getBottom()), dashes, 2, draggingSkip ? 2.0f : 1.2f);
                    juce::Path handle;
                    const float hy = (float) r.getY() + 22.0f;
                    handle.addTriangle (lx - 6.0f, hy, lx + 6.0f, hy, lx, hy + 8.0f);
                    g.fillPath (handle);
                    g.setFont (juce::FontOptions (11.0f));
                    g.drawText ("Skip", juce::Rectangle<float> (lx + 5.0f, (float) r.getBottom() - 18.0f, 40.0f, 14.0f), juce::Justification::centredLeft);
                }
            }
            // lo que queda después de la línea de Fin: oscurecido (no se exporta ni suena)
            if (endSample > 0 && endSample < (size_t) totalLen())
            {
                const float ex = (float) r.getX() + (float) sampleToX ((double) endSample);
                if (ex < (float) r.getRight())
                {
                    g.setColour (juce::Colour (0xcc0e1013));
                    g.fillRect (juce::Rectangle<float> (juce::jmax ((float) r.getX(), ex), (float) r.getY(),
                                                        (float) r.getRight() - juce::jmax ((float) r.getX(), ex), (float) r.getHeight()));
                }
            }
            // zona de loop (verde) con sus dos puntas arrastrables (manijas abajo)
            if (showSkipLine && hasLoop())
            {
                const float x0 = (float) r.getX() + (float) sampleToX ((double) loopStart);
                const float x1 = (float) r.getX() + (float) sampleToX ((double) loopEnd);
                const float a0 = juce::jmax ((float) r.getX(), x0), a1 = juce::jmin ((float) r.getRight(), x1);
                if (a1 > a0)
                {
                    g.setColour (juce::Colour (movingLoop ? 0x483ddc84 : 0x2a3ddc84));
                    g.fillRect (juce::Rectangle<float> (a0, (float) r.getY(), a1 - a0, (float) r.getHeight()));
                }
                const float dashes[] = { 4.0f, 3.0f };
                for (int k = 0; k < 2; ++k)
                {
                    const float lx = k == 0 ? x0 : x1;
                    if (lx < (float) r.getX() - 1.0f || lx > (float) r.getRight() + 1.0f) continue;
                    const bool grabbed = draggingLoop == 3 + k;
                    g.setColour (juce::Colour (0xff3ddc84).withAlpha (grabbed ? 1.0f : 0.9f));
                    g.drawDashedLine (juce::Line<float> (lx, (float) r.getY(), lx, (float) r.getBottom()), dashes, 2, grabbed ? 2.0f : 1.2f);
                    juce::Path handle;
                    const float hy = (float) r.getBottom() - 38.0f;
                    handle.addTriangle (lx - 6.0f, hy + 8.0f, lx + 6.0f, hy + 8.0f, lx, hy);
                    g.fillPath (handle);
                }
                // crossfade (como en el Quick Sampler): lo que entra, antes del inicio, sube; lo que sale,
                // antes del fin, baja. La manija ✕ (abajo) marca dónde empieza el fundido.
                {
                    const float top = (float) r.getY() + 2.0f, bot = (float) r.getBottom() - 2.0f;
                    const size_t xl = std::min (xfadeLen, loopStart);
                    if (xl > 0)
                    {
                        const float xs0 = (float) r.getX() + (float) sampleToX ((double) (loopStart - xl));
                        const float xe0 = (float) r.getX() + (float) sampleToX ((double) (loopEnd - xl));
                        juce::Path in, out;
                        in.addTriangle (xs0, bot, x0, top, x0, bot);
                        out.addTriangle (xe0, top, x1, bot, xe0, bot);
                        juce::Graphics::ScopedSaveState ss (g);
                        g.reduceClipRegion (r);
                        g.setColour (juce::Colours::white.withAlpha (0.16f));
                        g.fillPath (in);
                        g.fillPath (out);
                        g.setColour (juce::Colours::white.withAlpha (0.45f));
                        g.drawLine (xs0, bot, x0, top, 1.0f);
                        g.drawLine (xe0, top, x1, bot, 1.0f);
                    }
                    const float hx = (float) r.getX() + xfadeHandleX();
                    if (onXfadeDragged != nullptr && hx >= (float) r.getX() - 1.0f && hx <= (float) r.getRight() + 1.0f)
                    {
                        const bool grabbedX = draggingLoop == 5;
                        if (xl > 0)
                        {
                            g.setColour (juce::Colours::white.withAlpha (0.8f));
                            g.drawVerticalLine ((int) hx, top, bot);
                        }
                        const auto box = juce::Rectangle<float> (hx - 8.0f, (float) r.getBottom() - 20.0f, 16.0f, 16.0f);
                        g.setColour (grabbedX ? juce::Colours::white : juce::Colour (0xffe8ecf2));
                        g.fillRoundedRectangle (box, 3.0f);
                        g.setColour (juce::Colour (0xff1b1e23));
                        const auto ib = box.reduced (4.5f);
                        g.drawLine (ib.getX(), ib.getY(), ib.getRight(), ib.getBottom(), 1.6f);
                        g.drawLine (ib.getRight(), ib.getY(), ib.getX(), ib.getBottom(), 1.6f);
                    }
                }
                if (x0 >= (float) r.getX() && x0 + 44.0f < (float) r.getRight())
                {
                    g.setColour (juce::Colour (0xff3ddc84));
                    g.setFont (juce::FontOptions (11.0f));
                    g.drawText ("Loop", juce::Rectangle<float> (x0 + 5.0f, (float) r.getY() + 4.0f, 40.0f, 14.0f), juce::Justification::centredLeft);
                }
            }
            if (showSkipLine && hasEndLine() && endLineVisible())
            {
                const float ex = (float) r.getX() + endLineX();
                {
                    g.setColour (juce::Colour (0xffe0605a).withAlpha (draggingEnd ? 1.0f : 0.9f));
                    const float dashes[] = { 4.0f, 3.0f };
                    g.drawDashedLine (juce::Line<float> (ex, (float) r.getY(), ex, (float) r.getBottom()), dashes, 2, draggingEnd ? 2.0f : 1.2f);
                    juce::Path handle;
                    const float hy = (float) r.getY() + 22.0f;
                    handle.addTriangle (ex - 6.0f, hy, ex + 6.0f, hy, ex, hy + 8.0f);
                    g.fillPath (handle);
                    g.setFont (juce::FontOptions (11.0f));
                    g.drawText ("End", juce::Rectangle<float> (ex - 34.0f, (float) r.getBottom() - 18.0f, 30.0f, 14.0f), juce::Justification::centredRight);
                }
            }
            // rojo donde pasa de 0 dBFS
            g.setColour (juce::Colour (0x40ff3030));
            g.drawHorizontalLine ((int) (mid - half), (float) r.getX(), (float) r.getRight());
            g.drawHorizontalLine ((int) (mid + half), (float) r.getX(), (float) r.getRight());
        }

        g.setColour (active ? juce::Colours::white : juce::Colour (0xff8a919c));
        g.setFont (juce::FontOptions (13.0f, active ? juce::Font::bold : juce::Font::plain));
        g.drawText (title + (active ? "  (playing)" : ""), r.reduced (8, 4), juce::Justification::topLeft);
    }

    void renderCache()
    {
        dirty = false;
        const int w = juce::jmax (1, getWidth()), h = juce::jmax (1, getHeight());
        cache = juce::Image (juce::Image::RGB, w, h, true);
        juce::Graphics g (cache);
        g.fillAll (juce::Colour (0xff0e1013));
        if (totalLen() == 0)
        {
            g.setColour (juce::Colour (0xff8a919c));
            g.setFont (juce::FontOptions (16.0f));
            g.drawText (juce::String ("Drop your 808s here (WAV / AIFF)"), getLocalBounds(), juce::Justification::centred);
            return;
        }
        const bool playingProc = isPlayingProcessed ? isPlayingProcessed() : true;
        auto r = getLocalBounds();
        auto top = r.removeFromTop (h / 2).withTrimmedBottom (1);
        drawLane (g, orig.get(), top, juce::Colour (0xff8f98a6), "Original", ! playingProc);
        drawLane (g, proc.get(), r.withTrimmedTop (1), juce::Colour (0xff4fb3ff), "Processed", playingProc, skipEnd, hasSkipLine());
    }
};
