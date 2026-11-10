// Motor de normalización onda por onda (sin JUCE, para poder probarlo solo).
//
// 1) analyse(): detecta cada ciclo (de cruce por cero ascendente a cruce por cero
//    ascendente) y mide su pico. Se hace una vez por archivo.
// 2) render(): con los ajustes (Amount, Target, Skip, Threshold) calcula una ganancia
//    por ciclo y la aplica. Es rápido, se puede repetir cada vez que se mueve una perilla.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace cyclenorm
{

using Channels = std::vector<std::vector<float>>; // [canal][muestra]

struct Settings
{
    double amount      = 1.0;    // 0 = original, 1 = todas las ondas al mismo pico
    bool   autoTarget  = true;   // true: el nivel objetivo es el pico de la primera onda normalizada
    double targetDb    = -1.0;   // nivel objetivo en dBFS (si autoTarget = false)
    int    skipCycles  = 0;      // ondas del principio que no se tocan (el ataque)
    double thresholdDb = -200.0; // ondas con pico por debajo de esto mantienen la ganancia anterior (-200 = apagado)
    double maxBoostDb  = 60.0;   // límite de subida, para no llevar el silencio digital a 0 dB
};

struct Analysis
{
    double sampleRate = 44100.0;
    size_t numSamples = 0;
    std::vector<size_t> bounds; // inicio de cada onda (cruce por cero ascendente)
    std::vector<float>  peaks;  // pico (todos los canales) de cada onda: bounds[k] .. bounds[k+1]
    float preRollPeak = 0.0f;   // pico antes del primer cruce
    float tailPeak    = 0.0f;   // pico después del último cruce

    size_t numCycles() const { return peaks.size(); }
};

namespace detail
{
// Biquad (RBJ) aplicado ida y vuelta: sin desfase, así los cruces del filtrado
// caen donde están los del audio real.
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;

    static Biquad lowpass (double fs, double fc, double q = 0.70710678)
    {
        const double w = 2.0 * M_PI * fc / fs, c = std::cos (w), al = std::sin (w) / (2.0 * q), a0 = 1.0 + al;
        Biquad f;
        f.b0 = (1.0 - c) / 2.0 / a0; f.b1 = (1.0 - c) / a0; f.b2 = f.b0;
        f.a1 = -2.0 * c / a0;        f.a2 = (1.0 - al) / a0;
        return f;
    }
    static Biquad highpass (double fs, double fc, double q = 0.70710678)
    {
        const double w = 2.0 * M_PI * fc / fs, c = std::cos (w), al = std::sin (w) / (2.0 * q), a0 = 1.0 + al;
        Biquad f;
        f.b0 = (1.0 + c) / 2.0 / a0; f.b1 = -(1.0 + c) / a0; f.b2 = f.b0;
        f.a1 = -2.0 * c / a0;         f.a2 = (1.0 - al) / a0;
        return f;
    }
    void run (std::vector<double>& x) const
    {
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        for (auto& v : x)
        {
            const double y = b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = v; y2 = y1; y1 = y; v = y;
        }
    }
    void runZeroPhase (std::vector<double>& x) const
    {
        run (x);
        std::reverse (x.begin(), x.end());
        run (x);
        std::reverse (x.begin(), x.end());
    }
};

inline double toDb (double lin)  { return lin > 1e-12 ? 20.0 * std::log10 (lin) : -240.0; }
inline double toLin (double db)  { return std::pow (10.0, db / 20.0); }
} // namespace detail

// ---------------------------------------------------------------------------------------------
inline Analysis analyse (const Channels& in, double sampleRate)
{
    using namespace detail;
    Analysis a;
    a.sampleRate = sampleRate;
    a.numSamples = in.empty() ? 0 : in[0].size();
    const size_t n = a.numSamples;
    if (n < 4) { a.tailPeak = 0; return a; }

    // Señal de detección: el canal con más energía (sumar L+R se puede anular si están desfasados).
    size_t best = 0; double bestE = -1;
    for (size_t c = 0; c < in.size(); ++c)
    {
        double e = 0;
        for (float v : in[c]) e += (double) v * v;
        if (e > bestE) { bestE = e; best = c; }
    }

    // Sin DC (para buscar el cruce real en el audio) ...
    std::vector<double> raw (in[best].begin(), in[best].end());
    Biquad::highpass (sampleRate, 10.0).runZeroPhase (raw);

    // ... y además sin agudos (para que los armónicos de un 808 saturado no agreguen cruces).
    std::vector<double> det = raw;
    const double lpHz = std::min (250.0, sampleRate * 0.4);
    Biquad::lowpass (sampleRate, lpHz).runZeroPhase (det);
    Biquad::lowpass (sampleRate, lpHz).runZeroPhase (det);

    // Envolvente (pico con caída lenta) para la histéresis relativa.
    std::vector<double> env (n);
    {
        const double rel = std::exp (-1.0 / (0.02 * sampleRate));
        double e = 0;
        for (size_t i = 0; i < n; ++i) { e = std::max (std::abs (det[i]), e * rel); env[i] = e; }
        e = 0; // pasada hacia atrás para que la envolvente no llegue tarde a un golpe nuevo
        for (size_t i = n; i-- > 0;) { e = std::max (std::abs (det[i]), e * rel); env[i] = std::max (env[i], e); }
    }

    const size_t minPeriod = (size_t) std::max (2.0, sampleRate / 1000.0); // ondas de más de 1 kHz no existen en un 808
    std::vector<size_t> rough;
    bool armed = false;
    for (size_t i = 1; i < n; ++i)
    {
        const double h = 0.05 * env[i];
        if (det[i] < -h) armed = true;
        if (armed && det[i - 1] < 0.0 && det[i] >= 0.0)
        {
            if (rough.empty() || i - rough.back() >= minPeriod)
                rough.push_back (i);
            armed = false;
        }
    }

    // Llevar cada cruce al cruce ascendente real del audio más cercano (así el cambio de
    // ganancia cae donde la onda vale ~0 y no hace click).
    std::vector<size_t>& b = a.bounds;
    b.reserve (rough.size());
    for (size_t k = 0; k < rough.size(); ++k)
    {
        const size_t c = rough[k];
        const size_t prevGap = k > 0 ? c - rough[k - 1] : minPeriod * 4;
        const size_t nextGap = k + 1 < rough.size() ? rough[k + 1] - c : prevGap;
        const size_t win = std::max<size_t> (1, std::min (prevGap, nextGap) / 4);
        const size_t lo = c > win ? c - win : 1;
        const size_t hi = std::min (n - 1, c + win);
        size_t pick = c; size_t bestDist = (size_t) -1;
        for (size_t i = std::max<size_t> (lo, 1); i <= hi; ++i)
        {
            if (raw[i - 1] < 0.0 && raw[i] >= 0.0)
            {
                const size_t z = std::abs (raw[i - 1]) < std::abs (raw[i]) ? i - 1 : i;
                const size_t d = z > c ? z - c : c - z;
                if (d < bestDist) { bestDist = d; pick = z; }
            }
        }
        if (b.empty() || pick > b.back() + minPeriod / 2)
            b.push_back (pick);
    }

    auto peakOf = [&] (size_t from, size_t to) {
        float p = 0;
        for (auto& ch : in)
            for (size_t i = from; i < to; ++i) p = std::max (p, std::abs (ch[i]));
        return p;
    };

    if (b.empty()) { a.preRollPeak = peakOf (0, n); return a; }
    a.preRollPeak = peakOf (0, b.front());
    a.peaks.resize (b.size() - 1);
    for (size_t k = 0; k + 1 < b.size(); ++k) a.peaks[k] = peakOf (b[k], b[k + 1]);
    a.tailPeak = peakOf (b.back(), n);
    return a;
}

// Ganancia (dB) de cada onda según los ajustes.
inline std::vector<double> cycleGainsDb (const Analysis& a, const Settings& s, double* targetUsedDb = nullptr)
{
    using namespace detail;
    const size_t K = a.numCycles();
    std::vector<double> g (K, 0.0);
    if (K == 0) { if (targetUsedDb) *targetUsedDb = 0; return g; }

    const size_t skip = (size_t) std::max (0, s.skipCycles);
    const double thr = s.thresholdDb;

    double target = s.targetDb;
    if (s.autoTarget)
    {
        // el pico de la primera onda que se normaliza (la que sigue a las salteadas)
        target = -240.0;
        for (size_t k = std::min (skip, K - 1); k < K; ++k)
            if (toDb (a.peaks[k]) > thr) { target = toDb (a.peaks[k]); break; }
        if (target <= -240.0) target = toDb (a.peaks[std::min (skip, K - 1)]);
    }
    if (targetUsedDb) *targetUsedDb = target;

    double last = 0.0;
    for (size_t k = 0; k < K; ++k)
    {
        if (k < skip) { g[k] = 0.0; last = 0.0; continue; }
        const double pDb = toDb (a.peaks[k]);
        if (a.peaks[k] <= 1e-9f || pDb < thr)
        {
            g[k] = (k == skip) ? 0.0 : last; // por debajo del umbral: sigue con la última ganancia
        }
        else
        {
            g[k] = std::clamp (s.amount * (target - pDb), -s.maxBoostDb, s.maxBoostDb);
        }
        last = g[k];
    }
    return g;
}

// Curva de ganancia (lineal, una por muestra). Entre ondas hay una rampa corta, centrada en el cruce.
inline std::vector<float> gainCurve (const Analysis& a, const Settings& s, double* targetUsedDb = nullptr)
{
    using namespace detail;
    const size_t n = a.numSamples;
    std::vector<float> curve (n, 1.0f);
    const size_t K = a.numCycles();
    if (K == 0) return curve;

    const auto g = cycleGainsDb (a, s, targetUsedDb);
    const auto& b = a.bounds;

    // tramo antes del primer cruce: misma ganancia que la primera onda
    const float g0 = (float) toLin (g.front());
    for (size_t i = 0; i < b.front(); ++i) curve[i] = g0;
    for (size_t k = 0; k < K; ++k)
    {
        const float gl = (float) toLin (g[k]);
        for (size_t i = b[k]; i < b[k + 1]; ++i) curve[i] = gl;
    }
    const float gEnd = (float) toLin (g.back());
    for (size_t i = b.back(); i < n; ++i) curve[i] = gEnd;

    // rampas: 1 ms como máximo, o el 6% de la onda más corta de las dos
    const size_t maxRamp = (size_t) std::max (1.0, a.sampleRate * 0.001);
    for (size_t k = 0; k < b.size(); ++k)
    {
        const double before = k == 0 ? g.front() : g[k - 1];
        const double after  = k < K ? g[k] : g.back();
        if (std::abs (after - before) < 1e-6) continue;
        const size_t lenPrev = k > 0 ? b[k] - b[k - 1] : maxRamp * 20;
        const size_t lenNext = k + 1 < b.size() ? b[k + 1] - b[k] : maxRamp * 20;
        const size_t half = std::max<size_t> (1, std::min (maxRamp, (size_t) (0.06 * (double) std::min (lenPrev, lenNext))) / 2);
        const size_t from = b[k] > half ? b[k] - half : 0;
        const size_t to   = std::min (n, b[k] + half);
        for (size_t i = from; i < to; ++i)
        {
            const double t = (double) (i - from + 0.5) / (double) (to - from);
            const double sm = 0.5 - 0.5 * std::cos (M_PI * t);
            curve[i] = (float) toLin (before + (after - before) * sm); // interpolación en dB
        }
    }
    return curve;
}

inline Channels render (const Channels& in, const Analysis& a, const Settings& s, double* targetUsedDb = nullptr)
{
    const auto curve = gainCurve (a, s, targetUsedDb);
    Channels out = in;
    for (auto& ch : out)
        for (size_t i = 0; i < ch.size() && i < curve.size(); ++i) ch[i] *= curve[i];
    return out;
}

} // namespace cyclenorm
