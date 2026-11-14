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
    std::function<void (size_t)> onStartDragged;  // arrastrando la línea de Start: nueva muestra inicial
    std::function<void (int)> onFadeDragged;      // arrastrando la manija del fade in: cuántos semiciclos dura
    std::function<void (int)> onFadeOutDragged;   // arrastrando la manija del fade out: cuántos semiciclos dura
    std::function<void (size_t, size_t)> onLoopDragged; // arrastrando una punta del loop: nuevo inicio y fin
    std::function<void (size_t)> onXfadeDragged;  // arrastrando la manija del crossfade: dónde empieza el fundido
    int maxSkip = 64;
    std::function<bool()> isPlayingProcessed;     // para resaltar el carril que suena

    WaveformView() { startTimerHz (30); }

    void setData (BufferPtr original, BufferPtr processed, std::vector<size_t> cycleBounds, double sampleRate, bool resetView)
    {
        const double oldTotal = (double) totalLen();
        orig = std::move (original);
        proc = std::move (processed);
        bounds = std::move (cycleBounds);
        sr = sampleRate;
        if (resetView || viewLen <= 0 || oldTotal <= 0) { viewStart = 0; viewLen = (double) totalLen(); zoomSpan = 0.0; pitchPan = 0.0; }
        else
        {   // el audio cambió de largo (re-pitch): se ve la misma parte del sonido, del mismo ancho
            // (el gráfico no cambia: con la vista completa, sigue completa)
            const double k = (double) totalLen() / oldTotal;
            viewStart *= k; viewLen *= k;
        }
        clampView();
        dirty = true;
        repaint();
    }
    void setProcessed (BufferPtr processed) { proc = std::move (processed); dirtyProc = true; repaint(); }
    // lo que se dibuja (si no, los de setData): el original y el procesado sin re-afinar, así el
    // dibujo no cambia al mover el pitch (se estiran al largo del audio afinado)
    void setDisplay (BufferPtr original, BufferPtr processed)
    {
        if (original != showOrig) dirty = true;
        if (processed != showProc) dirtyProc = true;
        showOrig = std::move (original); showProc = std::move (processed);
        repaint();
    }
    void clear() { setData (nullptr, nullptr, {}, 44100.0, true); }
    void refresh() { dirty = true; repaint(); }
    // el audio se estiró (re-pitch): la vista se estira igual, así se sigue viendo la misma parte del sonido
    // hasta qué muestra llegan las ondas salteadas con Skip (en el procesado se dibujan en gris)
    void setSkipEnd (size_t sample) { if (sample != skipEnd) { skipEnd = sample; dirtyProc = true; repaint(); } }
    // dónde se corta el final (la línea de Fin); el largo total = sin cortar
    void setEndSample (size_t sample) { if (sample != endSample) { endSample = sample; repaint(); } } // (lo de después de End se oscurece encima: no rehace la onda) // (las líneas y zonas se dibujan encima de la imagen: no hace falta rehacerla)
    // dónde empieza (la línea de Start); 0 = desde el principio del archivo
    void setStartSample (size_t sample) { if (sample != startSample) { startSample = sample; dirty = true; repaint(); } }
    // fade in desde Start: cuántos semiciclos dura (0 = sin fade)
    void setFadeHalves (int n) { if (n != fadeHalves) { fadeHalves = n; repaint(); } }
    // el renglón del pitch medido: va en el visor Original, en la mitad de arriba de los 40 de abajo
    // y, con loop, el del loop en la mitad de abajo
    void setPitchInfo (const juce::String& t, juce::Colour c, const juce::String& t2 = {}, juce::Colour c2 = {})
    {
        if (t != pitchInfo || c != pitchInfoColour || t2 != pitchInfo2 || c2 != pitchInfoColour2)
        { pitchInfo = t; pitchInfoColour = c; pitchInfo2 = t2; pitchInfoColour2 = c2; repaint(); }
    }
    // cuánto queda entre donde se pide dibujar y donde empieza la tinta de la primera letra
    static float leftBearing (const juce::Font& f, const juce::String& t)
    {
        if (t.isEmpty()) return 0.0f;
        juce::GlyphArrangement ga;
        ga.addLineOfText (f, t.substring (0, 1), 0.0f, 0.0f);
        juce::Path p; ga.createPath (p);
        return p.getBounds().getX();
    }
    juce::String pitchInfo, pitchInfo2; juce::Colour pitchInfoColour { 0xff8b9096 }, pitchInfoColour2;
    void setFadeOutHalves (int n) { if (n != fadeOutHalves) { fadeOutHalves = n; repaint(); } }
    // dónde empieza el fade out: n semiciclos antes de End (en los cortes), sin pasar 'lower'
    static size_t fadeOutStartOf (const std::vector<size_t>& B, size_t end, int halves, size_t lower)
    {
        if (halves <= 0) return end;
        size_t st = end;
        int k = 0;
        for (auto it = B.rbegin(); it != B.rend(); ++it)
        {
            const size_t v = *it;
            if (v >= end) continue;
            if (v < lower) break;
            st = v;
            if (++k >= halves) break;
        }
        return st;
    }
    // dónde termina el fade in: n semiciclos después de Start (en los cortes), sin pasar el loop ni End
    static size_t fadeEndOf (const std::vector<size_t>& B, size_t start, int halves, size_t limit)
    {
        if (halves <= 0) return start;
        size_t e = start;
        int k = 0;
        for (size_t v : B)
        {
            if (v <= start) continue;
            if (v > limit) break;
            e = v;
            if (++k >= halves) break;
        }
        return e;
    }
    // zona de loop (0/0 = sin loop) y los cruces ascendentes (ondas enteras) donde pueden caer sus puntas
    void setLoop (size_t start, size_t end) { if (start != loopStart || end != loopEnd) { loopStart = start; loopEnd = end; repaint(); } }
    void setLoopBounds (std::vector<size_t> b) { loopBounds = std::move (b); }
    // largo del crossfade del loop en muestras (0 = Off)
    void setLoopXfade (size_t len) { if (len != xfadeLen) { xfadeLen = len; repaint(); } }
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
        g.reduceClipRegion (visibleBounds()); // el visor: de borde a borde del recuadro (esquinas rectas)
        // la imagen se arma a la resolución real de la pantalla (Retina = el doble de píxeles), así no se ve borrosa
        const float scale = juce::jmax (1.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
        const int pw = juce::roundToInt ((float) getWidth() * scale), ph = juce::roundToInt ((float) getHeight() * scale);
        if (dirty || cache.getWidth() != pw || cache.getHeight() != ph) { cacheScale = scale; renderCache(); }
        else if (dirtyProc) renderProcLane();
        g.drawImage (cache, getLocalBounds().toFloat());
        // encima de la imagen: las líneas (Start, Skip, End, loop), las zonas oscurecidas y los títulos.
        // Se dibujan cada vez (son baratas), así arrastrar una línea no rehace las ondas.
        if (totalLen() > 0)
        {
            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (0, 0, plotWidth(), getHeight()); // sin pisar el teclado
            const bool playingProc = isPlayingProcessed ? isPlayingProcessed() : true;
            const bool hasPitch = pitchPts.size() >= 2;
            drawLane (g, origToDraw(), topLaneRect(), juce::Colour (0xff8f98a6), "Original", ! playingProc, 0, false,
                      hasPitch ? std::function<void()> ([] {}) : std::function<void()>(), 1);
            drawLane (g, procToDraw(), procLaneRect(), juce::Colour (0xff4fb3ff), "Processed", playingProc, skipEnd, hasSkipLine(), {}, 1);
            drawTabs (g);
            if (hasPitch) drawPitchCurve (g, topLaneRect(), 2); // la pildorita de los cents: sigue a End, encima de lo oscurecido
        }

        // el cursor: en la x que calculó el timer (la misma franja que mandó a redibujar; si se
        // calculara de nuevo acá, el audio ya avanzó y caería afuera de la franja: titilaba)
        if (totalLen() > 0 && getPlayheadSeconds)
        {
            if (! (isPlaying && isPlaying())) playheadPx = sampleToX (getPlayheadSeconds() * sr); // quieto: donde está
            const double x = playheadPx;
            if (x >= plotLeft() && x <= plotWidth())
            {
                g.setColour (juce::Colours::white.withAlpha (0.85f));
                // sólo en el visor de lo que está sonando (Original o Processed), sobre su franja de 185
                const bool procPlaying = isPlayingProcessed ? isPlayingProcessed() : true;
                for (auto lr : { procPlaying ? procLaneRect() : topLaneRect() })
                {
                    const float mid = (float) lr.getCentreY(), half = (float) (lr.getHeight() - 80) / 2.0f;
                    const float vTop = (float) (int) (mid - half), vBot = (float) (int) (mid + half) + 1.0f;
                    g.fillRect ((float) std::round (x), vTop, 1.0f, vBot - vTop); // finita, como en UNISON
                }
            }
        }
        drawZoomBar (g);
    }

    // Con zoom (como el osciloscopio de UNISON): abajo, una barrita con la parte que se ve
    // y "Zoom ×2.4 · Show all"; "Show all" se clickea y vuelve a mostrar todo.
    bool isZoomed() const
    {
        return totalLen() > 0 && (viewStart > 0.5 || viewLen < (double) totalLen() - 0.5 || zoomSpan > 0.0);
    }
    void drawZoomBar (juce::Graphics& g)
    {
        showAllArea = {};
        if (totalLen() == 0) return;
        const auto b = visibleBounds().toFloat();
        const double total = (double) totalLen();
        if (viewStart > 0.5 || viewLen < total - 0.5)
        {
            const auto track = juce::Rectangle<float> (b.getX() + 6.0f, b.getBottom() - 4.0f, b.getWidth() - 12.0f, 3.0f);
            g.setColour (juce::Colours::white.withAlpha (0.08f));
            g.fillRect (track);
            g.setColour (juce::Colours::white.withAlpha (0.45f));
            g.fillRect (track.withX (track.getX() + (float) (viewStart / total) * track.getWidth())
                                         .withWidth (juce::jmax (4.0f, (float) (viewLen / total) * track.getWidth())));
        }
        // arriba a la derecha del visor Original (en la franja del título): siempre "Zoom X n · Y n"
        // (sin zoom: 0); con zoom, además "Show all", que se clickea y vuelve a mostrar todo
        // el número va de 0 (sin zoom) a 100 (el máximo), en escala logarítmica
        auto pct = [] (double f, double fmax) {
            if (f <= 1.001 || fmax <= 1.001) return juce::String ("0");
            return juce::String (juce::jlimit (1, 100, juce::roundToInt (100.0 * std::log (f) / std::log (fmax))));
        };
        const double hf = total / juce::jmax (1.0, viewLen);
        const double hmax = total / juce::jmin (total, juce::jmax (32.0, (double) plotSpan() / 8.0)); // (el mismo tope que zoomAround)
        const double vf = zoomSpan > 0.0 ? autoSpan / zoomSpan : 1.0;
        const double vmax = autoSpan / 0.005;                                                         // (el mismo tope que zoomPitch)
        const juce::String dot = juce::String::fromUTF8 ("  \xc2\xb7  ");
        g.setFont (uiFont (13.0f));
        const auto font = g.getCurrentFont();
        auto wOf = [&] (const juce::String& t) { return juce::GlyphArrangement::getStringWidth (font, t); };
        const juce::String showAll ("Show all");
        const float sw = wOf (showAll);
        const float right = (float) plotWidth() - 5.0f, top = (float) kHiddenPad + 1.0f; // la mitad de abajo de los 40 (la de arriba no se ve)
        // cada número en un lugar del ancho de "100": al pasar de 0 a 100 no se mueve ningún texto
        const float numW = wOf ("100");
        const juce::String pieces[] = { "Zoom  X ", pct (hf, hmax), dot + "Y ", pct (vf, vmax), dot };
        float rowW = 0.0f;
        for (int k = 0; k < 5; ++k) rowW += (k == 1 || k == 3) ? numW : wOf (pieces[k]);
        float x = right - sw - rowW;
        g.setColour (juce::Colour (0xff8b9096));
        for (int k = 0; k < 5; ++k)
        {
            const float pw = (k == 1 || k == 3) ? numW : wOf (pieces[k]);
            g.drawText (pieces[k], juce::Rectangle<float> (x, top, pw + 2.0f, 18.0f), juce::Justification::centredLeft);
            x += pw;
        }
        {   // "Show all" siempre: azul (y se clickea) con zoom; gris sin zoom
            const auto textArea = juce::Rectangle<float> (right - sw, top, sw + 2.0f, 18.0f);
            if (! isZoomed())
            {
                g.setColour (juce::Colour (0xff8b9096).withAlpha (0.6f));
                g.drawText (showAll, textArea, juce::Justification::centredLeft);
            }
            else
            {
                showAllArea = textArea.expanded (4.0f, 3.0f);
                const bool hot = showAllArea.contains (hoverPoint);
                const juce::Colour accentBright (0xff5aa9ff);
                g.setColour (hot ? accentBright.brighter (0.4f) : accentBright);
                g.drawText (showAll, textArea, juce::Justification::centredLeft);
                if (hot) g.fillRect (textArea.getX(), textArea.getCentreY() + 7.0f, sw, 1.0f);
            }
        }
    }

    void resized() override { dirty = true; }
    // lo que se ve: sin los 20 vacíos de arriba (Original) y de abajo (Processed)
    static constexpr int kHiddenPad = 20;
    int visibleBottom = -1; // dónde termina el recuadro (desde arriba del componente; -1 = 20 antes del final)
    juce::Rectangle<int> visibleBounds() const
    {
        auto b = getLocalBounds().withTrimmedTop (kHiddenPad);
        return b.withBottom (visibleBottom > 0 ? visibleBottom : getHeight() - kHiddenPad);
    }
    bool hitTest (int x, int y) override { return visibleBounds().contains (x, y); }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const bool wasHot = showAllArea.contains (hoverPoint);
        const int oldTab = tabAt (hoverPoint);
        hoverPoint = e.position;
        if (tabAt (hoverPoint) != oldTab) repaint(); // la pestaña bajo el mouse se ilumina
        if (wasHot != showAllArea.contains (hoverPoint)) repaint (showAllArea.getSmallestIntegerContainer().expanded (2));
        if (showAllArea.contains (e.position)) setMouseCursor (juce::MouseCursor::PointingHandCursor);
        else if (whichLine (e.position) != 0) setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
        else if (nearPitchCurve (e.position) || pitchTagArea.expanded (2.0f).contains (e.position)) setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
        else if (insideLoop (e.position)) setMouseCursor (juce::MouseCursor::DraggingHandCursor);
        else setMouseCursor (juce::MouseCursor::NormalCursor);
    }
    void mouseExit (const juce::MouseEvent&) override
    {
        if (showAllArea.contains (hoverPoint)) repaint (showAllArea.getSmallestIntegerContainer().expanded (2));
        hoverPoint = { -1.0f, -1.0f };
    }
    void mouseDown (const juce::MouseEvent& e) override
    {
        pitchDragArmed = pitchDragging = false;
        if (showAllArea.contains (e.position)) { showAll(); showAllClicked = true; return; } // "Show all"
        showAllClicked = false;
        // clic en una tecla del piano: el pitch se acomoda justo en esa nota (0 cents)
        if (pitchPts.size() >= 2 && pitchAreaH > 1.0f && e.position.x >= (float) plotWidth()
            && e.position.y <= pitchAreaBottom && e.position.y >= pitchAreaBottom - pitchAreaH
            && getPitchReference && getPitchCents && onPitchDragged)
        {
            const double m = pitchLo + (double) (pitchAreaBottom - e.position.y) / (double) pitchAreaH * (pitchHi - pitchLo);
            const double note = std::round (m), ref = getPitchReference();
            if (ref > 1.0)
            {
                double cents = getPitchCents() + (note - ref) * 100.0;
                cents = juce::jlimit (-600.0, 600.0, std::round (cents * 10.0) / 10.0);
                onPitchDragged (cents);
            }
            return;
        }
        const int line = whichLine (e.position);
        if (line == 8) // la manija del fade out (⌥ + clic: sin fade)
        {
            if (e.mods.isAltDown()) { if (onFadeOutDragged) onFadeOutDragged (0); return; }
            draggingFadeOut = true;
            dragFadeOutTo (e.position.x);
            return;
        }
        if (line == 7) // la manija del fade in (⌥ + clic: sin fade)
        {
            if (e.mods.isAltDown()) { if (onFadeDragged) onFadeDragged (0); return; }
            draggingFade = true;
            dragFadeTo (e.position.x);
            return;
        }
        // sobre la línea celeste: arrastrando arriba/abajo se cambia el Pitch
        pitchDragArmed = pitchDragging = false;
        // ⌥ Option + clic sobre la línea: vuelve al pitch original (0 cents)
        const bool onPitch = nearPitchCurve (e.position) || pitchTagArea.expanded (2.0f).contains (e.position); // la línea o su pildorita
        if (line == 0 && e.mods.isAltDown() && onPitch && onPitchDragged)
        {
            onPitchDragged (0.0);
            return;
        }
        // ⌥ + clic en un lugar vacío de cualquiera de los dos visores: los dos zooms (X e Y) a 0
        if (line == 0 && e.mods.isAltDown() && linesNear (e.position).empty())
        {
            viewStart = 0; viewLen = (double) totalLen();
            zoomSpan = 0.0; pitchPan = 0.0;
            dirty = true; repaint();
            return;
        }
        if (line == 0 && onPitch && onPitchDragged && getPitchCents)
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
        // ⌥ Option + clic sobre la línea de Skip: vuelve a 0 (sin saltear nada)
        const auto nearLines = linesNear (e.position);
        if (e.mods.isAltDown() && std::find (nearLines.begin(), nearLines.end(), 6) != nearLines.end() && onStartDragged)
        {
            onStartDragged (0); // ⌥ + clic sobre Start: vuelve al principio
            return;
        }
        if (e.mods.isAltDown() && std::find (nearLines.begin(), nearLines.end(), 1) != nearLines.end() && onSkipDragged)
        {
            onSkipDragged (0);
            return;
        }
        draggingSkip = line == 1;
        draggingEnd = line == 2;
        draggingStart = line == 6;
        draggingLoop = (line >= 3 && line <= 5) ? line : 0;
        if (draggingStart) { dragStartTo (e.position.x); return; }
        if (draggingSkip) { dragSkipTo (e.position.x); return; }
        if (draggingEnd) { dragEndTo (e.position.x); return; }
        if (draggingLoop) { dragLoopTo (e.position.x); return; }
        if (totalLen() > 0 && onSeek) onSeek (juce::jlimit ((double) startSample, (double) endOrLen(), xToSample (e.position.x)) / sr);
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (showAllClicked) return;
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
        if (draggingFade) { dragFadeTo (e.position.x); return; }
        if (draggingFadeOut) { dragFadeOutTo (e.position.x); return; }
        if (draggingStart) { dragStartTo (e.position.x); return; }
        if (draggingSkip) { dragSkipTo (e.position.x); return; }
        if (draggingEnd) { dragEndTo (e.position.x); return; }
        if (draggingLoop) { dragLoopTo (e.position.x); return; }
        mouseDown (e);
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (showAllClicked) { showAllClicked = false; mouseMove (e); return; }
        if (pendingLoopMove && ! movingLoop && totalLen() > 0 && onSeek) // clic sin arrastrar: mover el cursor
            onSeek (juce::jlimit ((double) startSample, (double) endOrLen(), xToSample (e.position.x)) / sr);
        if (pitchDragArmed && ! pitchDragging && totalLen() > 0 && onSeek) // clic sobre la línea sin arrastrar: mover el cursor
            onSeek (juce::jlimit ((double) startSample, (double) endOrLen(), xToSample (e.position.x)) / sr);
        pitchDragArmed = pitchDragging = false;
        pendingLoopMove = movingLoop = false;
        draggingSkip = draggingEnd = draggingStart = draggingFade = draggingFadeOut = false;
        draggingLoop = 0;
        dirty = true; repaint();
        mouseMove (e);
    }
    // letras del visor con la misma escala que el resto (textScale de UNISON)
    static juce::FontOptions uiFont (float h, int style = juce::Font::plain) { return juce::FontOptions (h * 1.22f, style); }

    void showAll() { viewStart = 0; viewLen = (double) totalLen(); zoomSpan = 0.0; dirty = true; repaint(); }
    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        viewStart = 0; viewLen = (double) totalLen();
        zoomSpan = 0.0; pitchPan = 0.0; // también el zoom vertical del pitch (y vuelve a centrar la nota)
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
            if (zoomSpan <= 0.0 && overPitch && std::abs (w.deltaY) > 0.0f)
            {   // sin zoom: dos dedos arriba / abajo corren el piano hasta 3 semitonos más para cada lado
                pitchPan = juce::jlimit (-3.0, 3.0, pitchPan + w.deltaY * 7.0 * 0.8);
            }
            if (zoomSpan > 0.0 && overPitch && std::abs (w.deltaY) > 0.0f) // (sólo sobre el Original: en el Processed el vertical no mueve nada)
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
    BufferPtr orig, proc, showOrig, showProc;
    const juce::AudioBuffer<float>* origToDraw() const { return showOrig ? showOrig.get() : orig.get(); }
    const juce::AudioBuffer<float>* procToDraw() const { return showProc ? showProc.get() : proc.get(); }
    std::vector<size_t> bounds;
    double sr = 44100.0, viewStart = 0, viewLen = 0;
    juce::Image cache;
    juce::Rectangle<float> showAllArea;
    juce::Rectangle<float> pitchTagArea;           // la pildorita de los cents (se arrastra como la línea)            // "Show all" (con zoom)
    juce::Point<float> hoverPoint { -1.0f, -1.0f };
    bool showAllClicked = false;
    float cacheScale = 1.0f;
    bool dirty = true;      // rehacer toda la imagen de las ondas
    bool dirtyProc = false; // rehacer sólo el carril de abajo (el procesado cambió)
    size_t skipEnd = 0, endSample = 0, startSample = 0, loopStart = 0, loopEnd = 0;
    bool draggingSkip = false, draggingEnd = false, draggingStart = false, draggingFade = false, draggingFadeOut = false;
    int fadeHalves = 0, fadeOutHalves = 0;
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
    double pitchPan = 0.0; // dos dedos arriba / abajo sin zoom: cuánto se corrió el piano (± 3 semitonos)
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

    // layer 0: el fondo (franjas del piano roll, debajo de la onda); layer 1: teclado, cents, curva y etiqueta
    void drawPitchCurve (juce::Graphics& g, juce::Rectangle<int> r, int layer)
    {
        if (pitchPts.size() < 2 || viewLen <= 0) return;
        auto midiOf = [this] (double hz) { return 69.0 + 12.0 * std::log2 (hz / 440.0) + pitchPreviewSemis; };
        // rango fijo: centrado en la nota más cercana a la que viene afinado el 808 (sin re-pitch), con
        // 3 semitonos para arriba y 3 para abajo. Así al cambiar el pitch el gráfico no cambia: sólo se
        // mueve la línea.
        double lo, hi;
        {
            const double r0 = getPitchReference ? getPitchReference() : 0.0;
            double centre;
            if (r0 > 1.0 && std::isfinite (r0)) centre = (double) std::lround (r0 - pitchApplied - pitchPreviewSemis);
            else
            {
                std::vector<double> ms;
                for (auto& p : pitchPts) ms.push_back (midiOf (p.second) - pitchPreviewSemis - pitchApplied);
                std::nth_element (ms.begin(), ms.begin() + (long) ms.size() / 2, ms.end());
                centre = (double) std::lround (ms[ms.size() / 2]);
            }
            lo = centre - 3.5 + pitchPan; hi = centre + 3.5 + pitchPan;
        }
        frozenLo = lo; frozenHi = hi; frozenValid = true;
        autoSpan = hi - lo;
        if (zoomSpan > 0.0) // zoom vertical: alto fijo en semitonos, centrado donde se hizo zoom
        {
            lo = pitchCenter - zoomSpan * 0.5; hi = pitchCenter + zoomSpan * 0.5;
        }
        // el piano roll (franjas, teclado y curva) sólo en la parte del medio (los 185 de la onda): los 40
        // de arriba y de abajo quedan del fondo, como en el visor de abajo
        const auto area = r.toFloat().reduced (0.0f, 40.0f);
        pitchLo = lo; pitchHi = hi; pitchAreaBottom = area.getBottom(); pitchAreaH = area.getHeight();
        auto yOf = [&] (double m) { return area.getBottom() - (float) ((m - lo) / (hi - lo)) * area.getHeight(); };

        // de la línea de 0 dB de arriba a la de abajo (incluidas), como las dibuja drawLane: el teclado llega justo a las dos
        const float lm = (float) r.getCentreY(), lh = (r.getHeight() - 80.0f) * 0.5f;
        const float kTop = (float) (int) (lm - lh), kBot = (float) (int) (lm + lh) + 1.0f;
        juce::Graphics::ScopedSaveState ss (g);
        g.reduceClipRegion (layer == 2 ? r : juce::Rectangle<float> ((float) r.getX(), kTop, (float) r.getWidth(), kBot - kTop).toNearestInt()); // (la pildorita de los cents puede salir)
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        auto noteStr = [&] (int m) { return juce::String (names[((m % 12) + 12) % 12]) + juce::String (m / 12 - 1); };
        auto isBlack = [] (int m) { const int pc = ((m % 12) + 12) % 12; return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10; };
        const double spanSemis = hi - lo;
        // el teclado (a la derecha) y donde terminan las líneas
        constexpr float kbW = (float) kKeyboardW;
        const float kbX = (float) r.getRight() - kbW;
        const float lineEnd = kbX;
        pitchLineEnd = lineEnd;
        // la nota del pitch medido (la de arriba a la derecha), para resaltarla
        const double ref = getPitchReference ? getPitchReference() : 0.0;
        const bool hasRef = ref > 1.0 && std::isfinite (ref);
        const int refNote = hasRef ? (int) std::lround (ref) : -1000;
        const int n0 = (int) std::floor (lo) - 1, n1 = (int) std::ceil (hi) + 1;

        // etiqueta con los cents (a la altura del pitch medido: cuánto se aleja de la nota). Va pegada
        // a la línea de End (o al teclado), encima de la zona oscurecida: se dibuja aparte (layer 2)
        if (layer == 2)
        {
            if (! hasRef) return;
            const double dev = (ref - (double) refNote) * 100.0;
            const float y = juce::jlimit (area.getY() - 14.0f, area.getBottom() + 14.0f, yOf (ref));
            const juce::String t = (dev >= 0.0 ? "+" : juce::String::fromUTF8 ("\xe2\x88\x92")) + juce::String (std::abs (dev), 1) + "c";
            g.setFont (uiFont (11.0f, juce::Font::bold));
            const float tw = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), t) + 10.0f;
            float right = lineEnd - 2.0f;
            if (endSample > 0 && endSample < (size_t) totalLen())
                right = juce::jmin (right, (float) r.getX() + (float) sampleToX ((double) endSample) - 4.0f);
            right = juce::jmax ((float) r.getX() + tw + 2.0f, right);
            const auto tag = juce::Rectangle<float> (right - tw, y - 8.0f, tw, 16.0f);
            pitchTagArea = tag; // (también se agarra de acá para mover el pitch)
            g.setColour (juce::Colour (0xff6fd6ff));
            g.fillRect (tag);
            g.setColour (juce::Colour (0xff0b0c0e));
            g.drawText (t, tag, juce::Justification::centred);
            return;
        }
        if (layer == 0)
        {
            // franjas por semitono (como el piano roll): las de teclas blancas apenas más claras,
            // la de la nota medida en celeste; una línea fina en el centro de cada nota
            for (int m = n0; m <= n1; ++m)
            {
                const float y0 = yOf (m + 0.5), y1 = yOf (m - 0.5);
                const auto row = juce::Rectangle<float> ((float) r.getX(), y0, lineEnd - (float) r.getX(), y1 - y0);
                if (m == refNote) { g.setColour (juce::Colour (0x226fd6ff)); g.fillRect (row); }
                else if (! isBlack (m)) { g.setColour (juce::Colour (0x0bffffff)); g.fillRect (row); }
                g.setColour (juce::Colour (0x12ffffff));
                g.fillRect (row.withHeight (1.0f)); // borde entre carriles
                const bool isC = ((m % 12) + 12) % 12 == 0;
                const float yc = yOf ((double) m);
                if (y1 - y0 >= 6.0f || isC)
                {
                    g.setColour (juce::Colour (m == refNote ? 0x806fd6ff : (isC ? 0x40ffffff : 0x1fffffff)));
                    g.fillRect (juce::Rectangle<float> ((float) r.getX(), std::round (yc) - 0.5f, lineEnd - (float) r.getX(), 1.0f));
                }
            }
            return;
        }

        // con zoom fino: líneas de cents punteadas, con su valor al lado del teclado
        if (spanSemis < 1.5)
        {
            const double spanC = spanSemis * 100.0;
            double stepC = 0.1;
            for (double cand : { 50.0, 25.0, 10.0, 5.0, 2.0, 1.0, 0.5, 0.2, 0.1 })
                if (spanC / cand >= 4.0) { stepC = cand; break; }
            const double c0 = std::ceil (lo * 100.0 / stepC) * stepC, c1 = hi * 100.0;
            g.setFont (uiFont (10.5f));
            for (double cc = c0; cc <= c1 + 1e-9; cc += stepC)
            {
                const double m = cc / 100.0;
                const int note = (int) std::lround (m);
                const double dev = (m - note) * 100.0;
                if (std::abs (dev) < 1e-4) continue; // la nota ya tiene su línea
                const float y = yOf (m);
                g.setColour (juce::Colour (0x2effffff));
                const float dashes[] = { 3.0f, 4.0f };
                g.drawDashedLine (juce::Line<float> ((float) r.getX(), std::round (y), lineEnd - 40.0f, std::round (y)), dashes, 2, 1.0f);
                g.setColour (juce::Colour (0xff7c8188));
                const int dec = stepC < 1.0 ? 1 : 0;
                g.drawText ((dev > 0 ? "+" : "") + juce::String (dev, dec) + "c", juce::Rectangle<float> (lineEnd - 40.0f, y - 6.5f, 38.0f, 13.0f), juce::Justification::centredRight);
            }
        }

        // el teclado (como el de un piano roll, a la derecha): teclas blancas y negras,
        // nombre en las C (y en la nota medida, resaltada en celeste)
        {
            const auto kb = juce::Rectangle<float> (kbX, kTop, kbW, kBot - kTop);
            juce::Graphics::ScopedSaveState ks (g);
            g.reduceClipRegion (kb.toNearestInt());
            g.setColour (juce::Colour (0xffd9dce1));
            g.fillRect (kb);
            for (int m = n0; m <= n1; ++m)
            {
                const float y0 = yOf (m + 0.5), y1 = yOf (m - 0.5), yc = 0.5f * (y0 + y1);
                const int pc = ((m % 12) + 12) % 12;
                if (m == refNote && ! isBlack (m)) { g.setColour (juce::Colour (0xff6fd6ff)); g.fillRect (juce::Rectangle<float> (kbX, y0, kbW, y1 - y0)); }
                if (isBlack (m))
                {
                    g.setColour (m == refNote ? juce::Colour (0xff2f9fd0) : juce::Colour (0xff232325));
                    g.fillRect (juce::Rectangle<float> (kbX, y0, kbW * 0.6f, y1 - y0).reduced (0.0f, 0.5f));
                    // el borde entre las dos blancas pasa por el medio de la negra
                    g.setColour (juce::Colour (0x40000000));
                    g.fillRect (juce::Rectangle<float> (kbX + kbW * 0.6f, std::round (yc) - 0.5f, kbW * 0.4f, 1.0f));
                }
                if (pc == 0 || pc == 5) // borde entre B|C y E|F (dos blancas juntas)
                {
                    g.setColour (juce::Colour (0x40000000));
                    g.fillRect (juce::Rectangle<float> (kbX, std::round (y1) - 0.5f, kbW, 1.0f));
                }
                const float keyH = y1 - y0;
                if ((pc == 0 || m == refNote) && keyH >= 5.0f)
                {
                    g.setColour (m == refNote ? (isBlack (m) ? juce::Colours::white : juce::Colour (0xff0b0c0e)) : juce::Colour (0xff2b2e33));
                    g.setFont (uiFont (juce::jlimit (8.5f, 11.0f, keyH * 0.8f), juce::Font::bold));
                    g.drawText (noteStr (m), juce::Rectangle<float> (kbX + 1.0f, yc - 7.0f, kbW - 3.0f, 14.0f),
                                isBlack (m) ? juce::Justification::centredLeft : juce::Justification::centredRight);
                }
            }
            g.setColour (juce::Colour (0x66000000));
            g.fillRect (juce::Rectangle<float> (kbX, kb.getY(), 1.0f, kb.getHeight()));
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
            g.strokePath (path, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            for (auto& p : pitchPts)
            {
                const float x = (float) r.getX() + (float) sampleToX (p.first);
                if (x < (float) r.getX() - 4 || x > (float) r.getRight() + 4) continue;
                if (plotSpan() / std::max (1.0, (double) pitchPts.size() * (viewLen / (double) std::max<juce::int64> (1, totalLen()))) < 6.0) break; // puntos solo si hay lugar
                g.fillEllipse (x - 1.5f, yOf (midiOf (p.second)) - 1.5f, 3.0f, 3.0f);
            }
        }
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
        const double target = from + (double) dx / plotSpan() * viewLen;
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
    bool endLineVisible() const { const float x = endLineRawX(); return x >= (float) plotLeft() - 1.0f && x <= (float) plotWidth() + 1.0f; }
    float endLineX() const { return juce::jlimit ((float) plotLeft(), (float) plotWidth() - 2.0f, endLineRawX()); }
    // 0 = ninguna, 1 = Skip, 2 = Fin (si las dos están cerca, la más cercana)
    float xfadeHandleX() const { return (float) sampleToX ((double) (loopEnd - std::min (xfadeLen, loopEnd))); }
    // todas las líneas a menos de 6 px (sin la manija del crossfade), en orden lógico:
    // Skip, inicio del loop, fin del loop, End (de izquierda a derecha)
    std::vector<int> linesNear (juce::Point<float> p) const
    {
        if (const int t = tabAt (p)) return { t }; // (con pestañas no hay líneas pegadas que elegir)
        return {};
    }
    int whichLine (juce::Point<float> p) const
    {
        if (p.y < getHeight() / 2.0f) return 0; // solo en el carril de abajo (procesado)
        return tabAt (p); // cada línea se agarra sólo de su pestaña
    }
    // ---- Manijas: cada línea se agarra sólo de su pestaña (con su nombre), y cada una tiene su franja:
    //      arriba Start / End (1ª fila) y los fades (2ª fila); abajo Skip / crossfade (1ª fila desde abajo)
    //      y las puntas del loop (2ª). Así, aunque dos líneas estén pegadas, sus pestañas no se pisan.
    // id: 1 Skip, 2 End, 3 inicio del loop, 4 fin del loop, 5 crossfade, 6 Start, 7 fade in, 8 fade out
    static constexpr float kTabBoxH = 16.0f; // el alto de la caja (dentro del lugar de kTabH)
    static constexpr float kTabH = 18.0f; // la mitad de abajo de los 40 libres (menos 1 de aire con la línea y 1 con el título)
    static juce::Font tabFont() { return uiFont (11.0f); }
    static juce::String tabText (int id)
    {
        switch (id) { case 1: return "Skip"; case 2: return "End"; case 3: return "Loop"; case 4: return "Loop end";
                      case 5: return "Xfade"; case 6: return "Start"; case 7: return "Fade in"; case 8: return "Fade out"; }
        return {};
    }
    juce::Rectangle<float> tabRect (int id) const
    {
        if (totalLen() == 0) return {};
        const auto lane = procLaneRect().toFloat();
        double sample = 0; int side = 1; float y = 0;
        // las dos líneas de 0 dB del procesado (como las dibuja drawLane): cada pestaña a 1 px de una
        const float mid = (float) procLaneRect().getCentreY(), half = juce::jmax (10.0f, (lane.getHeight() - 80.0f) * 0.5f);
        const float topLine = (float) (int) (mid - half), botLine = (float) (int) (mid + half);
        switch (id)
        {
            // arriba de la línea de arriba: Start / End; abajo de ella: los fades.
            // arriba de la línea de abajo: las puntas del loop; abajo de ella: Skip / Xfade. (1 px de aire)
            case 6: if (! hasStartLine()) return {}; sample = (double) startSample; side = 1;  y = topLine - 1.0f - kTabH; break;
            case 2: if (! hasEndLine()) return {};   sample = (double) endOrLen();  side = -1; y = topLine - 1.0f - kTabH; break;
            case 7: if (! hasFadeHandle()) return {};    sample = (double) fadeEndSample();      side = 1;  y = topLine + 2.0f; break;
            case 8: if (! hasFadeOutHandle()) return {}; sample = (double) fadeOutStartSample(); side = -1; y = topLine + 2.0f; break;
            case 1: if (! hasSkipLine()) return {}; sample = skipLineSample(); side = 1; y = botLine + 2.0f; break;
            case 5: if (! hasLoop() || onXfadeDragged == nullptr) return {}; sample = (double) (loopEnd - std::min (xfadeLen, loopEnd)); side = -1; y = botLine + 2.0f; break;
            case 3: if (! hasLoop()) return {}; sample = (double) loopStart; side = 1;  y = topLine + 2.0f; break; // (en la fila de los fades)
            case 4: if (! hasLoop()) return {}; sample = (double) loopEnd;   side = -1; y = botLine - 1.0f - kTabH; break;
            default: return {};
        }
        float x = (float) sampleToX (sample);
        if (x < (float) plotLeft() - 1.0f || x > (float) plotWidth() + 1.0f) return {}; // fuera de la vista (con zoom)
        x = juce::jlimit ((float) plotLeft(), (float) plotWidth() - 2.0f, x); // (igual que las líneas: así queda el mismo aire a cada lado)
        const float w = juce::GlyphArrangement::getStringWidth (tabFont(), tabText (id)) + 12.0f;
        // 1 px de aire con la línea vertical (que ocupa el píxel de x)
        const float lx = std::round (x), xr = lx + 2.0f, xl = lx - 1.0f - w;
        // la caja es de 16 de alto, centrada en el lugar de 18 (1 menos arriba y 1 menos abajo: el texto no se mueve)
        auto rr = juce::Rectangle<float> (side > 0 ? xr : xl, y + (kTabH - kTabBoxH) * 0.5f, w, kTabBoxH);
        // que no se salga del gráfico (pegada al borde, se da vuelta)
        if (rr.getRight() > (float) plotWidth()) rr.setX (xl);
        if (rr.getX() < 0.0f) rr.setX (xr);
        if (id == 3 || id == 8) // Loop y Fade out comparten fila con Fade in: si se pisan, se corren a la derecha de esa
        {
            const auto fi = tabRect (7);
            if (! fi.isEmpty() && rr.getX() < fi.getRight() + 2.0f && rr.getRight() > fi.getX() - 2.0f)
                rr.setX (fi.getRight() + 2.0f); // (siempre a 2 de Fade in, nunca encima)
        }
        return rr;
    }
    int tabAt (juce::Point<float> p) const
    {
        for (int id : { 7, 8, 6, 2, 1, 5, 3, 4 })
            if (tabRect (id).expanded (2.0f, 2.0f).contains (p)) return id;
        return 0;
    }
    bool isDraggingTab (int id) const
    {
        return (id == 1 && draggingSkip) || (id == 2 && draggingEnd) || (id == 6 && draggingStart) || (id == 7 && draggingFade)
            || (id == 8 && draggingFadeOut) || (id >= 3 && id <= 5 && draggingLoop == id);
    }
    void drawTabs (juce::Graphics& g) const
    {
        for (int id : { 3, 4, 5, 1, 6, 2, 7, 8 })
        {
            const auto rr = tabRect (id);
            if (rr.isEmpty()) continue;
            const bool loopTab = id >= 3 && id <= 5 && id != 5;
            const bool hot = isDraggingTab (id) || rr.expanded (2.0f).contains (hoverPoint);
            const juce::Colour base = loopTab ? juce::Colour (0xffe0782f) : juce::Colour (0xffd9dee5);
            g.setColour (hot ? base.brighter (0.25f) : base.withAlpha (0.92f));
            g.fillRect (rr);
            g.setColour (juce::Colour (0xff15181c));
            g.setFont (tabFont());
            g.drawText (tabText (id), rr, juce::Justification::centred);
        }
    }
    // fade in: la manija va arriba (debajo de "Start"), donde termina el fade
    size_t fadeLimit() const { return hasLoop() ? loopStart : endOrLen(); }
    size_t fadeEndSample() const { return fadeEndOf (bounds, startSample, fadeHalves, fadeLimit()); }
    bool hasFadeHandle() const { return totalLen() > 0 && onFadeDragged != nullptr; }
    void dragFadeTo (float x)
    {
        const double target = xToSample (x);
        int best = 0; double bestD = std::abs (target - (double) startSample);
        int k = 0;
        const size_t limit = fadeLimit();
        for (size_t v : bounds)
        {
            if (v <= startSample) continue;
            if (v > limit) break;
            ++k;
            const double d = std::abs ((double) v - target);
            if (d < bestD) { bestD = d; best = k; }
        }
        if (onFadeDragged) onFadeDragged (best);
    }
    // fade out: termina en End; la manija va arriba, donde empieza. Con loop no hay (se corta en el loop)
    size_t fadeOutLower() const { return std::max (fadeEndSample(), startSample); }
    size_t fadeOutStartSample() const { return fadeOutStartOf (bounds, endOrLen(), fadeOutHalves, fadeOutLower()); }
    bool hasFadeOutHandle() const { return totalLen() > 0 && onFadeOutDragged != nullptr && ! hasLoop(); }
    void dragFadeOutTo (float x)
    {
        const double target = xToSample (x);
        const size_t end = endOrLen(), lower = fadeOutLower();
        int best = 0; double bestD = std::abs (target - (double) end);
        int k = 0;
        for (auto it = bounds.rbegin(); it != bounds.rend(); ++it)
        {
            const size_t v = *it;
            if (v >= end) continue;
            if (v < lower) break;
            ++k;
            const double d = std::abs ((double) v - target);
            if (d < bestD) { bestD = d; best = k; }
        }
        if (onFadeOutDragged) onFadeOutDragged (best);
    }
    // la línea de Start: como la de Fin, salta de semiciclo en semiciclo (corta en un cruce por cero)
    bool hasStartLine() const { return totalLen() > 0 && onStartDragged != nullptr; }
    float startLineRawX() const { return (float) sampleToX ((double) startSample); }
    bool startLineVisible() const { const float x = startLineRawX(); return x >= (float) plotLeft() - 1.0f && x <= (float) plotWidth() + 1.0f; }
    float startLineX() const { return juce::jlimit ((float) plotLeft(), (float) plotWidth() - 2.0f, startLineRawX()); }
    void dragStartTo (float x)
    {
        const double target = xToSample (x);
        const size_t limit = hasLoop() ? loopStart : endOrLen(); // antes del loop y de Fin
        size_t best = 0; double bestD = std::abs (target);
        for (size_t v : bounds)
        {
            if (v >= limit) break;
            const double d = std::abs ((double) v - target);
            if (d < bestD) { bestD = d; best = v; }
        }
        if (onStartDragged) onStartDragged (best);
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
    // ancho del gráfico (el tiempo): sin la columna del teclado, así las dos ondas terminan justo donde empieza
    static constexpr int kKeyboardW = 40;
    // la onda va de plotLeft() (20 libres a la izquierda: lo mismo que arriba y abajo del visor) a plotWidth()
    static constexpr int kLeftPad = 20;
    int plotWidth() const { return juce::jmax (kLeftPad + 1, getWidth() - (pitchPts.size() >= 2 ? kKeyboardW : 0)); }
    int plotLeft() const { return kLeftPad; }
    int plotSpan() const { return juce::jmax (1, plotWidth() - kLeftPad); }
    double sampleToX (double s) const { return viewLen > 0 ? plotLeft() + (s - viewStart) / viewLen * plotSpan() : plotLeft(); }
    double xToSample (double x) const { return viewStart + (x - plotLeft()) / plotSpan() * viewLen; }

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
        const double minLen = juce::jmin ((double) totalLen(), juce::jmax (32.0, (double) plotSpan() / 8.0));
        viewLen = juce::jlimit (minLen, (double) totalLen(), viewLen * factor);
        viewStart = anchor - ((x - plotLeft()) / plotSpan()) * viewLen;
    }
    void clampView()
    {
        const double len = (double) totalLen();
        viewLen = juce::jlimit (0.0, len, viewLen);
        viewStart = juce::jlimit (0.0, juce::jmax (0.0, len - viewLen), viewStart);
        if (viewStart < viewLen * 0.005) viewStart = 0.0; // a unos píxeles del principio: pegado al principio
        else if (len - (viewStart + viewLen) < viewLen * 0.005) viewStart = juce::jmax (0.0, len - viewLen); // idem al final
    }

    // sonando: sólo se redibuja la franjita del cursor (donde estaba y donde está), no todo el visor
    void timerCallback() override
    {
        const bool playing = isPlaying && isPlaying();
        int x = -1;
        double px = -1.0;
        if (totalLen() > 0 && getPlayheadSeconds)
        {
            px = sampleToX (getPlayheadSeconds() * sr);
            if (px >= plotLeft() && px <= plotWidth()) x = (int) std::floor (px);
        }
        if (! playing && x == lastPlayheadX) return;
        if (x != lastPlayheadX)
        {
            playheadPx = px;
            if (lastPlayheadX >= 0) repaint (lastPlayheadX - 3, 0, 8, getHeight());
            if (x >= 0) repaint (x - 3, 0, 8, getHeight());
            lastPlayheadX = x;
        }
    }
    int lastPlayheadX = -1;
    double playheadPx = -1.0; // dónde se dibuja el cursor (lo pone el timer)

    void drawLane (juce::Graphics& g, const juce::AudioBuffer<float>* buf, juce::Rectangle<int> r,
                   juce::Colour col, const juce::String& title, bool active, size_t greyUntil = 0, bool showSkipLine = false,
                   std::function<void()> underlay = {}, int pass = 0) // pass 0: el fondo y la onda (en la imagen); 1: lo de encima
    {
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (r); // que un archivo que pasa de 0 dBFS no invada el otro carril
        if (pass == 0)
        {
            g.setColour (juce::Colour (0xff1e1e20)); // alrededor de las ondas: negro (como la barra de la izquierda)
            g.fillRect (r);
        }
        const float mid = (float) r.getCentreY(), half = juce::jmax (10.0f, (r.getHeight() - 80.0f) * 0.5f); // la onda en el medio (185) y 40 libres arriba y abajo
        // las líneas verticales (Start, Skip, End, fades, loop) van sólo entre las dos de 0 dB (los 185)
        const float vTop = (float) (int) (mid - half), vBot = (float) (int) (mid + half) + 1.0f;
        if (pass == 0) // la franja de la onda (185, entre los 40 de la izquierda y el teclado), apenas más clara
        {
            g.setColour (juce::Colour (0xff28282a));
            g.fillRect (juce::Rectangle<float> ((float) (r.getX() + plotLeft()), vTop, (float) plotSpan(), vBot - vTop));
        }

        if (pass == 1) {}
        else if (underlay) underlay(); // el carril de arriba: el fondo del piano roll (debajo de la onda)
        else
        {
            // líneas de -6 dB y centro
            g.setColour (juce::Colour (0xff323235));
            g.drawHorizontalLine ((int) (mid - half * 0.5f), (float) r.getX(), (float) r.getRight());
            g.drawHorizontalLine ((int) (mid + half * 0.5f), (float) r.getX(), (float) r.getRight());
            g.setColour (juce::Colour (0xff3c3c40));
            g.drawHorizontalLine ((int) mid, (float) r.getX(), (float) r.getRight());
        }

        // zona salteada con Skip: fondo apenas distinto y una línea donde empieza a normalizar
        const float skipX = greyUntil > 0 ? (float) r.getX() + (float) sampleToX ((double) greyUntil) : (float) r.getX();
        // el fondo siempre igual: lo único que cambia es el color de la onda. Gris oscuro lo de antes
        // de Start, gris claro lo que saltea Skip, y el color del carril el resto (después de End: su
        // color, oscurecido encima)
        auto xOf = [&] (size_t smp) { return (float) r.getX() + (float) sampleToX ((double) smp); };
        float darkBefore = -1.0e9f; // (después de End: su color, oscurecido encima)
        if (totalLen() > 0)
        {
            // (los fades van del color que les toca: azul, o gris claro si caen en lo salteado)
            darkBefore = xOf (startSample);
        }
        const juce::Colour kDark (0xff3c4048), kLight (0xff8f98a6);

        // greyBefore: hasta esta x (el Skip) las columnas van en gris; así se arma la onda una sola vez
        auto drawWave = [&] (juce::Colour waveColour, float greyBefore = -1.0e9f)
        {
            const int w = plotSpan(); // (sin los 40 de la izquierda ni la columna del teclado)
            const double spp = viewLen / juce::jmax (1, w);
            const int nCh = buf->getNumChannels(), n = buf->getNumSamples();
            // el buffer que se dibuja puede ser el sin re-afinar (otro largo): se estira al largo actual,
            // así el dibujo no cambia con el pitch
            const double k = totalLen() > 0 ? (double) n / (double) totalLen() : 1.0;
            const double vs = viewStart * k, vl = viewLen * k;
            g.setColour (waveColour);
            if (spp < 1.0)
            {
                juce::Path p;
                const int s0 = juce::jmax (0, (int) std::floor (vs)), s1 = juce::jmin (n - 1, (int) std::ceil (vs + vl) + 1);
                for (int s = s0; s <= s1; ++s)
                {
                    float v = 0; // el canal con mayor valor absoluto
                    for (int c = 0; c < nCh; ++c) { const float x = buf->getSample (c, s); if (std::abs (x) > std::abs (v)) v = x; }
                    const float px = (float) sampleToX (s / k) + (float) r.getX(), py = mid - juce::jlimit (-1.2f, 1.2f, v) * half;
                    if (s == s0) p.startNewSubPath (px, py); else p.lineTo (px, py);
                }
                // de cerca: la misma línea, recortada por zonas de color
                const float L = (float) r.getX(), R = (float) r.getRight(), T = (float) r.getY(), H = (float) r.getHeight();
                auto strokeIn = [&] (float x0, float x1, juce::Colour c) {
                    x0 = juce::jmax (x0, L); x1 = juce::jmin (x1, R);
                    if (x1 <= x0) return;
                    juce::Graphics::ScopedSaveState ss (g);
                    g.reduceClipRegion (juce::Rectangle<float> (x0, T, x1 - x0, H).getSmallestIntegerContainer());
                    g.setColour (c);
                    g.strokePath (p, juce::PathStrokeType (1.3f));
                };
                const float g0 = juce::jmax (darkBefore, greyBefore);
                strokeIn (L, darkBefore, kDark);
                strokeIn (darkBefore, g0, kLight);
                strokeIn (g0, R, waveColour);
            }
            else
            {
                // como el osciloscopio de UNISON: la onda como línea (sin rellenar hasta el centro).
                // Columnas de medio píxel con el mínimo y el máximo reales de sus muestras; cada una
                // arranca en la última muestra de la anterior, así la línea no se corta en las subidas.
                const float* chp[2] = { buf->getReadPointer (0), buf->getReadPointer (juce::jmin (1, nCh - 1)) };
                auto valueAt = [&] (int s) { // el canal con mayor valor absoluto (igual que la línea de cerca)
                    const float a = chp[0][s], b = chp[1][s];
                    return std::abs (b) > std::abs (a) ? b : a;
                };
                // una columna por píxel real de la pantalla (Retina incluida), alineada a la grilla: la
                // máxima resolución posible, sin columnas superpuestas (la mitad de trabajo que antes)
                const float colW = 1.0f / juce::jmax (1.0f, cacheScale);
                const int cols = juce::jmax (1, juce::roundToInt ((float) w / colW));
                const double spc = vl / juce::jmax (1, cols);
                juce::RectangleList<float> peaks, greyPeaks, darkPeaks;
                peaks.ensureStorageAllocated (cols);
                for (int x = 0; x < cols; ++x)
                {
                    const int s0 = juce::jmax (0, (int) (vs + x * spc)), s1 = juce::jmin (n, (int) (vs + (x + 1) * spc) + 1);
                    if (s0 >= n) break;
                    float lo = 1e9f, hi = -1e9f;
                    for (int s = juce::jmax (0, s0 - 1); s < s1; ++s) { const float v = valueAt (s); lo = juce::jmin (lo, v); hi = juce::jmax (hi, v); }
                    if (hi < lo) continue;
                    const float y0 = mid - juce::jlimit (-1.2f, 1.2f, hi) * half, y1 = mid - juce::jlimit (-1.2f, 1.2f, lo) * half;
                    const juce::Rectangle<float> cr { (float) r.getX() + (float) plotLeft() + (float) x * colW, y0, colW, juce::jmax (0.6f, y1 - y0) };
                    const float cx = cr.getX() + colW * 0.5f;
                    (cx < darkBefore ? darkPeaks : (cx < greyBefore ? greyPeaks : peaks)).addWithoutMerging (cr);
                }
                g.fillRectList (peaks);
                if (! greyPeaks.isEmpty()) { g.setColour (kLight); g.fillRectList (greyPeaks); }
                if (! darkPeaks.isEmpty()) { g.setColour (kDark); g.fillRectList (darkPeaks); }
            }
        };

        if (buf != nullptr && buf->getNumSamples() > 0 && viewLen > 0)
        {
            if (pass == 0)
                drawWave (col, greyUntil > 0 ? skipX : -1.0e9f); // todo en una pasada (los colores por zona)
            if (pass == 0) return; // lo que sigue va encima de la imagen (paint)

            if (showSkipLine)
            {
                // línea de Skip (se arrastra): punteada, con una manija arriba
                // (en 0 queda pegada al borde izquierdo; se corre 1 píxel para que se vea entera)
                const float rawX = (float) r.getX() + (float) sampleToX (skipLineSample());
                const float lx = juce::jmax ((float) r.getX() + (float) plotLeft(), rawX);
                if (rawX >= (float) r.getX() - 1.0f && lx <= (float) r.getRight())
                {
                    g.setColour (juce::Colour (0xffaeb4bc).withAlpha (draggingSkip ? 1.0f : 0.85f));
                    const float dashes[] = { 4.0f, 3.0f };
                    g.drawDashedLine (juce::Line<float> (std::round (lx) + 0.5f, vTop, std::round (lx) + 0.5f, vBot), dashes, 2, draggingSkip ? 1.5f : 1.0f);
                }
            }
            // lo que queda antes de la línea de Start: oscurecido (no se exporta ni suena)
            if (hasStartLine() && startLineVisible() && (showSkipLine || startSample > 0)) // (en Original sólo si se movió: si no, sería el borde)
            {
                const float sx = (float) r.getX() + startLineX();
                g.setColour (juce::Colour (0xffaeb4bc).withAlpha (draggingStart ? 1.0f : 0.9f));
                g.drawLine (juce::Line<float> (std::round (sx) + 0.5f, vTop, std::round (sx) + 0.5f, vBot), draggingStart ? 1.5f : 1.0f); // entera
            }
            // fade in: la rampa (de Start, abajo, hasta donde termina, arriba) y su manija
            if (showSkipLine && hasFadeHandle())
            {
                const size_t fe = fadeEndSample();
                const float x0 = (float) r.getX() + (float) sampleToX ((double) startSample);
                const float x1 = (float) r.getX() + (float) sampleToX ((double) fe);
                const float top = (float) (int) (mid - half), bot = (float) (int) (mid + half); // de la línea de 0 dB de abajo a la de arriba
                if (fe > startSample && x1 > (float) r.getX() && x0 < (float) r.getRight())
                {
                    g.setColour (juce::Colours::white.withAlpha (0.55f));
                    g.drawLine (x0, bot, x1, top, 1.0f);
                    // hasta dónde llega el fade: una línea vertical
                    g.setColour (juce::Colours::white.withAlpha (0.45f));
                    g.drawLine (std::round (x1) + 0.5f, vTop, std::round (x1) + 0.5f, vBot, 1.0f);
                }
            }
            // fade out: la rampa (de donde empieza, arriba, hasta End, abajo) y su manija
            if (showSkipLine && hasFadeOutHandle())
            {
                const size_t fs = fadeOutStartSample(), fe = endOrLen();
                const float x0 = (float) r.getX() + (float) sampleToX ((double) fs);
                const float x1 = (float) r.getX() + (float) sampleToX ((double) fe);
                const float top = (float) (int) (mid - half), bot = (float) (int) (mid + half); // de la línea de 0 dB de arriba a la de abajo
                if (fe > fs && x1 > (float) r.getX() && x0 < (float) r.getRight())
                {
                    g.setColour (juce::Colours::white.withAlpha (0.55f));
                    g.drawLine (x0, top, x1, bot, 1.0f);
                    // desde dónde arranca el fade: una línea vertical
                    g.setColour (juce::Colours::white.withAlpha (0.45f));
                    g.drawLine (std::round (x0) + 0.5f, vTop, std::round (x0) + 0.5f, vBot, 1.0f);
                }
            }
            // lo que queda después de la línea de Fin: oscurecido (no se exporta ni suena)
            // lo que queda después de la línea de End: oscurecido encima (no se exporta ni suena)
            if (endSample > 0 && endSample < (size_t) totalLen())
            {
                const float ex = (float) r.getX() + (float) sampleToX ((double) endSample);
                const float px1 = (float) (r.getX() + plotWidth());
                if (ex < px1)
                {   // sólo sobre la franja de la onda (alrededor todo negro; el teclado no se oscurece)
                    g.setColour (juce::Colour (0xcc1c1c1e));
                    const float x0 = juce::jmax ((float) (r.getX() + plotLeft()), ex);
                    g.fillRect (juce::Rectangle<float> (x0, vTop, px1 - x0, vBot - vTop));
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
                    g.setColour (juce::Colour (movingLoop ? 0x48e0782f : 0x2ae0782f));
                    g.fillRect (juce::Rectangle<float> (a0, vTop, a1 - a0, vBot - vTop)); // (sólo entre las líneas de 0 dB)
                }
                const float dashes[] = { 4.0f, 3.0f };
                for (int k = 0; k < 2; ++k)
                {
                    const float lx = k == 0 ? x0 : x1;
                    if (lx < (float) r.getX() - 1.0f || lx > (float) r.getRight() + 1.0f) continue;
                    const bool grabbed = draggingLoop == 3 + k;
                    g.setColour (juce::Colour (0xffe0782f).withAlpha (grabbed ? 1.0f : 0.9f));
                    g.drawDashedLine (juce::Line<float> (std::round (lx) + 0.5f, vTop, std::round (lx) + 0.5f, vBot), dashes, 2, grabbed ? 1.5f : 1.0f);
                }
                // crossfade (como en el Quick Sampler): lo que entra, antes del inicio, sube; lo que sale,
                // antes del fin, baja. La manija ✕ (abajo) marca dónde empieza el fundido.
                {
                    const float top = vTop, bot = vBot - 1.0f; // (el crossfade también, entre las líneas de 0 dB)
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
                        juce::ignoreUnused (grabbedX);
                    }
                }
            }
            if (hasEndLine() && endLineVisible() && (showSkipLine || endSample > 0)) // (ídem: en Original sólo si se movió)
            {
                const float ex = (float) r.getX() + endLineX();
                {
                    g.setColour (juce::Colour (0xffaeb4bc).withAlpha (draggingEnd ? 1.0f : 0.9f));
                    g.drawLine (juce::Line<float> (std::round (ex) + 0.5f, vTop, std::round (ex) + 0.5f, vBot), draggingEnd ? 1.5f : 1.0f); // entera
                }
            }
            // la línea de 0 dBFS (arriba y abajo), en blanco
            g.setColour (juce::Colour (0x59ffffff));
            const float x1 = (float) (r.getX() + plotWidth()); // hasta donde termina la onda (no pisa el teclado)
            const float x0 = (float) (r.getX() + plotLeft()); // no entra en los 40 de la izquierda
            g.drawHorizontalLine ((int) (mid - half), x0, x1);
            g.drawHorizontalLine ((int) (mid + half), x0, x1);
        }

        if (pass == 0) return;
        g.setColour (active ? juce::Colours::white : juce::Colour (0xff8a919c));
        g.setFont (uiFont (13.0f, active ? juce::Font::bold : juce::Font::plain));
        // Processed: el título en la mitad de arriba de los 40 libres (la de abajo es de las pestañas);
        // Original: en la de abajo (la de arriba no se ve)
        // a 5 del borde izquierdo (lo mismo que hay arriba hasta las mayúsculas), medido a la letra
        const float titleY = (float) r.getY() + (r.getY() == topLaneRect().getY() ? (float) kHiddenPad : 0.0f) + 1.0f;
        g.drawText (title + (active ? "  (playing)" : ""), juce::Rectangle<float> ((float) r.getX() + (float) plotLeft() + 5.0f - leftBearing (g.getCurrentFont(), title), titleY, (float) r.getWidth() - 16.0f, 18.0f), // 18, con 1 de aire arriba y abajo
                    juce::Justification::centredLeft);
        if (! showSkipLine && pitchInfo.isNotEmpty()) // el Original: el pitch medido, debajo de la onda (mismo tamaño que el título)
        {
            // los dos renglones por columnas, con lugar fijo para cada valor: quedan alineados entre sí
            // y no se mueven cuando cambian los números
            g.setFont (uiFont (13.0f));
            const auto f = g.getCurrentFont();
            auto wOf = [&] (const juce::String& t) { return juce::GlyphArrangement::getStringWidth (f, t); };
            const juce::StringArray a1 = juce::StringArray::fromTokens (pitchInfo, "\t", {}),
                                    a2 = juce::StringArray::fromTokens (pitchInfo2, "\t", {});
            // el ancho de cada columna: el más ancho de los dos renglones, y nunca menos que un valor "grande"
            const juce::String tmpl[] = { "Pitch (sustain)", "G#8", juce::String::fromUTF8 ("−99.9 cents"), "9999.999 Hz", juce::String::fromUTF8 ("tuned −1200.0 cents"), juce::String::fromUTF8 ("drift −99.9 cents") };
            float colW[6];
            for (int k = 0; k < 6; ++k)
                colW[k] = juce::jmax (wOf (tmpl[k]), wOf (a1[k]), wOf (a2[k]));
            const juce::String colon (":"), dot = juce::String::fromUTF8 ("  \xc2\xb7  ");
            const float x0 = (float) r.getX() + (float) plotLeft() + 5.0f - leftBearing (f, "P"); // a 5 de donde empieza la onda, como los títulos
            auto drawRow = [&] (const juce::StringArray& t, juce::Colour c, float y) {
                if (t.isEmpty()) return;
                g.setColour (c);
                float x = x0;
                for (int k = 0; k < t.size() && k < 6; ++k)
                {
                    g.drawText (t[k], juce::Rectangle<float> (x, y, colW[k] + 2.0f, 18.0f), juce::Justification::centredLeft);
                    x += colW[k];
                    if (k == 0) { g.drawText (colon, juce::Rectangle<float> (x, y, 20.0f, 18.0f), juce::Justification::centredLeft); x += wOf (colon) + 5.0f; }
                    else if (k + 1 < t.size()) { g.drawText (k == 1 ? juce::String (" ") : dot, juce::Rectangle<float> (x, y, 40.0f, 18.0f), juce::Justification::centredLeft);
                                                 x += k == 1 ? wOf (" ") : wOf (dot); }
                }
            };
            drawRow (a1, pitchInfoColour,  (float) (int) (mid + half) + 1.0f);
            drawRow (a2, pitchInfoColour2, (float) (int) (mid + half) + 21.0f);
        }
    }

    void renderCache()
    {
        dirty = false;
        const int w = juce::jmax (1, getWidth()), h = juce::jmax (1, getHeight());
        cache = juce::Image (juce::Image::RGB, juce::jmax (1, juce::roundToInt ((float) w * cacheScale)),
                             juce::jmax (1, juce::roundToInt ((float) h * cacheScale)), true);
        juce::Graphics g (cache);
        g.addTransform (juce::AffineTransform::scale (cacheScale)); // se dibuja en coordenadas normales

        g.fillAll (juce::Colour (0xff1e1e20));
        if (totalLen() == 0)
        {
            g.setColour (juce::Colour (0xff8a919c));
            g.setFont (uiFont (16.0f));
            g.drawText (juce::String::fromUTF8 ("Drop your 808s here (WAV / AIFF)  ·  or File ▸ Open… (⌘O)"), getLocalBounds(), juce::Justification::centred);
            return;
        }
        dirtyProc = false;
        const bool playingProc = isPlayingProcessed ? isPlayingProcessed() : true;
        auto r = getLocalBounds();
        auto top = r.removeFromTop (h / 2).withTrimmedBottom (1);
        const bool hasPitch = pitchPts.size() >= 2;
        drawLane (g, origToDraw(), top, juce::Colour (0xff8f98a6), "Original", ! playingProc, 0, false,
                  hasPitch ? std::function<void()> ([&] { drawPitchCurve (g, top, 0); }) : std::function<void()>());
        drawPitchCurve (g, top, 1);
        drawLane (g, procToDraw(), r.withTrimmedTop (1), juce::Colour (0xff4fb3ff), "Processed", playingProc, skipEnd, hasSkipLine());
        drawKeyboardStrip (g);
    }
    juce::Rectangle<int> topLaneRect() const { return getLocalBounds().removeFromTop (getHeight() / 2).withTrimmedBottom (1); }
    juce::Rectangle<int> procLaneRect() const { auto r = getLocalBounds(); r.removeFromTop (getHeight() / 2); return r.withTrimmedTop (1); }
    // sólo cambió el procesado (Ramp, Skip, el crossfade...): se rehace nada más el carril de abajo
    void renderProcLane()
    {
        dirtyProc = false;
        juce::Graphics g (cache);
        g.addTransform (juce::AffineTransform::scale (cacheScale));
        g.reduceClipRegion (procLaneRect());
        g.fillAll (juce::Colour (0xff1e1e20));
        const bool playingProc = isPlayingProcessed ? isPlayingProcessed() : true;
        drawLane (g, procToDraw(), procLaneRect(), juce::Colour (0xff4fb3ff), "Processed", playingProc, skipEnd, hasSkipLine());
        drawKeyboardStrip (g);
    }
    void drawKeyboardStrip (juce::Graphics& g)
    {
        // los 40 de la izquierda, en los dos visores: vacíos (como debajo del teclado)
        for (auto lane : { topLaneRect(), procLaneRect() })
        {
            const auto pad = lane.withWidth (plotLeft());
            g.setColour (juce::Colour (0xff1e1e20));
            g.fillRect (pad); // (todo negro: sin rayita de borde)
        }
        auto r = procLaneRect();
        if (plotWidth() < getWidth()) // debajo del teclado: vacío, así las dos ondas terminan en la misma línea
        {
            const auto strip = r.withTrimmedLeft (plotWidth());
            g.setColour (juce::Colour (0xff1e1e20));
            g.fillRect (strip);
        }
    }
};
