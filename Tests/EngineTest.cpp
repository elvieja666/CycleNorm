// Prueba del motor con 808 sintéticos. Compilar:
//   g++ -O2 -std=c++17 -fsanitize=address,undefined -I../Source EngineTest.cpp -o engine_test && ./engine_test
#include <functional>
#include "LoopFinder.h"
#include "Resample.h"
#include <cstdio>
#include <random>

using namespace cyclenorm;

// Ruido gaussiano propio (y no std::normal_distribution): da exactamente los mismos números en
// cualquier compilador (Linux, Mac), así las pruebas dan igual en todos lados.
struct PortableNoise
{
    explicit PortableNoise (unsigned seed, double sigma) : state (seed ? seed : 1u), sd (sigma) {}
    double operator() () // (mismo uso que nd (rng))
    {
        auto next = [this] { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return ((double) state + 1.0) / 4294967297.0; };
        const double u1 = next(), u2 = next();
        return sd * std::sqrt (-2.0 * std::log (u1)) * std::cos (2.0 * M_PI * u2);
    }
    unsigned state; double sd;
};

static Channels make808 (double fs, double seconds, double f0, double f1, double glideS, double tau,
                         double drive, double dc, double noiseDb, bool stereoInvert, double hitAt2 = -1, double negScale = 1.0)
{
    const size_t n = (size_t) (fs * seconds);
    Channels c (2, std::vector<float> (n, 0.0f));
    PortableNoise nd (1u, std::pow (10.0, noiseDb / 20.0));
    double ph = 0;
    for (size_t i = 0; i < n; ++i)
    {
        double t = i / fs;
        double tt = t;
        if (hitAt2 > 0 && t >= hitAt2) { tt = t - hitAt2; if (tt < 1.0 / fs) ph = 0; }
        const double f = tt < glideS ? f0 + (f1 - f0) * (tt / glideS) : f1;
        ph += 2 * M_PI * f / fs;
        double env = std::exp (-tt / tau);
        double click = tt < 0.004 ? 0.4 * std::exp (-tt / 0.0008) * std::sin (2 * M_PI * 1800 * tt) : 0;
        double sv = std::sin (ph);
        if (sv < 0) sv *= negScale * (0.7 + 0.3 * env); // semiciclo negativo más chico, y cada vez más asimétrico
        double x = std::tanh (drive * env * sv) / std::tanh (drive) + click + dc + nd();
        c[0][i] = (float) (0.9 * x);
        c[1][i] = (float) ((stereoInvert ? -0.9 : 0.85) * x);
    }
    return c;
}

static int check (const char* name, const Channels& in, double fs, Settings s, double maxSpreadDb, double maxStepRatio)
{
    auto a = analyse (in, fs);
    double target = 0;
    auto out = render (in, a, s, &target);
    // picos por onda de la salida, con los mismos cortes
    double lo = 1e9, hi = -1e9; size_t counted = 0;
    const auto& B = segmentBounds (a, s);
    const auto& P = segmentPeaks (a, s);
    for (size_t k = (size_t) s.skipCycles * (s.halfCycles ? 2 : 1); k + 1 < B.size(); ++k)
    {
        if (detail::toDb (P[k]) < s.thresholdDb || P[k] < 1e-6f) continue;
        float p = 0;
        for (auto& ch : out) for (size_t i = B[k]; i < B[k + 1]; ++i) p = std::max (p, std::abs (ch[i]));
        double d = detail::toDb (p);
        lo = std::min (lo, d); hi = std::max (hi, d); ++counted;
    }
    // saltos muestra a muestra (clicks): comparo el mayor salto con el mayor salto esperable de la onda
    double maxJumpOut = 0, maxJumpIdeal = 0;
    auto curve = gainCurve (a, s);
    for (size_t i = 1 + a.bounds.front(); i < out[0].size(); ++i)
    {
        maxJumpOut = std::max (maxJumpOut, (double) std::abs (out[0][i] - out[0][i - 1]));
        maxJumpIdeal = std::max (maxJumpIdeal, (double) std::abs (in[0][i] - in[0][i - 1]) * curve[i]);
    }
    // en ningún lado (tampoco en los pedazos del principio y del final) puede pasar del objetivo,
    // salvo en las ondas salteadas (el ataque original)
    float worst = 0;
    const size_t startCheck = s.skipCycles > 0 ? B[(size_t) s.skipCycles * (s.halfCycles ? 2 : 1)] : 0;
    for (auto& ch : out) for (size_t i = startCheck; i < ch.size(); ++i) worst = std::max (worst, std::abs (ch[i]));
    const bool overOk = detail::toDb (worst) <= target + 0.05;
    if (! overOk) std::printf ("  !! pasa del objetivo: %.2f dB\n", detail::toDb (worst));
    const double spread = hi - lo;
    const bool ok = overOk && counted > 5 && spread <= maxSpreadDb && maxJumpOut <= maxJumpIdeal * maxStepRatio;
    std::printf ("%-40s ondas=%4zu medidas=%4zu objetivo=%6.2f dB  picos %7.2f..%7.2f (spread %.3f dB)  salto %.4f/%.4f  %s\n",
                 name, a.numCycles(), counted, target, lo, hi, spread, maxJumpOut, maxJumpIdeal, ok ? "OK" : "FALLA");
    return ok ? 0 : 1;
}

int main()
{
    int fails = 0;
    Settings s; s.skipCycles = 2;
    fails += check ("808 limpio 44.1k", make808 (44100, 2.0, 140, 45, 0.08, 0.35, 1.0, 0.0, -100, false), 44100, s, 0.3, 1.05);
    fails += check ("808 saturado + DC + ruido 48k", make808 (48000, 2.0, 160, 38, 0.12, 0.4, 6.0, 0.01, -80, false), 48000, s, 0.5, 1.05);
    fails += check ("808 estereo invertido 96k", make808 (96000, 1.5, 120, 50, 0.05, 0.3, 2.0, 0.0, -90, true), 96000, s, 0.3, 1.05);
    fails += check ("dos golpes 44.1k", make808 (44100, 2.0, 150, 42, 0.08, 0.2, 3.0, 0.0, -90, false, 1.0), 44100, s, 0.6, 1.05);
    // sin pozo antes de un golpe nuevo: en los 40 ms antes del segundo golpe, cada ventana de 5 ms
    // tiene que llegar cerca del objetivo (la cola sostenida hasta el golpe)
    {
        auto in = make808 (44100, 2.0, 150, 42, 0.08, 0.2, 3.0, 0.0, -90, false, 1.0);
        auto a = analyse (in, 44100);
        Settings z = s; z.autoTarget = false; z.targetDb = 0.0;
        auto out = render (in, a, z);
        double worst = 0;
        const size_t hit = 44100, w = 44100 * 5 / 1000;
        for (size_t st = hit - 8 * w; st + w <= hit; st += w)
        {
            float p = 0;
            for (size_t i = st; i < st + w; ++i) p = std::max (p, std::abs (out[0][i]));
            worst = std::min (worst, detail::toDb (p));
        }
        // ventanas de 5 ms con un 808 de 42 Hz (media onda = 12 ms): alguna puede caer cerca del cruce
        const bool ok = worst > -12.0;
        std::printf ("sin pozo antes del golpe: peor ventana %.1f dB  %s\n", worst, ok ? "OK" : "FALLA");
        fails += ! ok;
    }
    // 808 con armónicos fuertes (2º y 3º casi tan fuertes como la fundamental): la onda cruza el 0
    // varias veces por período. Tiene que contar solo las ondas de la fundamental, sin tramos chicos.
    {
        const double fs = 44100, f = 33.0;
        const size_t n = (size_t) (fs * 2.0);
        Channels in (1, std::vector<float> (n));
        for (size_t i = 0; i < n; ++i)
        {
            const double t = i / fs, env = std::exp (-t / 0.5), w = 2 * M_PI * f * t;
            in[0][i] = (float) (env * (0.5 * std::sin (w) + 0.45 * std::sin (2 * w + 1.0) + 0.4 * std::sin (3 * w + 2.0) + 0.15 * std::sin (5 * w)));
        }
        auto a = analyse (in, fs);
        size_t tiny = 0;
        const auto& B = a.halfBounds;
        for (size_t k = 0; k + 1 < B.size(); ++k) if ((double) (B[k + 1] - B[k]) < 0.3 * fs / (2 * f)) ++tiny;
        // cada onda tiene que durar un período de la fundamental (±10 %)
        size_t wrong = 0;
        for (size_t k = 0; k + 1 < a.bounds.size(); ++k)
            if (std::abs ((double) (a.bounds[k + 1] - a.bounds[k]) - fs / f) > 0.1 * fs / f) ++wrong;
        const bool ok = a.numCycles() > 55 && wrong == 0 && tiny == 0;
        std::printf ("armonicos fuertes: %zu ondas, %zu de largo incorrecto, %zu tramos chicos  %s\n", a.numCycles(), wrong, tiny, ok ? "OK" : "FALLA");
        fails += ! ok;
        Settings z = s; z.autoTarget = false; z.targetDb = 0;
        fails += check ("armonicos fuertes, picos", in, fs, z, 0.3, 1.5);
    }
    // ataque con glide muy rápido (de 120 a 34 Hz en 50 ms, como el 808 de Lex): las primeras ondas
    // del ataque tienen que estar detectadas (si no, todo el ataque queda como un solo bloque y la
    // línea de Skip no puede ir más atrás)
    {
        const double fs = 44100;
        const size_t n = (size_t) (fs * 1.5);
        Channels in (1, std::vector<float> (n));
        double ph = 0;
        size_t expectedBefore50 = 0; double lastPh = 0;
        for (size_t i = 0; i < n; ++i)
        {
            const double t = i / fs, f = t < 0.05 ? 120.0 - 86.0 * (t / 0.05) : 34.0;
            ph += 2 * M_PI * f / fs;
            if (t < 0.05 && std::floor (ph / (2 * M_PI)) > std::floor (lastPh / (2 * M_PI))) ++expectedBefore50;
            lastPh = ph;
            in[0][i] = (float) (std::exp (-t / 0.6) * std::tanh (2.0 * std::sin (ph)) / std::tanh (2.0));
        }
        auto a = analyse (in, fs);
        size_t before50 = 0;
        for (auto v : a.bounds) if (v < (size_t) (0.05 * fs)) ++before50;
        const bool ok = before50 + 1 >= expectedBefore50 && a.bounds.front() < (size_t) (0.015 * fs);
        std::printf ("glide rapido en el ataque: %zu ondas detectadas en los primeros 50 ms (hay %zu), primera a %.1f ms  %s\n",
                     before50, expectedBefore50, a.bounds.front() * 1000.0 / fs, ok ? "OK" : "FALLA");
        fails += ! ok;
    }
    // semiciclos: un 808 asimétrico (el negativo más chico), los dos semiciclos tienen que quedar iguales
    {
        auto in = make808 (44100, 2.0, 140, 45, 0.08, 0.35, 2.0, 0.0, -100, false, -1, 0.6);
        fails += check ("asimetrico, semiciclos", in, 44100, s, 0.3, 1.05);
        auto a = analyse (in, 44100);
        // cada semiciclo tiene que empezar en un cruce real (descendente en los impares)
        size_t bad = 0;
        for (size_t k = 1; k + 1 < a.halfBounds.size(); k += 2)
        {
            const size_t m = a.halfBounds[k];
            if (! (a.halfBounds[k - 1] < m && m < a.halfBounds[k + 1])) ++bad;
            if (std::abs (in[0][m]) > 0.02f) ++bad;
        }
        std::printf ("semiciclos en cruces reales: %zu semiciclos, %zu mal  %s\n", a.halfPeaks.size(), bad, bad ? "FALLA" : "OK");
        fails += bad != 0;
        Settings w = s; w.halfCycles = false;
        // en modo onda entera el negativo NO llega al objetivo (es lo que se quería arreglar)
        double target = 0; auto out = render (in, a, w, &target);
        // el semiciclo negativo de la onda 60: el que sigue al corte donde empieza esa onda
        const size_t h60 = (size_t) std::distance (a.halfBounds.begin(), std::find (a.halfBounds.begin(), a.halfBounds.end(), a.bounds[60]));
        float negPeak = 0;
        for (size_t i = a.halfBounds[h60 + 1]; i < a.halfBounds[h60 + 2]; ++i) negPeak = std::max (negPeak, std::abs (out[0][i]));
        double target2 = 0; auto out2 = render (in, a, s, &target2);
        float negPeak2 = 0;
        for (size_t i = a.halfBounds[h60 + 1]; i < a.halfBounds[h60 + 2]; ++i) negPeak2 = std::max (negPeak2, std::max (std::abs (out2[0][i]), std::abs (out2[1][i])));
        const bool ok = detail::toDb (negPeak) < target - 2.0 && std::abs (detail::toDb (negPeak2) - target2) < 0.05;
        std::printf ("negativo onda 60: modo onda %.2f dB / modo semiciclo %.2f dB (objetivo %.2f)  %s\n",
                     detail::toDb (negPeak), detail::toDb (negPeak2), target2, ok ? "OK" : "FALLA");
        fails += ! ok;
        fails += check ("asimetrico, onda entera", in, 44100, w, 0.3, 1.05);
    }
    {   // rampa al máximo: ningún punto pasa del objetivo y los semiciclos siguen llegando
        Settings r = s; r.ramp = 1.0; r.autoTarget = false; r.targetDb = 0.0;
        fails += check ("rampa 100% (asimetrico)", make808 (44100, 2.0, 140, 45, 0.08, 0.35, 2.5, 0.0, -100, false, -1, 0.6), 44100, r, 0.5, 1.5);
        fails += check ("rampa 50% (dos golpes)", make808 (44100, 2.0, 150, 42, 0.08, 0.2, 3.0, 0.0, -90, false, 1.0), 44100, [&] { Settings q = r; q.ramp = 0.5; return q; }(), 0.5, 1.5);
    }
    Settings t = s; t.thresholdDb = -40;
    fails += check ("umbral -40 dB", make808 (44100, 3.0, 140, 45, 0.08, 0.35, 1.0, 0.0, -100, false), 44100, t, 0.3, 1.05);
    Settings m = s; m.autoTarget = false; m.targetDb = -6;
    fails += check ("target -6 dB", make808 (44100, 2.0, 140, 45, 0.08, 0.35, 2.0, 0.0, -100, false), 44100, m, 0.3, 1.05);

    // Amount 50%: la cola tiene que quedar entre el original y el plano
    {
        auto in = make808 (44100, 2.0, 140, 45, 0.08, 0.35, 1.0, 0.0, -100, false);
        auto a = analyse (in, 44100);
        Settings h = s; h.amount = 0.5;
        auto g = cycleGainsDb (a, h);
        Settings f = s; auto gf = cycleGainsDb (a, f);
        bool ok = std::abs (g[50] - gf[50] * 0.5) < 1e-9;
        std::printf ("amount 50%%: ganancia onda 50 = %.2f dB (100%% = %.2f dB) %s\n", g[50], gf[50], ok ? "OK" : "FALLA");
        fails += !ok;
    }
    // Skip (la línea): N = cuántos pedazos se saltean, contando el pedacito antes del primer cruce.
    // 1 = solo ese pedacito (termina en el primer corte), 4 = ese y 3 semiciclos.
    {
        auto in = make808 (44100, 1.0, 140, 45, 0.08, 0.35, 2.0, 0.0, -100, false);
        auto a = analyse (in, 44100);
        Settings h; h.skipHalves = 4; h.autoTarget = false; h.targetDb = 0;
        auto g = cycleGainsDb (a, h);
        Settings one = h; one.skipHalves = 1;
        auto g1 = cycleGainsDb (a, one);
        Settings zero = h; zero.skipHalves = 0;
        const bool ok = skipEndSample (a, h) == a.halfBounds[3] && g[0] == 0 && g[1] == 0 && g[2] == 0 && g[3] != 0
                        && skipEndSample (a, one) == a.halfBounds[0] && g1[0] != 0
                        && skipEndSample (a, zero) == 0;
        std::printf ("skip: 4 -> termina en el corte 4 (%.1f %.1f %.1f | %.1f dB), 1 -> en el primer corte, 0 -> al principio  %s\n",
                     g[0], g[1], g[2], g[3], ok ? "OK" : "FALLA");
        fails += ! ok;
    }
    // Ataque que arranca con un rulito chico para arriba y después el glide: el primer semiciclo
    // grande tiene que tener su corte (la línea de Skip tiene que poder pararse ahí)
    {
        const double fs = 44100;
        const size_t n = (size_t) (fs * 1.2);
        Channels in (1, std::vector<float> (n, 0.0f));
        const size_t pre = (size_t) (0.003 * fs); // 3 ms de rulito
        for (size_t i = 0; i < pre; ++i) in[0][i] = (float) (0.04 * std::sin (M_PI * (double) i / (double) pre));
        double ph = 0;
        for (size_t i = pre; i < n; ++i)
        {
            const double t = (double) (i - pre) / fs, f = t < 0.06 ? 85.0 - 50.0 * (t / 0.06) : 35.0;
            ph += 2 * M_PI * f / fs;
            in[0][i] = (float) (std::exp (-t / 0.5) * std::tanh (2.5 * std::sin (ph)) / std::tanh (2.5));
        }
        auto a = analyse (in, fs);
        // el primer cruce real (donde termina el primer semiciclo grande, positivo) tiene que ser un corte:
        // así la línea de Skip se puede parar después del primer semiciclo y no saltea varios juntos
        size_t firstCross = 0;
        for (size_t i = pre + 1; i < n && firstCross == 0; ++i) if ((in[0][i - 1] < 0) != (in[0][i] < 0)) firstCross = i;
        const bool found = ! a.halfBounds.empty() && std::abs ((double) a.halfBounds.front() - (double) firstCross) < 0.001 * fs;
        std::printf ("ataque con rulito para arriba: primer corte a %.1f ms (primer cruce real a %.1f ms)  %s\n",
                     a.halfBounds.empty() ? -1.0 : a.halfBounds.front() * 1000.0 / fs, firstCross * 1000.0 / fs, found ? "OK" : "FALLA");
        fails += ! found;
    }
    // Loop: en un 808 normalizado con armónicos y un poco de ruido, el buscador tiene que encontrar un
    // loop de ondas enteras, que empalme sin salto (en fase) y con buena calidad. Con crossfade, el
    // empalme queda igual o mejor.
    {
        const double fs = 44100, f = 41.0;
        const size_t n = (size_t) (fs * 2.5);
        Channels in (2, std::vector<float> (n));
        PortableNoise nd (7u, 0.002);
        double ph = 0;
        for (size_t i = 0; i < n; ++i)
        {
            const double t = i / fs, fr = f * (1.0 + 0.004 * std::exp (-t / 0.4)); // un poquito de caída de pitch
            ph += 2 * M_PI * fr / fs;
            const double v = std::exp (-t / 0.5) * (std::sin (ph) + 0.3 * std::sin (2 * ph + 0.5) + 0.15 * std::sin (3 * ph)) + nd();
            in[0][i] = (float) v; in[1][i] = (float) (0.9 * v);
        }
        auto a = analyse (in, fs);
        Settings z; z.skipHalves = 8; z.autoTarget = false; z.targetDb = 0;
        auto y = render (in, a, z);
        AudioView v; v.ch = { y[0].data(), y[1].data() }; v.n = n;
        auto L = findLoop (a, v, skipEndSample (a, z), n);
        double maxStep = 0;
        for (size_t i = L.start + 1; i < L.end; ++i) maxStep = std::max (maxStep, (double) std::abs (y[0][i] - y[0][i - 1]));
        const double jump = std::abs (y[0][L.start] - y[0][L.end - 1]);
        // las puntas caen en ondas enteras (con el ajuste fino, a lo sumo 8 muestras del cruce)
        auto nearBound = [&] (size_t v) { for (auto b : a.bounds) if ((b > v ? b - v : v - b) <= 8) return true; return false; };
        const bool onBounds = nearBound (L.start) && nearBound (L.end);
        bool ok = L.found && onBounds && L.cycles >= 4 && L.quality > 0.85 && jump <= 1.5 * maxStep;
        std::printf ("loop: %d ondas (%.3f-%.3f s), calidad %.1f%%, salto en el empalme %.4f (paso normal max %.4f)  %s\n",
                     L.cycles, L.start / fs, L.end / fs, L.quality * 100, jump, maxStep, ok ? "OK" : "FALLA");
        fails += ! ok;
        // crossfade de 2 ondas
        std::vector<float*> chs { y[0].data(), y[1].data() };
        const size_t xf = 2 * (size_t) (fs / f);
        applyLoopCrossfade (chs, L.start, L.end, xf);
        const double jump2 = std::abs (y[0][L.start] - y[0][L.end - 1]);
        AudioView v2; v2.ch = { y[0].data(), y[1].data() }; v2.n = n;
        const double q2 = loopQuality (a, v2, L.start, L.end);
        // con crossfade, al volver al inicio el salto es un paso normal de la onda: sin click
        ok = jump2 <= 1.5 * maxStep && q2 >= 0.85;
        std::printf ("loop con crossfade de 2 ondas: salto %.4f (paso normal max %.4f), calidad %.1f%%  %s\n", jump2, maxStep, q2 * 100, ok ? "OK" : "FALLA");
        fails += ! ok;
        // varios candidatos distintos, progreso hasta 1 y cancelar
        {
            double lastP = 0; int calls = 0;
            auto many = findLoops (a, v, skipEndSample (a, z), n, 4, 5, [&] (double pr) { lastP = pr; ++calls; return true; });
            bool distinct = many.size() >= 3;
            for (size_t x = 0; x < many.size(); ++x)
                for (size_t w = x + 1; w < many.size(); ++w)
                    if (many[x].start == many[w].start && many[x].end == many[w].end) distinct = false;
            int seen = 0;
            auto cancelled = findLoops (a, v, skipEndSample (a, z), n, 4, 5, [&] (double) { return ++seen < 3; });
            const bool okMany = distinct && lastP >= 1.0 && calls > 3 && cancelled.empty();
            std::printf ("loop: %zu candidatos distintos, progreso hasta %.0f%%, cancelar  %s\n", many.size(), lastP * 100, okMany ? "OK" : "FALLA");
            fails += ! okMany;
        }
        const double note = midiNoteOf (a, L.start, L.end), expectedNote = 69.0 + 12.0 * std::log2 (f / 440.0);
        ok = std::abs (note - expectedNote) < 0.2;
        std::printf ("nota del loop: %.2f (esperada %.2f)  %s\n", note, expectedNote, ok ? "OK" : "FALLA");
        fails += ! ok;
    }
    // Pitch preciso (cruces exactos entre muestras): 808 saturado con armónicos y glide, en varias notas
    {
        bool ok = true;
        for (double f : { 32.703, 41.2, 55.0, 30.87 })
        {
            const double fs = 44100;
            const size_t n = (size_t) (fs * 2);
            Channels in (1, std::vector<float> (n));
            double ph = 0;
            for (size_t i = 0; i < n; ++i)
            {
                const double t = i / fs, fr = t < 0.06 ? f * 2.5 - f * 1.5 * (t / 0.06) : f;
                ph += 2 * M_PI * fr / fs;
                in[0][i] = (float) (std::exp (-t / 0.6) * (std::tanh (2 * std::sin (ph)) + 0.3 * std::sin (2 * ph + 0.4)));
            }
            auto a = analyse (in, fs);
            auto p = measurePitch (a, (size_t) (0.2 * fs), (size_t) (1.9 * fs));
            const double err = 1200.0 * std::log2 (p.hz / f);
            std::printf ("pitch %.3f Hz: medido %.4f Hz (%s), error %.3f cents  %s\n", f, p.hz, noteName (p.midi).c_str(), err, std::abs (err) < 0.5 ? "OK" : "FALLA");
            ok = ok && p.valid && std::abs (err) < 0.5;
        }
        fails += ! ok;
    }
    // Tune: un 808 en C#1 llevado a C1 con el re-sampleo tiene que quedar exacto; y el re-sampleo
    // de un seno no tiene que agregar casi nada (ruido/distorsión < -60 dB)
    {
        const double fs = 44100, f = 34.648; // C#1
        const size_t n = (size_t) (fs * 2.0);
        Channels in (1, std::vector<float> (n));
        double ph = 0;
        for (size_t i = 0; i < n; ++i)
        {
            const double t = i / fs, fr = t < 0.06 ? f * 2.2 - f * 1.2 * (t / 0.06) : f;
            ph += 2 * M_PI * fr / fs;
            in[0][i] = (float) (std::exp (-t / 0.6) * (std::tanh (2.5 * std::sin (ph)) + 0.3 * std::sin (2 * ph + 0.4)));
        }
        auto a = analyse (in, fs);
        auto p = measurePitch (a, (size_t) (0.2 * fs), (size_t) (1.9 * fs));
        const double speed = std::pow (2.0, (24.0 - p.midi) / 12.0);
        auto out = resample (in, speed);
        auto a2 = analyse (out, fs);
        auto p2 = measurePitch (a2, (size_t) (0.2 * fs / speed), (size_t) (1.9 * fs / speed));
        const double err = 100.0 * (p2.midi - 24.0);
        // seno 1 kHz subido 1 semitono
        std::vector<std::vector<float>> sn (1, std::vector<float> (44100));
        for (size_t i = 0; i < sn[0].size(); ++i) sn[0][i] = 0.5f * (float) std::sin (2 * M_PI * 1000 * i / fs);
        auto so = resample (sn, std::pow (2.0, 1.0 / 12.0));
        const double fr2 = 1000 * std::pow (2.0, 1.0 / 12.0);
        const size_t s0 = 3000, s1 = so[0].size() - 3000;
        double re = 0, im = 0;
        for (size_t i = s0; i < s1; ++i) { re += so[0][i] * std::sin (2 * M_PI * fr2 * i / fs); im += so[0][i] * std::cos (2 * M_PI * fr2 * i / fs); }
        re *= 2.0 / (double) (s1 - s0); im *= 2.0 / (double) (s1 - s0);
        double res = 0, tot = 0;
        for (size_t i = s0; i < s1; ++i) { const double fit = re * std::sin (2 * M_PI * fr2 * i / fs) + im * std::cos (2 * M_PI * fr2 * i / fs); res += (so[0][i] - fit) * (so[0][i] - fit); tot += so[0][i] * so[0][i]; }
        const double resDb = 10 * std::log10 (res / tot);
        const bool ok = std::abs (err) < 0.5 && resDb < -60.0;
        std::printf ("tune: C#1 -> %s %+.3f cents; re-sampleo de un seno: residuo %.1f dB  %s\n", noteName (p2.midi).c_str(), err, resDb, ok ? "OK" : "FALLA");
        fails += ! ok;
    }
    // Pitch de la zona estable: glide lento (de +3 semitonos, se asienta en ~0,4 s) y la nota fija
    // en C1 +12 cents; tiene que medir +12 (no más agudo por el glide). Y un glide que nunca para
    // (de E1 a A#0 en todo el sonido) tiene que salir marcado como gliding.
    {
        const double fs = 44100;
        auto synth = [&] (std::function<double (double)> freq, double secs) {
            const size_t n = (size_t) (fs * secs);
            Channels in (1, std::vector<float> (n));
            double ph = 0;
            for (size_t i = 0; i < n; ++i)
            {
                const double t = i / fs;
                ph += 2 * M_PI * freq (t) / fs;
                in[0][i] = (float) (std::exp (-t / 0.7) * (std::tanh (2.0 * std::sin (ph)) + 0.25 * std::sin (2 * ph + 0.3)));
            }
            return in;
        };
        const double fNote = 32.703 * std::pow (2.0, 12.0 / 1200.0); // C1 +12 cents
        auto settle = synth ([&] (double t) { return fNote * std::pow (2.0, 3.0 * std::exp (-t / 0.08) / 12.0); }, 2.0);
        auto a = analyse (settle, fs);
        auto pS = measureSustainPitch (a, 0, settle[0].size());
        auto pOld = measurePitch (a, 0, settle[0].size());
        const double err = 1200.0 * std::log2 (pS.hz / fNote), errOld = 1200.0 * std::log2 (pOld.hz / fNote);
        auto glide = synth ([&] (double t) { return 41.2 * std::pow (2.0, -6.0 * (t / 1.5) / 12.0); }, 1.5);
        auto ag = analyse (glide, fs);
        auto pG = measureSustainPitch (ag, 0, glide[0].size());
        const bool ok = pS.valid && pS.sustain && std::abs (err) < 1.0 && pG.valid && pG.gliding && ! pG.sustain;
        std::printf ("pitch de la zona estable: error %+.2f cents (todo el archivo: %+.1f), desde %.2f s; glide continuo -> %s  %s\n",
                     err, errOld, pS.zoneStart / fs, pG.gliding ? "gliding" : "estable", ok ? "OK" : "FALLA");
        fails += ! ok;
    }
    // 808 con un "rulo" al pasar por cero (un armónico agudo fuerte: sube, baja y vuelve a subir)
    // y piquitos chicos sueltos: antes el corte saltaba de un cruce a otro y el largo de las ondas
    // salía en zigzag (±130 cents). Tienen que salir todas parejas (C1, a menos de 5 cents).
    {
        const double fs = 44100, f = 32.703;
        const size_t n = (size_t) (fs * 2.0);
        Channels in (1, std::vector<float> (n));
        PortableNoise nz (7, 0.004);
        for (size_t i = 0; i < n; ++i)
        {
            const double t = i / fs, ph = 2 * M_PI * f * t;
            // el 7º armónico, al revés, es más empinado que la fundamental en el cruce:
            // cruza bajando justo en el medio y subiendo a cada lado (~100 muestras)
            double v = std::sin (ph) + 0.2 * std::sin (7.0 * ph + M_PI);
            v += nz();
            in[0][i] = (float) (0.8 * std::exp (-t / 1.5) * v);
        }
        // piquitos chicos justo en el medio de algunos cruces (agregan un cruce subiendo ahí)
        for (int k = 12; k < 60; k += 3)
            for (int d = 1; d <= 3; ++d)
            {
                const size_t i = (size_t) std::llround (k * fs / f) + (size_t) d;
                if (i < n) in[0][i] += 0.02f;
            }
        auto a = analyse (in, fs);
        int bad = 0, total = 0;
        for (size_t k = 3; k + 2 < a.boundsFrac.size(); ++k) // (las primeras: el arranque)
        {
            const double hz = fs / (a.boundsFrac[k + 1] - a.boundsFrac[k]);
            const double c = 1200.0 * std::log2 (hz / f);
            ++total; if (std::abs (c) > 5.0) ++bad;
        }
        const bool ok = total > 40 && bad == 0;
        std::printf ("ondas con rulo al cruzar por cero: %d de %d fuera de +-5 cents  %s\n", bad, total, ok ? "OK" : "FALLA");
        fails += ! ok;
    }
    // casos raros: silencio, archivo cortísimo, mono
    {
        Channels z (1, std::vector<float> (1000, 0.0f));
        auto a = analyse (z, 44100); auto o = render (z, a, s);
        Channels tiny (1, std::vector<float> (3, 0.5f));
        auto a2 = analyse (tiny, 44100); auto o2 = render (tiny, a2, s);
        bool ok = o[0][500] == 0.0f && o2[0][1] == 0.5f;
        std::printf ("silencio / archivo corto: %s\n", ok ? "OK" : "FALLA");
        fails += !ok;
    }
    std::printf (fails ? "\n%d FALLAS\n" : "\nTODO OK\n", fails);
    return fails;
}
