// CycleNorm: normaliza un 808 onda por onda (cada ciclo al mismo pico) y lo exporta.
#include <juce_audio_utils/juce_audio_utils.h>
#include "CycleEngine.h"
#include "ABSource.h"
#include "WaveformView.h"

namespace
{
const juce::Colour kAccent (0xff4fb3ff);
const juce::Colour kBg (0xff1b1e23);
const juce::Colour kPanel (0xff23272e);
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
struct Item
{
    juce::File file;
    double sampleRate = 44100.0;
    int bits = 24;
    bool isFloat = false;
    std::shared_ptr<juce::AudioBuffer<float>> original, processed;
    cyclenorm::Analysis analysis;
    double targetUsedDb = 0.0;
    float outPeak = 0.0f;
};

//==============================================================================
class DragOut : public juce::Component
{
public:
    std::function<juce::File()> makeFile; // escribe el procesado en un archivo y lo devuelve

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (isEnabled() ? kAccent.withAlpha (hover ? 0.25f : 0.12f) : juce::Colours::transparentBlack);
        g.fillRoundedRectangle (r, 6.0f);
        g.setColour (isEnabled() ? kAccent : juce::Colour (0xff555a63));
        g.drawRoundedRectangle (r, 6.0f, 1.2f);
        g.setFont (juce::FontOptions (13.0f));
        g.drawText (u8 ("⇢  Arrastrá el procesado al DAW"), getLocalBounds(), juce::Justification::centred);
    }
    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override { hover = false; repaint(); }
    void mouseDown (const juce::MouseEvent&) override { started = false; }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (started || e.getDistanceFromDragStart() < 6 || ! makeFile) return;
        started = true;
        const auto f = makeFile();
        if (f.existsAsFile())
            juce::DragAndDropContainer::performExternalDragDropOfFiles ({ f.getFullPathName() }, false, this);
    }

private:
    bool hover = false, started = false;
};

//==============================================================================
class MainComponent : public juce::Component,
                      public juce::FileDragAndDropTarget,
                      private juce::ListBoxModel,
                      private juce::Timer
{
public:
    MainComponent()
    {
        formats.registerBasicFormats();

        juce::PropertiesFile::Options o;
        o.applicationName = "CycleNorm";
        o.filenameSuffix = ".settings";
        o.osxLibrarySubFolder = "Application Support";
        props = std::make_unique<juce::PropertiesFile> (o);

        auto setupButton = [this] (juce::Button& b, const juce::String& text) {
            b.setButtonText (text);
            b.setWantsKeyboardFocus (false);
            addAndMakeVisible (b);
        };
        setupButton (openButton, u8 ("Abrir…"));
        setupButton (removeButton, "Quitar");
        setupButton (playButton, "Play");
        setupButton (loopButton, "Loop");
        setupButton (abButton, "Escuchando: Procesado");
        setupButton (exportButton, u8 ("Exportar…"));
        setupButton (exportAllButton, u8 ("Exportar todos…"));
        loopButton.setClickingTogglesState (true);
        abButton.setClickingTogglesState (true);
        abButton.setToggleState (true, juce::dontSendNotification);
        abButton.setColour (juce::TextButton::buttonOnColourId, kAccent.withAlpha (0.55f));
        loopButton.setColour (juce::TextButton::buttonOnColourId, kAccent.withAlpha (0.55f));

        openButton.onClick = [this] { openFiles(); };
        removeButton.onClick = [this] { removeSelected(); };
        playButton.onClick = [this] { togglePlay(); };
        loopButton.onClick = [this] { source.setLooping (loopButton.getToggleState()); props->setValue ("loop", loopButton.getToggleState()); };
        abButton.onClick = [this] { updateAB(); };
        exportButton.onClick = [this] { exportSelected(); };
        exportAllButton.onClick = [this] { exportAll(); };

        formatBox.addItem ("Formato: igual al original", 1);
        formatBox.addItem ("Formato: WAV 24 bit", 2);
        formatBox.addItem ("Formato: WAV 32 bit float", 3);
        formatBox.setSelectedId (props->getIntValue ("format", 1), juce::dontSendNotification);
        formatBox.onChange = [this] { props->setValue ("format", formatBox.getSelectedId()); updateInfo(); };
        formatBox.setWantsKeyboardFocus (false);
        addAndMakeVisible (formatBox);


        setupSlider (amount, amountLabel, "Amount", 0, 100, 1, " %", props->getDoubleValue ("amount", 100.0));
        setupSlider (skip, skipLabel, "Skip (ondas)", 0, 256, 1, "", props->getDoubleValue ("skip", 2.0));
        setupSlider (ramp, rampLabel, "Rampa", 0, 100, 1, " %", props->getDoubleValue ("ramp", 0.0));

        loopButton.setToggleState (props->getBoolValue ("loop", false), juce::dontSendNotification);
        source.setLooping (loopButton.getToggleState());

        list.setModel (this);
        list.setRowHeight (34);
        list.setColour (juce::ListBox::backgroundColourId, kPanel);
        list.setWantsKeyboardFocus (false);
        addAndMakeVisible (list);

        wave.getPlayheadSeconds = [this] { return transport.getCurrentPosition(); };
        wave.isPlaying = [this] { return transport.isPlaying(); };
        wave.isPlayingProcessed = [this] { return abButton.getToggleState(); };
        wave.onSeek = [this] (double s) { transport.setPosition (s); wave.repaint(); };
        wave.maxSkip = (int) skip.getMaximum();
        wave.onSkipDragged = [this] (int n)
        {
            if ((int) skip.getValue() == n) { wave.refresh(); return; }
            skip.setValue (n, juce::sendNotificationSync); // mueve la barra y reprocesa
            updateSkipView();                               // la línea sigue al mouse enseguida
        };
        addAndMakeVisible (wave);

        info.setColour (juce::Label::textColourId, juce::Colour (0xffc8ced6));
        info.setFont (juce::FontOptions (13.0f));
        addAndMakeVisible (info);

        dragOut.makeFile = [this] { return writeForDrag(); };
        addAndMakeVisible (dragOut);

        deviceManager.initialiseWithDefaultDevices (0, 2);
        deviceManager.addAudioCallback (&player);
        player.setSource (&transport);

        setWantsKeyboardFocus (true);
        setSize (1040, 660);
        updateButtons();
        startTimerHz (15);
    }

    ~MainComponent() override
    {
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
        g.fillAll (kBg);
        g.setColour (kPanel);
        g.fillRect (controlsArea);
        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (18.0f, juce::Font::bold));
        g.drawText ("CycleNorm", titleArea, juce::Justification::centredLeft);
        g.setColour (juce::Colour (0xff8a919c));
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (u8 ("normaliza semiciclo por semiciclo · espacio = play · A = original/procesado"),
                    titleArea.withTrimmedLeft (110), juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        titleArea = r.removeFromTop (28);
        r.removeFromTop (6);

        auto top = r.removeFromTop (30);
        openButton.setBounds (top.removeFromLeft (90)); top.removeFromLeft (6);
        removeButton.setBounds (top.removeFromLeft (70)); top.removeFromLeft (16);
        playButton.setBounds (top.removeFromLeft (70)); top.removeFromLeft (6);
        loopButton.setBounds (top.removeFromLeft (60)); top.removeFromLeft (6);
        abButton.setBounds (top.removeFromLeft (180));
        exportAllButton.setBounds (top.removeFromRight (130)); top.removeFromRight (6);
        exportButton.setBounds (top.removeFromRight (100)); top.removeFromRight (6);
        formatBox.setBounds (top.removeFromRight (210));
        r.removeFromTop (8);

        controlsArea = r.removeFromBottom (118);
        auto c = controlsArea.reduced (12, 10);
        auto bottomRow = c.removeFromBottom (30);
        dragOut.setBounds (bottomRow.removeFromRight (260));
        info.setBounds (bottomRow);
        c.removeFromBottom (6);
        const int colW = c.getWidth() / 3;
        auto place = [&] (juce::Label& l, juce::Slider& s, juce::Component* extra) {
            auto col = c.removeFromLeft (colW).reduced (6, 0);
            auto head = col.removeFromTop (20);
            if (extra != nullptr) extra->setBounds (head.removeFromRight (60));
            l.setBounds (head);
            s.setBounds (col.removeFromTop (28));
        };
        place (amountLabel, amount, nullptr);
        place (skipLabel, skip, nullptr);
        place (rampLabel, ramp, nullptr);
        r.removeFromBottom (8);

        list.setBounds (r.removeFromLeft (230));
        r.removeFromLeft (8);
        wave.setBounds (r);
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::spaceKey) { togglePlay(); return true; }
        if (k.getTextCharacter() == 'a' || k.getTextCharacter() == 'A')
        {
            abButton.setToggleState (! abButton.getToggleState(), juce::dontSendNotification);
            updateAB();
            return true;
        }
        if (k == juce::KeyPress::upKey || k == juce::KeyPress::downKey)
        {
            const int row = list.getSelectedRow() + (k == juce::KeyPress::upKey ? -1 : 1);
            if (juce::isPositiveAndBelow (row, (int) items.size())) list.selectRow (row);
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
            const bool ok = writeItem (*items.front(), juce::File (testOut), err);
            std::printf ("test export: %s %s\n", ok ? "OK" : "FALLA", err.toRawUTF8());
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

    juce::TextButton openButton, removeButton, playButton, loopButton, abButton, exportButton, exportAllButton;
    juce::ComboBox formatBox;
    juce::Slider amount, skip, ramp;
    juce::Label amountLabel, skipLabel, rampLabel, info;
    juce::ListBox list;
    WaveformView wave;
    DragOut dragOut;
    juce::Rectangle<int> titleArea, controlsArea;
    bool pendingReprocess = false;
    juce::uint32 lastChange = 0;

    //==========================================================================
    void setupSlider (juce::Slider& s, juce::Label& l, const juce::String& name, double lo, double hi, double step,
                      const juce::String& suffix, double value)
    {
        s.setSliderStyle (juce::Slider::LinearBar);
        s.setRange (lo, hi, step);
        s.setTextValueSuffix (suffix);
        s.setValue (value, juce::dontSendNotification);
        s.setColour (juce::Slider::trackColourId, kAccent.withAlpha (0.45f));
        s.setWantsKeyboardFocus (false);
        s.onValueChange = [this] { settingsChanged(); };
        addAndMakeVisible (s);
        l.setText (name, juce::dontSendNotification);
        l.setFont (juce::FontOptions (13.0f));
        l.setColour (juce::Label::textColourId, juce::Colour (0xffc8ced6));
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
        s.skipCycles = (int) skip.getValue();
        s.thresholdDb = -200.0; // sin umbral: se normaliza todo, también la cola y el ruido
        return s;
    }

    void saveSettings()
    {
        props->setValue ("amount", amount.getValue());
        props->setValue ("skip", skip.getValue());
        props->setValue ("ramp", ramp.getValue());
        props->saveIfNeeded();
    }

    void settingsChanged()
    {
        pendingReprocess = true;
        lastChange = juce::Time::getMillisecondCounter();
    }

    void timerCallback() override
    {
        if (pendingReprocess && juce::Time::getMillisecondCounter() - lastChange > 40)
        {
            pendingReprocess = false;
            processCurrent();
            list.repaint();
        }
        playButton.setButtonText (transport.isPlaying() ? "Stop" : "Play");
        // que espacio / A / flechas anden siempre (salvo mientras se escribe un valor)
        if (juce::Component::getCurrentlyFocusedComponent() == nullptr && isShowing()
            && juce::Process::isForegroundProcess())
            grabKeyboardFocus();
    }

    //== archivos ==============================================================
    bool isAudioFile (const juce::File& f) const
    {
        return f.isDirectory() || f.hasFileExtension ("wav;aif;aiff;flac;mp3;m4a;caf");
    }

    void openFiles()
    {
        chooser = std::make_unique<juce::FileChooser> ("Elegir audio", juce::File (props->getValue ("lastDir")),
                                                       "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.m4a;*.caf");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::canSelectMultipleItems,
                              [this] (const juce::FileChooser& fc) { addFiles (fc.getResults()); });
    }

    void addFiles (const juce::Array<juce::File>& files)
    {
        juce::StringArray failed;
        int firstNew = -1;
        for (auto& f : files)
        {
            if (! f.existsAsFile() || ! isAudioFile (f)) continue;
            auto item = loadItem (f);
            if (item == nullptr) { failed.add (f.getFileName()); continue; }
            props->setValue ("lastDir", f.getParentDirectory().getFullPathName());
            items.push_back (std::move (item));
            if (firstNew < 0) firstNew = (int) items.size() - 1;
        }
        list.updateContent();
        if (firstNew >= 0) list.selectRow (firstNew);
        if (! failed.isEmpty())
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "No se pudieron abrir",
                                                    failed.joinIntoString ("\n"));
        updateButtons();
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
        it->original = buf;
        it->analysis = cyclenorm::analyse (toChannels (*buf), r->sampleRate);
        return it;
    }

    void removeSelected()
    {
        if (! juce::isPositiveAndBelow (current, (int) items.size())) return;
        transport.stop();
        transport.setSource (nullptr);
        const int removed = current;
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
        const auto s = readSettings();
        auto curve = cyclenorm::gainCurve (it.analysis, s, &it.targetUsedDb);
        const auto& src = *it.original;
        cyclenorm::limitCurve (curve, cyclenorm::skipEndSample (it.analysis, s), it.targetUsedDb, [&] (size_t i) {
            float m = 0;
            for (int ch = 0; ch < src.getNumChannels(); ++ch) m = juce::jmax (m, std::abs (src.getSample (ch, (int) i)));
            return m;
        });
        auto out = std::make_shared<juce::AudioBuffer<float>> (*it.original);
        float peak = 0.0f;
        for (int ch = 0; ch < out->getNumChannels(); ++ch)
        {
            auto* d = out->getWritePointer (ch);
            for (int i = 0; i < out->getNumSamples(); ++i)
            {
                d[i] *= curve[(size_t) i];
                peak = juce::jmax (peak, std::abs (d[i]));
            }
        }
        it.processed = out;
        it.outPeak = peak;
    }

    void processCurrent()
    {
        if (! juce::isPositiveAndBelow (current, (int) items.size())) return;
        auto& it = *items[(size_t) current];
        processItem (it);
        source.setProcessed (it.processed);
        wave.setProcessed (it.processed);
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
        processItem (it);
        source.setBuffers (it.original, it.processed);
        source.setNextReadPosition (0);
        transport.setSource (&source, 0, nullptr, it.sampleRate, 2);
        source.setLooping (loopButton.getToggleState());
        wave.setData (it.original, it.processed, cyclenorm::segmentBounds (it.analysis, readSettings()), it.sampleRate, true);
        updateSkipView();
        if (wasPlaying) transport.start();
        updateInfo();
        updateButtons();
    }

    // dónde terminan las ondas salteadas con Skip (para pintarlas en gris en el procesado)
    void updateSkipView()
    {
        if (! juce::isPositiveAndBelow (current, (int) items.size())) return;
        wave.setSkipEnd (cyclenorm::skipEndSample (items[(size_t) current]->analysis, readSettings()));
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
    }

    void updateAB()
    {
        source.setPlayProcessed (abButton.getToggleState());
        abButton.setButtonText (abButton.getToggleState() ? "Escuchando: Procesado" : "Escuchando: Original");
        wave.refresh();
    }

    void updateButtons()
    {
        const bool has = juce::isPositiveAndBelow (current, (int) items.size());
        removeButton.setEnabled (has);
        playButton.setEnabled (has);
        exportButton.setEnabled (has);
        exportAllButton.setEnabled (! items.empty());
        dragOut.setEnabled (has);
        dragOut.repaint();
    }

    bool exportsFloat (const Item& it) const
    {
        const int id = formatBox.getSelectedId();
        if (id == 3) return true;
        if (id == 2) return false;
        return it.isFloat || it.bits > 24;
    }

    void updateInfo()
    {
        if (! juce::isPositiveAndBelow (current, (int) items.size())) { info.setText ({}, juce::dontSendNotification); return; }
        auto& it = *items[(size_t) current];
        const double peakDb = juce::Decibels::gainToDecibels (it.outPeak, -200.0f);
        juce::String t;
        t << (int) it.analysis.numCycles() << u8 (" ondas (") << (int) it.analysis.halfPeaks.size() << u8 (" semiciclos) · todo a 0 dBFS · pico de salida ") << juce::String (peakDb, 1) << " dBFS";
        if (it.outPeak > 1.0001f && ! exportsFloat (it)) // (el ataque salteado con Skip puede venir pasado de 0 dB)
        {
            t << u8 ("  ·  ¡SATURA! El ataque original pasa de 0 dBFS: exportá en 32 bit float");
            info.setColour (juce::Label::textColourId, juce::Colour (0xffff6b6b));
        }
        else info.setColour (juce::Label::textColourId, juce::Colour (0xffc8ced6));
        info.setText (t, juce::dontSendNotification);
    }

    //== exportar ==============================================================
    struct OutFormat { bool aiff; int bits; };

    OutFormat outFormatFor (const Item& it) const
    {
        const int id = formatBox.getSelectedId();
        if (id == 2) return { false, 24 };
        if (id == 3) return { false, 32 };
        const bool srcAiff = it.file.hasFileExtension ("aif;aiff");
        if (srcAiff && ! it.isFloat && it.bits <= 24) return { true, juce::jmax (8, it.bits) };
        if (it.isFloat || it.bits > 24) return { false, 32 };
        return { false, it.bits == 8 || it.bits == 16 || it.bits == 24 ? it.bits : 24 };
    }

    juce::String defaultName (const Item& it) const
    {
        return it.file.getFileNameWithoutExtension() + "_norm" + (outFormatFor (it).aiff ? ".aif" : ".wav");
    }

    bool writeItem (const Item& it, const juce::File& dest, juce::String& error) const
    {
        const auto fmtInfo = outFormatFor (it);
        juce::WavAudioFormat wav;
        juce::AiffAudioFormat aiff;
        juce::AudioFormat& fmt = fmtInfo.aiff ? static_cast<juce::AudioFormat&> (aiff) : static_cast<juce::AudioFormat&> (wav);

        dest.deleteFile();
        auto stream = std::make_unique<juce::FileOutputStream> (dest);
        if (! stream->openedOk()) { error = "No se pudo escribir " + dest.getFullPathName(); return false; }
        std::unique_ptr<juce::AudioFormatWriter> w (fmt.createWriterFor (stream.get(), it.sampleRate,
                                                                          (unsigned int) it.processed->getNumChannels(),
                                                                          fmtInfo.bits, {}, 0));
        if (w == nullptr) { error = "Formato no soportado"; return false; }
        stream.release(); // ahora es del writer
        if (! w->writeFromAudioSampleBuffer (*it.processed, 0, it.processed->getNumSamples()))
        {
            error = "Error al escribir " + dest.getFileName();
            return false;
        }
        return true;
    }

    void exportSelected()
    {
        if (! juce::isPositiveAndBelow (current, (int) items.size())) return;
        auto& it = *items[(size_t) current];
        const juce::File dir (props->getValue ("exportDir", it.file.getParentDirectory().getFullPathName()));
        chooser = std::make_unique<juce::FileChooser> ("Exportar", dir.getChildFile (defaultName (it)),
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
        chooser = std::make_unique<juce::FileChooser> (u8 ("Elegí la carpeta de destino"),
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
                                      "Exportar todos",
                                      juce::String (ok) + " archivo(s) exportados en\n" + dir.getFullPathName()
                                          + (errors.isEmpty() ? juce::String() : "\n\n" + errors.joinIntoString ("\n")));
                              });
    }

    juce::File writeForDrag()
    {
        if (! juce::isPositiveAndBelow (current, (int) items.size())) return {};
        auto& it = *items[(size_t) current];
        // carpeta fija (no temporal): el DAW puede seguir leyendo el archivo después
        auto dir = juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("CycleNorm");
        dir.createDirectory();
        auto dest = dir.getChildFile (defaultName (it)).getNonexistentSibling (false);
        juce::String err;
        return writeItem (it, dest, err) ? dest : juce::File();
    }

    //== lista =================================================================
    int getNumRows() override { return (int) items.size(); }
    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        if (! juce::isPositiveAndBelow (row, (int) items.size())) return;
        auto& it = *items[(size_t) row];
        if (selected) { g.setColour (kAccent.withAlpha (0.28f)); g.fillRect (0, 0, w, h); }
        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (13.0f));
        g.drawText (it.file.getFileName(), 8, 2, w - 12, h / 2, juce::Justification::bottomLeft, true);
        g.setColour (juce::Colour (0xff8a919c));
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (juce::String ((int) it.analysis.numCycles()) + u8 (" ondas · ") + juce::String (it.sampleRate / 1000.0, 1)
                        + " kHz · " + (it.isFloat ? juce::String ("32f") : juce::String (it.bits)) + " bit",
                    8, h / 2, w - 12, h / 2 - 2, juce::Justification::topLeft, true);
    }
    void selectedRowsChanged (int row) override { if (row >= 0 && row != current) selectItem (row); }
};

//==============================================================================
class CycleNormApp : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "CycleNorm"; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise (const juce::String& commandLine) override
    {
        laf.setColourScheme (juce::LookAndFeel_V4::getMidnightColourScheme());
        laf.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2d323a));
        laf.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff2d323a));
        laf.setColour (juce::ResizableWindow::backgroundColourId, kBg);
        juce::LookAndFeel::setDefaultLookAndFeel (&laf);
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
    class MainWindow : public juce::DocumentWindow
    {
    public:
        MainWindow() : DocumentWindow ("CycleNorm", kBg, DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            content = new MainComponent();
            setContentOwned (content, true);
            setResizable (true, true);
            setResizeLimits (820, 520, 4000, 3000);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
        MainComponent* content = nullptr;
    };

    juce::LookAndFeel_V4 laf;
    std::unique_ptr<MainWindow> window;
};

START_JUCE_APPLICATION (CycleNormApp)
