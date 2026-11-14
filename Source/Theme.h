#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "BinaryData.h"

// ---------------------------------------------------------------------------
//  Estilo visual: grises oscuros como Logic Pro (todo plano, bordes finos),
//  visor en gris más oscuro, azul como color principal y la letra Inter.
// ---------------------------------------------------------------------------
namespace Theme
{
    // Parte de arriba (clara)
    const juce::Colour topLight   { 0xff2f2f31 };     // degradé de arriba...
    const juce::Colour topDark    { 0xff2f2f31 };     // ...hacia abajo
    const juce::Colour ink        { 0xffe0e0e2 };     // texto principal
    const juce::Colour inkStrong  { 0xfff0f0f2 };     // números grandes
    const juce::Colour inkDim     { 0xff9c9ca0 };     // títulos chicos
    const juce::Colour inkFaint   { 0xff6e6e72 };     // ayudas
    const juce::Colour pill       { 0xff444447 };     // pastillas (menú, Link ID, ms|Sync)
    const juce::Colour well       { 0xff232325 };     // recuadros hundidos (números)
    const juce::Colour raisedTop  { 0xff46464a };     // botones en relieve
    const juce::Colour raisedBot  { 0xff3c3c40 };
    const juce::Colour line       { 0xff1a1a1c };     // bordes finos (todo plano, como una herramienta)
    const juce::Colour track      { 0xff3a3a3d };     // arco de fondo de la perilla
    const juce::Colour accent     { 0xff4a90f0 };     // azul sobre fondo claro
    const juce::Colour good       { 0xff4a90f0 };
    const juce::Colour bad        { 0xffd04848 };
    const juce::Colour warn       { 0xffe0782f };     // naranja: algo desactivado a propósito (bypass)
    const juce::Colour preInk     { 0xff9c9ca0 };     // botón grande en PRE FX (gris)

    // Osciloscopio (oscuro)
    const juce::Colour darkBg     { 0xff1e1e20 };
    const juce::Colour darkPanel  { 0xff28282a };
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

    // Todos los textos un 22% más grandes que el diseño base (igual que UNISON).
    constexpr float textScale = 1.22f;

    inline juce::Font font (float height, int weight = 400, float kerning = 0.0f)
    {
        return juce::Font (juce::FontOptions (typeface (weight)).withHeight (height * textScale).withKerningFactor (kerning));
    }

    // Recuadro (barras): plano, fondo claro y borde fino
    inline void drawWell (juce::Graphics& g, juce::Rectangle<float> r, float)
    {
        g.setColour (well);
        g.fillRect (r);
        g.setColour (line);
        g.drawRect (r, 1.0f);
    }

    // Botón: plano, fondo claro y borde fino (sin relieve ni sombra); con el mouse encima o
    // apretado, apenas más oscuro
    inline void drawRaised (juce::Graphics& g, juce::Rectangle<float> r, float, bool over, bool down)
    {
        g.setColour (down ? raisedBot : over ? raisedTop.brighter (0.08f) : raisedTop);
        g.fillRect (r);
        g.setColour (line);
        g.drawRect (r, 1.0f);
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
