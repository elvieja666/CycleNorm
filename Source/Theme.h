#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "BinaryData.h"

// ---------------------------------------------------------------------------
//  Estilo visual (el mismo de UNISON / MeteringAB): controles en gris claro
//  (en relieve y pastillas), visor de onda en oscuro, azul como color
//  principal y la letra Inter incluida en la app.
// ---------------------------------------------------------------------------
namespace Theme
{
    // Parte de arriba (clara)
    const juce::Colour topLight   { 0xffe9ebed };     // degradé de arriba...
    const juce::Colour topDark    { 0xffdcdfe2 };     // ...hacia abajo
    const juce::Colour ink        { 0xff2b2e33 };     // texto principal
    const juce::Colour inkStrong  { 0xff23262b };     // números grandes
    const juce::Colour inkDim     { 0xff6b7078 };     // títulos chicos
    const juce::Colour inkFaint   { 0xff8b9096 };     // ayudas
    const juce::Colour pill       { 0xffd3d6da };     // pastillas (menú, Link ID, ms|Sync)
    const juce::Colour well       { 0xfff1f2f3 };     // recuadros hundidos (números)
    const juce::Colour raisedTop  { 0xfff8f9fa };     // botones en relieve
    const juce::Colour raisedBot  { 0xffe6e8ea };
    const juce::Colour track      { 0xffc9ccd1 };     // arco de fondo de la perilla
    const juce::Colour accent     { 0xff2f74d0 };     // azul sobre fondo claro
    const juce::Colour good       { 0xff2f74d0 };
    const juce::Colour bad        { 0xffd04848 };
    const juce::Colour warn       { 0xffe0782f };     // naranja: algo desactivado a propósito (bypass)
    const juce::Colour preInk     { 0xff6b7078 };     // botón grande en PRE FX (gris)

    // Osciloscopio (oscuro)
    const juce::Colour darkBg     { 0xff0b0c0e };
    const juce::Colour darkPanel  { 0xff0f1114 };
    const juce::Colour chipLine   { 0xff2c3036 };
    const juce::Colour chipText   { 0xff9a9fa6 };
    const juce::Colour darkText   { 0xffd5d7da };
    const juce::Colour darkDim    { 0xff7c8188 };
    const juce::Colour accentBright { 0xff5aa9ff };   // azul sobre fondo oscuro

    // Letra Inter
    inline juce::Typeface::Ptr typeface (int weight)
    {
        static juce::Typeface::Ptr light    = juce::Typeface::createSystemTypefaceFor (BinaryData::InterLight_otf,    BinaryData::InterLight_otfSize);
        static juce::Typeface::Ptr regular  = juce::Typeface::createSystemTypefaceFor (BinaryData::InterRegular_otf,  BinaryData::InterRegular_otfSize);
        static juce::Typeface::Ptr medium   = juce::Typeface::createSystemTypefaceFor (BinaryData::InterMedium_otf,   BinaryData::InterMedium_otfSize);
        static juce::Typeface::Ptr semibold = juce::Typeface::createSystemTypefaceFor (BinaryData::InterSemiBold_otf, BinaryData::InterSemiBold_otfSize);

        switch (weight)
        {
            case 300: return light;
            case 500: return medium;
            case 600: return semibold;
            default:  return regular;
        }
    }

    // Letra del título: Akira Expanded (Typologic, con licencia).
    inline juce::Typeface::Ptr titleTypeface()
    {
        static juce::Typeface::Ptr t = juce::Typeface::createSystemTypefaceFor (BinaryData::AkiraExpandedDemo_otf, BinaryData::AkiraExpandedDemo_otfSize);
        return t;
    }

    // La letra del título con mayúsculas de capHeight px (se mide la "B": cada sistema
    // calcula el alto de la letra un poco distinto).
    inline juce::Font titleFont (float capHeight)
    {
        static const float capPerHeight = []
        {
            const auto probe = juce::Font (juce::FontOptions (titleTypeface()).withHeight (100.0f));
            juce::GlyphArrangement ga;
            ga.addLineOfText (probe, "B", 0.0f, 0.0f);
            juce::Path p;
            ga.createPath (p);                           // el dibujo de la letra (no su caja de renglón)
            const float h = p.getBounds().getHeight();
            return h > 1.0f ? h / 100.0f : 0.67f;
        }();
        return juce::Font (juce::FontOptions (titleTypeface()).withHeight (capHeight / capPerHeight));
    }

    constexpr float textScale = 1.0f; // en Toolkit los tamaños ya van en px finales

    inline juce::Font font (float height, int weight = 400, float kerning = 0.0f)
    {
        return juce::Font (juce::FontOptions (typeface (weight)).withHeight (height * textScale).withKerningFactor (kerning));
    }

    // Recuadro hundido (fondo claro con sombra interior arriba).
    inline void drawWell (juce::Graphics& g, juce::Rectangle<float> r, float radius)
    {
        g.setColour (well);
        g.fillRoundedRectangle (r, radius);

        juce::Graphics::ScopedSaveState save (g);
        juce::Path clip;
        clip.addRoundedRectangle (r, radius);
        g.reduceClipRegion (clip);
        g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.13f), 0.0f, r.getY(),
                                                 juce::Colours::transparentBlack, 0.0f, r.getY() + 5.0f, false));
        g.fillRect (r.withHeight (6.0f));
        g.setColour (juce::Colours::black.withAlpha (0.06f));
        g.drawRoundedRectangle (r.reduced (0.5f), radius, 1.0f);
    }

    // Botón en relieve (claro, con sombra abajo).
    inline void drawRaised (juce::Graphics& g, juce::Rectangle<float> r, float radius, bool over, bool down)
    {
        juce::Path shape;
        shape.addRoundedRectangle (r, radius);

        if (! down)
            juce::DropShadow (juce::Colours::black.withAlpha (0.2f), 5, { 0, 2 }).drawForPath (g, shape);

        const float lift = over && ! down ? 0.02f : 0.0f;
        g.setGradientFill (juce::ColourGradient (raisedTop.brighter (lift), 0.0f, r.getY(),
                                                 down ? raisedBot.darker (0.04f) : raisedBot.brighter (lift), 0.0f, r.getBottom(), false));
        g.fillPath (shape);

        g.setColour (juce::Colours::white.withAlpha (down ? 0.3f : 0.9f));
        g.drawLine (r.getX() + radius, r.getY() + 0.75f, r.getRight() - radius, r.getY() + 0.75f, 1.0f);
    }

    // Íconos: parlante = lo que suena; ojo = lo que se ve (solo dibujo).
    inline void drawSpeaker (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c)
    {
        const float s = r.getHeight();
        const float x = r.getX(), y = r.getY();
        juce::Path body;
        body.startNewSubPath (x + 0.08f * s, y + 0.36f * s);
        body.lineTo (x + 0.26f * s, y + 0.36f * s);
        body.lineTo (x + 0.48f * s, y + 0.16f * s);
        body.lineTo (x + 0.48f * s, y + 0.84f * s);
        body.lineTo (x + 0.26f * s, y + 0.64f * s);
        body.lineTo (x + 0.08f * s, y + 0.64f * s);
        body.closeSubPath();
        g.setColour (c);
        g.fillPath (body);
        juce::Path waves;
        waves.addCentredArc (x + 0.48f * s, y + 0.5f * s, 0.2f * s, 0.2f * s, 0.0f, 0.75f, 2.39f, true);
        waves.addCentredArc (x + 0.48f * s, y + 0.5f * s, 0.38f * s, 0.38f * s, 0.0f, 0.72f, 2.42f, true);
        g.strokePath (waves, juce::PathStrokeType (juce::jmax (1.2f, 0.09f * s), juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
}
