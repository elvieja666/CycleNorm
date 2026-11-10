// Motor de normalización onda por onda (sin JUCE, para poder probarlo solo).
//
// 1) analyse(): detecta cada ciclo (de cruce por cero ascendente a cruce por cero
//    ascendente) y mide su pico. Se hace una vez por archivo.
// 2) render(): con los ajustes (Modo, Amount, Target, Skip, Threshold) calcula una ganancia
//    por semiciclo (o por onda entera) y la aplica. Es rápido, se puede repetir cada vez que se mueve una perilla.
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
    bool   halfCycles  = true;   // true: cada semiciclo (positivo y negativo) por separado; false: la onda entera
    int    skipCycles  = 0;      // ondas del principio que no se tocan (el ataque)
    int    skipHalves  = -1;     // si es 0 o más: semiciclos que no se tocan (manda sobre skipCycles)
    double thresholdDb = -200.0; // ondas con pico por debajo de esto mantienen la ganancia anterior (-200 = apagado)
    double maxBoostDb  = 60.0;   // límite de subida, para no llevar el silencio digital a 0 dB
    double ramp        = 0.0;    // 0 = rampa mínima en cada cambio de ganancia (1 ms como mucho, o el 6% del tramo)
                                 // 1 = la rampa cubre el 40% de cada tramo a cada lado del cruce
};

struct Analysis
{
    double sampleRate = 44100.0;
    size_t numSamples = 0;
    std::vector<size_t> bounds; // inicio de cada onda (cruce por cero ascendente)
    std::vector<float>  peaks;  // pico (todos los canales) de cada onda: bounds[k] .. bounds[k+1]
    std::vector<size_t> halfBounds; // inicio de cada semiciclo: cruce ascendente, descendente, ascendente...
    std::vector<float>  halfPeaks;  // pico de cada semiciclo: halfBounds[k] .. halfBounds[k+1]
    std::vector<float>  f0;         // la nota (fundamental, Hz) en cada muestra
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

// Seguimiento de la nota (fundamental) a lo largo del archivo, con YIN sobre una versión
// submuestreada. Devuelve f0 en Hz para cada muestra (0 si no hay nada que medir).
// winSec = 0: ventana de un período de la nota más grave (20 Hz), estable. winSec > 0: ventana
// corta, para seguir el glide rápido del ataque.
inline std::vector<float> trackF0 (const std::vector<double>& lp, double fs, double winSec = 0.0, double hopSec = 0.005)
{
    const size_t n = lp.size();
    std::vector<float> out (n, 0.0f);
    const size_t D = (size_t) std::max (1.0, std::floor (fs / 2000.0));
    const double fsD = fs / (double) D;
    std::vector<double> x (n / D);
    for (size_t i = 0; i < x.size(); ++i) x[i] = lp[i * D];
    const size_t tauMin = (size_t) std::max (2.0, fsD / 300.0), tauMax = (size_t) (fsD / 20.0) + 2;
    const size_t W = winSec > 0 ? (size_t) std::max (8.0, fsD * winSec) : tauMax, hop = (size_t) std::max (1.0, fsD * hopSec);
    if (x.size() < W + tauMax + 2) return out;

    struct Frame { double centre; double f0; double energy; };
    std::vector<Frame> frames;
    std::vector<double> d (tauMax + 1), cm (tauMax + 1);
    double maxE = 0;
    for (size_t p = 0; p + W + tauMax < x.size(); p += hop)
    {
        double e = 0;
        for (size_t j = 0; j < W; ++j) e += x[p + j] * x[p + j];
        maxE = std::max (maxE, e);
        d[0] = 0; cm[0] = 1;
        double run = 0;
        for (size_t t = 1; t <= tauMax; ++t)
        {
            double acc = 0;
            for (size_t j = 0; j < W; ++j) { const double df = x[p + j] - x[p + j + t]; acc += df * df; }
            d[t] = acc; run += acc;
            cm[t] = run > 0 ? acc * (double) t / run : 1.0;
        }
        size_t best = 0;
        for (size_t t = tauMin; t <= tauMax; ++t)
            if (cm[t] < 0.12) { while (t + 1 <= tauMax && cm[t + 1] < cm[t]) ++t; best = t; break; }
        if (best == 0)
        {
            double mn = 1e300;
            for (size_t t = tauMin; t <= tauMax; ++t) if (cm[t] < mn) { mn = cm[t]; best = t; }
            if (mn > 0.5) best = 0; // no hay una nota clara
        }
        double tau = (double) best;
        if (best > tauMin && best < tauMax) // interpolación parabólica
        {
            const double a = cm[best - 1], b = cm[best], c = cm[best + 1], den = a - 2 * b + c;
            if (std::abs (den) > 1e-12) tau += 0.5 * (a - c) / den;
        }
        // centro: el medio de la ventana más medio período (lo que de verdad se compara)
        frames.push_back ({ ((double) p + (double) W * 0.5 + tau * 0.5) * (double) D, best ? fsD / tau : 0.0, e });
    }
    // sin energía o sin nota: se completa con la nota de al lado
    for (auto& f : frames) if (f.energy < maxE * 1e-5) f.f0 = 0;
    double lastF = 0;
    for (auto& f : frames) { if (f.f0 > 0) lastF = f.f0; else f.f0 = lastF; }
    lastF = 0;
    for (size_t i = frames.size(); i-- > 0;) { if (frames[i].f0 > 0) lastF = frames[i].f0; else frames[i].f0 = lastF; }
    if (frames.empty() || frames.front().f0 <= 0) return out;
    // mediana de 7 cuadros: saca saltos sueltos (errores de octava)
    {
        std::vector<double> med (frames.size());
        for (size_t i = 0; i < frames.size(); ++i)
        {
            std::vector<double> w;
            for (size_t j = (i >= 3 ? i - 3 : 0); j <= std::min (frames.size() - 1, i + 3); ++j) w.push_back (frames[j].f0);
            std::nth_element (w.begin(), w.begin() + (long) w.size() / 2, w.end());
            med[i] = w[w.size() / 2];
        }
        for (size_t i = 0; i < frames.size(); ++i) frames[i].f0 = med[i];
    }
    size_t fi = 0;
    for (size_t i = 0; i < n; ++i)
    {
        while (fi + 1 < frames.size() && frames[fi + 1].centre <= (double) i) ++fi;
        if ((double) i <= frames.front().centre) { out[i] = (float) frames.front().f0; continue; }
        if (fi + 1 >= frames.size()) { out[i] = (float) frames.back().f0; continue; }
        const double t = ((double) i - frames[fi].centre) / (frames[fi + 1].centre - frames[fi].centre);
        out[i] = (float) (frames[fi].f0 + (frames[fi + 1].f0 - frames[fi].f0) * t);
    }
    return out;
}

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

    // El audio tal cual: ahí se busca el cruce real (donde la onda vale 0, así el cambio de
    // ganancia no hace click). Sin sacarle el DC: un 808 asimétrico tiene DC propio y sus
    // semiciclos empiezan y terminan donde el audio cruza el 0, no donde cruza su promedio.
    const std::vector<double> raw (in[best].begin(), in[best].end());

    // Para detectar las ondas: sin DC y sin agudos (para que los armónicos de un 808 saturado
    // no agreguen cruces falsos).
    std::vector<double> hp = raw;
    Biquad::highpass (sampleRate, 10.0).runZeroPhase (hp);
    std::vector<double> det = hp;
    const double lpHz = std::min (250.0, sampleRate * 0.4);
    Biquad::lowpass (sampleRate, lpHz).runZeroPhase (det);
    Biquad::lowpass (sampleRate, lpHz).runZeroPhase (det);

    // Solo la fundamental: se mide la nota en cada momento (sigue el glide) y se filtra justo
    // arriba de ella (1,4 veces la fundamental). Así los armónicos fuertes (un 808 con mucho
    // 2º y 3º) no agregan cruces: los rulos de los armónicos quedan adentro del semiciclo de la
    // fundamental y reciben la misma ganancia, en vez de normalizarse como si fueran semiciclos.
    a.f0 = trackF0 (det, sampleRate);
    const std::vector<double> det250 = det; // la versión abierta (hasta 250 Hz), para los ataques
    if (! a.f0.empty() && a.f0[0] > 0.0f)
    {
        std::vector<double> cuts;
        for (double c = 25.0; c < 600.0; c *= 1.25) cuts.push_back (c);
        std::vector<double> mix (n, 0.0), tmp;
        std::vector<int>   lo (n);
        std::vector<float> w (n);

        for (size_t i = 0; i < n; ++i)
        {
            const double c = std::clamp (1.4 * (double) a.f0[i], cuts.front(), cuts.back() * 0.999);
            const double pos = std::log (c / cuts.front()) / std::log (1.25);
            lo[i] = (int) std::floor (pos);
            w[i] = (float) (pos - lo[i]);
        }
        for (size_t k = 0; k < cuts.size(); ++k)
        {
            bool used = false;
            for (size_t i = 0; i < n && ! used; ++i) used = (lo[i] == (int) k || lo[i] + 1 == (int) k);
            if (! used || cuts[k] >= sampleRate * 0.45) continue;
            tmp = hp;
            Biquad::lowpass (sampleRate, cuts[k]).runZeroPhase (tmp);
            Biquad::lowpass (sampleRate, cuts[k]).runZeroPhase (tmp);
            for (size_t i = 0; i < n; ++i)
            {
                if (lo[i] == (int) k)          mix[i] += (1.0 - w[i]) * tmp[i];
                else if (lo[i] + 1 == (int) k) mix[i] += w[i] * tmp[i];
            }
        }
        det.swap (mix);
    }

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
        // sin un cruce real cerca (pasa en el arranque de un golpe nuevo) no se corta acá:
        // esa onda se une con la siguiente, así no hay un cambio de ganancia donde la onda no vale 0
        if (bestDist == (size_t) -1) continue;
        if (b.empty() || pick > b.back() + minPeriod / 2)
            b.push_back (pick);
    }

    // Ataques: al arrancar un golpe la nota baja rápido (el glide) y el medidor de nota, que mira
    // ventanas de ~100 ms, todavía ve la nota grave, así que el filtro de la nota se pierde las
    // primeras ondas (más rápidas). En el hueco entre el arranque del golpe y el primer corte que
    // encontró, se buscan las ondas con el filtro abierto (250 Hz). Solo en ese hueco: en el resto
    // siguen mandando los cruces de la fundamental.
    {
        const size_t hop = (size_t) std::max (1.0, sampleRate * 0.005), win = 2 * hop;
        std::vector<double> rms;
        double maxR = 0;
        for (size_t p = 0; p + win <= n; p += hop)
        {
            double e = 0;
            for (size_t j = p; j < p + win; ++j) e += hp[j] * hp[j];
            rms.push_back (std::sqrt (e / (double) win));
            maxR = std::max (maxR, rms.back());
        }
        std::vector<size_t> onsets;
        bool sounding = false;
        for (size_t f = 0; f < rms.size(); ++f)
        {
            const double before = f >= 6 ? *std::max_element (rms.begin() + (long) f - 6, rms.begin() + (long) f) : 0.0;
            const bool loud = rms[f] > maxR * 0.01;
            if ((loud && ! sounding) || (sounding && rms[f] > 4.0 * before && rms[f] > maxR * 0.05))
                onsets.push_back (f * hop);
            if (loud) sounding = true;
            else if (rms[f] < maxR * 0.003) sounding = false;
        }
        std::vector<size_t> extra;
        for (size_t o : onsets)
        {
            const auto firstAfter = std::lower_bound (b.begin(), b.end(), o);
            const size_t gapEnd = firstAfter == b.end() ? n : *firstAfter;
            const size_t gapStart = (firstAfter == b.begin()) ? o : std::max (o, *(firstAfter - 1) + minPeriod);
            if (gapEnd <= gapStart + 2 * minPeriod) continue;
            double mx = 0;
            for (size_t i = gapStart; i < gapEnd; ++i) mx = std::max (mx, std::abs (det250[i]));
            const double h = 0.05 * mx;
            bool arm = false;
            size_t last = gapStart;
            std::vector<size_t> found;
            for (size_t i = std::max<size_t> (gapStart, 1); i + minPeriod < gapEnd; ++i)
            {
                if (det250[i] < -h) arm = true;
                if (arm && det250[i - 1] < 0.0 && det250[i] >= 0.0 && (found.empty() || i - last >= minPeriod))
                {
                    found.push_back (i); last = i; arm = false;
                }
            }
            for (size_t k = 0; k < found.size(); ++k)
            {
                const size_t c = found[k];
                const size_t gapL = k > 0 ? c - found[k - 1] : (k + 1 < found.size() ? found[k + 1] - c : gapEnd - c);
                const size_t wnd = std::max<size_t> (1, gapL / 4);
                size_t pick = 0, bestD = (size_t) -1;
                for (size_t i = std::max<size_t> (1, c > wnd ? c - wnd : 1); i <= std::min (n - 1, c + wnd); ++i)
                    if (raw[i - 1] < 0.0 && raw[i] >= 0.0)
                    {
                        const size_t z = std::abs (raw[i - 1]) < std::abs (raw[i]) ? i - 1 : i;
                        const size_t d = z > c ? z - c : c - z;
                        if (d < bestD) { bestD = d; pick = z; }
                    }
                if (bestD != (size_t) -1 && pick + minPeriod < gapEnd && pick >= gapStart) extra.push_back (pick);
            }
        }
        if (! extra.empty())
        {
            b.insert (b.end(), extra.begin(), extra.end());
            std::sort (b.begin(), b.end());
            std::vector<size_t> clean;
            for (size_t v : b) if (clean.empty() || v >= clean.back() + minPeriod / 2) clean.push_back (v);
            b.swap (clean);
        }
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

    // Semiciclos: dentro de cada onda, el cruce descendente (de + a -). Primero en la señal
    // filtrada (el más cercano a la mitad de la onda), después el cruce real del audio más cercano.
    auto& hb = a.halfBounds;
    hb.reserve (b.size() * 2);
    for (size_t k = 0; k + 1 < b.size(); ++k)
    {
        const size_t s0 = b[k], s1 = b[k + 1], len = s1 - s0, midIdx = s0 + len / 2;
        hb.push_back (s0);
        if (len < 4) continue;
        size_t m = midIdx; size_t bestD = (size_t) -1;
        for (size_t i = s0 + std::max<size_t> (1, len / 8); i < s1 - len / 8; ++i)
            if (det[i - 1] >= 0.0 && det[i] < 0.0)
            {
                const size_t d = i > midIdx ? i - midIdx : midIdx - i;
                if (d < bestD) { bestD = d; m = i; }
            }
        const size_t win = std::max<size_t> (1, len / 4);
        size_t pick = m; bestD = (size_t) -1;
        for (size_t i = std::max (s0 + 1, m > win ? m - win : 1); i <= std::min (s1 - 1, m + win); ++i)
            if (raw[i - 1] >= 0.0 && raw[i] < 0.0)
            {
                const size_t z = std::abs (raw[i - 1]) < std::abs (raw[i]) ? i - 1 : i;
                const size_t d = z > m ? z - m : m - z;
                if (d < bestD) { bestD = d; pick = z; }
            }
        if (bestD != (size_t) -1 && pick > s0 && pick < s1) hb.push_back (pick); // si no hay cruce real, la onda queda entera
    }
    hb.push_back (b.back());

    // Relleno de huecos: si un tramo quedó mucho más largo que los anteriores (pasa justo antes
    // de un golpe nuevo, donde la detección filtrada pierde los últimos cruces de la cola), se
    // corta en los cruces reales del audio que tenga adentro. Si no, la cola que queda en ese
    // tramo tomaría la ganancia del golpe nuevo y se escucharía un pozo antes del golpe.
    {
        std::vector<char> rising (hb.size(), 0);
        for (size_t k = 0, j = 0; k < hb.size(); ++k)
        {
            while (j < b.size() && b[j] < hb[k]) ++j;
            rising[k] = (j < b.size() && b[j] == hb[k]) ? 1 : 0;
        }
        std::vector<size_t> nb;  std::vector<char> nr;
        nb.reserve (hb.size() + 16); nr.reserve (hb.size() + 16);
        for (size_t k = 0; k < hb.size(); ++k)
        {
            nb.push_back (hb[k]); nr.push_back (rising[k]);
            if (k + 1 >= hb.size()) break;
            const size_t s0 = hb[k], s1 = hb[k + 1];
            // largo esperado de un semiciclo, según la nota medida en ese punto
            const size_t midS = s0 + (s1 - s0) / 2;
            const double f0Here = a.f0.empty() ? 0.0 : (double) a.f0[std::min (midS, n - 1)];
            if (f0Here <= 0.0) continue;
            const double E = sampleRate / (2.0 * f0Here);
            if ((double) (s1 - s0) <= 1.5 * E) continue;

            std::vector<size_t> ins; std::vector<char> insR;
            bool wantRising = ! rising[k];
            size_t last = s0;
            for (size_t i = s0 + 1; i < s1; ++i)
            {
                if ((double) (i - last) < 0.6 * E) continue; // más cerca: es un rulo de un armónico, no un semiciclo
                if ((double) (s1 - i) < 0.1 * E) break;
                const bool up = raw[i - 1] < 0.0 && raw[i] >= 0.0;
                const bool down = raw[i - 1] >= 0.0 && raw[i] < 0.0;
                if ((wantRising && up) || (! wantRising && down))
                {
                    const size_t z = std::abs (raw[i - 1]) < std::abs (raw[i]) ? i - 1 : i;
                    ins.push_back (z); insR.push_back (wantRising ? 1 : 0);
                    last = z; wantRising = ! wantRising;
                }
            }
            // (si el último agregado es del mismo tipo que el cruce que sigue no pasa nada:
            // ese tramo es el arranque del golpe nuevo y se normaliza con su propio pico)
            for (size_t j = 0; j < ins.size(); ++j) { nb.push_back (ins[j]); nr.push_back (insR[j]); }
        }
        // Arranque de un golpe nuevo en medio de un tramo (la cola y el golpe pegados sin pasar
        // por 0): se corta justo donde la señal salta, así la cola no toma la ganancia del golpe.
        {
            std::vector<size_t> ob;  std::vector<char> orr;
            ob.reserve (nb.size() + 8); orr.reserve (nb.size() + 8);
            for (size_t k = 0; k < nb.size(); ++k)
            {
                ob.push_back (nb[k]); orr.push_back (nr[k]);
                if (k + 1 >= nb.size() || k == 0) continue;
                const size_t s0 = nb[k], s1 = nb[k + 1];
                const double E = (double) (nb[k] - nb[k - 1]); // largo del tramo anterior
                // un golpe nuevo es mucho más fuerte que el tramo anterior entero (un rulo de armónico
                // al principio de un semiciclo normal no: ese semiciclo es parecido al anterior)
                double prevPeak = 0, segPeak = 0;
                for (size_t i = nb[k - 1]; i < s0; ++i) prevPeak = std::max (prevPeak, std::abs (raw[i]));
                for (size_t i = s0; i < s1; ++i) segPeak = std::max (segPeak, std::abs (raw[i]));
                if (segPeak < 3.0 * prevPeak) continue;
                const size_t minPre = (size_t) std::max (8.0, 0.2 * E);
                // se compara con el pico de la cola hasta 1 ms antes (un golpe sube en unas muestras,
                // pero no de una: comparar con la muestra anterior no lo vería)
                const size_t lag = (size_t) std::max (2.0, sampleRate * 0.001);
                std::vector<double> pre (s1 - s0);
                double m = 0;
                for (size_t i = s0; i < s1; ++i) { m = std::max (m, std::abs (raw[i])); pre[i - s0] = m; }
                for (size_t i = s0 + std::max (minPre, lag); i < s1; ++i)
                {
                    const double tail = pre[i - s0 - lag];
                    if (tail > 1e-6 && std::abs (raw[i]) > 3.0 * tail)
                    {
                        size_t j = i; // atrás hasta donde la señal todavía estaba al nivel de la cola
                        while (j > s0 + minPre / 2 && std::abs (raw[j - 1]) > tail) --j;
                        if (s1 - j >= 4 && j > s0 + minPre / 2) { ob.push_back (j); orr.push_back (1); }
                        break;
                    }
                }
            }
            nb.swap (ob); nr.swap (orr);
        }

        hb.swap (nb);
        // las ondas enteras (modo Onda entera) arrancan en los cruces ascendentes (y en los golpes nuevos)
        b.clear();
        for (size_t k = 0; k < hb.size(); ++k)
            if (nr[k] || k == 0 || k + 1 == hb.size()) b.push_back (hb[k]);
        a.peaks.resize (b.size() - 1);
        for (size_t k = 0; k + 1 < b.size(); ++k) a.peaks[k] = peakOf (b[k], b[k + 1]);
    }

    a.halfPeaks.resize (hb.size() - 1);
    for (size_t k = 0; k + 1 < hb.size(); ++k) a.halfPeaks[k] = peakOf (hb[k], hb[k + 1]);
    return a;
}

// Los tramos que se normalizan según el modo: ondas enteras o semiciclos.
// Cuántos tramos (semiciclos u ondas) del principio no se tocan.
inline size_t skipSegments (const Settings& s)
{
    if (s.skipHalves >= 0) return (size_t) s.skipHalves;
    return (size_t) std::max (0, s.skipCycles) * (s.halfCycles ? 2u : 1u);
}

inline const std::vector<size_t>& segmentBounds (const Analysis& a, const Settings& s) { return s.halfCycles ? a.halfBounds : a.bounds; }
inline const std::vector<float>&  segmentPeaks  (const Analysis& a, const Settings& s) { return s.halfCycles ? a.halfPeaks  : a.peaks; }

// Ganancia (dB) de cada tramo (onda o semiciclo) según los ajustes.
inline std::vector<double> cycleGainsDb (const Analysis& a, const Settings& s, double* targetUsedDb = nullptr)
{
    using namespace detail;
    const auto& peaks = segmentPeaks (a, s);
    const size_t K = peaks.size();
    std::vector<double> g (K, 0.0);
    if (K == 0) { if (targetUsedDb) *targetUsedDb = 0; return g; }

    // Skip se cuenta siempre en ondas enteras (en semiciclos son el doble de tramos)
    const size_t skip = skipSegments (s);
    const double thr = s.thresholdDb;

    double target = s.targetDb;
    if (s.autoTarget)
    {
        // el pico de la primera onda que se normaliza (la que sigue a las salteadas)
        const size_t first = std::min (skip, K - 1);
        const size_t span = s.halfCycles ? 2 : 1; // en semiciclos: el mayor de los dos de esa onda
        target = -240.0;
        for (size_t k = first; k < K; k += span)
        {
            float p = 0;
            for (size_t j = k; j < std::min (K, k + span); ++j) p = std::max (p, peaks[j]);
            if (toDb (p) > thr) { target = toDb (p); break; }
        }
        if (target <= -240.0) target = toDb (peaks[first]);
    }
    if (targetUsedDb) *targetUsedDb = target;

    double last = 0.0;
    for (size_t k = 0; k < K; ++k)
    {
        if (k < skip) { g[k] = 0.0; last = 0.0; continue; }
        const double pDb = toDb (peaks[k]);
        if (peaks[k] <= 1e-9f || pDb < thr)
            g[k] = (k == skip) ? 0.0 : last; // por debajo del umbral: sigue con la última ganancia
        else
            g[k] = std::clamp (s.amount * (target - pDb), -s.maxBoostDb, s.maxBoostDb);
        last = g[k];
    }
    return g;
}

// Curva de ganancia (lineal, una por muestra). Entre tramos hay una rampa corta, centrada en el cruce.
inline std::vector<float> gainCurve (const Analysis& a, const Settings& s, double* targetUsedDb = nullptr)
{
    using namespace detail;
    const size_t n = a.numSamples;
    std::vector<float> curve (n, 1.0f);
    const auto& b = segmentBounds (a, s);
    const size_t K = segmentPeaks (a, s).size();
    if (K == 0) return curve;

    double target = 0.0;
    const auto g = cycleGainsDb (a, s, &target);
    if (targetUsedDb) *targetUsedDb = target;

    // Los pedazos antes del primer cruce y después del último (onda incompleta) se normalizan
    // con su propio pico, sin pasarse nunca del objetivo. Antes del primero, si hay Skip, no se toca.
    auto ownGain = [&] (float peak, double fallback) {
        const double pDb = toDb (peak);
        if (peak <= 1e-9f || pDb < s.thresholdDb) return fallback;
        return std::min (fallback, std::clamp (s.amount * (target - pDb), -s.maxBoostDb, s.maxBoostDb));
    };
    const double gPre  = skipSegments (s) > 0 ? 0.0 : ownGain (a.preRollPeak, g.front());
    const double gTail = ownGain (a.tailPeak, s.maxBoostDb);
    const double gPost = (toDb (a.tailPeak) < s.thresholdDb || a.tailPeak <= 1e-9f) ? g.back() : gTail;

    for (size_t i = 0; i < b.front(); ++i) curve[i] = (float) toLin (gPre);
    for (size_t k = 0; k < K; ++k)
    {
        const float gl = (float) toLin (g[k]);
        for (size_t i = b[k]; i < b[k + 1]; ++i) curve[i] = gl;
    }
    for (size_t i = b.back(); i < n; ++i) curve[i] = (float) toLin (gPost);

    // Rampas, centradas en cada cruce. Con Rampa en 0: 1 ms como máximo, o el 6% del tramo más corto
    // de los dos. Con Rampa en 1: el 40% del tramo más corto a cada lado (sin llegar al pico, que en
    // una onda redonda está en la mitad). Dos rampas vecinas nunca se pisan (40% + 40% < 100%).
    const size_t maxRamp = (size_t) std::max (1.0, a.sampleRate * 0.001);
    const double r = std::clamp (s.ramp, 0.0, 1.0);
    for (size_t k = 0; k < b.size(); ++k)
    {
        const double before = k == 0 ? gPre : g[k - 1];
        const double after  = k < K ? g[k] : gPost;
        if (std::abs (after - before) < 1e-6) continue;
        const size_t lenPrev = k > 0 ? b[k] - b[k - 1] : maxRamp * 20;
        const size_t lenNext = k + 1 < b.size() ? b[k + 1] - b[k] : maxRamp * 20;
        const double shortest = (double) std::min (lenPrev, lenNext);
        const double halfMin = std::max (1.0, std::min ((double) maxRamp, 0.06 * shortest) / 2.0);
        const double halfMax = std::max (halfMin, 0.40 * shortest);
        const size_t half = (size_t) std::max (1.0, std::round (halfMin + (halfMax - halfMin) * r));
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

// Dónde termina lo salteado con Skip (de ahí en adelante se normaliza).
inline size_t skipEndSample (const Analysis& a, const Settings& s)
{
    const size_t idx = skipSegments (s);
    if (idx == 0) return 0;
    const auto& B = segmentBounds (a, s);
    return idx < B.size() ? B[idx] : a.numSamples;
}

// Seguro: con rampas largas, la ganancia de un tramo vecino (más alta) puede meterse en el borde
// de un tramo y pasarlo del objetivo. Acá se baja la ganancia justo en esas muestras para que
// ninguna pase del objetivo. absPeakAt(i) = el mayor valor absoluto de la muestra i entre canales.
template <typename AbsPeakAt>
inline void limitCurve (std::vector<float>& curve, size_t from, double targetDb, AbsPeakAt absPeakAt)
{
    const float lim = (float) detail::toLin (targetDb);
    for (size_t i = from; i < curve.size(); ++i)
    {
        const float m = absPeakAt (i);
        if (m * curve[i] > lim) curve[i] = lim / m;
    }
}

inline Channels render (const Channels& in, const Analysis& a, const Settings& s, double* targetUsedDb = nullptr)
{
    double target = 0.0;
    auto curve = gainCurve (a, s, &target);
    if (targetUsedDb) *targetUsedDb = target;
    limitCurve (curve, skipEndSample (a, s), target, [&] (size_t i) {
        float m = 0; for (auto& ch : in) m = std::max (m, std::abs (ch[i])); return m; });
    Channels out = in;
    for (auto& ch : out)
        for (size_t i = 0; i < ch.size() && i < curve.size(); ++i) ch[i] *= curve[i];
    return out;
}

} // namespace cyclenorm
