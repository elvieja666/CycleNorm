// Toolkit (antes CycleNorm): normaliza un 808 onda por onda (cada ciclo al mismo pico) y lo exporta.
#include <juce_audio_utils/juce_audio_utils.h>
#include "CycleEngine.h"
#include "LoopFinder.h"
#include "Resample.h"
#include "ABSource.h"
#include "NoiseDetect.h"
#include "WaveformView.h"
#include "Theme.h"

namespace
{
// colores: los de UNISON (Theme.h)
const juce::Colour kAccent = Theme::accent;
const juce::Colour kBg = Theme::topLight;

//==============================================================================
// El texto del valor de las barras (Ramp, Pitch): en estas barras un clic empieza a escribir un
// valor, así que el ⌥ Option + clic (volver a 0) se agarra acá. La barra dice a qué valor volver
// con la propiedad "resetValue".
struct ResetLabel : juce::Label
{
    ResetLabel()
    {
        setJustificationType (juce::Justification::centred);
        setBorderSize ({});
    }
    // como la píldora "3000 ms" de UNISON: se escribe encima de la misma píldora, centrado, con la
    // misma letra; sin caja, sin fondo y sin borde
    juce::TextEditor* createEditorComponent() override
    {
        auto* ed = juce::Label::createEditorComponent();
        const auto clear = juce::Colours::transparentBlack;
        ed->setColour (juce::TextEditor::backgroundColourId, clear);
        ed->setColour (juce::TextEditor::outlineColourId, clear);
        ed->setColour (juce::TextEditor::focusedOutlineColourId, clear);
        ed->setColour (juce::TextEditor::shadowColourId, clear);
        ed->setColour (juce::TextEditor::textColourId, findColour (juce::Label::textColourId));
        ed->setColour (juce::TextEditor::highlightColourId, Theme::accent.withAlpha (0.25f));
        ed->setColour (juce::CaretComponent::caretColourId, Theme::inkStrong);
        ed->setJustification (juce::Justification::centred);
        ed->setIndents (2, 0); // (el editor de JUCE centra 1 px más a la izquierda que el texto dibujado)
        ed->setBorder ({});
        return ed;
    }
    bool altClick = false;
    juce::Slider* slider() const { return dynamic_cast<juce::Slider*> (getParentComponent()); }
    void mouseDown (const juce::MouseEvent& e) override
    {
        altClick = e.mods.isAltDown() && slider() != nullptr && slider()->getProperties().contains ("resetValue");
        if (altClick) { slider()->setValue ((double) slider()->getProperties()["resetValue"], juce::sendNotificationSync); return; }
        juce::Label::mouseDown (e);
    }
    void mouseDrag (const juce::MouseEvent& e) override { if (! altClick) juce::Label::mouseDrag (e); }
    void mouseUp (const juce::MouseEvent& e) override { if (altClick) { altClick = false; return; } juce::Label::mouseUp (e); }
    void mouseDoubleClick (const juce::MouseEvent& e) override { if (! e.mods.isAltDown()) juce::Label::mouseDoubleClick (e); }
};

//==============================================================================
// Look & feel con la estética de UNISON: botones claros en relieve, pastillas,
// recuadros hundidos, azul de acento y letra Inter.
class CycleNormLookAndFeel : public juce::LookAndFeel_V4
{
public:
    CycleNormLookAndFeel()
    {
        setColourScheme (getLightColourScheme());
        setColour (juce::ResizableWindow::backgroundColourId, Theme::topLight);
        setColour (juce::Slider::textBoxTextColourId, Theme::inkStrong);
        setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxHighlightColourId, Theme::accent.withAlpha (0.25f));
        setColour (juce::TextEditor::backgroundColourId, Theme::well);
        setColour (juce::TextEditor::textColourId, Theme::inkStrong);
        setColour (juce::TextEditor::highlightColourId, Theme::accent.withAlpha (0.25f));
        setColour (juce::TextEditor::highlightedTextColourId, Theme::inkStrong);
        setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        setColour (juce::TextEditor::focusedOutlineColourId, Theme::accent);
        setColour (juce::CaretComponent::caretColourId, Theme::accent);
        setColour (juce::ComboBox::textColourId, Theme::ink);
        setColour (juce::ComboBox::arrowColourId, Theme::ink);
        setColour (juce::PopupMenu::backgroundColourId, Theme::well);
        setColour (juce::PopupMenu::textColourId, Theme::ink);
        setColour (juce::PopupMenu::headerTextColourId, Theme::inkDim);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, Theme::accent);
        setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
        setColour (juce::TextButton::buttonColourId, Theme::raisedBot);
        setColour (juce::TextButton::buttonOnColourId, Theme::raisedBot);
        setColour (juce::TextButton::textColourOffId, Theme::ink);
        setColour (juce::TextButton::textColourOnId, Theme::accent);
        setColour (juce::Label::textColourId, Theme::ink);
        setColour (juce::Label::backgroundWhenEditingColourId, Theme::well);
        setColour (juce::Label::textWhenEditingColourId, Theme::inkStrong);
        setColour (juce::Label::outlineWhenEditingColourId, Theme::accent);
        setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::ScrollBar::thumbColourId, Theme::track);
        setColour (juce::AlertWindow::backgroundColourId, Theme::topLight);
        setColour (juce::AlertWindow::textColourId, Theme::ink);
        setColour (juce::AlertWindow::outlineColourId, Theme::track);
        setColour (juce::ProgressBar::foregroundColourId, Theme::accent);
        setColour (juce::ProgressBar::backgroundColourId, Theme::well);
    }

    // toda la letra por defecto: Inter (negrita = SemiBold)
    juce::Typeface::Ptr getTypefaceForFont (const juce::Font& f) override
    {
        if (f.getTypefaceName() == juce::Font::getDefaultSansSerifFontName())
            return Theme::typeface (f.isBold() ? 600 : 400);
        return LookAndFeel_V4::getTypefaceForFont (f);
    }

    // zona visible del botón en relieve (deja lugar para la sombra)
    static juce::Rectangle<float> raisedShape (juce::Rectangle<float> b) { return b.reduced (4.0f, 2.0f).withTrimmedBottom (4.0f); }
    // transporte pegado (Play | Stop | Loop): cada parte llega hasta el borde de la de al lado;
    // sólo las puntas de afuera dejan lugar para la sombra
    static juce::Rectangle<float> segmentShape (const juce::Button& b)
    {
        const auto seg = b.getProperties()["segment"].toString();
        auto r = b.getLocalBounds().toFloat().withTrimmedTop (2.0f).withTrimmedBottom (6.0f);
        if (seg == "left")  r.removeFromLeft (4.0f);
        if (seg == "right") r.removeFromRight (4.0f);
        return r;
    }
    // la forma del botón grande PRE / POST FX de UNISON (esquinas suaves, no píldora): radio 18 sobre 74 de alto
    static float bigCorner (float height) { return height * 18.0f / 74.0f; }
    // la misma curvatura en todas las píldoras: la del transporte (33 de alto -> radio ≈ 8)
    static float pillCorner (float height) { return juce::jmin (bigCorner (33.0f), height * 0.5f); }

    juce::Font getTextButtonFont (juce::TextButton&, int) override { return Theme::font (14.0f, 400); } // como Measure / Bypass / Scope

    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool down) override
    {
        auto r = b.getLocalBounds().toFloat();
        if (b.getProperties().contains ("segment")) // transporte pegado: una sola pieza en relieve, con rayitas entre partes
        {
            const auto seg = b.getProperties()["segment"].toString();
            const auto shape = segmentShape (b);
            const float rad = pillCorner (shape.getHeight()); // la proporción del PRE / POST FX (≈ 8 con 33 de alto)
            const bool L = seg == "left", R = seg == "right";
            juce::Path p;
            p.addRoundedRectangle (shape.getX(), shape.getY(), shape.getWidth(), shape.getHeight(), rad, rad, L, R, L, R);
            // plano: fondo claro y borde oscuro (siempre igual); prendido (sonando / loop de escucha)
            // o apretado, sólo el ícono se pone azul
            g.setColour (down ? Theme::raisedBot.darker (0.04f) : Theme::raisedTop.brighter (over && b.isEnabled() ? 0.02f : 0.0f));
            g.fillPath (p);
            const auto edge = shape.reduced (0.5f);
            juce::Path outline;
            outline.addRoundedRectangle (edge.getX(), edge.getY(), edge.getWidth(), edge.getHeight(), rad, rad, L, R, L, R);
            g.setColour (Theme::inkDim); // el borde siempre igual, en el gris de "Ramp" (sólo el ícono se pone azul)
            g.strokePath (outline, juce::PathStrokeType (1.0f));
            return;
        }
        // los que se prenden (Processed, Normalize, Loop): iguales a los demás (en relieve);
        // prendidos, además con borde y texto azul
        const auto shape = raisedShape (r);
        Theme::drawRaised (g, shape, pillCorner (shape.getHeight()), over && b.isEnabled(), down);
        if (b.getClickingTogglesState() && b.getToggleState())
        {
            g.setColour (Theme::accent.withAlpha (0.7f));
            g.drawRoundedRectangle (shape.reduced (0.5f), pillCorner (shape.getHeight()), 1.2f);
        }
    }

    void drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool) override
    {
        if (b.getProperties().contains ("icon")) // ▶ ■ ⟲, en azul cuando está prendido (sonando / loop de escucha)
        {
            const auto icon = b.getProperties()["icon"].toString();
            const bool lit = b.getToggleState() || (bool) b.getProperties()["lit"] || b.isDown();
            auto colour = lit ? Theme::accent : Theme::inkDim; // (apagado: el gris de "Ramp", como el borde)
            if (! b.isEnabled()) colour = colour.withMultipliedAlpha (0.4f);
            const auto box = segmentShape (b); // plano: el ícono no se mueve
            const float s = box.getHeight() * 0.36f;
            const auto c = box.getCentre();
            g.setColour (colour);
            if (icon == "play")
            {
                juce::Path p;
                p.addTriangle (c.x - s * 0.42f, c.y - s * 0.55f, c.x - s * 0.42f, c.y + s * 0.55f, c.x + s * 0.55f, c.y);
                g.fillPath (p, juce::AffineTransform());
                g.strokePath (p, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }
            else if (icon == "stop")
            {
                g.fillRoundedRectangle (juce::Rectangle<float> (s * 0.95f, s * 0.95f).withCentre (c), 1.5f);
            }
            else // loop de escucha: flecha en círculo
            {
                const float rr = s * 0.55f;
                juce::Path arc;
                arc.addCentredArc (c.x, c.y, rr, rr, 0.0f, 0.55f, juce::MathConstants<float>::twoPi - 0.25f, true);
                g.strokePath (arc, juce::PathStrokeType (1.7f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                const auto tip = c.getPointOnCircumference (rr, juce::MathConstants<float>::twoPi - 0.25f);
                juce::Path head;
                head.addTriangle (tip.x - 3.6f, tip.y - 3.4f, tip.x + 3.6f, tip.y - 0.4f, tip.x - 1.2f, tip.y + 3.4f);
                g.fillPath (head);
            }
            return;
        }
        auto colour = (b.getClickingTogglesState() && b.getToggleState()) ? Theme::accent : Theme::ink;
        if (! b.isEnabled()) colour = colour.withMultipliedAlpha (0.4f);
        auto r = raisedShape (b.getLocalBounds().toFloat()).toNearestInt().translated (0, b.isDown() ? 1 : 0);
        g.setColour (colour);
        g.setFont (getTextButtonFont (b, b.getHeight()));
        if (b.getProperties().contains ("speaker")) // Hearing: parlante + texto, centrados juntos
        {
            const auto font = getTextButtonFont (b, b.getHeight());
            const float textW = juce::GlyphArrangement::getStringWidth (font, b.getButtonText());
            const float iconH = 14.0f, iconW = iconH * 0.95f, gap = 7.0f;
            const auto rf = r.toFloat();
            const float x0 = rf.getCentreX() - (iconW + gap + textW) * 0.5f;
            Theme::drawSpeaker (g, { x0, rf.getCentreY() - iconH * 0.5f, iconW, iconH }, colour);
            g.setColour (colour);
            g.drawText (b.getButtonText(), juce::Rectangle<float> (x0 + iconW + gap, rf.getY(), textW + 4.0f, rf.getHeight()),
                        juce::Justification::centredLeft, false);
            return;
        }
        g.drawFittedText (b.getButtonText(), r, juce::Justification::centred, 1);
    }

    // selector (Format): pastilla gris con flechita
    void drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box) override
    {
        auto r = raisedShape ({ 0.0f, 0.0f, (float) w, (float) h }).expanded (0.0f, 1.0f);
        const float radius = pillCorner (r.getHeight());
        g.setColour (Theme::pill.darker (box.isMouseOver (true) ? 0.03f : 0.0f));
        g.fillRoundedRectangle (r, radius);
        const float cx = r.getRight() - radius - 2.0f, cy = r.getCentreY();
        juce::Path arrow;
        arrow.startNewSubPath (cx - 4.5f, cy - 2.0f);
        arrow.lineTo (cx, cy + 2.5f);
        arrow.lineTo (cx + 4.5f, cy - 2.0f);
        g.setColour (Theme::ink);
        g.strokePath (arrow, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    juce::Font getComboBoxFont (juce::ComboBox&) override { return Theme::font (14.0f, 400); }
    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
    {
        label.setFont (getComboBoxFont (box));
        const auto r = raisedShape (box.getLocalBounds().toFloat()).toNearestInt();
        label.setBounds (r.getX() + 10, r.getY(), r.getWidth() - 10 - r.getHeight(), r.getHeight());
    }
    juce::Font getPopupMenuFont() override { return Theme::font (11.5f, 400); } // (el menú no se achica con la ventana)

    // barras (Amount, Ramp, Pitch): recuadro hundido con el valor en azul.
    // Las que van de negativo a positivo (Pitch) se llenan desde el 0.
    void drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float, float,
                           juce::Slider::SliderStyle style, juce::Slider& s) override
    {
        if (style != juce::Slider::LinearBar) { LookAndFeel_V4::drawLinearSlider (g, x, y, w, h, pos, 0, 0, style, s); return; }
        const auto r = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h);
        const float radius = pillCorner (r.getHeight());
        Theme::drawWell (g, r, radius);
        float from = r.getX();
        if (s.getMinimum() < 0.0 && s.getMaximum() > 0.0) from = r.getX() + (float) s.valueToProportionOfLength (0.0) * r.getWidth();
        const float a = std::min (from, pos), b = std::max (from, pos);
        if (b - a > 0.5f)
        {
            juce::Graphics::ScopedSaveState st (g);
            juce::Path clip; clip.addRoundedRectangle (r, radius);
            g.reduceClipRegion (clip);
            g.setGradientFill (juce::ColourGradient (Theme::accent.withAlpha (0.30f), 0.0f, r.getY(),
                                                     Theme::accent.withAlpha (0.20f), 0.0f, r.getBottom(), false));
            g.fillRect (juce::Rectangle<float> (a, r.getY(), b - a, r.getHeight())); // (sin línea en el borde)
        }
    }
    juce::Label* createSliderTextBox (juce::Slider& s) override
    {
        auto* base = LookAndFeel_V4::createSliderTextBox (s);
        auto* l = new ResetLabel();                      // igual que el de JUCE, más el ⌥ + clic y sin caja al escribir
        l->setMinimumHorizontalScale (base->getMinimumHorizontalScale());
        l->setEditable (base->isEditableOnSingleClick(), base->isEditableOnDoubleClick(), base->doesLossOfFocusDiscardChanges());
        delete base;
        l->setFont (Theme::font (14.0f, 400)); // como el valor de Window (3000 ms)
        l->setColour (juce::Label::textColourId, Theme::inkStrong);
        l->setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        l->setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
        return l;
    }

    void drawProgressBar (juce::Graphics& g, juce::ProgressBar&, int w, int h, double progress, const juce::String&) override
    {
        const auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) w, (float) h);
        Theme::drawWell (g, r, pillCorner (r.getHeight()));
        if (progress > 0.0)
        {
            const auto f = r.withWidth (juce::jmax (r.getHeight(), r.getWidth() * (float) juce::jlimit (0.0, 1.0, progress)));
            g.setColour (Theme::accent.withAlpha (0.85f));
            g.fillRoundedRectangle (f, pillCorner (r.getHeight()));
        }
    }
};
juce::String u8 (const char* s) { return juce::String::fromUTF8 (s); }

cyclenorm::Channels toChannels (const juce::AudioBuffer<float>& b)
{
    cyclenorm::Channels c ((size_t) b.getNumChannels());
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
        c[(size_t) ch].assign (b.getReadPointer (ch), b.getReadPointer (ch) + b.getNumSamples());
    return c;
}
} // namespace

//==============================================================================
// Barra que con la rueda del mouse se mueve de a un paso fijo (wheelStep; con Shift, el paso fino)
struct StepSlider : public juce::Slider
{
    double wheelStep = 1.0, wheelStepFine = 0.1;
    double resetValue = 0.0;   // ⌥ Option + clic vuelve a este valor
    bool resetClick = false;
    void mouseDown (const juce::MouseEvent& e) override
    {
        resetClick = e.mods.isAltDown() && isEnabled();
        if (resetClick) { setValue (resetValue, juce::sendNotificationSync); return; }
        juce::Slider::mouseDown (e);
    }
    void mouseDrag (const juce::MouseEvent& e) override { if (! resetClick) juce::Slider::mouseDrag (e); }
    void mouseUp (const juce::MouseEvent& e) override { if (resetClick) { resetClick = false; return; } juce::Slider::mouseUp (e); }
    void mouseDoubleClick (const juce::MouseEvent& e) override { if (! e.mods.isAltDown()) juce::Slider::mouseDoubleClick (e); }
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        const float d = std::abs (w.deltaY) > std::abs (w.deltaX) ? w.deltaY : -w.deltaX;
        if (std::abs (d) < 1e-6f || ! isEnabled()) return;
        const double step = e.mods.isShiftDown() ? wheelStepFine : wheelStep;
        setValue (getValue() + (d > 0 ? 1.0 : -1.0) * step, juce::sendNotificationAsync);
    }
};

//==============================================================================
struct Item
{
    juce::File file;
    double sampleRate = 44100.0;
    int bits = 24;
    bool isFloat = false;
    std::shared_ptr<juce::AudioBuffer<float>> original, processed; // original = el cargado, o el afinado con Tune
    std::shared_ptr<juce::AudioBuffer<float>> shownProcessed; // para dibujar: el procesado sin re-afinar (el dibujo no cambia con el pitch)
    cyclenorm::Analysis analysis;
    std::shared_ptr<juce::AudioBuffer<float>> loaded; // el archivo tal como se abrió (antes de Tune)
    cyclenorm::Analysis loadedAnalysis;
    int tuneSt = 0;          // Transpose (semitonos) puesto a mano
    double tuneCents = 0.0;  // Fine (cents) puesto a mano
    double tuneSemis = 0.0;  // lo aplicado: semitonos + cents/100
    double targetUsedDb = 0.0;
    float outPeak = 0.0f;
    size_t endSample = 0; // dónde se corta el final (la línea de Fin); 0 = hasta el final del archivo
    size_t startSample = 0; // dónde empieza (la línea de Start); 0 = desde el principio
    int fadeHalves = 0;     // fade in desde Start, en semiciclos (0 = sin fade)
    int fadeOutHalves = 0;  // fade out hasta End, en semiciclos (0 = sin fade; con loop no se usa)
    size_t loopStart = 0, loopEnd = 0; // zona de loop (0/0 = sin loop): de un cruce ascendente a otro
    double loopQuality = 0;
    int xfadeCycles = 0; // crossfade del loop, en ondas enteras (0 = Off; como máximo, las ondas del loop)
    std::vector<cyclenorm::LoopResult> loopCands; // los mejores loops que encontró la búsqueda
    int loopCandIdx = -1;                        // cuál de ellos está puesto (-1 = uno movido a mano)
    int skipOverride = -1;                       // (para el dibujo sin re-afinar: el Skip contado en su audio)
    bool hasLoop() const { return loopEnd > loopStart; }

    int lengthToWrite() const
    {
        const int full = original ? original->getNumSamples() : 0;
        return (endSample > 0 && (int) endSample < full) ? (int) endSample : full;
    }
    // lo que se exporta: con loop, termina donde termina el loop (lo que sigue el sampler no lo toca nunca)
    int exportLength() const
    {
        const int len = lengthToWrite();
        return (hasLoop() && (int) loopEnd > 0 && (int) loopEnd < len) ? (int) loopEnd : len;
    }
};

//==============================================================================
class DragOut : public juce::Component
{
public:
    std::function<juce::File()> makeFile; // escribe el procesado en un archivo y lo devuelve

    void paint (juce::Graphics& g) override
    {
        // botón azul en relieve (paleta de UNISON): degradé de celeste (arriba) a azul (abajo),
        // sombrita y un brillo fino arriba; se aclara con el mouse encima, gris si no hay 808
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        const float radius = CycleNormLookAndFeel::pillCorner (r.getHeight());
        juce::Colour text = juce::Colours::white;
        if (! isEnabled())
        {
            g.setColour (Theme::pill); g.fillRoundedRectangle (r, radius);
            text = Theme::inkFaint;
        }
        else
        {
            const juce::Colour blue = Theme::accent, blueLight = Theme::accentBright;
            // apretado: se hunde (sin sombra, 2 px más abajo, un poco más oscuro), como los otros botones
            auto body = r.withTrimmedBottom (2.0f).translated (0.0f, pressed ? 2.0f : 0.0f);
            if (! pressed)
            {
                g.setColour (juce::Colours::black.withAlpha (0.18f));
                g.fillRoundedRectangle (body.translated (0.0f, 2.0f), radius);
            }
            const float lift = hover && ! pressed ? 0.05f : 0.0f;
            g.setGradientFill (juce::ColourGradient (pressed ? blueLight.darker (0.06f) : blueLight.brighter (lift), 0.0f, body.getY(),
                                                     pressed ? blue.darker (0.08f) : blue.brighter (lift * 1.6f), 0.0f, body.getBottom(), false));
            g.fillRoundedRectangle (body, radius);
            g.setColour (juce::Colours::white.withAlpha (pressed ? 0.12f : 0.28f));
            g.drawLine (body.getX() + radius, body.getY() + 1.0f, body.getRight() - radius, body.getY() + 1.0f, 1.0f);
            r = body;
        }
        g.setColour (text);
        g.setFont (Theme::font (14.0f, 400));
        g.drawText ("Drag to DAW", r.toNearestInt(), juce::Justification::centred);
    }
    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override { hover = false; repaint(); }
    void mouseDown (const juce::MouseEvent&) override { started = false; if (isEnabled()) { pressed = true; repaint(); } }
    void mouseUp (const juce::MouseEvent&) override { pressed = false; repaint(); }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (started || e.getDistanceFromDragStart() < 6 || ! makeFile) return;
        started = true;
        const auto f = makeFile();
        if (f.existsAsFile())
            juce::DragAndDropContainer::performExternalDragDropOfFiles ({ f.getFullPathName() }, false, this,
                                                                         [safe = juce::Component::SafePointer<DragOut> (this)] {
                                                                             if (safe != nullptr) { safe->pressed = false; safe->repaint(); } });
    }

private:
    bool hover = false, started = false, pressed = false;
};

//==============================================================================
// Búsqueda de loop en segundo plano (exhaustiva: puede tardar un poco con archivos largos).
class LoopSearchJob : public juce::Thread
{
public:
    LoopSearchJob (Item* target, std::shared_ptr<const juce::AudioBuffer<float>> audio, cyclenorm::Analysis an, size_t from, size_t to)
        : juce::Thread ("Buscar loop"), item (target), buf (std::move (audio)), analysis (std::move (an)), zoneFrom (from), zoneTo (to) {}
    ~LoopSearchJob() override { stopThread (4000); }

    void run() override
    {
        cyclenorm::AudioView v;
        for (int ch = 0; ch < buf->getNumChannels(); ++ch) v.ch.push_back (buf->getReadPointer (ch));
        v.n = (size_t) buf->getNumSamples();
        results = cyclenorm::findLoops (analysis, v, zoneFrom, zoneTo, 4, 5, [this] (double p) {
            progress = p;
            return ! threadShouldExit();
        });
        cancelled = threadShouldExit();
        finished = true;
    }

    Item* const item;
    std::atomic<double> progress { 0.0 };
    std::atomic<bool> finished { false }, cancelled { false };
    std::vector<cyclenorm::LoopResult> results; // (se lee recién con finished == true)

private:
    std::shared_ptr<const juce::AudioBuffer<float>> buf;
    cyclenorm::Analysis analysis;
    size_t zoneFrom, zoneTo;
};

class MainComponent : public juce::Component,
                      public juce::FileDragAndDropTarget,
                      public juce::ApplicationCommandTarget,
                      public juce::MenuBarModel,
                      private juce::ListBoxModel,
                      private juce::Timer
{
public:
    // comandos de la barra de menú (File, Edit, Playback, Loop)
    enum Cmd
    {
        cmdOpen = 1, cmdRemove, cmdExport, cmdExportAll, cmdFmtSame, cmdFmt24, cmdFmtFloat,
        cmdUndo, cmdRedo,
        cmdPlay, cmdLoopPlay, cmdHearing, cmdFitView, cmdLoopSection,
        cmdFindLoop, cmdNextLoop, cmdClearLoop
    };

    MainComponent()
    {
        formats.registerBasicFormats();

        juce::PropertiesFile::Options o;
        o.applicationName = "Toolkit";
        o.filenameSuffix = ".settings";
        o.osxLibrarySubFolder = "Application Support";
        {   // la app se llamaba CycleNorm: si hay ajustes con el nombre viejo y no con el nuevo, se pasan
            auto oldOpts = o; oldOpts.applicationName = "CycleNorm";
            const auto newFile = o.getDefaultFile(), oldFile = oldOpts.getDefaultFile();
            if (! newFile.existsAsFile() && oldFile.existsAsFile()) { newFile.getParentDirectory().createDirectory(); oldFile.copyFileTo (newFile); }
        }
        props = std::make_unique<juce::PropertiesFile> (o);

        auto setupButton = [this] (juce::Button& b, const juce::String& text) {
            b.setButtonText (text);
            b.setWantsKeyboardFocus (false);
            addAndMakeVisible (b);
        };
        setupButton (openButton, u8 ("Open…"));
        setupButton (removeButton, "Remove");
        setupButton (playButton, "Play");
        setupButton (stopButton, "Stop");
        setupButton (repeatButton, "Loop");
        // transporte con íconos (como un DAW): el texto queda para el menú / accesibilidad
        playButton.getProperties().set ("icon", "play");
        playButton.getProperties().set ("segment", "left");
        stopButton.getProperties().set ("segment", "mid");
        repeatButton.getProperties().set ("segment", "right");
        stopButton.getProperties().set ("icon", "stop");
        repeatButton.getProperties().set ("icon", "repeat");
        repeatButton.setClickingTogglesState (true);
        repeatButton.setTooltip ("Loop playback (listening only)");
        repeatButton.onClick = [this] { source.setLooping (repeatButton.getToggleState()); props->setValue ("repeat", repeatButton.getToggleState()); };
        stopButton.onClick = [this] { stopPlayback(); };
        setupButton (loopButton, "Loop");
        setupButton (normButton, "Normalize");
        setupButton (abButton, "Processed");
        setupButton (findLoopButton, "Find loop");
        setupButton (clearLoopButton, "Clear loop");
        findLoopButton.onClick = [this] { if (loopJob != nullptr) cancelLoopSearch(); else findLoopForCurrent(); };
        setupButton (otherLoopButton, "Option 1"); // "Option N": la opción de loop que está puesta; cada clic pasa a la siguiente
        otherLoopButton.setTooltip ("Next loop option");
        otherLoopButton.onClick = [this] { nextLoopCandidate(); };
        addChildComponent (progressBar);
        progressBar.setTextToDisplay (" "); // el texto va en una etiqueta encima (el de la barra no se leía)
        progressLabel.setColour (juce::Label::textColourId, Theme::inkStrong);
        progressLabel.setFont (Theme::font (12.0f, 400));
        progressLabel.setJustificationType (juce::Justification::centred);
        progressLabel.setInterceptsMouseClicks (false, false);
        addChildComponent (progressLabel);
        clearLoopButton.onClick = [this] { setLoopForCurrent (0, 0); };
        setupButton (exportButton, u8 ("Export…"));
        setupButton (exportAllButton, u8 ("Export all…"));
        loopButton.setClickingTogglesState (true);
        // Normalize: prendido = cada semiciclo a 0 dBFS (Amount 100 %); apagado = sin normalizar (Amount 0 %)
        normButton.setClickingTogglesState (true);
        normButton.onClick = [this] { amount.setValue (normButton.getToggleState() ? 100.0 : 0.0, juce::sendNotificationSync); };
        abButton.setClickingTogglesState (true);
        abButton.setToggleState (true, juce::dontSendNotification);
        abButton.getProperties().set ("speaker", true);

        openButton.onClick = [this] { openFiles(); };
        removeButton.onClick = [this] { removeSelected(); };
        playButton.onClick = [this] { if (! transport.isPlaying()) togglePlay(); };
        // Loop: prende o apaga el loop. Prendido y sin loop, lo busca; apagado, se escucha y exporta sin loop
        loopButton.onClick = [this] {
            const bool on = loopButton.getToggleState();
            source.setRegionLooping (on);
            if (! on && loopJob != nullptr) cancelLoopSearch();
            if (juce::isPositiveAndBelow (current, (int) items.size()))
            {
                auto& it = *items[(size_t) current];
                showLoopInView (it);
                updateXfadeView();
                processCurrent(); // con / sin el crossfade del loop
                if (on) ensureLoop (false);
            }
            updateButtons();
        };
        abButton.onClick = [this] { updateAB(); };
        exportButton.onClick = [this] { exportSelected(); };
        exportAllButton.onClick = [this] { exportAll(); };

        formatBox.addItem ("Format: same as original", 1);
        formatBox.addItem ("Format: WAV 24-bit", 2);
        formatBox.addItem ("Format: WAV 32-bit float", 3);
        formatBox.setSelectedId (props->getIntValue ("format", 1), juce::dontSendNotification);
        formatBox.onChange = [this] { props->setValue ("format", formatBox.getSelectedId()); updateInfo(); };
        formatBox.setWantsKeyboardFocus (false);
        addChildComponent (formatBox); // ahora se elige en File ▸ Export Format (queda como modelo)
        // Open, Remove y Export pasan a la barra de menú (File): fuera de la ventana
        for (auto* b : { &openButton, &removeButton, &exportButton, &exportAllButton }) b->setVisible (false);

        // barra de menú de la Mac (y atajos de teclado en cualquier sistema)
        commandManager.registerAllCommandsForTarget (this);
        commandManager.setFirstCommandTarget (this);
        addKeyListener (commandManager.getKeyMappings());
        setApplicationCommandManagerToWatch (&commandManager);
       #if JUCE_MAC
        juce::MenuBarModel::setMacMainMenu (this);
       #endif


        setupSlider (amount, amountLabel, "Amount", 0, 100, 1, " %", 100.0);
        // se cambia arrastrando la línea en el visor; se cuenta en semiciclos (antes en ondas: 1 onda = 2)
        skipHalves = juce::jlimit (0, kMaxSkip, props->containsKey ("skipHalves") ? props->getIntValue ("skipHalves")
                                                                                 : props->getIntValue ("skip", 2) * 2);
        setupSlider (ramp, rampLabel, "Ramp", 0, 100, 1, " %", 0.0); // (por defecto: 0 %)
        ramp.getProperties().set ("resetValue", 0.0);      // ⌥ + clic: 0 %
        // Amount ya no está en la ventana: lo maneja el botón Normalize (100 % o 0 %)
        amount.setValue (100.0, juce::dontSendNotification); // (por defecto: Normalize prendido)
        normButton.setToggleState (amount.getValue() >= 50.0, juce::dontSendNotification);
        amount.setVisible (false);
        amountLabel.setVisible (false);
        // Re-pitch a mano, en cents: de -600 a +600 (6 semitonos para cada lado). Doble clic = 0.
        // Se aplica un instante después de moverla.
        setupSlider (pitchShift, pitchShiftLabel, "Pitch", -600, 600, 0.1, "", 0.0);
        pitchShift.textFromValueFunction = [] (double v) {
            return juce::String (v, 1) + " cents  (" + (v > 0.0 ? "+" : "") + juce::String (v / 100.0, 2) + " st)";
        };
        pitchShift.valueFromTextFunction = [] (const juce::String& t) { return t.getDoubleValue(); };
        pitchShift.updateText();
        pitchShift.wheelStep = 1.0;       // rueda: de a 1 cent
        pitchShift.wheelStepFine = 0.1;   // Shift + rueda: de a 0,1 cent
        pitchShift.setDoubleClickReturnValue (true, 0.0);
        pitchShift.getProperties().set ("resetValue", 0.0); // ⌥ + clic: 0 cents
        pitchShift.setVisible (false);
        pitchShiftLabel.setVisible (false);
        pitchShift.setVelocityModeParameters (0.15, 1, 0.0, true, juce::ModifierKeys::shiftModifier); // Shift + arrastrar = fino
        pitchShift.onValueChange = [this] {
            pendingRetune = true; lastRetuneChange = juce::Time::getMillisecondCounter();
            // al toque: la curva celeste y el pitch de arriba se corren mientras movés la barra;
            // el re-sampleo de verdad se hace cuando la soltás / dejás de moverla
            if (juce::isPositiveAndBelow (current, (int) items.size()))
            {
                const auto& it = *items[(size_t) current];
                const double extra = pitchShift.getValue() / 100.0 - it.tuneSemis;
                wave.setPitchPreview (extra);
                updatePitch (it, extra);
            }
        };

        loopButton.setToggleState (false, juce::dontSendNotification); // (por defecto: apagado)
        source.setRegionLooping (loopButton.getToggleState());
        repeatButton.setToggleState (props->getBoolValue ("repeat", false), juce::dontSendNotification);
        source.setLooping (repeatButton.getToggleState());

        list.setModel (this);
        list.setRowHeight (34);
        list.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
        list.setWantsKeyboardFocus (false);
        addChildComponent (list); // oculta: queda como modelo de la selección (el 808 se elige con el selector de arriba)


        wave.getPlayheadSeconds = [this] { return transport.getCurrentPosition(); };
        wave.isPlaying = [this] { return transport.isPlaying(); };
        wave.isPlayingProcessed = [this] { return abButton.getToggleState(); };
        wave.onSeek = [this] (double s) { transport.setPosition (s); wave.repaint(); };
        wave.getPitchCents = [this] { return pitchShift.getValue(); };
        wave.onPitchDragged = [this] (double cents) { pitchShift.setValue (cents, juce::sendNotificationSync); };
        // la referencia de la línea celeste (la pildorita de los cents, el imán y las teclas del piano):
        // siempre el sustain, como la curva; prender o apagar el loop no la cambia
        wave.getPitchReference = [this] {
            if (! juce::isPositiveAndBelow (current, (int) items.size())) return 0.0;
            const auto& it = *items[(size_t) current];
            const auto p = cyclenorm::measureSustainPitch (it.analysis, it.startSample, (size_t) it.lengthToWrite());
            return p.valid ? p.midi + (pitchShift.getValue() / 100.0 - it.tuneSemis) : 0.0;
        };
        wave.maxSkip = kMaxSkip;
        wave.onEndDragged = [this] (size_t e)
        {
            if (! juce::isPositiveAndBelow (current, (int) items.size())) return;
            auto& it = *items[(size_t) current];
            const size_t full = (size_t) it.original->getNumSamples();
            if (loopActive (it) && e < it.loopEnd) e = it.loopEnd; // Fin no puede cortar el loop
            if (e <= it.startSample) return;                      // ni pasar la de Start
            it.endSample = e >= full ? 0 : e;
            source.setEndSample ((juce::int64) it.endSample);
            wave.setEndSample (it.endSample);
            if (it.fadeOutHalves > 0) settingsChanged(); // el fade out termina en End: se corre con ella
            updateInfo();
        };
        // la línea de Start: si pasa la de Skip, la empuja (Skip nunca queda antes de Start)
        wave.onStartDragged = [this] (size_t s)
        {
            if (! juce::isPositiveAndBelow (current, (int) items.size())) return;
            auto& it = *items[(size_t) current];
            if (loopActive (it) && s >= it.loopStart) return;
            if (s >= (size_t) it.lengthToWrite()) return;
            const size_t oldStart = it.startSample;
            it.startSample = s;
            source.setStartSample ((juce::int64) s);
            wave.setStartSample (s);
            if (it.fadeHalves > 0) settingsChanged(); // el fade in arranca en Start: se corre con ella
            // (Skip no depende de Start: puede quedar a la izquierda)
            // el cursor: si estaba antes de Start, o pegado a Start, sigue a Start (para los dos lados)
            const double posS = transport.getCurrentPosition() * it.sampleRate;
            if (posS < (double) s || (! transport.isPlaying() && std::abs (posS - (double) oldStart) < 2.0))
                transport.setPosition ((double) s / it.sampleRate);
            updateInfo();
        };
        // la manija del fade in: cuántos semiciclos dura (desde Start)
        wave.onFadeDragged = [this] (int n)
        {
            if (! juce::isPositiveAndBelow (current, (int) items.size())) return;
            auto& it = *items[(size_t) current];
            n = juce::jmax (0, n);
            if (n == it.fadeHalves) return;
            it.fadeHalves = n;
            wave.setFadeHalves (n);
            settingsChanged(); // reprocesa (al toque, como Ramp)
        };
        wave.onFadeOutDragged = [this] (int n)
        {
            if (! juce::isPositiveAndBelow (current, (int) items.size())) return;
            auto& it = *items[(size_t) current];
            n = juce::jmax (0, n);
            if (n == it.fadeOutHalves) return;
            it.fadeOutHalves = n;
            wave.setFadeOutHalves (n);
            settingsChanged();
        };
        wave.onLoopDragged = [this] (size_t s0, size_t s1) { setLoopForCurrent (s0, s1); };
        // la manija del crossfade: la muestra donde empieza el fundido -> ondas enteras hasta el fin del loop
        wave.onXfadeDragged = [this] (size_t xfStart)
        {
            if (! juce::isPositiveAndBelow (current, (int) items.size())) return;
            auto& it = *items[(size_t) current];
            if (! it.hasLoop()) return;
            const auto& B = it.analysis.bounds;
            const size_t j = nearestBound (B, it.loopEnd), k = nearestBound (B, xfStart);
            const int n = juce::jlimit (0, maxXfadeCycles (it), j > k ? (int) (j - k) : 0);
            if (n == it.xfadeCycles) return;
            it.xfadeCycles = n;
            updateXfadeView();
            processCurrent();
        };
        wave.onSkipDragged = [this] (int n)
        {
            if (skipHalves == n) { wave.refresh(); return; }
            if (juce::isPositiveAndBelow (current, (int) items.size()))
            {   // Skip no puede pasar la línea de Fin
                auto& it = *items[(size_t) current];
                auto st = readSettings(); st.skipHalves = n;
                if (it.endSample > 0 && cyclenorm::skipEndSample (it.analysis, st) >= it.endSample) return;
                if (loopActive (it) && cyclenorm::skipEndSample (it.analysis, st) > it.loopStart) return; // ni el loop
            }
            skipHalves = n;
            settingsChanged();  // reprocesa
            updateSkipView();   // la línea sigue al mouse enseguida
        };
        addAndMakeVisible (wave);

        info.setColour (juce::Label::textColourId, Theme::inkDim);
        info.setFont (Theme::font (12.0f, 400)); // como el estado de UNISON
        info.setJustificationType (juce::Justification::centredLeft); // la info del loop, desde la línea de al lado de Loop section
        info.setBorderSize ({});
        addAndMakeVisible (info);

        dragOut.makeFile = [this] { return writeForDrag(); };
        addAndMakeVisible (dragOut);

        deviceManager.initialiseWithDefaultDevices (0, 2);
        deviceManager.addAudioCallback (&player);
        player.setSource (&transport);

        setWantsKeyboardFocus (true);
        setSize (928, 580); // (diseño base: 928 x 580, al 100 %)
        updateButtons();
        startTimerHz (60);
    }

    ~MainComponent() override
    {
       #if JUCE_MAC
        juce::MenuBarModel::setMacMainMenu (nullptr);
       #endif
        if (loopJob != nullptr) { loopJob->signalThreadShouldExit(); loopJob.reset(); }
        saveSettings();
        transport.stop();
        transport.setSource (nullptr);
        player.setSource (nullptr);
        deviceManager.removeAudioCallback (&player);
        list.setModel (nullptr);
    }

    //==========================================================================
    void paint (juce::Graphics& g) override
    {
        // fondo claro con degradé, como UNISON
        g.setGradientFill (juce::ColourGradient (Theme::topLight, 0.0f, 0.0f, Theme::topDark, 0.0f, (float) getHeight(), false));
        g.fillAll();
        // título en Akira Expanded
        // (igual que "UNISON": la T empieza justo a 20 del borde izquierdo y las mayúsculas quedan a la
        // misma distancia del borde de arriba que de la píldora de abajo en UNISON (que está en y = 40))
        g.setColour (Theme::inkStrong);
        {
            const auto tf = Theme::titleFont (kTitleCap);
            juce::GlyphArrangement ga, first;
            ga.addLineOfText (tf, "TOOLKIT", 0.0f, 0.0f);
            first.addLineOfText (tf, "T", 0.0f, 0.0f);
            juce::Path text, t;
            ga.createPath (text);
            first.createPath (t);
            const auto tb = t.getBounds();                       // la "T" marca el alto de las mayúsculas
            const float capTop = (40.0f - tb.getHeight()) * 0.5f; // = 13 (como UNISON)
            g.fillPath (text, juce::AffineTransform::translation ((float) kMargin - tb.getX(), capTop - tb.getY()));
        }
        // la info del loop: "Loop 99.1 % · 60 cycles · crossfade Off", cada número en un lugar fijo
        if (loopInfoVals.size() == 3 && info.isVisible())
        {
            const auto f = info.getFont();
            g.setFont (f);
            g.setColour (Theme::inkDim);
            auto wOf = [&] (const juce::String& t) { return juce::GlyphArrangement::getStringWidth (f, t); };
            // como los renglones del pitch: "Loop:" y después columnas de ancho fijo (cada valor con su
            // unidad, alineado a la izquierda), separadas con " · "
            const juce::String dot = juce::String::fromUTF8 ("  \xc2\xb7  ");
            const juce::String cols[] = { loopInfoVals[0] + " %", loopInfoVals[1] + " cycles", "crossfade " + loopInfoVals[2] };
            const juce::String tmpl[] = { "100.0 %", "999 cycles", "crossfade Off" };
            float x = (float) info.getX();
            g.drawSingleLineText ("Loop:", juce::roundToInt (x), (int) kTitleBaseline);
            x += wOf ("Loop:") + 5.0f;
            for (int k = 0; k < 3; ++k)
            {
                const float w = juce::jmax (wOf (tmpl[k]), wOf (cols[k]));
                g.drawSingleLineText (cols[k], juce::roundToInt (x), (int) kTitleBaseline);
                x += w;
                if (k < 2) { g.drawSingleLineText (dot, juce::roundToInt (x), (int) kTitleBaseline); x += wOf (dot); }
            }
        }
        // el ruido que agregó el proceso: arriba de Ramp, como la info del loop
        if (noisePct >= 0.0 && info.isVisible())
        {
            const auto f = info.getFont();
            g.setFont (f);
            const float x = (float) rampLabel.getX();
            g.setColour (Theme::inkDim);
            g.drawSingleLineText ("Noise:", juce::roundToInt (x), (int) kTitleBaseline);
            const float x2 = x + juce::GlyphArrangement::getStringWidth (f, "Noise:") + 5.0f;
            g.drawSingleLineText (juce::String (noisePct, 2) + " %", juce::roundToInt (x2), (int) kTitleBaseline);
        }
        // el 808 abierto, al lado del título
        if (juce::isPositiveAndBelow (current, (int) items.size()))
        {
            g.setColour (Theme::ink);
            const auto f = Theme::font (14.0f, 400);
            g.setFont (f);
            const auto b = baselineRow (f);
            g.drawText (items[(size_t) current]->file.getFileName(), fileName.toFloat().withY (b.getY()).withHeight (b.getHeight()),
                        juce::Justification::centredLeft, true);
        }
        // el visor: panel negro con esquinas redondeadas (como el osciloscopio de UNISON)
        g.setColour (Theme::darkBg);
        g.fillRoundedRectangle (waveArea.toFloat(), 12.0f); // esquinas redondeadas (como al principio)
        // divisores finos entre los grupos de botones
        g.setColour (juce::Colours::black.withAlpha (0.10f));
        for (int x : groupDividers) g.fillRect ((float) x, (float) buttonRowY, 1.0f, (float) buttonRowH);
        if (groupDividersRight > 0 && groupDividersRight < dragOut.getX()) g.fillRect ((float) groupDividersRight, (float) buttonRowY, 1.0f, (float) buttonRowH);
    }

    // Medidas del diseño base de UNISON (después toda la ventana se achica al 80,3 %, como UNISON):
    // márgenes de 20, fila del título de 38 (título de 14 de alto de mayúsculas), botones de 33 de
    // alto visible con letra de 14, barras de 24 de alto (las pastillas de UNISON), visor con margen de 7.
    static constexpr int kMargin = 10, kButtonH = 33, kPillH = 24;
    static constexpr float kTitleCap = 14.0f;

    // ⌥ + clic en el margen derecho (donde se agarra para escalar): la ventana vuelve al tamaño por defecto
    std::function<void()> onResetWindowSize;
    void mouseDown (const juce::MouseEvent& e) override
    {
        // (en el margen de la derecha o en el de abajo: la esquina de abajo a la derecha, de donde se agranda)
        if (e.mods.isAltDown() && (e.x >= waveArea.getRight() || e.y >= waveArea.getBottom()) && onResetWindowSize) onResetWindowSize();
    }

    void resized() override
    {
        // altos (diseño de 580): barra de arriba 80 · recuadro del visor 490 (Original 20 + 185 + 40 · Processed 40 + 185 + 20) · 10 abajo
        auto r = getLocalBounds();
        r.removeFromLeft (kMargin); r.removeFromRight (kMargin);
        titleArea = r.withHeight (40); // el título: mayúsculas de 13 a 27 (como UNISON); el nombre centrado en 20
        r.removeFromTop (35);          // los botones: lo visible de 37 a 70 (10 de aire arriba, hasta el título, y abajo, hasta el visor)
        {
            // fila del título: TOOLKIT · el 808 abierto · info del loop · pitch medido (a la derecha)
            auto t = titleArea;
            const float titleW = juce::GlyphArrangement::getStringWidth (Theme::titleFont (kTitleCap), "TOOLKIT");
            t.removeFromLeft ((int) titleW + 22);
            titleRest = t; // el nombre, el pitch y la info del loop: se acomodan en layoutTitleRow (abajo)
        }

        // botones: del ancho de su texto (como las pastillas de UNISON); el componente lleva la sombra
        // (4 a los costados, 2 arriba, 6 abajo), así lo visible mide kButtonH de alto
        auto row = r.removeFromTop (kButtonH + 8);
        row.setLeft (row.getX() - 4); // lo visible del primer botón (el transporte) arranca a 20, justo debajo del título
        const auto bf = Theme::font (14.0f, 400);
        auto widthFor = [&] (juce::Button& b, float extra) {
            return (int) std::ceil (juce::GlyphArrangement::getStringWidth (bf, b.getButtonText()) + 2.0f * 14.0f + extra) + 8;
        };
        auto place = [&] (juce::Button& b, float extra) { b.setBounds (row.removeFromLeft (widthFor (b, extra))); };
        // transporte pegado: cada parte de 50 de ancho visible; las puntas de afuera llevan 4 más (la sombra)
        auto placeIcon = [&] (juce::Button& b) {
            const auto seg = b.getProperties()["segment"].toString();
            b.setBounds (row.removeFromLeft (50 + (seg == "mid" ? 0 : 4)));
        };
        // entre secciones: 20 visibles de botón a botón (cada botón lleva 4 de sombra), con la rayita a 9
        auto divider = [&] { row.removeFromLeft (5); groupDividers.push_back (row.getX()); row.removeFromLeft (1 + 6); };
        groupDividers.clear();
        // 1) transporte: ▶ ■ ⟲ (el loop de escucha)
        placeIcon (playButton); placeIcon (stopButton); placeIcon (repeatButton);
        divider();
        // 2) qué se escucha
        {
            const int w = (int) std::ceil (juce::GlyphArrangement::getStringWidth (bf, "Processed") + 2.0f * 14.0f + 24.0f) + 8;
            abButton.setBounds (row.removeFromLeft (w)); // con el parlante (mismo ancho con Processed u Original)
        }
        divider();
        pitchInfoX = groupDividers.back(); // la info del pitch arranca en esta línea (entre Processed y Normalize)
        // 5) a la derecha de todo: Drag to DAW
        {
            const int w = (int) std::ceil (juce::GlyphArrangement::getStringWidth (bf, "Drag to DAW") + 2.0f * 20.0f);
            dragOut.setBounds (row.removeFromRight (w + 6).withTrimmedLeft (6).withTrimmedTop (2).withHeight (kButtonH + 1).translated (1, 0)); // (el borde visible a 20 del de la ventana, como el título)
        }
        // 3) el proceso: Normalize y Ramp
        place (normButton, 0);
        {
            const auto lf = Theme::font (12.5f, 300);
            const int textW = (int) std::ceil (juce::GlyphArrangement::getStringWidth (lf, "Ramp"));
            row.removeFromLeft (6); // "Ramp" a 10 del borde visible de Normalize
            const int cy = row.getY() + 2 + kButtonH / 2;
            rampLabel.setBorderSize ({});
            rampLabel.setBounds (juce::Rectangle<int> (row.getX(), cy - kPillH / 2, textW + 2, kPillH));
            row.removeFromLeft (textW + 10);
            ramp.setBounds (row.removeFromLeft (125).withHeight (kPillH).withY (cy - kPillH / 2)); // la barra de Ramp: del mismo largo que antes
            // Pitch ya no tiene barra: se cambia solo arrastrando la línea en el visor
            // (el slider queda oculto, guarda el valor)
        }
        // de la barra (sin sombra) a Loop: 20 visibles, la rayita a 9
        row.removeFromLeft (9); groupDividers.push_back (row.getX()); row.removeFromLeft (1 + 6);
        // 4) al lado de Ramp, el loop de sección (el que se exporta): prenderlo, buscarlo, el siguiente, borrarlo
        {
            const int wNum = (int) std::ceil (juce::GlyphArrangement::getStringWidth (bf, "Option 5") + 2.0f * 14.0f) + 8; // fijo: no salta al cambiar el número
            auto wOf = [&] (juce::Button& b) { return &b == &otherLoopButton ? wNum : widthFor (b, 0); };
            for (auto* b : { (juce::Button*) &loopButton, (juce::Button*) &otherLoopButton })
                b->setBounds (row.removeFromLeft (wOf (*b)));
            findLoopButton.setVisible (false);  // (quedan para el menú y los atajos: Loop ya busca solo)
            clearLoopButton.setVisible (false);
        }
        // de Option N a Drag to DAW: 20 visibles, la rayita a 9
        groupDividersRight = otherLoopButton.getRight() - 4 + 9;
        loopInfoX = loopButton.getX() + 4; // la info del loop arranca en el borde izquierdo (visible) de Loop section
        buttonRowY = playButton.getY() + 4; buttonRowH = kButtonH - 6;
        layoutTitleRow();
        // el visor: el recuadro negro de 100 a 630 (530), a 20 de los costados; abajo quedan 20 libres
        // el recuadro negro: a 10 de los costados y de abajo (como todo lo de arriba)
        waveArea = juce::Rectangle<int> (kMargin, 80, getWidth() - 2 * kMargin, getHeight() - 80 - kMargin); // (a la misma línea que los botones y el título)
        // el visor arma dos carriles de 265 (40 + 185 + 40); los 20 de más arriba no se ven (el
        // componente sobresale 20 por arriba del recuadro: recorta y no recibe clics ahí); abajo
        // sobran 10 dentro del recuadro
        wave.setBounds (waveArea.getX(), waveArea.getY() - WaveformView::kHiddenPad, waveArea.getWidth(), 530);
        wave.visibleBottom = waveArea.getBottom() - wave.getY(); // de borde a borde del recuadro // dos visores de 265: 40 + 185 (la onda) + 40 cada uno; a la derecha, hasta el borde (el teclado de 40)
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::spaceKey) { togglePlay(); return true; }
        // ⌘Z / ⇧⌘Z y los demás atajos: comandos del menú (commandManager)
        if (k.getTextCharacter() == 'a' || k.getTextCharacter() == 'A')
        {
            abButton.setToggleState (! abButton.getToggleState(), juce::dontSendNotification);
            updateAB();
            return true;
        }
        return false;
    }

    // Archivos que llegan al abrir la app (argumentos, o "Abrir con" / soltar sobre el ícono del Dock)
    void openFromCommandLine (const juce::String& commandLine)
    {
        juce::Array<juce::File> fs;
        for (auto& a : juce::StringArray::fromTokens (commandLine, true))
        {
            const juce::File f (a.unquoted());
            if (a.isNotEmpty() && ! a.startsWith ("-") && f.exists()) fs.add (f);
        }
        if (! fs.isEmpty()) addFiles (fs);

        // solo para pruebas: CYCLENORM_TEST_EXPORT=/ruta/salida.wav exporta el primero y cierra
        const auto testOut = juce::SystemStats::getEnvironmentVariable ("CYCLENORM_TEST_EXPORT", {});
        if (testOut.isNotEmpty() && ! items.empty())
        {
            juce::String err;
            const auto testEnd = juce::SystemStats::getEnvironmentVariable ("CYCLENORM_TEST_END", {});
            if (testEnd.isNotEmpty()) // la muestra más cercana a la pedida, como hace la línea de Fin
            {
                auto& it0 = *items.front();
                const auto st = readSettings();
                const auto& B = cyclenorm::segmentBounds (it0.analysis, st);
                size_t best = 0; double bd = 1e300;
                for (auto v : B) if (std::abs ((double) v - testEnd.getDoubleValue()) < bd) { bd = std::abs ((double) v - testEnd.getDoubleValue()); best = v; }
                it0.endSample = best;
            }
            const auto testStart = juce::SystemStats::getEnvironmentVariable ("CYCLENORM_TEST_START", {});
            if (testStart.isNotEmpty())
            {
                auto& it0 = *items.front();
                const auto B = cyclenorm::segmentBounds (it0.analysis, readSettings());
                size_t best = 0; double bd = 1e300;
                for (auto v : B) if (std::abs ((double) v - testStart.getDoubleValue()) < bd) { bd = std::abs ((double) v - testStart.getDoubleValue()); best = v; }
                it0.startSample = best;
                processItem (it0);
            }
            if (juce::SystemStats::getEnvironmentVariable ("CYCLENORM_TEST_LOOP", {}).isNotEmpty())
            {
                list.selectRow (0);
                findLoopForCurrent();
                if (loopJob != nullptr) { loopJob->waitForThreadToExit (20000); loopSearchFinished(); }
                const int xf = juce::SystemStats::getEnvironmentVariable ("CYCLENORM_TEST_XFADE", "0").getIntValue();
                items.front()->xfadeCycles = juce::jmin (xf, maxXfadeCycles (*items.front()));
                processCurrent();
                if (juce::SystemStats::getEnvironmentVariable ("CYCLENORM_TEST_LOOPOFF", {}).isNotEmpty())
                    loopButton.setToggleState (false, juce::sendNotificationSync); // (prueba: exportar con Loop apagado)
            }
            const bool ok = writeItem (*items.front(), juce::File (testOut), err);
            std::printf ("test export: %s %s\n", ok ? "OK" : "FAIL", err.toRawUTF8());
            juce::Timer::callAfterDelay (1500, [] { juce::JUCEApplication::quit(); });
        }
    }

    //== arrastrar archivos ====================================================
    bool isInterestedInFileDrag (const juce::StringArray& files) override
    {
        for (auto& f : files) if (isAudioFile (juce::File (f))) return true;
        return false;
    }
    void filesDropped (const juce::StringArray& files, int, int) override
    {
        juce::Array<juce::File> fs;
        for (auto& f : files)
        {
            juce::File file (f);
            if (file.isDirectory()) file.findChildFiles (fs, juce::File::findFiles, false);
            else fs.add (file);
        }
        addFiles (fs);
    }

private:
    juce::AudioFormatManager formats;
    juce::AudioDeviceManager deviceManager;
    juce::AudioSourcePlayer player;
    juce::AudioTransportSource transport;
    ABSource source;
    std::unique_ptr<juce::PropertiesFile> props;
    std::unique_ptr<juce::FileChooser> chooser;

    std::vector<std::unique_ptr<Item>> items;
    int current = -1;

    juce::TextButton normButton, stopButton, repeatButton;
    juce::TextButton openButton, removeButton, playButton, loopButton, abButton, exportButton, exportAllButton,
                     findLoopButton, otherLoopButton, clearLoopButton;
    double progressValue = 0.0;
    juce::ProgressBar progressBar { progressValue };
    juce::Label progressLabel;
    std::unique_ptr<LoopSearchJob> loopJob;
    juce::ComboBox formatBox;
    juce::ApplicationCommandManager commandManager;
    juce::Slider amount;
    StepSlider ramp; // (⌥ + clic = 0 %)
    StepSlider pitchShift;
    static constexpr int kMaxSkip = 512; // semiciclos
    int skipHalves = 4;

    // deshacer / rehacer: fotos del estado editable (por archivo + los controles globales)
    struct EditState
    {
        Item* item = nullptr;
        double amount = 0, ramp = 0, pitch = 0;
        int skip = 0, xfade = 0, candIdx = -1, fade = 0, fadeOut = 0;
        size_t endS = 0, startS = 0, l0 = 0, l1 = 0;
        std::vector<cyclenorm::LoopResult> cands;
        bool same (const EditState& o) const
        {
            return item == o.item && std::abs (amount - o.amount) < 1e-9 && std::abs (ramp - o.ramp) < 1e-9 && std::abs (pitch - o.pitch) < 1e-9
                && skip == o.skip && xfade == o.xfade && candIdx == o.candIdx && endS == o.endS && startS == o.startS && fade == o.fade && fadeOut == o.fadeOut
                && l0 == o.l0 && l1 == o.l1 && cands.size() == o.cands.size();
        }
    };
    std::vector<EditState> undoStack, redoStack;
    EditState lastState;
    bool haveLastState = false;
    juce::Label amountLabel, rampLabel, info, pitchShiftLabel;
    juce::ListBox list;
    WaveformView wave;
    DragOut dragOut;
    juce::Rectangle<int> titleArea;
    juce::Rectangle<int> waveArea, fileName, titleRest;
    int pitchInfoX = 0, loopInfoX = 0;
    // los textos de la fila del título, apoyados en el mismo renglón que TOOLKIT: la base de las
    // letras en y = 27, a 20 del borde de arriba (visible) de los botones
    static constexpr float kTitleBaseline = 27.0f;
    static juce::Rectangle<float> baselineRow (const juce::Font& f)
    {
        return { 0.0f, kTitleBaseline - f.getAscent(), 0.0f, f.getHeight() };
    }
    juce::StringArray loopInfoVals; // los números de la info del loop (se dibujan con lugar fijo)
    // el ruido que agregó el proceso en lo que suena (de Start a End, o hasta el fin del loop)
    void updateNoise (const Item& it)
    {
        noisePct = 0.0;
        if (it.processed != nullptr && it.original != nullptr
            && it.processed->getNumSamples() == it.original->getNumSamples()
            && it.processed->getNumChannels() == it.original->getNumChannels())
        {
            std::vector<const float*> P, O;
            for (int ch = 0; ch < it.processed->getNumChannels(); ++ch)
            {
                P.push_back (it.processed->getReadPointer (ch));
                O.push_back (it.original->getReadPointer (ch));
            }
            const bool lp = loopActive (it);
            noisePct = cyclenorm::measureNoise (P, O, (size_t) it.processed->getNumSamples(), it.startSample,
                                                lp ? it.loopEnd : (size_t) it.lengthToWrite(), it.sampleRate);
        }
    }
    bool noiseDirty = false;
    juce::uint32 noiseChangedMs = 0;
    double noisePct = -1.0; // el ruido que agregó el proceso (% de la energía, ponderada como el oído; -1 = nada abierto)
    std::vector<int> groupDividers; int groupDividersRight = 0, buttonRowY = 0, buttonRowH = 0;
    bool pendingReprocess = false, pendingRetune = false;
    juce::uint32 lastRetuneChange = 0;
    int outputCheckTicks = 0;
    juce::String lastFailedOutput;
    juce::uint32 lastChange = 0, lastProcessMs = 0;

    //==========================================================================
    void setupSlider (juce::Slider& s, juce::Label& l, const juce::String& name, double lo, double hi, double step,
                      const juce::String& suffix, double value)
    {
        s.setSliderStyle (juce::Slider::LinearBar);
        s.setRange (lo, hi, step);
        s.setTextValueSuffix (suffix);
        s.setValue (value, juce::dontSendNotification);
        s.setWantsKeyboardFocus (false);
        s.onValueChange = [this] { settingsChanged(); };
        addAndMakeVisible (s);
        l.setText (name, juce::dontSendNotification);
        l.setFont (Theme::font (12.5f, 300)); // como los títulos de UNISON (Window, Latency, Listen)
        l.setColour (juce::Label::textColourId, Theme::inkDim);
        addAndMakeVisible (l);
    }

    cyclenorm::Settings readSettings() const
    {
        cyclenorm::Settings s;
        s.halfCycles = true; // siempre semiciclo por semiciclo
        s.amount = amount.getValue() / 100.0;
        s.ramp = ramp.getValue() / 100.0;
        s.autoTarget = false;
        s.targetDb = 0.0; // el objetivo es siempre 0 dBFS
        s.skipHalves = skipHalves;
        s.thresholdDb = -200.0; // sin umbral: se normaliza todo, también la cola y el ruido
        return s;
    }

    void saveSettings()
    {
        props->setValue ("skipHalves", skipHalves);
        props->saveIfNeeded();
    }

    // Re-procesar es rápido (0,5 a 2 ms), así que se hace al toque mientras arrastrás Skip o movés Ramp:
    // como mucho una vez cada ~16 ms (lo que dura un cuadro de pantalla); si llega antes, lo hace el timer.
    // Reprocesar mientras se arrastra: como mucho a 60 por segundo, pero con un 808 largo (que tarda
    // más) se deja el doble de lo que tardó, así entre una y otra hay tiempo de dibujar y el mouse
    // no se traba
    double lastProcessCost = 0.0;
    juce::uint32 reprocessGap() const { return (juce::uint32) juce::jmax (16.0, lastProcessCost * 2.0 + 4.0); }
    void runProcessTimed()
    {
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        processCurrent();
        lastProcessCost = juce::Time::getMillisecondCounterHiRes() - t0;
    }
    void settingsChanged()
    {
        lastChange = juce::Time::getMillisecondCounter();
        if (loopJob == nullptr && lastChange - lastProcessMs >= reprocessGap())
        {
            pendingReprocess = false;
            lastProcessMs = lastChange;
            runProcessTimed();
            list.repaint();
        }
        else pendingReprocess = true;
    }

    void timerCallback() override
    {
        commitUndoStep (false);
        if (loopJob != nullptr)
        {
            progressValue = loopJob->progress.load();
            progressLabel.setText (u8 ("Finding the best loop (trying every pair)…  ") + juce::String ((int) std::round (progressValue * 100.0)) + " %",
                                   juce::dontSendNotification);
            if (loopJob->finished.load()) loopSearchFinished();
        }
        // arrastrando la línea celeste: sólo vista previa; el re-sampleo, al soltar
        if (pendingRetune && ! wave.isDraggingPitch() && juce::Time::getMillisecondCounter() - lastRetuneChange > 150)
        {
            pendingRetune = false;
            retuneCurrent (0, pitchShift.getValue());
        }
        if (pendingReprocess && juce::Time::getMillisecondCounter() - lastProcessMs >= reprocessGap())
        {
            pendingReprocess = false;
            lastProcessMs = juce::Time::getMillisecondCounter();
            runProcessTimed();
            list.repaint();
        }
        // el ruido: se mide cuando se dejó de mover (120 ms sin cambios), así arrastrar Ramp no se traba
        if (noiseDirty && ! pendingReprocess && juce::Time::getMillisecondCounter() - noiseChangedMs > 120)
        {
            noiseDirty = false;
            if (juce::isPositiveAndBelow (current, (int) items.size())) { updateNoise (*items[(size_t) current]); repaint (titleArea); }
        }
        if ((bool) playButton.getProperties()["lit"] != transport.isPlaying()) // Play prendido (azul) mientras suena
        {
            playButton.getProperties().set ("lit", transport.isPlaying());
            playButton.repaint();
        }
        if (++outputCheckTicks >= 60) { outputCheckTicks = 0; followSystemOutput(); } // una vez por segundo
        // que espacio / A / flechas anden siempre (salvo mientras se escribe un valor)
        if (juce::Component::getCurrentlyFocusedComponent() == nullptr && isShowing()
            && juce::Process::isForegroundProcess())
            grabKeyboardFocus();
    }

    //== salida de audio =======================================================
    // Seguir siempre la salida que tenga elegida la Mac: si enchufás auriculares (que en las Mac
    // nuevas son un dispositivo aparte), o conectás unos bluetooth, o cambiás la salida en
    // Ajustes > Sonido, la app pasa a esa salida sola, sin cortar la reproducción.
    void followSystemOutput()
    {
        auto* type = deviceManager.getCurrentDeviceTypeObject();
        if (type == nullptr) return;
        type->scanForDevices(); // la lista cambia al enchufar / desenchufar
        const auto names = type->getDeviceNames (false);
        const int idx = type->getDefaultDeviceIndex (false);
        if (! juce::isPositiveAndBelow (idx, names.size())) return;
        const auto wanted = names[idx];

        auto setup = deviceManager.getAudioDeviceSetup();
        const bool deviceOpen = deviceManager.getCurrentAudioDevice() != nullptr;
        if (deviceOpen && setup.outputDeviceName == wanted) { lastFailedOutput.clear(); return; }
        if (wanted == lastFailedOutput) return; // ya se intentó y no abrió: no insistir cada segundo

        const bool wasPlaying = transport.isPlaying();
        setup.outputDeviceName = wanted;
        setup.inputDeviceName = {};
        setup.useDefaultOutputChannels = true;
        setup.sampleRate = 0;   // el que use ese dispositivo
        setup.bufferSize = 0;
        const auto err = deviceManager.setAudioDeviceSetup (setup, true);
        lastFailedOutput = err.isEmpty() ? juce::String() : wanted;
        if (wasPlaying && ! transport.isPlaying()) transport.start();
    }

    //== archivos ==============================================================
    bool isAudioFile (const juce::File& f) const
    {
        return f.isDirectory() || f.hasFileExtension ("wav;aif;aiff;flac;mp3;m4a;caf");
    }

    void openFiles()
    {
        chooser = std::make_unique<juce::FileChooser> ("Choose audio", juce::File (props->getValue ("lastDir")),
                                                       "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.m4a;*.caf");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc) { addFiles (fc.getResults()); });
    }

    // Un 808 por vez: el que se abre (o se suelta) reemplaza al que estaba.
    // Si llegan varios (o una carpeta), se abre el primero que se pueda leer.
    void addFiles (const juce::Array<juce::File>& files)
    {
        juce::StringArray failed;
        std::unique_ptr<Item> item;
        for (auto& f : files)
        {
            if (! f.existsAsFile() || ! isAudioFile (f)) continue;
            item = loadItem (f);
            if (item == nullptr) { failed.add (f.getFileName()); continue; }
            props->setValue ("lastDir", f.getParentDirectory().getFullPathName());
            break;
        }
        if (item != nullptr)
        {
            if (loopJob != nullptr) cancelLoopSearch();
            transport.stop();
            transport.setSource (nullptr);
            for (auto& old : items) forgetUndo (old.get());
            items.clear();
            current = -1;
            resetToDefaults(); // cada sample nuevo arranca con los valores por defecto
            items.push_back (std::move (item));
            list.updateContent();
            list.deselectAllRows();
            list.selectRow (0);
            if (current != 0) selectItem (0);
        }
        if (! failed.isEmpty())
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Could not open",
                                                    failed.joinIntoString ("\n"));
        updateButtons();
    }

    // los valores por defecto (al abrir la app y con cada sample nuevo): Processed y Normalize
    // prendidos, Ramp en 0 % y Loop apagado
    void resetToDefaults()
    {
        abButton.setToggleState (true, juce::dontSendNotification);
        abButton.setButtonText ("Processed");
        source.setPlayProcessed (true);
        amount.setValue (100.0, juce::dontSendNotification);
        normButton.setToggleState (true, juce::dontSendNotification);
        ramp.setValue (0.0, juce::dontSendNotification);
        loopButton.setToggleState (false, juce::dontSendNotification);
        source.setRegionLooping (false);
        saveSettings();
    }

    std::unique_ptr<Item> loadItem (const juce::File& f)
    {
        std::unique_ptr<juce::AudioFormatReader> r (formats.createReaderFor (f));
        if (r == nullptr || r->lengthInSamples <= 0 || r->numChannels == 0) return nullptr;
        if (r->lengthInSamples > (juce::int64) (r->sampleRate * 60 * 30)) return nullptr; // más de 30 minutos, no
        auto it = std::make_unique<Item>();
        it->file = f;
        it->sampleRate = r->sampleRate;
        it->bits = (int) r->bitsPerSample;
        it->isFloat = r->usesFloatingPointData;
        auto buf = std::make_shared<juce::AudioBuffer<float>> ((int) r->numChannels, (int) r->lengthInSamples);
        r->read (buf.get(), 0, (int) r->lengthInSamples, 0, true, true);
        // un WAV en coma flotante puede venir pasado de 0 dB (no recortado): se baja entero hasta que el
        // pico quede justo en 0 dBFS, así nunca cruza la línea (ni en el Original ni al exportar)
        {
            float pk = 0.0f;
            for (int ch = 0; ch < buf->getNumChannels(); ++ch)
                pk = juce::jmax (pk, buf->getMagnitude (ch, 0, buf->getNumSamples()));
            if (pk > 1.0f) buf->applyGain (1.0f / pk);
        }
        it->original = buf;
        it->analysis = cyclenorm::analyse (toChannels (*buf), r->sampleRate);
        it->loaded = buf;
        it->loadedAnalysis = it->analysis;
        return it;
    }

    void removeSelected()
    {
        if (! juce::isPositiveAndBelow (current, (int) items.size())) return;
        transport.stop();
        transport.setSource (nullptr);
        const int removed = current;
        forgetUndo (items[(size_t) current].get());
        items.erase (items.begin() + current);
        current = -1;
        list.updateContent();
        list.deselectAllRows();
        if (! items.empty()) list.selectRow (juce::jmin (removed, (int) items.size() - 1));
        else { wave.clear(); source.setBuffers (nullptr, nullptr); }
        updateButtons();
        updateInfo();
    }

    //== proceso ===============================================================
    void processItem (Item& it) const
    {
        auto s = readSettings();
        if (it.skipOverride >= 0) s.skipHalves = it.skipOverride;
        auto curve = cyclenorm::gainCurve (it.analysis, s, &it.targetUsedDb);
        const auto& src = *it.original;
        cyclenorm::limitCurve (curve, cyclenorm::skipEndSample (it.analysis, s), it.targetUsedDb, [&] (size_t i) {
            float m = 0;
            for (int ch = 0; ch < src.getNumChannels(); ++ch) m = juce::jmax (m, std::abs (src.getSample (ch, (int) i)));
            return m;
        }, &it.analysis.halfBounds);
        auto out = std::make_shared<juce::AudioBuffer<float>> (it.original->getNumChannels(), it.original->getNumSamples()); // (se llena abajo: original × curva)
        float peak = 0.0f;
        for (int ch = 0; ch < out->getNumChannels(); ++ch)
        {
            auto* d = out->getWritePointer (ch);
            const auto* in = src.getReadPointer (ch);
            const int n = out->getNumSamples();
            float pk = 0.0f;
            for (int i = 0; i < n; ++i)
            {
                d[i] = in[i] * curve[(size_t) i];
                pk = juce::jmax (pk, std::abs (d[i]));
            }
            peak = juce::jmax (peak, pk);
        }
        // fade in: desde Start, lineal, hasta n semiciclos después (siempre termina en un cruce)
        if (it.fadeHalves > 0)
        {
            const size_t limit = loopActive (it) ? it.loopStart : (size_t) it.lengthToWrite();
            const size_t f0 = it.startSample;
            const size_t f1 = WaveformView::fadeEndOf (cyclenorm::segmentBounds (it.analysis, s), f0, it.fadeHalves, limit);
            if (f1 > f0)
                for (int ch = 0; ch < out->getNumChannels(); ++ch)
                {
                    auto* d = out->getWritePointer (ch);
                    const double len = (double) (f1 - f0);
                    for (size_t i = f0; i < f1 && i < (size_t) out->getNumSamples(); ++i)
                        d[i] *= (float) ((double) (i - f0) / len);
                }
        }
        // fade out: hasta End, lineal, desde n semiciclos antes (con loop no: se corta en el fin del loop)
        if (it.fadeOutHalves > 0 && ! loopActive (it))
        {
            const auto& B = cyclenorm::segmentBounds (it.analysis, s);
            size_t lower = it.startSample;
            if (it.fadeHalves > 0) lower = WaveformView::fadeEndOf (B, it.startSample, it.fadeHalves, (size_t) it.lengthToWrite());
            const size_t f1 = (size_t) it.lengthToWrite();
            const size_t f0 = WaveformView::fadeOutStartOf (B, f1, it.fadeOutHalves, lower);
            if (f1 > f0)
                for (int ch = 0; ch < out->getNumChannels(); ++ch)
                {
                    auto* d = out->getWritePointer (ch);
                    const double len = (double) (f1 - f0);
                    for (size_t i = f0; i < f1 && i < (size_t) out->getNumSamples(); ++i)
                        d[i] *= (float) ((double) (f1 - i) / len);
                }
        }
        // crossfade del loop (metido en el audio, así suena igual al tocarlo que en el sampler)
        if (it.hasLoop() && it.loopEnd <= (size_t) out->getNumSamples())
        {
            // la calidad mide los puntos de loop (lo mismo que mide la búsqueda), antes del crossfade
            cyclenorm::AudioView v;
            for (int ch = 0; ch < out->getNumChannels(); ++ch) v.ch.push_back (out->getReadPointer (ch));
            v.n = (size_t) out->getNumSamples();
            it.loopQuality = cyclenorm::loopQuality (it.analysis, v, it.loopStart, it.loopEnd);
            // el crossfade cambia la onda: sólo con Loop prendido (apagado, se escucha y se ve sin él;
            // al exportar se aplica igual, ver writeItem)
            const size_t xfLen = loopButton.getToggleState() ? xfadeSamples (it) : 0;
            if (xfLen > 0)
            {
                std::vector<float*> chs;
                for (int ch = 0; ch < out->getNumChannels(); ++ch) chs.push_back (out->getWritePointer (ch));
                cyclenorm::applyLoopCrossfade (chs, it.loopStart, it.loopEnd, xfLen);
            }
        }
        it.processed = out;
        it.outPeak = peak;
        processShown (it);
    }
    // El dibujo de abajo con el pitch movido: el mismo proceso (normalizar, Ramp, fade in, crossfade)
    // pero sobre el audio sin re-afinar, así el dibujo no cambia con el pitch (sólo se estira al largo).
    void processShown (Item& it) const
    {
        if (std::abs (it.tuneSemis) < 1e-9 || it.loaded == nullptr) { it.shownProcessed = nullptr; return; }
        Item d;
        d.original = it.loaded; d.analysis = it.loadedAnalysis; d.sampleRate = it.sampleRate;
        d.fadeHalves = it.fadeHalves; d.fadeOutHalves = it.fadeOutHalves; d.xfadeCycles = it.xfadeCycles;
        // las posiciones, llevadas por tiempo (el afinado es el cargado estirado) y pegadas al cruce
        // más cercano. (No por número de semiciclo: el ruido antes del 808 tiene cruces que cambian
        // al re-afinar, y el fade o el Skip quedaban corridos en el dibujo.)
        const double speed = std::pow (2.0, it.tuneSemis / 12.0);
        auto toLoaded = [&] (size_t v) { return (size_t) std::llround ((double) v * speed); };
        const auto& LH = it.loadedAnalysis.halfBounds;
        const auto& LB = it.loadedAnalysis.bounds;
        auto mapHalf = [&] (size_t v) { return (v == 0 || LH.empty()) ? (size_t) 0 : LH[nearestBound (LH, toLoaded (v))]; };
        auto mapCycle = [&] (size_t v) { return LB.empty() ? (size_t) 0 : LB[nearestBound (LB, toLoaded (v))]; };
        d.startSample = mapHalf (it.startSample);
        d.endSample = mapHalf (it.endSample);
        if (it.hasLoop()) { d.loopStart = mapCycle (it.loopStart); d.loopEnd = mapCycle (it.loopEnd); }
        // Skip: el mismo lugar, contado en semiciclos del audio sin re-afinar
        if (skipHalves >= 1 && ! LH.empty())
        {
            const auto& HB = it.analysis.halfBounds;
            const size_t e = (size_t) skipHalves - 1 < HB.size() ? HB[(size_t) skipHalves - 1] : it.analysis.numSamples;
            d.skipOverride = juce::jlimit (1, kMaxSkip, (int) nearestBound (LH, toLoaded (e)) + 1);
        }
        processItem (d); // (d no está re-afinado: no vuelve a entrar acá)
        it.shownProcessed = d.processed;
    }
    // lo que dibuja el visor: con el pitch movido, el audio sin re-afinar (estirado al largo actual)
    void showBuffersOf (const Item& it)
    {
        const bool tuned = std::abs (it.tuneSemis) > 1e-9 && it.loaded != nullptr && it.shownProcessed != nullptr;
        wave.setDisplay (tuned ? it.loaded : nullptr, tuned ? it.shownProcessed : nullptr);
    }

    //== loop ==================================================================
    // índice de la onda entera más cercana a una muestra (las puntas del loop pueden estar unas
    // muestras corridas del cruce por el ajuste fino)
    static size_t nearestBound (const std::vector<size_t>& B, size_t v)
    {
        size_t best = 0, bd = (size_t) -1;
        for (size_t k = 0; k < B.size(); ++k) { const size_t d = B[k] > v ? B[k] - v : v - B[k]; if (d < bd) { bd = d; best = k; } }
        return best;
    }
    // ondas de crossfade posibles: como mucho las ondas del loop, y las que hay antes del inicio
    static int maxXfadeCycles (const Item& it)
    {
        if (! it.hasLoop()) return 0;
        const auto& B = it.analysis.bounds;
        if (B.empty()) return 0;
        const size_t i = nearestBound (B, it.loopStart), j = nearestBound (B, it.loopEnd);
        return (int) juce::jmin (j > i ? j - i : (size_t) 0, i);
    }
    // largo del crossfade en muestras (ondas enteras antes del fin del loop, en sus cruces por cero)
    static size_t xfadeSamples (const Item& it)
    {
        const int n = juce::jmin (it.xfadeCycles, maxXfadeCycles (it));
        if (n <= 0) return 0;
        const auto& B = it.analysis.bounds;
        const size_t j = nearestBound (B, it.loopEnd);
        const size_t from = B[j - (size_t) n];
        return it.loopEnd > from ? it.loopEnd - from : 0;
    }
    void updateXfadeView()
    {
        if (! juce::isPositiveAndBelow (current, (int) items.size())) return;
        wave.setLoopXfade (xfadeSamples (*items[(size_t) current]));
    }

    // Loop prendido y con un loop puesto (apagado, el loop queda guardado pero no se usa ni se ve)
    bool loopActive (const Item& it) const { return it.hasLoop() && loopButton.getToggleState(); }
    void showLoopInView (const Item& it)
    {
        const bool on = loopButton.getToggleState();
        wave.setLoop (on ? it.loopStart : 0, on ? it.loopEnd : 0);
    }
    // Loop prendido y sin loop (o con uno que quedó afuera de Start / Skip / End): lo busca solo
    void ensureLoop (bool silent)
    {
        if (! loopButton.getToggleState() || loopJob != nullptr || ! juce::isPositiveAndBelow (current, (int) items.size())) return;
        auto& it = *items[(size_t) current];
        if (it.hasLoop())
        {
            const size_t from = cyclenorm::skipEndSample (it.analysis, readSettings());
            if (it.loopStart >= juce::jmax (from, it.startSample) && (int) it.loopEnd <= it.lengthToWrite()) return;
            it.loopStart = it.loopEnd = 0; it.loopCands.clear(); it.loopCandIdx = -1;
        }
        silentLoopSearch = silent;
        findLoopForCurrent();
    }
    bool silentLoopSearch = false;

    void findLoopForCurrent()
    {
        if (! juce::isPositiveAndBelow (current, (int) items.size())) return;
        auto& it = *items[(size_t) current];
        // se busca sobre el normalizado sin crossfade (una copia, en otro hilo)
        const auto saved = std::make_pair (it.loopStart, it.loopEnd);
        it.loopStart = it.loopEnd = 0;
        processItem (it);
        auto copy = std::make_shared<juce::AudioBuffer<float>> (*it.processed);
        it.loopStart = saved.first; it.loopEnd = saved.second;
        processItem (it);
        const auto from = cyclenorm::skipEndSample (it.analysis, readSettings());
        loopJob = std::make_unique<LoopSearchJob> (&it, copy, it.analysis, from, (size_t) it.lengthToWrite());
        progressValue = 0.0;
        progressBar.setVisible (true);
        progressLabel.setVisible (true);
        progressLabel.toFront (false);
        info.setVisible (false);
        findLoopButton.setButtonText ("Cancel");
        loopJob->startThread();
    }

    void cancelLoopSearch()
    {
        if (loopJob == nullptr) return;
        loopJob->signalThreadShouldExit();
        loopJob->stopThread (4000);
        loopJob.reset();
        endLoopSearchUi();
    }

    void endLoopSearchUi()
    {
        progressBar.setVisible (false);
        progressLabel.setVisible (false);
        info.setVisible (true);
        findLoopButton.setButtonText ("Find loop");
        updateButtons();
    }

    void loopSearchFinished()
    {
        auto job = std::move (loopJob);
        job->stopThread (1000);
        endLoopSearchUi();
        if (job->cancelled.load()) return;
        // el archivo puede haberse quitado o cambiado mientras buscaba
        const bool stillThere = std::any_of (items.begin(), items.end(), [&] (auto& p) { return p.get() == job->item; });
        if (! stillThere) return;
        auto& it = *job->item;
        if (job->results.empty())
        {
            if (silentLoopSearch) return; // (búsqueda automática al abrir otro 808: sin cartel)
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Find loop",
                u8 ("No loop found between the Skip and End lines: at least about 6 steady cycles are needed."));
            return;
        }
        it.loopCands = job->results;
        if (items[(size_t) current].get() != &it) { it.loopCandIdx = 0; it.loopStart = it.loopCands[0].start; it.loopEnd = it.loopCands[0].end; return; }
        applyLoopCandidate (0);
        if (! loopButton.getToggleState()) { loopButton.setToggleState (true, juce::sendNotification); }
    }

    void applyLoopCandidate (int idx)
    {
        if (! juce::isPositiveAndBelow (current, (int) items.size())) return;
        auto& it = *items[(size_t) current];
        if (! juce::isPositiveAndBelow (idx, (int) it.loopCands.size())) return;
        const auto& c = it.loopCands[(size_t) idx];
        it.xfadeCycles = 0; // cada opción (Next loop o una búsqueda nueva) arranca sin crossfade
        setLoopForCurrent (c.start, c.end);
        it.loopCandIdx = idx;
        updateInfo();
        updateButtons();
    }

    // "Otro loop": el siguiente de los mejores candidatos (vuelve al primero al final)
    void nextLoopCandidate()
    {
        if (! juce::isPositiveAndBelow (current, (int) items.size())) return;
        auto& it = *items[(size_t) current];
        if (it.loopCands.empty()) return;
        applyLoopCandidate ((it.loopCandIdx + 1) % (int) it.loopCands.size());
    }

    void setLoopForCurrent (size_t s0, size_t s1)
    {
        if (! juce::isPositiveAndBelow (current, (int) items.size())) return;
        auto& it = *items[(size_t) current];
        if (s1 <= s0) s0 = s1 = 0;
        it.loopStart = s0; it.loopEnd = s1;
        it.loopCandIdx = -1; // (applyLoopCandidate lo vuelve a poner)
        if (s1 == 0) it.loopCands.clear();
        it.xfadeCycles = juce::jmin (it.xfadeCycles, maxXfadeCycles (it)); // si el loop se achicó
        source.setLoopRegion ((juce::int64) s0, (juce::int64) s1);
        showLoopInView (it);
        updateXfadeView();
        processCurrent();   // aplica el crossfade y mide la calidad
        updateButtons();
    }

    //== deshacer / rehacer =====================================================
    EditState captureState() const
    {
        EditState st;
        st.amount = amount.getValue(); st.ramp = ramp.getValue(); st.skip = skipHalves;
        if (juce::isPositiveAndBelow (current, (int) items.size()))
        {
            const auto& it = *items[(size_t) current];
            st.item = items[(size_t) current].get();
            st.pitch = (double) it.tuneSt * 100.0 + it.tuneCents; // el aplicado
            st.endS = it.endSample; st.startS = it.startSample; st.fade = it.fadeHalves; st.fadeOut = it.fadeOutHalves; st.l0 = it.loopStart; st.l1 = it.loopEnd;
            st.xfade = it.xfadeCycles; st.cands = it.loopCands; st.candIdx = it.loopCandIdx;
        }
        return st;
    }
    // Cuando el estado se asienta (nada pendiente y sin el mouse apretado) y cambió, guarda un paso.
    // Así un arrastre entero (Skip, End, loop, Pitch) es un solo paso de deshacer.
    void commitUndoStep (bool force)
    {
        if (! force && (pendingRetune || pendingReprocess || loopJob != nullptr || wave.isDraggingPitch()
                        || juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown()))
            return;
        auto st = captureState();
        if (! haveLastState || st.item != lastState.item) { lastState = std::move (st); haveLastState = true; return; } // cambio de archivo: no es un paso
        if (st.same (lastState)) return;
        undoStack.push_back (std::move (lastState));
        if (undoStack.size() > 300) undoStack.erase (undoStack.begin());
        redoStack.clear();
        lastState = std::move (st);
        commandManager.commandStatusChanged();
    }
    void flushPendingEdits()
    {
        if (pendingRetune && juce::isPositiveAndBelow (current, (int) items.size())) { pendingRetune = false; retuneCurrent (0, pitchShift.getValue()); }
        if (pendingReprocess) { pendingReprocess = false; processCurrent(); list.repaint(); }
        commitUndoStep (true);
    }
    void undo()
    {
        flushPendingEdits();
        if (undoStack.empty()) return;
        auto st = std::move (undoStack.back()); undoStack.pop_back();
        redoStack.push_back (captureState());
        applyState (st);
    }
    void redo()
    {
        flushPendingEdits();
        if (redoStack.empty()) return;
        auto st = std::move (redoStack.back()); redoStack.pop_back();
        undoStack.push_back (captureState());
        applyState (st);
    }
    void applyState (const EditState& st)
    {
        if (loopJob != nullptr) cancelLoopSearch();
        int row = -1;
        for (size_t i = 0; i < items.size(); ++i) if (items[i].get() == st.item) row = (int) i;
        if (row >= 0 && row != current) { list.selectRow (row); if (current != row) selectItem (row); }
        amount.setValue (st.amount, juce::dontSendNotification);
        normButton.setToggleState (st.amount >= 50.0, juce::dontSendNotification);
        ramp.setValue (st.ramp, juce::dontSendNotification);
        skipHalves = st.skip;
        saveSettings();
        pendingReprocess = false;
        if (row >= 0)
        {
            auto& it = *items[(size_t) row];
            pitchShift.setValue (st.pitch, juce::dontSendNotification);
            pendingRetune = false;
            retuneCurrent (0, st.pitch);
            it.endSample = st.endS; it.startSample = st.startS; it.fadeHalves = st.fade; it.fadeOutHalves = st.fadeOut; it.loopStart = st.l0; it.loopEnd = st.l1;
            it.xfadeCycles = st.xfade; it.loopCands = st.cands; it.loopCandIdx = st.candIdx;
            source.setEndSample ((juce::int64) it.endSample);
            source.setStartSample ((juce::int64) it.startSample);
            wave.setStartSample (it.startSample);
            wave.setFadeHalves (it.fadeHalves);
            wave.setFadeOutHalves (it.fadeOutHalves);
            source.setLoopRegion ((juce::int64) it.loopStart, (juce::int64) it.loopEnd);
            wave.setEndSample (it.endSample);
            showLoopInView (it);
            updateXfadeView();
            processCurrent();
            updatePitch (it);
        }
        list.repaint();
        updateButtons();
        lastState = captureState(); haveLastState = true;
        commandManager.commandStatusChanged();
    }
    void forgetUndo (const Item* gone)
    {
        auto drop = [gone] (std::vector<EditState>& v) {
            v.erase (std::remove_if (v.begin(), v.end(), [gone] (const EditState& e) { return e.item == gone; }), v.end()); };
        drop (undoStack); drop (redoStack);
        if (lastState.item == gone) haveLastState = false;
    }

    void processCurrent()
    {
        if (! juce::isPositiveAndBelow (current, (int) items.size())) return;
        auto& it = *items[(size_t) current];
        processItem (it);
        source.setProcessed (it.processed);
        wave.setProcessed (it.processed);
        showBuffersOf (it);
        updateSkipView();
        updateInfo();
    }

    void selectItem (int row)
    {
        if (! juce::isPositiveAndBelow (row, (int) items.size())) return;
        const bool wasPlaying = transport.isPlaying();
        transport.stop();
        current = row;
        auto& it = *items[(size_t) row];
        pitchShift.setValue ((double) it.tuneSt * 100.0 + it.tuneCents, juce::dontSendNotification);
        pendingRetune = false;
        processItem (it);
        source.setBuffers (it.original, it.processed);
        source.setFadeLength ((int) std::round (it.sampleRate * 0.010)); // fundidos de 10 ms (original/procesado, cambios)
        source.setEndSample ((juce::int64) it.endSample);
        source.setStartSample ((juce::int64) it.startSample);
        source.setNextReadPosition ((juce::int64) it.startSample);
        transport.setSource (&source, 0, nullptr, it.sampleRate, 2);
        source.setRegionLooping (loopButton.getToggleState());
        source.setLooping (repeatButton.getToggleState());
        wave.setData (it.original, it.processed, cyclenorm::segmentBounds (it.analysis, readSettings()), it.sampleRate, true);
        showBuffersOf (it);
        wave.setEndSample (it.endSample);
        wave.setStartSample (it.startSample);
        wave.setFadeHalves (it.fadeHalves);
        wave.setFadeOutHalves (it.fadeOutHalves);
        wave.setLoopBounds (it.analysis.bounds);
        wave.setPitchCurve (pitchCurveOf (it), it.tuneSemis);
        showLoopInView (it);
        wave.setLoopXfade (xfadeSamples (it));
        source.setLoopRegion ((juce::int64) it.loopStart, (juce::int64) it.loopEnd);
        updateSkipView();
        if (wasPlaying) transport.start();
        updateInfo();
        updateButtons();
        ensureLoop (true); // con Loop prendido: busca el loop de este 808 si no tiene
    }

    // dónde terminan las ondas salteadas con Skip (para pintarlas en gris en el procesado)
    void updateSkipView()
    {
        if (! juce::isPositiveAndBelow (current, (int) items.size())) return;
        wave.setSkipEnd (cyclenorm::skipEndSample (items[(size_t) current]->analysis, readSettings()));
    }

    void stopPlayback()
    {
        transport.stop();
        transport.setPosition (juce::isPositiveAndBelow (current, (int) items.size())
                                   ? (double) items[(size_t) current]->startSample / items[(size_t) current]->sampleRate : 0.0);
        wave.repaint();
        commandManager.commandStatusChanged();
    }
    void togglePlay()
    {
        if (current < 0) return;
        if (transport.isPlaying())
        {
            transport.stop();
            transport.setPosition (0); // el próximo play arranca desde el principio
            wave.repaint();
        }
        else
        {
            if (transport.getCurrentPosition() >= transport.getLengthInSeconds() - 0.001) transport.setPosition (0);
            transport.start();
        }
        commandManager.commandStatusChanged(); // Play / Stop en el menú
    }

    void updateAB()
    {
        source.setPlayProcessed (abButton.getToggleState());
        abButton.setButtonText (abButton.getToggleState() ? "Processed" : "Original"); // (el parlante ya dice que es lo que se escucha)
        wave.refresh();
    }

    void updateButtons()
    {
        const bool has = juce::isPositiveAndBelow (current, (int) items.size());
        removeButton.setEnabled (has);
        playButton.setEnabled (has);
        exportButton.setEnabled (has);
        exportAllButton.setEnabled (! items.empty());
        findLoopButton.setEnabled (has || loopJob != nullptr);
        otherLoopButton.setEnabled (has && loopJob == nullptr && loopButton.getToggleState() && items[(size_t) current]->loopCands.size() > 1);
        {   // "Loop N": el número de la opción que está sonando (o "Loop" solo si no hay opciones)
            const bool any = has && items[(size_t) current]->loopCandIdx >= 0 && ! items[(size_t) current]->loopCands.empty();
            otherLoopButton.setButtonText ("Option " + juce::String (any ? items[(size_t) current]->loopCandIdx + 1 : 1));
        }
        clearLoopButton.setEnabled (has && items[(size_t) current]->hasLoop());
        dragOut.setEnabled (has);
        dragOut.repaint();
        commandManager.commandStatusChanged(); // el menú habilita / deshabilita lo mismo
    }

    bool exportsFloat (const Item& it) const
    {
        const int id = formatBox.getSelectedId();
        if (id == 3) return true;
        if (id == 2) return false;
        return it.isFloat || it.bits > 24;
    }

    // fila del título: el nombre del 808 hasta la línea de Processed | Normalize y a la derecha de todo
    // la info del loop (el pitch va abajo de los gráficos)
    void layoutTitleRow()
    {
        auto t = titleRest;
        if (t.isEmpty()) return;
        const int px = juce::jlimit (t.getX(), t.getRight(), pitchInfoX);
        fileName = t.withRight (juce::jlimit (t.getX(), t.getRight(), juce::jmin (loopInfoX, rampLabel.getX()) - 16)); // hasta donde empieza la info de los clicks
        // el nombre arranca justo en el borde izquierdo (visible) del botón Processed / Original
        fileName.setLeft (juce::jlimit (t.getX(), fileName.getRight(), abButton.getX() + 4));
        auto rest = t.withLeft (juce::jlimit (px, t.getRight(), loopInfoX));
        {   // (los avisos: con la base de las letras en el mismo renglón)
            const auto f = info.getFont();
            info.setBounds (rest.withY ((int) std::round (kTitleBaseline - f.getAscent())).withHeight ((int) std::ceil (f.getHeight())));
        }
        progressBar.setBounds (rest.withSizeKeepingCentre (rest.getWidth() - 12, 18).withX (rest.getX()));
        progressLabel.setBounds (progressBar.getBounds());
        repaint (titleArea);
    }
    void updateInfo()
    {
        repaint (titleArea); // el nombre del 808 abierto
        if (! juce::isPositiveAndBelow (current, (int) items.size()))
        {
            noisePct = -1.0;
            info.setText ({}, juce::dontSendNotification);
            return;
        }
        auto& it = *items[(size_t) current];
        updatePitch (it);
        noiseDirty = true; noiseChangedMs = juce::Time::getMillisecondCounter(); // (se mide en el timer: no frena al arrastrar)
        const double peakDb = juce::Decibels::gainToDecibels (it.outPeak, -200.0f);
        juce::ignoreUnused (peakDb);
        // corto (va en la fila del título): el loop y los avisos
        juce::String t;
        loopInfoVals.clear();
        if (loopActive (it))
        {
            // se dibuja aparte (en paint), con un lugar fijo para cada número: el texto no se mueve
            const auto& B = it.analysis.bounds;
            const auto nCyc = std::distance (std::lower_bound (B.begin(), B.end(), it.loopStart), std::lower_bound (B.begin(), B.end(), it.loopEnd));
            loopInfoVals = { juce::String (it.loopQuality * 100.0, 1), juce::String ((int) nCyc),
                             it.xfadeCycles > 0 ? juce::String (juce::jmin (it.xfadeCycles, maxXfadeCycles (it))) : juce::String ("Off") };
        }
        repaint (info.getBounds());
        if (it.outPeak > 1.0001f && ! exportsFloat (it)) // (el ataque salteado con Skip puede venir pasado de 0 dB)
        {
            loopInfoVals.clear();
            t = u8 ("CLIPPING! The attack goes over 0 dBFS: export as 32-bit float");
            info.setColour (juce::Label::textColourId, Theme::bad);
        }
        else info.setColour (juce::Label::textColourId, Theme::inkDim);
        info.setText (t, juce::dontSendNotification);
    }

    //== Tune ==================================================================
    // Re-pitch a mano (Transpose + Fine): re-samplea el archivo, como un sampler al tocar otra tecla:
    // cambia el tono y el largo juntos. El loop, End y Skip quedan en las mismas ondas.
    void retuneCurrent (int st, double cents)
    {
        if (! juce::isPositiveAndBelow (current, (int) items.size())) return;
        auto& it = *items[(size_t) current];
        if (st == it.tuneSt && std::abs (cents - it.tuneCents) < 1e-9) return;
        if (loopJob != nullptr) cancelLoopSearch();

        // las posiciones actuales (en muestras): se llevan al audio afinado por tiempo (estirado) y se
        // pegan al cruce más cercano. (No por número de onda: el ruido antes del 808 tiene cruces que
        // cambian al re-afinar, y Skip, Start o el loop saltaban de lugar.)
        const auto& HB = it.analysis.halfBounds;
        const bool hadLoop = it.hasLoop();
        const size_t oldLs = it.loopStart, oldLe = it.loopEnd;
        const bool hadEnd = it.endSample > 0;
        const bool hadStart = it.startSample > 0;
        const size_t oldEnd = it.endSample, oldStart = it.startSample;
        auto skipEndOf = [&] (int n) { return n >= 1 ? ((size_t) n - 1 < HB.size() ? HB[(size_t) n - 1] : it.analysis.numSamples) : (size_t) 0; };
        const size_t oldSkipEnd = skipEndOf (skipHalves);
        const double semis = (double) st + cents / 100.0;
        const double oldSpeed = std::pow (2.0, it.tuneSemis / 12.0), newSpeed = std::pow (2.0, semis / 12.0);
        auto scaled = [&] (size_t v) { return (size_t) std::llround ((double) v * oldSpeed / newSpeed); };

        if (std::abs (semis) < 1e-9)
        {
            it.original = it.loaded;
            it.analysis = it.loadedAnalysis;
        }
        else
        {
            const double speed = std::pow (2.0, semis / 12.0);
            auto out = cyclenorm::resample (toChannels (*it.loaded), speed);
            auto buf = std::make_shared<juce::AudioBuffer<float>> ((int) out.size(), (int) out[0].size());
            for (int ch = 0; ch < buf->getNumChannels(); ++ch)
                buf->copyFrom (ch, 0, out[(size_t) ch].data(), buf->getNumSamples());
            it.original = buf;
            it.analysis = cyclenorm::analyse (out, it.sampleRate);
        }
        it.tuneSt = st;
        it.tuneCents = cents;
        it.tuneSemis = semis;

        const auto& B2 = it.analysis.bounds;
        const auto& HB2 = it.analysis.halfBounds;
        auto snapTo = [&] (const std::vector<size_t>& Bn, size_t v) { return Bn.empty() ? (size_t) 0 : Bn[nearestBound (Bn, scaled (v))]; };
        if (hadLoop && ! B2.empty())
        {
            const size_t ls = snapTo (B2, oldLs), le = snapTo (B2, oldLe);
            if (le > ls) { it.loopStart = ls; it.loopEnd = le; } else it.loopStart = it.loopEnd = 0;
        }
        else { it.loopStart = it.loopEnd = 0; }
        it.loopCands.clear();
        it.loopCandIdx = -1;
        it.endSample = hadEnd ? snapTo (HB2, oldEnd) : 0;
        it.startSample = hadStart ? snapTo (HB2, oldStart) : 0;
        // Skip: el mismo lugar (por tiempo), contado de nuevo en semiciclos del audio afinado
        auto skipFor = [&] (int n, size_t oldE) {
            if (n < 1 || HB2.empty()) return n;
            return juce::jlimit (1, kMaxSkip, (int) nearestBound (HB2, scaled (oldE)) + 1);
        };
        skipHalves = skipFor (skipHalves, oldSkipEnd);

        // refrescar sin cortar: sigue sonando desde el mismo punto del sonido y mantiene el zoom
        const double factor = oldSpeed / newSpeed; // cuánto se estiró el audio
        const double pos = transport.getCurrentPosition() * factor;
        processItem (it);
        source.setBuffers (it.original, it.processed);
        source.setEndSample ((juce::int64) it.endSample);
        source.setStartSample ((juce::int64) it.startSample);
        source.setLoopRegion ((juce::int64) it.loopStart, (juce::int64) it.loopEnd);
        transport.setPosition (pos);
        wave.setData (it.original, it.processed, cyclenorm::segmentBounds (it.analysis, readSettings()), it.sampleRate, false);
        showBuffersOf (it);
        wave.setEndSample (it.endSample);
        wave.setStartSample (it.startSample);
        wave.setLoopBounds (it.analysis.bounds);
        wave.setPitchCurve (pitchCurveOf (it), it.tuneSemis);
        showLoopInView (it);
        wave.setLoopXfade (xfadeSamples (it));
        updateSkipView();
        updateInfo();
        updateButtons();
    }

    // Curva de pitch para el visor: una onda = un punto (centro de la onda, Hz), con los cruces exactos.
    // Fuera los puntos que no son de la nota: los que saltan más de un 35 % respecto de sus vecinos
    // (el corte del final, un rulito) y las ondas casi en silencio.
    static std::vector<std::pair<double, double>> pitchCurveOf (const Item& it)
    {
        std::vector<std::pair<double, double>> pts;
        const auto& bf = it.analysis.boundsFrac;
        const auto& src = *it.original;
        if (bf.size() < 3) return pts;
        float maxAbs = 0;
        for (int ch = 0; ch < src.getNumChannels(); ++ch) maxAbs = juce::jmax (maxAbs, src.getMagnitude (ch, 0, src.getNumSamples()));
        std::vector<double> hz;
        for (size_t k = 0; k + 1 < bf.size(); ++k) hz.push_back (it.sampleRate / juce::jmax (1.0, bf[k + 1] - bf[k]));
        for (size_t k = 0; k < hz.size(); ++k)
        {
            std::vector<double> nb;
            for (size_t q = (k >= 2 ? k - 2 : 0); q <= juce::jmin (hz.size() - 1, k + 2); ++q) if (q != k) nb.push_back (hz[q]);
            std::nth_element (nb.begin(), nb.begin() + (long) nb.size() / 2, nb.end());
            if (! nb.empty() && std::abs (hz[k] - nb[nb.size() / 2]) > 0.35 * nb[nb.size() / 2]) continue;
            const int s0 = (int) bf[k], s1 = juce::jmin ((int) bf[k + 1] + 1, src.getNumSamples());
            float pk = 0;
            for (int ch = 0; ch < src.getNumChannels(); ++ch) pk = juce::jmax (pk, src.getMagnitude (ch, s0, juce::jmax (1, s1 - s0)));
            if (pk < maxAbs * 0.003f) continue; // casi silencio (-50 dB)
            pts.push_back ({ 0.5 * (bf[k] + bf[k + 1]), hz[k] });
        }
        // la cola: desde el final hacia atrás, fuera los puntos que se alejan más de un 3 % de la
        // nota que venía sonando (mediana de los 8 anteriores): es el corte o el fade del archivo
        while (pts.size() > 10)
        {
            std::vector<double> prev;
            for (size_t q = pts.size() - 9; q < pts.size() - 1; ++q) prev.push_back (pts[q].second);
            std::nth_element (prev.begin(), prev.begin() + (long) prev.size() / 2, prev.end());
            const double med = prev[prev.size() / 2];
            if (std::abs (pts.back().second - med) > 0.03 * med) pts.pop_back(); else break;
        }
        return pts;
    }

    // Pitch preciso: en el loop si hay, si no entre la línea de Skip y la de End
    cyclenorm::PitchInfo pitchOf (const Item& it, const cyclenorm::Settings& st, bool& inLoop) const
    {
        inLoop = loopActive (it); // con Loop apagado: el sustain
        if (inLoop) return cyclenorm::measurePitch (it.analysis, it.loopStart > 8 ? it.loopStart - 8 : 0, it.loopEnd + 8);
        // sin loop: la parte estable (después del glide), en todo el sonido hasta End (el Skip es sólo para la normalización)
        juce::ignoreUnused (st);
        return cyclenorm::measureSustainPitch (it.analysis, it.startSample, (size_t) it.lengthToWrite());
    }
    void updatePitch (const Item& it, double previewSemis = 0.0)
    {
        // dos renglones en el visor Original: el del sustain (gris) y, con loop, el del loop (azul)
        // cada renglón va por columnas (separadas con tabs) y se dibujan con lugar fijo: los dos quedan
        // alineados aunque cambien los valores
        auto line = [&] (cyclenorm::PitchInfo p, bool inLoop) -> juce::String {
            p.midi += previewSemis;                                  // vista previa de la barra Pitch
            p.hz *= std::pow (2.0, previewSemis / 12.0);
            const juce::String label = juce::String ("Pitch (") + (inLoop ? "loop" : p.sustain ? "sustain" : p.gliding ? "gliding" : "Skip to End") + ")";
            if (! p.valid) return label + "\t--";
            auto sgn = [] (double v, int dec) { return (v >= 0 ? juce::String ("+") : juce::String (u8 ("−"))) + juce::String (std::abs (v), dec); };
            const double cents = (p.midi - std::round (p.midi)) * 100.0;
            const double tuned = it.tuneSemis + previewSemis;
            return label + "\t" + juce::String (cyclenorm::noteName (p.midi)) + "\t" + sgn (cents, 1) + " cents"
                 + "\t" + juce::String (p.hz, 3) + " Hz" + "\ttuned " + sgn (tuned * 100.0, 1) + " cents"
                 + "\tdrift " + (p.cycles >= 4 ? sgn (p.driftCents, 1) + " cents" : juce::String ("--"));
        };
        const auto sus = line (cyclenorm::measureSustainPitch (it.analysis, it.startSample, (size_t) it.lengthToWrite()), false);
        const bool inLoop = loopActive (it);
        const auto lp = inLoop ? line (cyclenorm::measurePitch (it.analysis, it.loopStart > 8 ? it.loopStart - 8 : 0, it.loopEnd + 8), true) : juce::String();
        wave.setPitchInfo (sus, juce::Colour (0xff8b9096), lp, kAccent); // (se muestran en el visor Original)
        layoutTitleRow();
    }

    //== exportar ==============================================================
    struct OutFormat { bool aiff; int bits; };

    OutFormat outFormatFor (const Item& it) const
    {
        const int id = formatBox.getSelectedId();
        if (id == 2) return { false, 24 };
        if (id == 3) return { false, 32 };
        const bool srcAiff = it.file.hasFileExtension ("aif;aiff") && ! loopActive (it); // con loop: WAV (lleva los puntos de loop)
        if (srcAiff && ! it.isFloat && it.bits <= 24) return { true, juce::jmax (8, it.bits) };
        if (it.isFloat || it.bits > 24) return { false, 32 };
        return { false, it.bits == 8 || it.bits == 16 || it.bits == 24 ? it.bits : 24 };
    }

    juce::String defaultName (const Item& it) const
    {
        juce::String tag;
        if (std::abs (it.tuneSemis) > 1e-9)
        {
            bool inLoop = false;
            const auto p = pitchOf (it, readSettings(), inLoop);
            if (p.valid) tag = "_" + juce::String (cyclenorm::noteName (p.midi)).replace ("#", "s");
        }
        return it.file.getFileNameWithoutExtension() + "_norm" + tag + (outFormatFor (it).aiff ? ".aif" : ".wav");
    }

    bool writeItem (const Item& it, const juce::File& dest, juce::String& error) const
    {
        const auto fmtInfo = outFormatFor (it);
        juce::WavAudioFormat wav;
        juce::AiffAudioFormat aiff;
        juce::AudioFormat& fmt = fmtInfo.aiff ? static_cast<juce::AudioFormat&> (aiff) : static_cast<juce::AudioFormat&> (wav);

        dest.deleteFile();
        auto stream = std::make_unique<juce::FileOutputStream> (dest);
        if (! stream->openedOk()) { error = "Could not write " + dest.getFullPathName(); return false; }
        // Puntos de loop adentro del WAV (bloque "smpl", el que leen el Sampler de Logic, Kontakt,
        // Simpler...) y la nota raíz medida, así el sampler lo afina y lo loopea solo.
        // con Loop apagado se exporta sin loop: sin crossfade (ya no lo tiene), sin puntos de loop y
        // hasta la línea de End (no se corta en el fin del loop)
        const bool withLoop = it.hasLoop() && loopButton.getToggleState();
        auto outBuf = it.processed;
        juce::StringPairArray meta;
        if (withLoop && ! fmtInfo.aiff)
        {
            bool inLoop = false;
            const auto pm = pitchOf (it, readSettings(), inLoop);
            const double note = pm.valid ? pm.midi : cyclenorm::midiNoteOf (it.analysis, it.loopStart, it.loopEnd);
            const int unity = juce::jlimit (0, 127, (int) std::floor (note));
            const double frac = juce::jlimit (0.0, 0.999999, note - std::floor (note));
            meta.set ("MidiUnityNote", juce::String (unity));
            meta.set ("MidiPitchFraction", juce::String ((juce::int32) (juce::uint32) (frac * 4294967296.0))); // (como entero de 32 bits)
            meta.set ("SamplePeriod", juce::String ((int) std::round (1.0e9 / it.sampleRate)));
            meta.set ("NumSampleLoops", "1");
            meta.set ("Loop0Identifier", "0");
            meta.set ("Loop0Type", "0"); // hacia adelante
            meta.set ("Loop0Start", juce::String ((juce::int64) (it.loopStart - it.startSample))); // (el archivo empieza en Start)
            meta.set ("Loop0End", juce::String ((juce::int64) (it.loopEnd - it.startSample) - 1)); // en el WAV el fin es la última muestra del loop
            meta.set ("Loop0Fraction", "0");
            meta.set ("Loop0PlayCount", "0"); // infinito
        }
        std::unique_ptr<juce::AudioFormatWriter> w (fmt.createWriterFor (stream.get(), it.sampleRate,
                                                                          (unsigned int) outBuf->getNumChannels(),
                                                                          fmtInfo.bits, meta, 0));
        if (w == nullptr) { error = "Format not supported"; return false; }
        stream.release(); // ahora es del writer
        const int from = juce::jlimit (0, juce::jmax (0, it.lengthToWrite() - 1), (int) it.startSample); // desde la línea de Start
        if (! w->writeFromAudioSampleBuffer (*outBuf, from, (withLoop ? it.exportLength() : it.lengthToWrite()) - from)) // hasta el fin del loop (o la línea de End)
        {
            error = "Error writing " + dest.getFileName();
            return false;
        }
        return true;
    }

    void exportSelected()
    {
        if (! juce::isPositiveAndBelow (current, (int) items.size())) return;
        auto& it = *items[(size_t) current];
        const juce::File dir (props->getValue ("exportDir", it.file.getParentDirectory().getFullPathName()));
        chooser = std::make_unique<juce::FileChooser> ("Export", dir.getChildFile (defaultName (it)),
                                                       outFormatFor (it).aiff ? "*.aif" : "*.wav");
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [this, itPtr = &it] (const juce::FileChooser& fc) {
                                  auto f = fc.getResult();
                                  if (f == juce::File()) return;
                                  const bool stillThere = std::any_of (items.begin(), items.end(), [itPtr] (auto& p) { return p.get() == itPtr; });
                                  if (! stillThere) return;
                                  props->setValue ("exportDir", f.getParentDirectory().getFullPathName());
                                  juce::String err;
                                  if (! writeItem (*itPtr, f, err))
                                      juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Error", err);
                              });
    }

    void exportAll()
    {
        if (items.empty()) return;
        chooser = std::make_unique<juce::FileChooser> (u8 ("Choose the destination folder"),
                                                       juce::File (props->getValue ("exportDir", items.front()->file.getParentDirectory().getFullPathName())));
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [this] (const juce::FileChooser& fc) {
                                  auto dir = fc.getResult();
                                  if (! dir.isDirectory()) return;
                                  props->setValue ("exportDir", dir.getFullPathName());
                                  juce::StringArray errors;
                                  int ok = 0;
                                  for (auto& p : items)
                                  {
                                      processItem (*p); // con los ajustes actuales
                                      juce::String err;
                                      const auto dest = dir.getChildFile (defaultName (*p)).getNonexistentSibling (false);
                                      if (writeItem (*p, dest, err)) ++ok; else errors.add (err);
                                  }
                                  juce::AlertWindow::showMessageBoxAsync (
                                      errors.isEmpty() ? juce::MessageBoxIconType::InfoIcon : juce::MessageBoxIconType::WarningIcon,
                                      "Export all",
                                      juce::String (ok) + " file(s) exported to\n" + dir.getFullPathName()
                                          + (errors.isEmpty() ? juce::String() : "\n\n" + errors.joinIntoString ("\n")));
                              });
    }

    juce::File writeForDrag()
    {
        if (! juce::isPositiveAndBelow (current, (int) items.size())) return {};
        auto& it = *items[(size_t) current];
        // carpeta fija (no temporal): el DAW puede seguir leyendo el archivo después
        auto dir = juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("Toolkit");
        dir.createDirectory();
        auto dest = dir.getChildFile (defaultName (it)).getNonexistentSibling (false);
        juce::String err;
        return writeItem (it, dest, err) ? dest : juce::File();
    }

    //== barra de menú / comandos =============================================
    juce::ApplicationCommandTarget* getNextCommandTarget() override { return nullptr; }
    void getAllCommands (juce::Array<juce::CommandID>& c) override
    {
        c.addArray ({ cmdOpen, cmdRemove, cmdExport, cmdFmtSame, cmdFmt24, cmdFmtFloat,
                      cmdUndo, cmdRedo, cmdPlay, cmdLoopPlay, cmdHearing, cmdFitView, cmdLoopSection,
                      cmdFindLoop, cmdNextLoop, cmdClearLoop });
    }
    void getCommandInfo (juce::CommandID id, juce::ApplicationCommandInfo& result) override
    {
        const bool has = juce::isPositiveAndBelow (current, (int) items.size());
        const auto cmd = juce::ModifierKeys::commandModifier, shift = juce::ModifierKeys::shiftModifier;
        switch (id)
        {
            case cmdOpen:      result.setInfo (u8 ("Open…"), "Add 808s", "File", 0); result.addDefaultKeypress ('o', cmd); break;
            case cmdRemove:    result.setInfo ("Close", "", "File", 0); result.addDefaultKeypress ('w', cmd); result.setActive (has); break;
            case cmdExport:    result.setInfo (u8 ("Export…"), "", "File", 0); result.addDefaultKeypress ('e', cmd); result.setActive (has); break;
            case cmdExportAll: result.setInfo (u8 ("Export All…"), "", "File", 0); result.addDefaultKeypress ('e', cmd | shift); result.setActive (! items.empty()); break;
            case cmdFmtSame:   result.setInfo ("Same as Original", "", "File", 0); result.setTicked (formatBox.getSelectedId() == 1); break;
            case cmdFmt24:     result.setInfo ("WAV 24-bit", "", "File", 0); result.setTicked (formatBox.getSelectedId() == 2); break;
            case cmdFmtFloat:  result.setInfo ("WAV 32-bit Float", "", "File", 0); result.setTicked (formatBox.getSelectedId() == 3); break;
            case cmdUndo:      result.setInfo ("Undo", "", "Edit", 0); result.addDefaultKeypress ('z', cmd); result.setActive (! undoStack.empty() || pendingRetune || pendingReprocess || (haveLastState && ! captureState().same (lastState))); break;
            case cmdRedo:      result.setInfo ("Redo", "", "Edit", 0); result.addDefaultKeypress ('z', cmd | shift); result.setActive (! redoStack.empty()); break;
            case cmdPlay:      result.setInfo (transport.isPlaying() ? "Stop" : "Play", "Space", "Playback", 0); result.setActive (has); break;
            case cmdLoopPlay:  result.setInfo ("Loop Playback", "", "Playback", 0); result.addDefaultKeypress ('r', cmd); result.setTicked (repeatButton.getToggleState()); break;
            case cmdLoopSection: result.setInfo ("Loop", "", "Loop", 0); result.addDefaultKeypress ('l', cmd | shift); result.setTicked (loopButton.getToggleState()); break;
            case cmdHearing:   result.setInfo ("Hear Processed", "A", "Playback", 0); result.setTicked (abButton.getToggleState()); break;
            case cmdFitView:   result.setInfo ("Show Whole Sound", "", "Playback", 0); result.addDefaultKeypress ('0', cmd); result.setActive (has); break;
            case cmdFindLoop:  result.setInfo (loopJob != nullptr ? "Cancel Loop Search" : "Find Loop", "", "Loop", 0); result.addDefaultKeypress ('l', cmd); result.setActive (has || loopJob != nullptr); break;
            case cmdNextLoop:  result.setInfo ("Next Loop", "", "Loop", 0); result.addDefaultKeypress ('n', cmd); result.setActive (otherLoopButton.isEnabled()); break;
            case cmdClearLoop: result.setInfo ("Clear Loop", "", "Loop", 0); result.setActive (clearLoopButton.isEnabled()); break;
            default: break;
        }
    }
    bool perform (const InvocationInfo& inv) override
    {
        auto setFormat = [this] (int id) { formatBox.setSelectedId (id, juce::sendNotificationSync); commandManager.commandStatusChanged(); };
        switch (inv.commandID)
        {
            case cmdOpen:      openFiles(); break;
            case cmdRemove:    removeSelected(); break;
            case cmdExport:    exportSelected(); break;
            case cmdExportAll: exportAll(); break;
            case cmdFmtSame:   setFormat (1); break;
            case cmdFmt24:     setFormat (2); break;
            case cmdFmtFloat:  setFormat (3); break;
            case cmdUndo:      undo(); break;
            case cmdRedo:      redo(); break;
            case cmdPlay:      togglePlay(); break;
            case cmdLoopPlay:  repeatButton.triggerClick(); break;
            case cmdLoopSection: loopButton.triggerClick(); break;
            case cmdHearing:   abButton.setToggleState (! abButton.getToggleState(), juce::dontSendNotification); updateAB(); break;
            case cmdFitView:   wave.showAll(); break;
            case cmdFindLoop:  findLoopButton.triggerClick(); break;
            case cmdNextLoop:  nextLoopCandidate(); break;
            case cmdClearLoop: setLoopForCurrent (0, 0); break;
            default: return false;
        }
        commandManager.commandStatusChanged();
        return true;
    }
    juce::StringArray getMenuBarNames() override { return { "File", "Edit", "Playback", "Loop" }; }
    juce::PopupMenu getMenuForIndex (int index, const juce::String&) override
    {
        juce::PopupMenu m;
        auto* cm = &commandManager;
        if (index == 0)
        {
            m.addCommandItem (cm, cmdOpen);
            m.addCommandItem (cm, cmdRemove);
            m.addSeparator();
            m.addCommandItem (cm, cmdExport);
            juce::PopupMenu fmt;
            fmt.addCommandItem (cm, cmdFmtSame);
            fmt.addCommandItem (cm, cmdFmt24);
            fmt.addCommandItem (cm, cmdFmtFloat);
            m.addSubMenu ("Export Format", fmt);
        }
        else if (index == 1)
        {
            m.addCommandItem (cm, cmdUndo);
            m.addCommandItem (cm, cmdRedo);
        }
        else if (index == 2)
        {
            m.addCommandItem (cm, cmdPlay);
            m.addCommandItem (cm, cmdLoopPlay);
            m.addCommandItem (cm, cmdHearing);
            m.addSeparator();
            m.addCommandItem (cm, cmdFitView);
        }
        else if (index == 3)
        {
            m.addCommandItem (cm, cmdLoopSection);
            m.addSeparator();
            m.addCommandItem (cm, cmdFindLoop);
            m.addCommandItem (cm, cmdNextLoop);
            m.addCommandItem (cm, cmdClearLoop);
        }
        return m;
    }
    void menuItemSelected (int, int) override {}

    //== lista =================================================================
    int getNumRows() override { return (int) items.size(); }
    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        if (! juce::isPositiveAndBelow (row, (int) items.size())) return;
        auto& it = *items[(size_t) row];
        if (selected) Theme::drawRaised (g, juce::Rectangle<float> (3.0f, 2.0f, (float) w - 6.0f, (float) h - 5.0f), 8.0f, false, false);
        g.setColour (selected ? Theme::accent : Theme::ink);
        g.setFont (Theme::font (13.0f, selected ? 500 : 400));
        g.drawText (it.file.getFileName(), 10, 2, w - 16, h / 2, juce::Justification::bottomLeft, true);
        g.setColour (Theme::inkFaint);
        g.setFont (Theme::font (11.0f, 400));
        g.drawText (juce::String ((int) it.analysis.numCycles()) + u8 (" cycles · ") + juce::String (it.sampleRate / 1000.0, 1)
                        + " kHz · " + (it.isFloat ? juce::String ("32f") : juce::String (it.bits)) + " bit",
                    10, h / 2, w - 16, h / 2 - 2, juce::Justification::topLeft, true);
    }
    void selectedRowsChanged (int row) override { if (row >= 0 && row != current) selectItem (row); }
};

//==============================================================================
class CycleNormApp : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "Toolkit"; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise (const juce::String& commandLine) override
    {
        juce::LookAndFeel::setDefaultLookAndFeel (&laf);
        // sólo para pruebas: CYCLENORM_TEST_SCALE=2 simula una pantalla Retina
        const auto testScale = juce::SystemStats::getEnvironmentVariable ("CYCLENORM_TEST_SCALE", {}).getFloatValue();
        if (testScale > 0.5f) juce::Desktop::getInstance().setGlobalScaleFactor (testScale);
        window = std::make_unique<MainWindow>();
        window->content->openFromCommandLine (commandLine);
    }
    void anotherInstanceStarted (const juce::String& commandLine) override
    {
        if (window != nullptr) window->content->openFromCommandLine (commandLine);
    }
    void shutdown() override
    {
        window = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    }
    void systemRequestedQuit() override { quit(); }

private:
    // Todo se diseña en estas medidas y se muestra al 100 % (sin achicar: así las líneas y las letras
    // caen justo en los píxeles y se ven nítidas); al agrandar la ventana se agranda todo junto.
    struct ScaledHolder : juce::Component
    {
        static constexpr float kScale = 1.0f; // al 100 %: cada línea cae justo en un píxel (nítido)
        ScaledHolder() { addAndMakeVisible (content); }
        int designW = 928, designH = 580;
        // escalable: el diseño es siempre el mismo y se agranda entero (como un zoom), sin cambiar
        // la proporción; el tamaño por defecto (80,3 %) es el mínimo
        void resized() override
        {
            // lo que entre entero (si la ventana no tiene justo la proporción, sobra un poco a los costados
            // o abajo, pero nunca se corta nada)
            const float s = juce::jmax (0.1f, juce::jmin ((float) getWidth() / (float) designW, (float) getHeight() / (float) designH));
            const float ox = juce::jmax (0.0f, ((float) getWidth() - (float) designW * s) * 0.5f);
            content.setTransform (juce::AffineTransform::scale (s).translated (ox, 0.0f));
            content.setBounds (0, 0, designW, designH);
        }
        void paint (juce::Graphics& g) override
        {
            g.setGradientFill (juce::ColourGradient (Theme::topLight, 0.0f, 0.0f, Theme::topDark, 0.0f, (float) getHeight(), false));
            g.fillAll();
        }
        MainComponent content;
    };

    class MainWindow : public juce::DocumentWindow
    {
    public:
        MainWindow() : DocumentWindow ("Toolkit", kBg, DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            holder = new ScaledHolder();
            content = &holder->content;
            const int designW = juce::SystemStats::getEnvironmentVariable ("CYCLENORM_TEST_DESIGNW", "928").getIntValue(); // (928: entre secciones 20 visibles, también de Option N a Drag to DAW)
            holder->designW = designW;
            const int w0 = juce::roundToInt (designW * ScaledHolder::kScale), h0 = juce::roundToInt (580 * ScaledHolder::kScale);
            holder->setSize (w0, h0); // como UNISON
            setContentOwned (holder, true);
            setResizable (true, true);
            // se agranda en proporción (todo escala junto); no se achica más que el tamaño por defecto.
            // (en la Mac el sistema pasa el marco entero, con la barra del título: se descuenta)
            contentConstrainer.ratio = (double) w0 / (double) h0;
            contentConstrainer.minW = w0;
            contentConstrainer.frame = [this] {
                if (auto* peer = getPeer())
                    if (auto f = peer->getFrameSizeIfPresent()) return *f;
                return juce::BorderSize<int>();
            };
            setConstrainer (&contentConstrainer);
            content->onResetWindowSize = [this, w0, h0] { setContentComponentSize (w0, h0); };
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
        ScaledHolder* holder = nullptr;
        MainComponent* content = nullptr;
        // la proporción y el mínimo, sobre el contenido (no sobre el marco con la barra del título)
        struct ContentConstrainer : juce::ComponentBoundsConstrainer
        {
            double ratio = 1.0;
            int minW = 100;
            std::function<juce::BorderSize<int>()> frame;
            void checkBounds (juce::Rectangle<int>& b, const juce::Rectangle<int>&, const juce::Rectangle<int>&,
                              bool top, bool left, bool bottom, bool right) override
            {
                const auto f = frame ? frame() : juce::BorderSize<int>();
                auto c = f.subtractedFrom (b);
                int w = c.getWidth(), h = c.getHeight();
                if ((top || bottom) && ! (left || right)) w = juce::roundToInt (h * ratio); // tirando de arriba/abajo: manda el alto
                w = juce::jlimit (minW, minW * 4, w);
                h = juce::roundToInt (w / ratio);
                if (left) c.setLeft (c.getRight() - w); else c.setWidth (w);
                if (top)  c.setTop (c.getBottom() - h); else c.setHeight (h);
                b = f.addedTo (c);
            }
        } contentConstrainer;
    };

    CycleNormLookAndFeel laf;
    std::unique_ptr<MainWindow> window;
};

START_JUCE_APPLICATION (CycleNormApp)
