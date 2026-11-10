// Buscador de loop: encuentra de dónde a dónde loopear el 808 (ya normalizado) para que el
// empalme no se note. El loop siempre arranca y termina en un cruce por cero ascendente de la
// fundamental (ondas enteras), así la fase empalma y no hay click.
//
// Cómo elige: para cada par inicio/fin posible compara
//   - la onda antes del fin con la onda antes del inicio, y la onda en el fin con la del inicio
//     (lo que se escucha al saltar vs. lo que se escucharía sin saltar): forma de onda y nivel,
//   - el largo de esas ondas (la nota: que no haya salto de pitch),
//   - el "brillo" de esas ondas (armónicos y ruido de fondo),
// se queda con los mejores, los mide muestra a muestra alrededor del empalme y, entre los que
// empalman casi igual de bien, prefiere el loop más largo (suena más natural).
#pragma once

#include "CycleEngine.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace cyclenorm
{

struct AudioView
{
    std::vector<const float*> ch;
    size_t n = 0;
    float mono (size_t i) const
    {
        float s = 0;
        for (auto* c : ch) s += c[i];
        return ch.empty() ? 0.0f : s / (float) ch.size();
    }
};

struct LoopResult
{
    bool   found   = false;
    size_t start   = 0;   // primera muestra del loop
    size_t end     = 0;   // primera muestra después del loop (se salta de end-1 a start)
    int    cycles  = 0;   // ondas enteras adentro del loop
    double quality = 0;   // 0..1 (1 = empalme perfecto)
};

namespace loopdetail
{
constexpr int kShape = 64;

struct CycleFeat
{
    double len = 0;
    double hf = 0;                 // energía de la derivada / energía (brillo, ruido)
    double energy = 0;
    double shape[kShape] = {};
};

inline CycleFeat features (const AudioView& y, size_t from, size_t to)
{
    CycleFeat f;
    f.len = (double) (to - from);
    double e = 0, ed = 0;
    float prev = y.mono (from);
    for (size_t i = from; i < to; ++i)
    {
        const float v = y.mono (i);
        e += (double) v * v;
        ed += (double) (v - prev) * (v - prev);
        prev = v;
    }
    f.energy = e / std::max (1.0, f.len);
    f.hf = e > 1e-20 ? ed / e : 0.0;
    for (int k = 0; k < kShape; ++k)
    {
        const double pos = (double) from + (f.len - 1.0) * (double) k / (double) (kShape - 1);
        const size_t i0 = (size_t) pos;
        const double t = pos - (double) i0;
        const float a = y.mono (std::min (i0, to - 1)), b = y.mono (std::min (i0 + 1, to - 1));
        f.shape[k] = a + (b - a) * t;
    }
    return f;
}

inline double shapeDist (const CycleFeat& a, const CycleFeat& b)
{
    double d = 0, na = 0;
    for (int k = 0; k < kShape; ++k) { const double x = a.shape[k] - b.shape[k]; d += x * x; na += a.shape[k] * a.shape[k]; }
    return d / std::max (1e-12, na);
}

// Error del empalme medido muestra a muestra: lo que suena al saltar (lo que sigue a start)
// contra lo que seguiría sin saltar (lo que sigue a end), una onda antes y una después.
inline double spliceError (const AudioView& y, size_t start, size_t end, size_t win)
{
    double d = 0, e = 0;
    for (size_t k = 0; k < 2 * win; ++k)
    {
        if (start + k < win || end + k < win) continue;
        const size_t ia = start + k - win, ib = end + k - win;
        if (ia >= y.n || ib >= y.n) break;
        for (auto* c : y.ch)
        {
            const double x = (double) c[ia] - (double) c[ib];
            d += x * x;
            e += (double) c[ia] * c[ia];
        }
    }
    return e > 1e-20 ? std::sqrt (d / e) : 1.0;
}
} // namespace loopdetail

// y: el audio procesado (normalizado). from/to: la zona donde puede estar el loop (después de lo
// salteado con Skip y antes de la línea de Fin). minCycles: ondas mínimas adentro del loop.
inline LoopResult findLoop (const Analysis& a, const AudioView& y, size_t from, size_t to,
                            int minCycles = 4, int maxCycles = 400)
{
    using namespace loopdetail;
    LoopResult r;
    const auto& b = a.bounds;
    if (b.size() < (size_t) minCycles + 3 || y.n == 0) return r;
    to = std::min (to, y.n);

    // ondas enteras disponibles: cycle k = b[k] .. b[k+1]
    const size_t K = b.size() - 1;
    std::vector<CycleFeat> F (K);
    for (size_t k = 0; k < K; ++k) F[k] = features (y, b[k], std::min (b[k + 1], y.n));

    // índices de inicio posibles: b[i] dentro de la zona, con una onda antes (i-1) también adentro
    struct Cand { size_t i, j; double cost; };
    std::vector<Cand> cands;
    for (size_t i = 1; i < K; ++i)
    {
        if (b[i - 1] < from) continue;
        for (size_t j = i + (size_t) minCycles; j < K && j <= i + (size_t) maxCycles; ++j)
        {
            if (b[j] > to) break;
            // j es el fin (se salta de b[j]-1 a b[i]). Ondas que se comparan: j-1 con i-1, y j con i
            const double dShape = shapeDist (F[i - 1], F[j - 1]) + shapeDist (F[i], F[j]);
            const double dPitch = std::abs (F[j - 1].len - F[i - 1].len) / F[i - 1].len
                                + std::abs (F[j].len - F[i].len) / F[i].len;
            const double dHf = std::abs (std::log ((F[j - 1].hf + 1e-12) / (F[i - 1].hf + 1e-12)))
                             + std::abs (std::log ((F[j].hf + 1e-12) / (F[i].hf + 1e-12)));
            const double dLevel = std::abs (std::log ((F[j - 1].energy + 1e-20) / (F[i - 1].energy + 1e-20)));
            cands.push_back ({ i, j, dShape + 8.0 * dPitch + 0.3 * dHf + 0.5 * dLevel });
        }
    }
    if (cands.empty()) return r;

    // los 400 mejores se miden muestra a muestra
    const size_t top = std::min<size_t> (400, cands.size());
    std::partial_sort (cands.begin(), cands.begin() + (long) top, cands.end(),
                       [] (const Cand& x, const Cand& z) { return x.cost < z.cost; });
    struct Scored { size_t i, j; double err; };
    std::vector<Scored> sc;
    double bestErr = 1e300;
    for (size_t t = 0; t < top; ++t)
    {
        const auto& c = cands[t];
        const size_t win = (size_t) std::max (8.0, F[c.i].len);
        const double err = spliceError (y, b[c.i], b[c.j], win);
        sc.push_back ({ c.i, c.j, err });
        bestErr = std::min (bestErr, err);
    }
    // entre los que empalman casi igual de bien que el mejor, el más largo
    const Scored* pick = nullptr;
    for (auto& s : sc)
        if (s.err <= bestErr * 1.5 + 0.004)
            if (pick == nullptr || (s.j - s.i) > (pick->j - pick->i) || ((s.j - s.i) == (pick->j - pick->i) && s.err < pick->err))
                pick = &s;
    if (pick == nullptr) return r;

    r.found = true;
    r.start = b[pick->i];
    r.end = b[pick->j];
    r.cycles = (int) (pick->j - pick->i);
    r.quality = std::clamp (1.0 - pick->err, 0.0, 1.0);
    return r;
}

// Calidad de un loop puesto a mano (mismo criterio que el buscador).
inline double loopQuality (const AudioView& y, size_t start, size_t end)
{
    if (end <= start) return 0;
    const size_t win = std::max<size_t> (8, (end - start) / 8);
    return std::clamp (1.0 - loopdetail::spliceError (y, start, end, std::min<size_t> (win, 4096)), 0.0, 1.0);
}

// Crossfade del empalme, metido en el audio: las últimas `xfadeSamples` muestras antes del fin
// del loop se funden con las que están antes del inicio. Así, al saltar del fin al inicio, lo que
// venía sonando ya es (casi) lo mismo que lo que sigue. Fundido lineal: las dos señales son casi
// iguales (correlacionadas), así el nivel no baja en el medio.
inline void applyLoopCrossfade (std::vector<float*>& ch, size_t start, size_t end, size_t xfadeSamples)
{
    if (end <= start) return;
    xfadeSamples = std::min ({ xfadeSamples, start, end - start });
    if (xfadeSamples < 2) return;
    const size_t from = end - xfadeSamples;
    for (auto* c : ch)
        for (size_t n = from; n < end; ++n)
        {
            const double t = (double) (n - from + 1) / (double) xfadeSamples;
            const size_t m = start - (end - n); // la muestra que corresponde antes del inicio
            c[n] = (float) ((1.0 - t) * c[n] + t * c[m]);
        }
}

// Nota MIDI (con fracción) de la fundamental medida en una zona: para la nota raíz del sampler.
inline double midiNoteOf (const Analysis& a, size_t from, size_t to)
{
    std::vector<float> v;
    for (size_t i = from; i < to && i < a.f0.size(); i += 64) if (a.f0[i] > 0) v.push_back (a.f0[i]);
    if (v.empty()) return 60.0;
    std::nth_element (v.begin(), v.begin() + (long) v.size() / 2, v.end());
    return 69.0 + 12.0 * std::log2 ((double) v[v.size() / 2] / 440.0);
}

} // namespace cyclenorm
