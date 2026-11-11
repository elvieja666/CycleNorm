// CycleNorm: normaliza un 808 onda por onda (cada ciclo al mismo pico) y lo exporta.
#include <juce_audio_utils/juce_audio_utils.h>
#include "CycleEngine.h"
#include "LoopFinder.h"
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
    size_t endSample = 0; // dónde se corta el final (la línea de Fin); 0 = hasta el final del archivo
    size_t loopStart = 0, loopEnd = 0; // zona de loop (0/0 = sin loop): de un cruce ascendente a otro
    double loopQuality = 0;
    std::vector<cyclenorm::LoopResult> loopCands; // los mejores loops que encontró la búsqueda
    int loopCandIdx = -1;                        // cuál de ellos está puesto (-1 = uno movido a mano)
    bool hasLoop() const { return loopEnd > loopStart; }

    int lengthToWrite() const
    {
        const int full = original ? original->getNumSamples() : 0;
        return (endSample > 0 && (int) endSample < full) ? (int) endSample : full;
    }
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
        setupButton (findLoopButton, "Buscar loop");
        setupButton (clearLoopButton, "Quitar loop");
        findLoopButton.onClick = [this] { if (loopJob != nullptr) cancelLoopSearch(); else findLoopForCurrent(); };
        setupButton (otherLoopButton, "Otro loop");
        otherLoopButton.onClick = [this] { nextLoopCandidate(); };
        addChildComponent (progressBar);
        progressBar.setColour (juce::ProgressBar::foregroundColourId, kAccent);
        progressBar.setColour (juce::ProgressBar::backgroundColourId, juce::Colour (0xff2d323a));
        progressBar.setTextToDisplay (" "); // el texto va en una etiqueta encima (el de la barra no se leía)
        progressLabel.setColour (juce::Label::textColourId, juce::Colours::white);
        progressLabel.setFont (juce::FontOptions (13.0f));
        progressLabel.setJustificationType (juce::Justification::centred);
        progressLabel.setInterceptsMouseClicks (false, false);
        addChildComponent (progressLabel);
        clearLoopButton.onClick = [this] { setLoopForCurrent (0, 0); };
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
        // se cambia arrastrando la línea en el visor; se cuenta en semiciclos (antes en ondas: 1 onda = 2)
        skipHalves = juce::jlimit (0, kMaxSkip, props->containsKey ("skipHalves") ? props->getIntValue ("skipHalves")
                                                                                 : props->getIntValue ("skip", 2) * 2);
        setupSlider (ramp, rampLabel, "Rampa", 0, 100, 1, " %", props->getDoubleValue ("ramp", 0.0));
        setupSlider (xfade, xfadeLabel, "Crossfade del loop", 0, 16, 1, "", props->getDoubleValue ("xfade", 0.0));
        xfade.textFromValueFunction = [] (double v) { const int n = (int) v; return n == 0 ? juce::String ("Off") : juce::String (n) + (n == 1 ? " onda" : " ondas"); };
        xfade.valueFromTextFunction = [] (const juce::String& t) { return t.trim().equalsIgnoreCase ("off") ? 0.0 : (double) t.getIntValue(); };
        xfade.updateText();

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
        wave.maxSkip = kMaxSkip;
        wave.onEndDragged = [this] (size_t e)
        {
            if (! juce::isPositiveAndBelow (current, (int) items.size())) return;
            auto& it = *items[(size_t) current];
            const size_t full = (size_t) it.original->getNumSamples();
            if (it.hasLoop() && e < it.loopEnd) e = it.loopEnd; // Fin no puede cortar el loop
            it.endSample = e >= full ? 0 : e;
            source.setEndSample ((juce::int64) it.endSample);
            wave.setEndSample (it.endSample);
            updateInfo();
        };
        wave.onLoopDragged = [this] (size_t s0, size_t s1) { setLoopForCurrent (s0, s1); };
        wave.onSkipDragged = [this] (int n)
        {
            if (skipHalves == n) { wave.refresh(); return; }
            if (juce::isPositiveAndBelow (current, (int) items.size()))
            {   // Skip no puede pasar la línea de Fin
                auto& it = *items[(size_t) current];
                auto st = readSettings(); st.skipHalves = n;
                if (it.endSample > 0 && cyclenorm::skipEndSample (it.analysis, st) >= it.endSample) return;
                if (it.hasLoop() && cyclenorm::skipEndSample (it.analysis, st) > it.loopStart) return; // ni el loop
            }
            skipHalves = n;
            settingsChanged();  // reprocesa
            updateSkipView();   // la línea sigue al mouse enseguida
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
        setSize (1290, 680);
        updateButtons();
        startTimerHz (15);
    }

    ~MainComponent() override
    {
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
        abButton.setBounds (top.removeFromLeft (180)); top.removeFromLeft (16);
        findLoopButton.setBounds (top.removeFromLeft (100)); top.removeFromLeft (6);
        otherLoopButton.setBounds (top.removeFromLeft (85)); top.removeFromLeft (6);
        clearLoopButton.setBounds (top.removeFromLeft (95));
        exportAllButton.setBounds (top.removeFromRight (130)); top.removeFromRight (6);
        exportButton.setBounds (top.removeFromRight (100)); top.removeFromRight (6);
        formatBox.setBounds (top.removeFromRight (210));
        r.removeFromTop (8);

        controlsArea = r.removeFromBottom (118);
        auto c = controlsArea.reduced (12, 10);
        auto bottomRow = c.removeFromBottom (30);
        dragOut.setBounds (bottomRow.removeFromRight (260));
        info.setBounds (bottomRow);
        progressBar.setBounds (bottomRow.reduced (0, 4).withTrimmedRight (12));
        progressLabel.setBounds (progressBar.getBounds());
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
        place (rampLabel, ramp, nullptr);
        place (xfadeLabel, xfade, nullptr);
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
            if (juce::SystemStats::getEnvironmentVariable ("CYCLENORM_TEST_LOOP", {}).isNotEmpty())
            {
                list.selectRow (0);
                findLoopForCurrent();
            }
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

    juce::TextButton openButton, removeButton, playButton, loopButton, abButton, exportButton, exportAllButton,
                     findLoopButton, otherLoopButton, clearLoopButton;
    double progressValue = 0.0;
    juce::ProgressBar progressBar { progressValue };
    juce::Label progressLabel;
    std::unique_ptr<LoopSearchJob> loopJob;
    juce::ComboBox formatBox;
    juce::Slider amount, ramp, xfade;
    static constexpr int kMaxSkip = 512; // semiciclos
    int skipHalves = 4;
    juce::Label amountLabel, rampLabel, xfadeLabel, info;
    juce::ListBox list;
    WaveformView wave;
    DragOut dragOut;
    juce::Rectangle<int> titleArea, controlsArea;
    bool pendingReprocess = false;
    int outputCheckTicks = 0;
    juce::String lastFailedOutput;
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
        s.skipHalves = skipHalves;
        s.thresholdDb = -200.0; // sin umbral: se normaliza todo, también la cola y el ruido
        return s;
    }

    void saveSettings()
    {
        props->setValue ("amount", amount.getValue());
        props->setValue ("skipHalves", skipHalves);
        props->setValue ("ramp", ramp.getValue());
        props->setValue ("xfade", xfade.getValue());
        props->saveIfNeeded();
    }

    void settingsChanged()
    {
        pendingReprocess = true;
        lastChange = juce::Time::getMillisecondCounter();
    }

    void timerCallback() override
    {
        if (loopJob != nullptr)
        {
            progressValue = loopJob->progress.load();
            progressLabel.setText (u8 ("Buscando el mejor loop (probando todos los pares)…  ") + juce::String ((int) std::round (progressValue * 100.0)) + " %",
                                   juce::dontSendNotification);
            if (loopJob->finished.load()) loopSearchFinished();
        }
        if (pendingReprocess && juce::Time::getMillisecondCounter() - lastChange > 40)
        {
            pendingReprocess = false;
            processCurrent();
            list.repaint();
        }
        playButton.setButtonText (transport.isPlaying() ? "Stop" : "Play");
        if (++outputCheckTicks >= 15) { outputCheckTicks = 0; followSystemOutput(); } // una vez por segundo
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
        // crossfade del loop (metido en el audio, así suena igual al tocarlo que en el sampler)
        if (it.hasLoop() && it.loopEnd <= (size_t) out->getNumSamples())
        {
            const int nx = (int) xfade.getValue();
            if (nx > 0)
            {
                const auto& B = it.analysis.bounds;
                const auto at = std::lower_bound (B.begin(), B.end(), it.loopEnd);
                const long idx = (long) std::distance (B.begin(), at);
                const size_t from = B[(size_t) juce::jmax (0L, idx - nx)];
                std::vector<float*> chs;
                for (int ch = 0; ch < out->getNumChannels(); ++ch) chs.push_back (out->getWritePointer (ch));
                cyclenorm::applyLoopCrossfade (chs, it.loopStart, it.loopEnd, it.loopEnd - from);
            }
            cyclenorm::AudioView v;
            for (int ch = 0; ch < out->getNumChannels(); ++ch) v.ch.push_back (out->getReadPointer (ch));
            v.n = (size_t) out->getNumSamples();
            it.loopQuality = cyclenorm::loopQuality (it.analysis, v, it.loopStart, it.loopEnd);
        }
        it.processed = out;
        it.outPeak = peak;
    }

    //== loop ==================================================================
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
        findLoopButton.setButtonText ("Cancelar");
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
        findLoopButton.setButtonText ("Buscar loop");
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
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Buscar loop",
                u8 ("No encontré un loop entre la línea de Skip y la de Fin: hacen falta al menos unas 6 ondas estables."));
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
        source.setLoopRegion ((juce::int64) s0, (juce::int64) s1);
        wave.setLoop (s0, s1);
        processCurrent();   // aplica el crossfade y mide la calidad
        updateButtons();
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
        source.setEndSample ((juce::int64) it.endSample);
        source.setNextReadPosition (0);
        transport.setSource (&source, 0, nullptr, it.sampleRate, 2);
        source.setLooping (loopButton.getToggleState());
        wave.setData (it.original, it.processed, cyclenorm::segmentBounds (it.analysis, readSettings()), it.sampleRate, true);
        wave.setEndSample (it.endSample);
        wave.setLoopBounds (it.analysis.bounds);
        wave.setLoop (it.loopStart, it.loopEnd);
        source.setLoopRegion ((juce::int64) it.loopStart, (juce::int64) it.loopEnd);
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
        findLoopButton.setEnabled (has || loopJob != nullptr);
        otherLoopButton.setEnabled (has && loopJob == nullptr && items[(size_t) current]->loopCands.size() > 1);
        clearLoopButton.setEnabled (has && items[(size_t) current]->hasLoop());
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
        t << (int) it.analysis.numCycles() << u8 (" ondas (") << (int) it.analysis.halfPeaks.size() << u8 (" semiciclos) · Skip: ") << skipHalves << u8 (" semiciclos · todo a 0 dBFS · pico de salida ") << juce::String (peakDb, 1) << " dBFS";
        if (it.endSample > 0)
            t << u8 (" · Fin: ") << juce::String ((double) it.endSample / it.sampleRate, 2) << " s (de "
              << juce::String ((double) it.original->getNumSamples() / it.sampleRate, 2) << " s)";
        if (it.hasLoop())
        {
            const auto& B = it.analysis.bounds;
            const auto nCyc = std::distance (std::lower_bound (B.begin(), B.end(), it.loopStart), std::lower_bound (B.begin(), B.end(), it.loopEnd));
            t << u8 (" · Loop: ") << juce::String ((double) it.loopStart / it.sampleRate, 3) << u8 ("–")
              << juce::String ((double) it.loopEnd / it.sampleRate, 3) << " s (" << (int) nCyc << u8 (" ondas, calidad ")
              << juce::String (it.loopQuality * 100.0, 1) << " %)";
            if (it.loopCandIdx >= 0 && it.loopCands.size() > 1)
                t << u8 (" · opción ") << (it.loopCandIdx + 1) << " de " << (int) it.loopCands.size();
        }
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
        const bool srcAiff = it.file.hasFileExtension ("aif;aiff") && ! it.hasLoop(); // con loop: WAV (lleva los puntos de loop)
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
        // Puntos de loop adentro del WAV (bloque "smpl", el que leen el Sampler de Logic, Kontakt,
        // Simpler...) y la nota raíz medida, así el sampler lo afina y lo loopea solo.
        juce::StringPairArray meta;
        if (it.hasLoop() && ! fmtInfo.aiff)
        {
            const double note = cyclenorm::midiNoteOf (it.analysis, it.loopStart, it.loopEnd);
            const int unity = juce::jlimit (0, 127, (int) std::floor (note));
            const double frac = juce::jlimit (0.0, 0.999999, note - std::floor (note));
            meta.set ("MidiUnityNote", juce::String (unity));
            meta.set ("MidiPitchFraction", juce::String ((juce::int32) (juce::uint32) (frac * 4294967296.0))); // (como entero de 32 bits)
            meta.set ("SamplePeriod", juce::String ((int) std::round (1.0e9 / it.sampleRate)));
            meta.set ("NumSampleLoops", "1");
            meta.set ("Loop0Identifier", "0");
            meta.set ("Loop0Type", "0"); // hacia adelante
            meta.set ("Loop0Start", juce::String ((juce::int64) it.loopStart));
            meta.set ("Loop0End", juce::String ((juce::int64) it.loopEnd - 1)); // en el WAV el fin es la última muestra del loop
            meta.set ("Loop0Fraction", "0");
            meta.set ("Loop0PlayCount", "0"); // infinito
        }
        std::unique_ptr<juce::AudioFormatWriter> w (fmt.createWriterFor (stream.get(), it.sampleRate,
                                                                          (unsigned int) it.processed->getNumChannels(),
                                                                          fmtInfo.bits, meta, 0));
        if (w == nullptr) { error = "Formato no soportado"; return false; }
        stream.release(); // ahora es del writer
        if (! w->writeFromAudioSampleBuffer (*it.processed, 0, it.lengthToWrite())) // hasta la línea de Fin
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
            setResizeLimits (1230, 560, 4000, 3000);
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
