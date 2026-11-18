#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "BinaryData.h"

// ---------------------------------------------------------------------------
//  Estilo visual: monocromo (negro, grises y blanco), líneas finas de 1 px, letra mono
//  (JetBrains Mono) con mayúsculas chicas y espaciadas. Lo prendido: blanco lleno.
// ---------------------------------------------------------------------------
namespace Theme
{
    // Monocromo: negro, grises y blanco
    const juce::Colour topLight   { 0xff141414 };     // el fondo de la ventana
    const juce::Colour ink        { 0xffc8c8c8 };     // texto principal
    const juce::Colour inkStrong  { 0xffececec };     // lo prendido (blanco)
    const juce::Colour inkDim     { 0xff6c6c6c };     // etiquetas chicas
    const juce::Colour inkFaint   { 0xff464646 };     // apagado
    const juce::Colour pill       { 0xff1a1a1a };     // pastillas (menús)
    const juce::Colour well       { 0xff141414 };     // recuadros hundidos
    const juce::Colour raisedTop  { 0xff141414 };     // botones
    const juce::Colour raisedBot  { 0xff1e1e1e };     // botón apretado
    const juce::Colour line       { 0xff3a3a3a };     // bordes finos
    const juce::Colour track      { 0xff2e2e2e };     // la línea de Ramp
    const juce::Colour accent     { 0xffececec };     // (lo que antes era azul: ahora blanco)
    const juce::Colour bad        { 0xffd04848 };     // error
    const juce::Colour darkBg     { 0xff0b0b0b };     // el visor
    const juce::Colour accentBright { 0xffffffff };

    // Letra JetBrains Mono
    inline juce::Typeface::Ptr typeface (int weight)
    {
        static juce::Typeface::Ptr light    = juce::Typeface::createSystemTypefaceFor (BinaryData::JetBrainsMonoNLLight_ttf,    BinaryData::JetBrainsMonoNLLight_ttfSize);
        static juce::Typeface::Ptr regular  = juce::Typeface::createSystemTypefaceFor (BinaryData::JetBrainsMonoNLRegular_ttf,  BinaryData::JetBrainsMonoNLRegular_ttfSize);
        static juce::Typeface::Ptr medium   = juce::Typeface::createSystemTypefaceFor (BinaryData::JetBrainsMonoNLMedium_ttf,   BinaryData::JetBrainsMonoNLMedium_ttfSize);
        static juce::Typeface::Ptr semibold = juce::Typeface::createSystemTypefaceFor (BinaryData::JetBrainsMonoNLSemiBold_ttf, BinaryData::JetBrainsMonoNLSemiBold_ttfSize);

        switch (weight)
        {
            case 300: return light;
            case 500: return medium;
            case 600: return semibold;
            default:  return regular;
        }
    }

    // Letra del título: la misma Inter del resto (en semibold)
    inline juce::Typeface::Ptr titleTypeface() { return typeface (300); } // (Light: un poco más fino que el resto)

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
        // con el mismo espaciado entre letras que las etiquetas (mayúsculas espaciadas)
        return juce::Font (juce::FontOptions (titleTypeface()).withHeight (capHeight / capPerHeight).withKerningFactor (0.14f));
    }

    // Todos los textos un 22% más grandes que el diseño base (igual que UNISON).
    constexpr float textScale = 1.22f;

    inline juce::Font font (float height, int weight = 400, float kerning = 0.0f)
    {
        return juce::Font (juce::FontOptions (typeface (weight)).withHeight (height * textScale).withKerningFactor (kerning));
    }

    // La letra de los botones (todos, también Drag to DAW)
    inline juce::Font buttonFont() { return font (9.5f, 500, 0.16f); } // (los textos van en mayúsculas)
    // las etiquetas chicas (RAMP, NOISE, LOOP, // ORIGINAL...): mayúsculas espaciadas
    inline juce::Font labelFont() { return font (9.0f, 400, 0.14f); }
    // El aire de los botones: del borde a lo dibujado (letras o ícono), a cada lado
    constexpr float kButtonPad = 15.0f;

    // Lo que ocupa lo dibujado de un texto (sin los márgenes de la letra), con la base en y = 0
    inline juce::Rectangle<float> inkBounds (const juce::Font& f, const juce::String& t)
    {
        juce::GlyphArrangement ga;
        ga.addLineOfText (f, t, 0.0f, 0.0f);
        juce::Path p;
        ga.createPath (p);
        return p.getBounds();
    }
    // Un texto con lo dibujado justo en el centro de r (a lo ancho por las letras, a lo alto por las
    // mayúsculas). Si inkLeft >= 0, en vez de centrarlo, lo dibujado arranca en esa x.
    inline void drawInkText (juce::Graphics& g, const juce::Font& f, const juce::String& t, juce::Rectangle<float> r, float inkLeft = -1.0f)
    {
        const auto ib = inkBounds (f, t);
        const float capH = inkBounds (f, "H").getHeight();
        const float x = inkLeft >= 0.0f ? inkLeft - ib.getX() : r.getCentreX() - ib.getCentreX();
        const float base = std::round (r.getCentreY() + capH * 0.5f);
        juce::GlyphArrangement ga;
        ga.addLineOfText (f, t, x, base);
        ga.draw (g);
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
        // sólo el borde fino (el fondo es el de la ventana); con el mouse encima, el borde más claro
        g.setColour (down ? raisedBot : raisedTop);
        g.fillRect (r);
        g.setColour (over || down ? juce::Colour (0xff6a6a6a) : line);
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
