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
        if (resetView || viewLen <= 0) { viewStart = 0; viewLen = (double) totalLen(); zoomSpan = 0.0; }
        clampView();
        dirty = true;
        repaint();
    }
    void setProcessed (BufferPtr processed) { proc = std::move (processed); dirty = true; repaint(); }
    void clear() { setData (nullptr, nullptr, {}, 44100.0, true); }
    void refresh() { dirty = true; repaint(); }
    // el audio se estiró (re-pitch): la vista se estira igual, así se sigue viendo la misma parte del sonido
    void scaleView (double factor) { viewStart *= factor; viewLen *= factor; clampView(); dirty = true; repaint(); }
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
    // Curva de pitch: un punto por onda (muestra del centro de la onda, Hz). Se dibuja sobre el original.
    // appliedSemis: re-pitch que ya tiene la curva (para que el rango vertical quede fijo y la línea se mueva)
    void setPitchCurve (std::vector<std::pair<double, double>> pts, double appliedSemis = 0.0)
    { pitchPts = std::move (pts); pitchApplied = appliedSemis; pitchPreviewSemis = 0.0; frozenValid = false; dirty = true; repaint(); }
    // arrastrar la línea celeste cambia el Pitch (en cents, como la barra)
    std::function<double()> getPitchCents;
    std::function<void (double)> onPitchDragged;
    std::function<double()> getPitchReference; // pitch medido (el de arriba), en MIDI; el imán lo pega a la nota
    bool isDraggingPitch() const { return pitchDragArmed; }

    // vista previa mientras se mueve la barra Pitch: corre la curva estos semitonos sin re-samplear
    void setPitchPreview (double semis)
    {
        if (std::abs (semis - pitchPreviewSemis) < 1e-9) return;
        pitchPreviewSemis = semis; dirty = true; repaint();
    }

    void paint (juce::Graphics& g) override
    {
        // la imagen se arma a la resolución real de la pantalla (Retina = el doble de píxeles), así no se ve borrosa
        const float scale = juce::jmax (1.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
        const int pw = juce::roundToInt ((float) getWidth() * scale), ph = juce::roundToInt ((float) getHeight() * scale);
        if (dirty || cache.getWidth() != pw || cache.getHeight() != ph) { cacheScale = scale; renderCache(); }
        g.drawImage (cache, getLocalBounds().toFloat());

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
        else if (nearPitchCurve (e.position)) setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
        else if (insideLoop (e.position)) setMouseCursor (juce::MouseCursor::DraggingHandCursor);
        else setMouseCursor (juce::MouseCursor::NormalCursor);
    }
    void mouseDown (const juce::MouseEvent& e) override
    {
        const int line = whichLine (e.position);
        // sobre la línea celeste: arrastrando arriba/abajo se cambia el Pitch
        pitchDragArmed = pitchDragging = false;
        // ⌥ Option + clic sobre la línea: vuelve al pitch original (0 cents)
        if (line == 0 && e.mods.isAltDown() && nearPitchCurve (e.position) && onPitchDragged)
        {
            onPitchDragged (0.0);
            return;
        }
        if (line == 0 && nearPitchCurve (e.position) && onPitchDragged && getPitchCents)
        {
            pitchDragArmed = true;
            pitchDragLastY = pitchDownY = e.position.y;
            pitchDragStartCents = getPitchCents();
            pitchDragAnchor = pitchMidiAt (xToSample (e.position.x));
            pitchDragAccum = 0.0;
            pitchDragRef = getPitchReference ? getPitchReference() : pitchDragAnchor;
            if (! std::isfinite (pitchDragRef) || pitchDragRef <= 0.0) pitchDragRef = pitchDragAnchor;
            pitchSemisPerPx = (pitchHi - pitchLo) / juce::jmax (1.0f, pitchAreaH);
            return;
        }
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
        if (pitchDragArmed)
        {
            if (! pitchDragging && std::abs (e.position.y - pitchDownY) >= 2.0f) pitchDragging = true;
            if (pitchDragging) dragPitchTo (e.position.y, e.mods.isShiftDown());
            return;
        }
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
        if (pitchDragArmed && ! pitchDragging && totalLen() > 0 && onSeek) // clic sobre la línea sin arrastrar: mover el cursor
            onSeek (juce::jlimit (0.0, (double) endOrLen(), xToSample (e.position.x)) / sr);
        pitchDragArmed = pitchDragging = false;
        pendingLoopMove = movingLoop = false;
        draggingSkip = draggingEnd = false;
        draggingLoop = 0;
        dirty = true; repaint();
        mouseMove (e);
    }
    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        viewStart = 0; viewLen = (double) totalLen();
        zoomSpan = 0.0; // también el zoom vertical del pitch
        dirty = true; repaint();
    }
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (totalLen() == 0) return;
        const bool overPitch = e.position.y < getHeight() / 2.0f && pitchPts.size() >= 2 && pitchAreaH > 1.0f;
        // ⌥ Option + rueda (o dos dedos) sobre el carril de arriba: zoom vertical del pitch
        if (e.mods.isAltDown() && overPitch)
        {
            const float d = std::abs (w.deltaY) > std::abs (w.deltaX) ? w.deltaY : w.deltaX;
            zoomPitch (e.position.x, std::pow (2.0, d * 3.0));
            return;
        }
        if (w.isSmooth)
        {
            // trackpad: dos dedos = moverse por dentro (horizontal = en el tiempo, vertical = en el pitch)
            viewStart -= w.deltaX * viewLen * 0.5;
            if (zoomSpan > 0.0 && std::abs (w.deltaY) > 0.0f)
            {
                pitchCenter = clampPitchCenter (pitchCenter + w.deltaY * zoomSpan * 0.8, zoomSpan);
                pitchLo = pitchCenter - zoomSpan * 0.5; pitchHi = pitchCenter + zoomSpan * 0.5;
            }
        }
        else if (std::abs (w.deltaX) > std::abs (w.deltaY))
        {
            viewStart -= w.deltaX * viewLen * 0.5;
        }
        else
        {
            zoomAround (e.position.x, std::pow (2.0, -w.deltaY * 3.0)); // rueda del mouse: zoom horizontal
        }
        clampView(); dirty = true; repaint();
    }
    // pellizco en el trackpad: zoom horizontal (en el tiempo)
    void mouseMagnify (const juce::MouseEvent& e, float scale) override
    {
        if (totalLen() == 0) return;
        zoomAround (e.position.x, 1.0 / juce::jmax (0.01f, scale));
        clampView(); dirty = true; repaint();
    }

private:
    BufferPtr orig, proc;
    std::vector<size_t> bounds;
    double sr = 44100.0, viewStart = 0, viewLen = 0;
    juce::Image cache;
    float cacheScale = 1.0f;
    bool dirty = true;
    size_t skipEnd = 0, endSample = 0, loopStart = 0, loopEnd = 0;
    bool draggingSkip = false, draggingEnd = false;
    int draggingLoop = 0; // 3 = inicio del loop, 4 = fin del loop, 5 = manija del crossfade
    size_t xfadeLen = 0;
    std::vector<std::pair<double, double>> pitchPts;
    // zoom vertical del pitch (1 = rango automático) y lo último dibujado (para el zoom con el mouse)
    double pitchPreviewSemis = 0.0, pitchApplied = 0.0;
    bool pitchDragArmed = false, pitchDragging = false;
    float pitchDownY = 0.0f, pitchDragLastY = 0.0f, pitchLineEnd = 0.0f;
    double pitchDragStartCents = 0.0, pitchDragAnchor = 0.0, pitchDragRef = 0.0, pitchDragAccum = 0.0, pitchSemisPerPx = 0.0;
    bool frozenValid = false; double frozenLo = 0.0, frozenHi = 1.0; // rango fijo mientras se mueve Pitch
    double zoomSpan = 0.0, pitchCenter = 0.0, autoSpan = 1.0, pitchLo = 0.0, pitchHi = 1.0;
    float pitchAreaBottom = 0.0f, pitchAreaH = 0.0f;

    // Curva de pitch sobre un carril: eje vertical en notas (escala logarítmica), con una línea por
    // semitono y su nombre. Si casi no cambia, muestra ±50 cents para que se vea el drift.
    // pitch (MIDI fraccionario) de la curva en un sample, interpolado
    double pitchMidiAt (double sample) const
    {
        auto midiOf = [this] (double hz) { return 69.0 + 12.0 * std::log2 (hz / 440.0) + pitchPreviewSemis; };
        if (sample <= pitchPts.front().first) return midiOf (pitchPts.front().second);
        if (sample >= pitchPts.back().first) return midiOf (pitchPts.back().second);
        auto it = std::lower_bound (pitchPts.begin(), pitchPts.end(), sample,
                                    [] (const std::pair<double, double>& p, double v) { return p.first < v; });
        const auto& b = *it; const auto& a = *(it - 1);
        const double t = (sample - a.first) / juce::jmax (1e-9, b.first - a.first);
        return midiOf (a.second) + t * (midiOf (b.second) - midiOf (a.second));
    }

    void drawPitchCurve (juce::Graphics& g, juce::Rectangle<int> r)
    {
        if (pitchPts.size() < 2 || viewLen <= 0) return;
        auto midiOf = [this] (double hz) { return 69.0 + 12.0 * std::log2 (hz / 440.0) + pitchPreviewSemis; };
        double lo = 1e9, hi = -1e9;
        // rango: la curva original (sin re-pitch) y la actual juntas, así al mover Pitch se mueve la línea y no la grilla
        for (auto& p : pitchPts)
        {
            const double m = midiOf (p.second), m0 = m - pitchPreviewSemis - pitchApplied;
            lo = std::min ({ lo, m, m0 }); hi = std::max ({ hi, m, m0 });
        }
        const double mid = 0.5 * (lo + hi);
        if (hi - lo < 1.0) { lo = mid - 0.5; hi = mid + 0.5; }
        const double pad = (hi - lo) * 0.12;
        lo -= pad; hi += pad;
        if (std::abs (pitchPreviewSemis) > 1e-9 && frozenValid)
        {
            // moviendo la barra Pitch: la grilla queda quieta y se mueve la línea;
            // el rango sólo se agranda si la línea se sale
            double clo = 1e9, chi = -1e9;
            for (auto& p : pitchPts) { const double m = midiOf (p.second); clo = std::min (clo, m); chi = std::max (chi, m); }
            const double margin = (frozenHi - frozenLo) * 0.05;
            frozenLo = std::min (frozenLo, clo - margin);
            frozenHi = std::max (frozenHi, chi + margin);
            lo = frozenLo; hi = frozenHi;
        }
        else { frozenLo = lo; frozenHi = hi; frozenValid = true; }
        autoSpan = hi - lo;
        if (zoomSpan > 0.0) // zoom vertical: alto fijo en semitonos, centrado donde se hizo zoom
        {
            lo = pitchCenter - zoomSpan * 0.5; hi = pitchCenter + zoomSpan * 0.5;
        }
        const auto area = r.toFloat().reduced (0.0f, 22.0f);
        pitchLo = lo; pitchHi = hi; pitchAreaBottom = area.getBottom(); pitchAreaH = area.getHeight();
        auto yOf = [&] (double m) { return area.getBottom() - (float) ((m - lo) / (hi - lo)) * area.getHeight(); };

        juce::Graphics::ScopedSaveState ss (g);
        g.reduceClipRegion (r);
        // líneas de semitono, con nombre (si no son demasiadas)
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        g.setFont (juce::FontOptions (10.5f));
        auto noteStr = [&] (int m) { return juce::String (names[((m % 12) + 12) % 12]) + juce::String (m / 12 - 1); };
        // nombres: las notas en gris claro y negrita; los cents más apagados
        auto label = [&] (float y, const juce::String& txt, bool strong) {
            const float w = strong ? 30.0f : 58.0f;
            const auto lb = juce::Rectangle<float> ((float) r.getRight() - w - 4.0f, y - 6.5f, w, 13.0f);
            g.setColour (juce::Colour (0xdd0b0c0e));
            g.fillRoundedRectangle (lb, 2.0f);
            g.setColour (juce::Colour (strong ? 0xffd5d7da : 0xff8b9096));
            g.setFont (juce::FontOptions (strong ? 11.0f : 10.5f, strong ? juce::Font::bold : juce::Font::plain));
            g.drawText (txt, lb, juce::Justification::centred);
        };
        // líneas: la de una nota, entera y clara (C más todavía: marca la octava);
        // la de cents, punteada y tenue
        auto noteLine = [&] (float y, int midiNote, float x1) {
            const bool isC = ((midiNote % 12) + 12) % 12 == 0;
            g.setColour (juce::Colour (isC ? 0x80ffffff : 0x4dffffff));
            g.fillRect (juce::Rectangle<float> ((float) r.getX(), std::round (y) - 0.5f, x1 - (float) r.getX(), isC ? 1.5f : 1.0f));
        };
        auto centsLine = [&] (float y, float x1) {
            g.setColour (juce::Colour (0x33ffffff));
            const float dashes[] = { 3.0f, 4.0f };
            g.drawDashedLine (juce::Line<float> ((float) r.getX(), std::round (y), x1, std::round (y)), dashes, 2, 1.0f);
        };
        const double spanSemis = hi - lo;
        // columna de los nombres: las líneas (grilla y curva) se cortan antes de llegar
        const float lineEnd = (float) r.getRight() - ((spanSemis >= 1.5 ? 30.0f : 58.0f) + 4.0f + 8.0f);
        pitchLineEnd = lineEnd;
        if (spanSemis >= 1.5)
        {
            // semitonos (de a 2 si el rango es muy grande; siempre aparece C)
            const int s1 = (int) std::floor (hi);
            const int stepS = (s1 - (int) std::ceil (lo)) > 14 ? 2 : 1;
            int s0 = (int) std::ceil (lo);
            while (((s0 % stepS) + stepS) % stepS != 0) ++s0;
            for (int m = s0; m <= s1; m += stepS)
            {
                const float y = yOf ((double) m);
                noteLine (y, m, lineEnd);
                label (y, noteStr (m), true);
            }
        }
        else
        {
            // zoom fino: líneas de cents (paso para que entren unas 4 a 10), la nota más fuerte
            const double spanC = spanSemis * 100.0;
            double stepC = 0.1;
            for (double cand : { 50.0, 25.0, 10.0, 5.0, 2.0, 1.0, 0.5, 0.2, 0.1 })
                if (spanC / cand >= 4.0) { stepC = cand; break; }
            const double c0 = std::ceil (lo * 100.0 / stepC) * stepC, c1 = hi * 100.0;
            for (double cc = c0; cc <= c1 + 1e-9; cc += stepC)
            {
                const double m = cc / 100.0;
                // siempre desde la nota de abajo: C1, C1 +25c, C1 +50c, C1 +75c, C#1...
                int note = (int) std::floor (m + 1e-6);
                double dev = (m - note) * 100.0;
                if (dev > 100.0 - 1e-4) { ++note; dev = 0.0; }
                const bool isNote = std::abs (dev) < 1e-4;
                const float y = yOf (m);
                if (isNote) noteLine (y, note, lineEnd); else centsLine (y, lineEnd);
                const int dec = stepC < 1.0 ? 1 : 0;
                label (y, isNote ? noteStr (note) : noteStr (note) + " " + (dev > 0 ? "+" : "") + juce::String (dev, dec) + "c", isNote);
            }
        }
        {
            // la curva (recortada antes de la columna de los nombres)
            juce::Graphics::ScopedSaveState clipState (g);
            g.reduceClipRegion (r.withRight ((int) lineEnd));
            juce::Path path;
            bool started = false;
            for (auto& p : pitchPts)
            {
                const float x = (float) r.getX() + (float) sampleToX (p.first);
                const float y = yOf (midiOf (p.second));
                if (! started) { path.startNewSubPath (x, y); started = true; }
                else path.lineTo (x, y);
            }
            g.setColour (juce::Colour (0xff6fd6ff));
            g.strokePath (path, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            for (auto& p : pitchPts)
            {
                const float x = (float) r.getX() + (float) sampleToX (p.first);
                if (x < (float) r.getX() - 4 || x > (float) r.getRight() + 4) continue;
                if (getWidth() / std::max (1.0, (double) pitchPts.size() * (viewLen / (double) std::max<juce::int64> (1, totalLen()))) < 6.0) break; // puntos solo si hay lugar
                g.fillEllipse (x - 2.0f, yOf (midiOf (p.second)) - 2.0f, 4.0f, 4.0f);
            }
        }
        g.setColour (juce::Colour (0xff8b9096));
        const double pitchZoom = zoomSpan > 0.0 ? autoSpan / zoomSpan : 1.0;
        g.drawText (zoomSpan > 0.0 ? juce::String (juce::String::fromUTF8 ("Pitch  ×")) + (pitchZoom < 10 ? juce::String (pitchZoom, 1) : juce::String ((int) std::lround (pitchZoom))) + juce::String::fromUTF8 ("  (⌥ + scroll · double-click = reset)")
                                    : juce::String (juce::String::fromUTF8 ("Pitch  (⌥ + scroll = zoom)")),
                    juce::Rectangle<float> ((float) r.getRight() - 330.0f, (float) r.getY() + 4.0f, 286.0f, 13.0f), juce::Justification::centredRight);
    }
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

    // ¿el mouse está sobre la línea celeste? (carril de arriba, a pocos píxeles de la curva)
    bool nearPitchCurve (juce::Point<float> p) const
    {
        if (pitchPts.size() < 2 || pitchAreaH <= 1.0f || p.y >= getHeight() / 2.0f || p.x > pitchLineEnd) return false;
        const double s = xToSample (p.x);
        if (s < pitchPts.front().first - 2.0 * sr / 30.0 || s > pitchPts.back().first) return false;
        const double m = pitchMidiAt (s);
        const float y = pitchAreaBottom - (float) ((m - pitchLo) / juce::jmax (1e-9, pitchHi - pitchLo)) * pitchAreaH;
        return std::abs (p.y - y) <= 7.0f;
    }
    // arrastre de la línea: Shift = fino (10 veces más lento) y sin imán.
    // Sin Shift, el punto agarrado se pega a la línea de la nota cuando pasa a menos de 5 píxeles.
    void dragPitchTo (float y, bool fine)
    {
        pitchDragAccum += (double) (pitchDragLastY - y) * pitchSemisPerPx * (fine ? 0.1 : 1.0);
        pitchDragLastY = y;
        double target = pitchDragRef + pitchDragAccum; // el pitch medido, movido por el arrastre
        if (! fine)
        {
            const double note = std::round (target);
            if (std::abs (target - note) / juce::jmax (1e-9, pitchSemisPerPx) <= 5.0) target = note;
        }
        double cents = pitchDragStartCents + (target - pitchDragRef) * 100.0;
        cents = std::round (cents * 10.0) / 10.0;
        onPitchDragged (cents);
    }

    // zoom vertical del pitch: factor > 1 acerca. Ancla: la línea celeste en la posición horizontal x
    // (con el mouse a 1 px de error ya serían ~5 cents; así la línea nunca se va de la pantalla)
    double clampPitchCenter (double c, double span) const
    {
        if (! frozenValid) return c;
        const double lo = frozenLo + span * 0.5, hi = frozenHi - span * 0.5;
        return lo <= hi ? juce::jlimit (lo, hi, c) : 0.5 * (frozenLo + frozenHi);
    }
    void zoomPitch (float x, double factor)
    {
        if (pitchPts.size() < 2 || pitchAreaH <= 1.0f) return;
        const double cur = zoomSpan > 0.0 ? zoomSpan : autoSpan;
        if (zoomSpan <= 0.0 && factor <= 1.0) return; // ya está todo a la vista: alejar no hace nada
        const double mAtMouse = pitchMidiAt (xToSample (x));
        const double frac = juce::jlimit (0.0, 1.0, (mAtMouse - pitchLo) / juce::jmax (1e-9, pitchHi - pitchLo));
        const double span = juce::jlimit (0.005, autoSpan, cur / factor); // hasta ~0,5 cents de alto
        zoomSpan = span >= autoSpan * 0.999 ? 0.0 : span; // el alto se guarda en semitonos (no cambia al re-pitchear)
        // la línea queda donde estaba, pero la vista nunca sale del rango completo: así, al alejar del todo,
        // empalma justo con la vista sin zoom (sin saltos ni vibración)
        pitchCenter = clampPitchCenter ((mAtMouse - frac * span) + span * 0.5, span);
        if (zoomSpan <= 0.0) { pitchLo = frozenLo; pitchHi = frozenHi; }
        else { pitchLo = pitchCenter - span * 0.5; pitchHi = pitchCenter + span * 0.5; } // varios eventos antes de repintar
        dirty = true; repaint();
    }

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
        g.setColour (juce::Colour (0xff0f1114));
        g.fillRect (r);
        const float mid = (float) r.getCentreY(), half = r.getHeight() * 0.46f;

        // líneas de -6 dB y centro
        g.setColour (juce::Colour (0xff22262c));
        g.drawHorizontalLine ((int) (mid - half * 0.5f), (float) r.getX(), (float) r.getRight());
        g.drawHorizontalLine ((int) (mid + half * 0.5f), (float) r.getX(), (float) r.getRight());
        g.setColour (juce::Colour (0xff2c3138));
        g.drawHorizontalLine ((int) mid, (float) r.getX(), (float) r.getRight());

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
                // como el osciloscopio de UNISON: la onda como línea (sin rellenar hasta el centro).
                // Columnas de medio píxel con el mínimo y el máximo reales de sus muestras; cada una
                // arranca en la última muestra de la anterior, así la línea no se corta en las subidas.
                auto valueAt = [&] (int s) { // el canal con mayor valor absoluto (igual que la línea de cerca)
                    float v = 0;
                    for (int c = 0; c < nCh; ++c) { const float x = buf->getSample (c, s); if (std::abs (x) > std::abs (v)) v = x; }
                    return v;
                };
                const int cols = w * 2;
                const double spc = viewLen / juce::jmax (1, cols);
                juce::RectangleList<float> peaks;
                for (int x = 0; x < cols; ++x)
                {
                    const int s0 = juce::jmax (0, (int) (viewStart + x * spc)), s1 = juce::jmin (n, (int) (viewStart + (x + 1) * spc) + 1);
                    if (s0 >= n) break;
                    float lo = 1e9f, hi = -1e9f;
                    for (int s = juce::jmax (0, s0 - 1); s < s1; ++s) { const float v = valueAt (s); lo = juce::jmin (lo, v); hi = juce::jmax (hi, v); }
                    if (hi < lo) continue;
                    const float y0 = mid - juce::jlimit (-1.2f, 1.2f, hi) * half, y1 = mid - juce::jlimit (-1.2f, 1.2f, lo) * half;
                    peaks.addWithoutMerging ({ (float) r.getX() + (float) x * 0.5f, y0, 0.85f, juce::jmax (0.9f, y1 - y0) });
                }
                g.fillRectList (peaks);
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
        cache = juce::Image (juce::Image::RGB, juce::jmax (1, juce::roundToInt ((float) w * cacheScale)),
                             juce::jmax (1, juce::roundToInt ((float) h * cacheScale)), true);
        juce::Graphics g (cache);
        g.addTransform (juce::AffineTransform::scale (cacheScale)); // se dibuja en coordenadas normales

        g.fillAll (juce::Colour (0xff0b0c0e));
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
        drawPitchCurve (g, top);
        drawLane (g, proc.get(), r.withTrimmedTop (1), juce::Colour (0xff4fb3ff), "Processed", playingProc, skipEnd, hasSkipLine());
    }
};
