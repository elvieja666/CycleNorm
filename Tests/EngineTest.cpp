// Prueba del motor con 808 sintéticos. Compilar:
//   g++ -O2 -std=c++17 -fsanitize=address,undefined -I../Source EngineTest.cpp -o engine_test && ./engine_test
#include "CycleEngine.h"
#include <cstdio>
#include <random>

using namespace cyclenorm;

static Channels make808 (double fs, double seconds, double f0, double f1, double glideS, double tau,
                         double drive, double dc, double noiseDb, bool stereoInvert, double hitAt2 = -1)
{
    const size_t n = (size_t) (fs * seconds);
    Channels c (2, std::vector<float> (n, 0.0f));
    std::mt19937 rng (1);
    std::normal_distribution<double> nd (0.0, std::pow (10.0, noiseDb / 20.0));
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
        double x = std::tanh (drive * env * std::sin (ph)) / std::tanh (drive) + click + dc + nd (rng);
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
    for (size_t k = (size_t) s.skipCycles; k + 1 < a.bounds.size(); ++k)
    {
        if (detail::toDb (a.peaks[k]) < s.thresholdDb || a.peaks[k] < 1e-6f) continue;
        float p = 0;
        for (auto& ch : out) for (size_t i = a.bounds[k]; i < a.bounds[k + 1]; ++i) p = std::max (p, std::abs (ch[i]));
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
    const double spread = hi - lo;
    const bool ok = counted > 5 && spread <= maxSpreadDb && maxJumpOut <= maxJumpIdeal * maxStepRatio;
    std::printf ("%-34s ondas=%4zu medidas=%4zu objetivo=%6.2f dB  picos %7.2f..%7.2f (spread %.3f dB)  salto %.4f/%.4f  %s\n",
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
