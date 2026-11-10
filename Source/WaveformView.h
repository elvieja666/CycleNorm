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

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (totalLen() > 0 && onSeek) onSeek (juce::jmax (0.0, xToSample (e.position.x)) / sr);
    }
    void mouseDrag (const juce::MouseEvent& e) override { mouseDown (e); }
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
    size_t skipEnd = 0;

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
    }

    void timerCallback() override
    {
        if (isPlaying && isPlaying()) repaint();
    }

    void drawLane (juce::Graphics& g, const juce::AudioBuffer<float>* buf, juce::Rectangle<int> r,
                   juce::Colour col, const juce::String& title, bool active, size_t greyUntil = 0)
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
                if (skipX < (float) r.getRight())
                {
                    g.setColour (juce::Colour (0xffd0a040).withAlpha (0.8f));
                    const float dashes[] = { 4.0f, 3.0f };
                    g.drawDashedLine (juce::Line<float> (skipX, (float) r.getY(), skipX, (float) r.getBottom()), dashes, 2, 1.0f);
                    g.setFont (juce::FontOptions (11.0f));
                    const float labelW = 44.0f;
                    if (skipX - (float) r.getX() > labelW + 6.0f)
                        g.drawText ("Skip", juce::Rectangle<float> (skipX - labelW - 4.0f, (float) r.getBottom() - 18.0f, labelW, 14.0f), juce::Justification::centredRight);
                }
            }
            else
                drawWave (col);
            // rojo donde pasa de 0 dBFS
            g.setColour (juce::Colour (0x40ff3030));
            g.drawHorizontalLine ((int) (mid - half), (float) r.getX(), (float) r.getRight());
            g.drawHorizontalLine ((int) (mid + half), (float) r.getX(), (float) r.getRight());
        }

        g.setColour (active ? juce::Colours::white : juce::Colour (0xff8a919c));
        g.setFont (juce::FontOptions (13.0f, active ? juce::Font::bold : juce::Font::plain));
        g.drawText (title + (active ? "  (sonando)" : ""), r.reduced (8, 4), juce::Justification::topLeft);
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
            g.drawText (juce::String::fromUTF8 ("Arrastrá acá tus 808 (WAV / AIFF)"), getLocalBounds(), juce::Justification::centred);
            return;
        }
        const bool playingProc = isPlayingProcessed ? isPlayingProcessed() : true;
        auto r = getLocalBounds();
        auto top = r.removeFromTop (h / 2).withTrimmedBottom (1);
        drawLane (g, orig.get(), top, juce::Colour (0xff8f98a6), "Original", ! playingProc);
        drawLane (g, proc.get(), r.withTrimmedTop (1), juce::Colour (0xff4fb3ff), "Procesado", playingProc, skipEnd);
    }
};
