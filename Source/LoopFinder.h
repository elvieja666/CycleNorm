// Buscador de loop: encuentra de dónde a dónde loopear el 808 (ya normalizado) para que el loop
// suene infinito sin que se note. El loop siempre arranca y termina en un cruce por cero
// ascendente de la fundamental (ondas enteras), así la fase empalma y no hay click.
//
// Búsqueda exhaustiva: prueba TODOS los pares inicio/fin posibles y a cada uno lo mide completo:
//   1. El empalme, fino: 2 ondas a cada lado del corte, muestra a muestra y por canal (lo que se
//      escucha al saltar del fin al inicio contra lo que se escucharía sin saltar), la continuidad
//      exacta en la muestra del salto (un salto ahí es un click) y la pendiente en el punto de
//      corte (un cambio de pendiente se oye como un "tic" suave).
//   2. La nota: que las ondas a los dos lados del empalme duren lo mismo (sin salto de pitch).
//   3. La estabilidad de todo el loop (no solo del empalme): cuánto varía la nota onda a onda y del
//      principio al final del loop, cuánto cambia el timbre (principio, medio y final), y cuánto
//      varían el nivel, el brillo y el ruido. Un loop que "ondula" adentro se nota al repetirse.
// Se queda con los mejores candidatos distintos entre sí (para poder probar otros de oído).
// Entre el mejor y uno más largo prácticamente igual de bueno, prefiere el más largo.
#pragma once

#include "CycleEngine.h"
#include <algorithm>
#include <cmath>
#include <functional>
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
    double quality = 0;   // 0..1 (1 = perfecto)
    double cost    = 0;   // menos es mejor
};

namespace loopdetail
{
constexpr int kShape = 128;

struct CycleFeat
{
    double len = 0;
    double rms = 0;
    double hf = 0;       // energía de la derivada / energía (brillo: armónicos y ruido)
    double irr = 0;      // cuánto difiere de la onda anterior (ruido, irregularidad)
    double shape[kShape] = {};
};

inline double shapeDist (const CycleFeat& a, const CycleFeat& b)
{
    double d = 0, na = 0, nb = 0;
    for (int k = 0; k < kShape; ++k)
    {
        const double x = a.shape[k] - b.shape[k];
        d += x * x; na += a.shape[k] * a.shape[k]; nb += b.shape[k] * b.shape[k];
    }
    return d / std::max (1e-12, 0.5 * (na + nb));
}

struct Features
{
    std::vector<size_t> b;       // inicios de onda entera
    std::vector<CycleFeat> F;    // onda k = b[k] .. b[k+1]
    // sumas acumuladas para medias y desvíos en O(1)
    std::vector<double> sL, sL2, sHf, sHf2, sR, sR2;

    double mean (const std::vector<double>& s, size_t i, size_t j) const { return (s[j] - s[i]) / (double) (j - i); }
    double cv (const std::vector<double>& s, const std::vector<double>& s2, size_t i, size_t j) const
    {
        const double m = mean (s, i, j), v = std::max (0.0, mean (s2, i, j) - m * m);
        return m > 1e-12 ? std::sqrt (v) / m : 0.0;
    }
};

inline Features build (const Analysis& a, const AudioView& y)
{
    Features f;
    f.b = a.bounds;
    while (! f.b.empty() && f.b.back() >= y.n) f.b.pop_back();
    if (f.b.size() < 2 || y.n == 0) { f.b.clear(); return f; }
    const size_t K = f.b.size() - 1;
    f.F.resize (K);
    for (size_t k = 0; k < K; ++k)
    {
        auto& c = f.F[k];
        const size_t from = f.b[k], to = f.b[k + 1];
        if (to <= from + 1) { c.len = 1; continue; }
        c.len = (double) (to - from);
        double e = 0, ed = 0;
        float prev = y.mono (from);
        for (size_t i = from; i < to; ++i)
        {
            const float v = y.mono (i);
            e += (double) v * v; ed += (double) (v - prev) * (v - prev); prev = v;
        }
        c.rms = std::sqrt (e / c.len);
        c.hf = e > 1e-20 ? ed / e : 0.0;
        for (int s = 0; s < kShape; ++s)
        {
            const double pos = (double) from + (c.len - 1.0) * (double) s / (double) (kShape - 1);
            const size_t i0 = (size_t) pos;
            const double t = pos - (double) i0;
            const float v0 = y.mono (std::min (i0, to - 1)), v1 = y.mono (std::min (i0 + 1, to - 1));
            c.shape[s] = v0 + (v1 - v0) * t;
        }
    }
    for (size_t k = 1; k < K; ++k) f.F[k].irr = shapeDist (f.F[k], f.F[k - 1]);
    if (K > 1) f.F[0].irr = f.F[1].irr;
    auto acc = [&] (std::vector<double>& s, std::vector<double>& s2, auto get) {
        s.assign (K + 1, 0.0); s2.assign (K + 1, 0.0);
        for (size_t k = 0; k < K; ++k) { const double v = get (f.F[k]); s[k + 1] = s[k] + v; s2[k + 1] = s2[k] + v * v; }
    };
    acc (f.sL, f.sL2, [] (const CycleFeat& c) { return c.len; });
    acc (f.sHf, f.sHf2, [] (const CycleFeat& c) { return c.hf; });
    acc (f.sR, f.sR2, [] (const CycleFeat& c) { return c.rms; });
    return f;
}

// El empalme, muestra a muestra y por canal: lo que suena alrededor del inicio contra lo que
// suena alrededor del fin (win muestras a cada lado). 0 = idénticos.
inline double spliceError (const AudioView& y, size_t start, size_t end, size_t win)
{
    if (start >= y.n || end >= y.n) return 1.0;
    double d = 0, e = 0;
    const size_t back = std::min ({ win, start, end });
    const size_t fwd = std::min (win, y.n - std::max (start, end));
    for (auto* c : y.ch)
        for (size_t k = 0; k < back + fwd; ++k)
        {
            const size_t ia = start - back + k, ib = end - back + k;
            const double x = (double) c[ia] - (double) c[ib];
            d += x * x;
            e += 0.5 * ((double) c[ia] * c[ia] + (double) c[ib] * c[ib]);
        }
    return e > 1e-20 ? std::sqrt (d / e) : 1.0;
}

// Cambio de pendiente en el corte: al saltar, la pendiente que sigue después del inicio contra la
// que seguiría sin saltar (después del fin), comparadas con la pendiente de la onda en ese punto.
inline double slopeError (const AudioView& y, size_t start, size_t end)
{
    if (start < 1 || end < 1 || start + 1 >= y.n || end + 1 >= y.n) return 0.0;
    double d = 0, ref = 0;
    for (auto* c : y.ch)
    {
        const double sStart = (double) c[start + 1] - (double) c[start];
        const double sNat = (double) c[end + 1] - (double) c[end];
        d += (sStart - sNat) * (sStart - sNat);
        ref += 0.5 * (sStart * sStart + sNat * sNat);
    }
    return ref > 1e-20 ? std::sqrt (d / ref) : 0.0;
}

// Continuidad exacta en el salto: después de la última muestra del loop suena la del inicio; sin
// saltar sonaría la del fin. Esa diferencia, comparada con el paso normal de la onda alrededor del
// fin (rms de la diferencia entre muestras vecinas). 0 = continuo; 2-3 o más = un click.
inline double jumpError (const AudioView& y, size_t start, size_t end, size_t win)
{
    if (start >= y.n || end >= y.n || end < 1) return 10.0;
    const size_t a0 = end > win ? end - win : 1, a1 = std::min (y.n, end + win);
    double d = 0, step = 0;
    size_t cnt = 0;
    for (auto* c : y.ch)
    {
        const double x = (double) c[start] - (double) c[end];
        d += x * x;
        for (size_t i = a0; i < a1; ++i) { const double s = (double) c[i] - (double) c[i - 1]; step += s * s; ++cnt; }
    }
    step = cnt > 0 ? step / (double) cnt : 0.0;
    return step > 1e-24 ? std::sqrt ((d / (double) y.ch.size()) / step) : 0.0;
}

// Puntaje completo del loop de la onda i a la onda j (loop = b[i] .. b[j]). Usa también las ondas
// i-1 y j (las vecinas del empalme). Menos es mejor.
// (s0/s1: dónde caen de verdad las puntas, si el ajuste fino las corrió unas muestras del cruce)
inline double score (const Features& f, const AudioView& y, size_t i, size_t j, size_t s0 = 0, size_t s1 = 0)
{
    const auto& F = f.F;
    const size_t K = F.size();
    if (i < 1 || j >= K || j <= i) return 1e9;
    if (s1 == 0) { s0 = f.b[i]; s1 = f.b[j]; }
    const double meanL = f.mean (f.sL, i, j);
    // 1. empalme
    const size_t win = (size_t) std::max (16.0, 2.0 * meanL);
    const double splice = spliceError (y, s0, s1, win);
    const double slope = slopeError (y, s0, s1);
    const double jump = jumpError (y, s0, s1, win);
    // 2. nota a los dos lados del empalme
    const double across = (std::abs (F[j - 1].len - F[i - 1].len) + std::abs (F[j].len - F[i].len)) / (2.0 * meanL);
    // 3. estabilidad adentro del loop
    const double pitchCv = f.cv (f.sL, f.sL2, i, j);
    const double pitchDrift = std::abs (F[j - 1].len - F[i].len) / meanL;
    const size_t mid = (i + j - 1) / 2;
    const double timbreDrift = 0.5 * (shapeDist (F[i], F[mid]) + shapeDist (F[mid], F[j - 1]));
    const double hfCv = f.cv (f.sHf, f.sHf2, i, j);
    const double levelCv = f.cv (f.sR, f.sR2, i, j);
    const double noiseMismatch = std::abs (F[j - 1].irr - F[i - 1].irr) + std::abs (F[j].irr - F[i].irr);

    return 1.0 * splice + 0.25 * slope + 0.02 * jump * jump
         + 10.0 * across + 6.0 * pitchCv + 4.0 * pitchDrift
         + 0.6 * timbreDrift + 0.4 * hfCv + 1.0 * levelCv + 0.3 * noiseMismatch;
}

// costo -> calidad (0..1): un empalme con 1 % de diferencia y un loop estable da ~97 %
inline double qualityOf (double cost) { return std::exp (-2.5 * cost); }
} // namespace loopdetail

// y: el audio procesado (normalizado). from/to: la zona donde puede estar el loop (después de lo
// salteado con Skip y antes de la línea de Fin). Devuelve hasta maxResults candidatos distintos,
// el mejor primero. progress (opcional) recibe 0..1 y devuelve false para cancelar.

// ---------------------------------------------------------------------------------------------
// Sonidos que modulan (reese, bajos con LFO, detune, chorus): la onda cambia de forma todo el
// tiempo, así que comparar sólo el empalme (2 ondas) elige loops cortitos que "congelan" el sonido.
// Para estos se compara un tramo largo alrededor del empalme (0,15 s a cada lado): el loop empalma
// en el mismo momento de la modulación.
//   - Si la modulación se repite (un LFO, un batido regular), el loop dura justo uno o más ciclos.
//   - Si cambia lento sin repetirse, el loop es largo (al menos la mitad de la parte sostenida),
//     con el mejor empalme posible; el crossfade termina de taparlo.
// Se detecta solo: la mediana de cuánto se parece cada onda a la que está 8 ondas después
// (forma de los dos canales). En los 808 da 0,91 o más; en los reese, 0,88 o menos.
namespace modloop
{
    struct Shapes
    {
        std::vector<std::vector<float>> F; // forma de cada onda (L y R, normalizada)
        std::vector<double> lvl;           // nivel de cada onda
        std::vector<size_t> b;             // inicio de cada onda (muestra)
        double period = 1;                 // largo medio de una onda (muestras)
    };
    inline Shapes build (const Analysis& a, const AudioView& y, size_t from, size_t to)
    {
        Shapes s;
        const auto& bf = a.boundsFrac;
        constexpr int K = 64;
        for (size_t k = 0; k + 1 < bf.size() && k + 1 < a.bounds.size(); ++k)
        {
            if (a.bounds[k] < from || a.bounds[k + 1] > std::min (to, y.n)) continue;
            const double s0 = bf[k], s1 = bf[k + 1];
            std::vector<float> f; f.reserve ((size_t) K * y.ch.size());
            double e = 0;
            for (auto* c : y.ch)
                for (int j = 0; j < K; ++j)
                {
                    const double t = s0 + (s1 - s0) * j / K;
                    const size_t i = (size_t) t; const double fr = t - (double) i;
                    const size_t i1 = std::min (i + 1, y.n - 1);
                    const float x = (float) (c[i] * (1.0 - fr) + c[i1] * fr);
                    f.push_back (x); e += (double) x * x;
                }
            e = std::sqrt (e / (double) std::max<size_t> (1, f.size())) + 1e-9;
            for (auto& x : f) x = (float) (x / e);
            s.F.push_back (std::move (f)); s.lvl.push_back (e); s.b.push_back (a.bounds[k]);
            if (s.b.size() == 1) s.period = s1 - s0;
        }
        if (s.b.size() >= 2) s.period = (double) (s.b.back() - s.b.front()) / (double) (s.b.size() - 1);
        return s;
    }
    inline double corr (const Shapes& s, size_t i, size_t j)
    {
        const auto& x = s.F[i]; const auto& y = s.F[j];
        double q = 0; for (size_t z = 0; z < x.size(); ++z) q += (double) x[z] * y[z];
        return q / (double) std::max<size_t> (1, x.size());
    }
    inline size_t attackOf (const Shapes& s) { return std::max<size_t> (s.F.size() / 10, 6); }
    inline bool isModulated (const Shapes& s)
    {
        const size_t N = s.F.size(), k0 = attackOf (s);
        if (N < k0 + 24) return false;
        std::vector<double> v;
        for (size_t i = k0; i + 8 < N; ++i) v.push_back (corr (s, i, i + 8));
        std::nth_element (v.begin(), v.begin() + (long) v.size() / 2, v.end());
        return v[v.size() / 2] < 0.90;
    }
    inline int windowOf (const Shapes& s, double sr) { return std::max (3, (int) std::lround (0.15 * sr / std::max (1.0, s.period))); }
    // parecido de las ondas alrededor del inicio (i) y del fin (j), y del nivel; 1 = perfecto
    template <typename CorrFn>
    inline double context (const Shapes& s, size_t i, size_t j, int W, CorrFn&& c)
    {
        const long N = (long) s.F.size();
        double q = 0, lv = 0; int n = 0;
        for (int d = -W; d < W; ++d)
        {
            const long a1 = (long) i + d, b1 = (long) j + d;
            if (a1 < 0 || b1 >= N) continue;
            q += c ((size_t) a1, (size_t) b1);
            lv += std::abs (std::log (s.lvl[(size_t) a1] / s.lvl[(size_t) b1]));
            ++n;
        }
        return n > 0 ? q / n - 0.5 * lv / n : 0.0;
    }

    inline std::vector<LoopResult> find (const Analysis& a, const Shapes& s, size_t maxResults,
                                         const std::function<bool (double)>& progress)
    {
        std::vector<LoopResult> out;
        const size_t N = s.F.size();
        if (N < 24) return out;
        // matriz de parecidos (simétrica)
        std::vector<float> C (N * N, 0.0f);
        for (size_t i = 0; i < N; ++i)
        {
            for (size_t j = i; j < N; ++j) C[i * N + j] = C[j * N + i] = (float) corr (s, i, j);
            if (progress && (i % 16) == 0 && ! progress (0.5 * (double) i / (double) N)) return out;
        }
        auto cc = [&] (size_t i, size_t j) { return (double) C[i * N + j]; };
        const int W = windowOf (s, a.sampleRate);
        const size_t k0 = attackOf (s);
        // largo en que la forma deja de parecerse (mitad entre 1 onda y lo más distinto)
        std::vector<double> ac (N / 2 + 1, 1.0);
        double acMin = 1.0;
        for (size_t L = 1; L <= N / 2; ++L)
        {
            double q = 0; int n = 0;
            for (size_t i = k0; i + L < N; ++i) { q += cc (i, i + L); ++n; }
            ac[L] = n > 0 ? q / n : 1.0;
            acMin = std::min (acMin, ac[L]);
        }
        const double mid = 0.5 * (ac[1] + acMin);
        size_t Ld = 1;
        while (Ld < N / 2 && ac[Ld] > mid) ++Ld;
        const size_t base = std::max<size_t> ({ 8, (size_t) std::lround (0.3 * a.sampleRate / s.period), (Ld * 3) / 2 });

        struct Cand { size_t i, j; double sc; };
        auto search = [&] (size_t minL, double baseProgress) {
            std::vector<Cand> all;
            for (size_t i = k0; i < N; ++i)
            {
                for (size_t j = i + minL; j + 2 < N; ++j) all.push_back ({ i, j, context (s, i, j, W, cc) });
                if (progress && (i % 16) == 0 && ! progress (baseProgress + 0.25 * (double) (i - k0) / (double) std::max<size_t> (1, N - k0))) return std::vector<Cand>();
            }
            std::sort (all.begin(), all.end(), [] (const Cand& x, const Cand& y) { return x.sc > y.sc; });
            return all;
        };
        auto all = search (base, 0.5);
        // ¿se repite la modulación? (un loop de un ciclo empalma casi perfecto)
        if (all.empty() || all.front().sc < 0.95)
        {
            const size_t sustain = N > k0 ? N - k0 : N;
            all = search (std::max (base, sustain / 2), 0.75); // no se repite: loop largo
        }
        std::vector<Cand> picked;
        for (const auto& c : all)
        {
            bool dup = false;
            for (const auto& q : picked)
                if (std::abs ((long) q.i - (long) c.i) < W && std::abs ((long) q.j - (long) c.j) < W) { dup = true; break; }
            if (! dup) picked.push_back (c);
            if (picked.size() >= maxResults) break;
        }
        for (const auto& c : picked)
        {
            LoopResult r;
            r.found = true; r.start = s.b[c.i]; r.end = s.b[c.j];
            r.cycles = (int) (c.j - c.i);
            r.quality = std::max (0.0, std::min (1.0, c.sc));
            r.cost = 1.0 - c.sc;
            out.push_back (r);
        }
        if (progress) progress (1.0);
        return out;
    }
}

inline std::vector<LoopResult> findLoops (const Analysis& a, const AudioView& y, size_t from, size_t to,
                                          int minCycles = 4, size_t maxResults = 5,
                                          const std::function<bool (double)>& progress = nullptr)
{
    using namespace loopdetail;
    std::vector<LoopResult> out;
    {   // ¿el sonido modula (reese, LFO, detune)? entonces se busca distinto (ver modloop)
        const auto sh = modloop::build (a, y, from, std::min (to, y.n));
        if (modloop::isModulated (sh)) return modloop::find (a, sh, maxResults, progress);
    }
    const auto f = build (a, y);
    const size_t K = f.F.size();
    if (K < (size_t) minCycles + 3) return out;
    to = std::min (to, y.n);

    // trabajo total (para el progreso): cantidad de pares
    double total = 0, done = 0;
    for (size_t i = 1; i + (size_t) minCycles < K; ++i) total += (double) (K - i - (size_t) minCycles);

    struct Cand { size_t i, j; double cost; };
    std::vector<Cand> all;
    for (size_t i = 1; i + (size_t) minCycles < K; ++i)
    {
        done += (double) (K - i - (size_t) minCycles);
        if (progress && ! progress (total > 0 ? done / total : 1.0)) return {};
        if (f.b[i - 1] < from) continue;
        for (size_t j = i + (size_t) minCycles; j < K; ++j)
        {
            if (f.b[j] > to) break;
            all.push_back ({ i, j, score (f, y, i, j) });
        }
    }
    if (progress) progress (1.0);
    if (all.empty()) return out;
    std::sort (all.begin(), all.end(), [] (const Cand& x, const Cand& z) { return x.cost < z.cost; });

    // el primero: el mejor, o uno más largo prácticamente igual de bueno (hasta 8 % peor)
    const double best = all.front().cost;
    const Cand* first = &all.front();
    for (const auto& c : all)
    {
        if (c.cost > best * 1.08 + 0.002) break;
        if (c.j - c.i > first->j - first->i) first = &c;
    }
    std::vector<const Cand*> picked { first };
    auto similar = [] (const Cand& x, const Cand& z) {
        auto near = [] (size_t p, size_t q) { return (p > q ? p - q : q - p) <= 2; };
        return near (x.i, z.i) && near (x.j, z.j);
    };
    for (const auto& c : all)
    {
        if (picked.size() >= maxResults) break;
        bool dup = false;
        for (auto* p : picked) dup = dup || similar (c, *p);
        if (! dup) picked.push_back (&c);
    }
    for (auto* p : picked)
    {
        LoopResult r;
        r.found = true;
        r.start = f.b[p->i];
        r.end = f.b[p->j];
        r.cycles = (int) (p->j - p->i);
        // Ajuste fino: cada punta se puede correr unas pocas muestras alrededor del cruce (hasta
        // ±1/40 de onda, máximo 8 muestras: sigue pegada al cero) si así empalma mejor.
        const double meanL = f.mean (f.sL, p->i, p->j);
        const int reach = (int) std::clamp (meanL / 40.0, 0.0, 8.0);
        const size_t win = (size_t) std::max (16.0, 2.0 * meanL);
        auto spliceCost = [&] (size_t s0, size_t s1) {
            const double j = jumpError (y, s0, s1, win);
            return spliceError (y, s0, s1, win) + 0.25 * slopeError (y, s0, s1) + 0.02 * j * j;
        };
        const double base = spliceCost (r.start, r.end);
        double bestSplice = base;
        size_t bs = r.start, be = r.end;
        for (int ds = -reach; ds <= reach; ++ds)
            for (int de = -reach; de <= reach; ++de)
            {
                const long s0 = (long) r.start + ds, s1 = (long) r.end + de;
                if (s0 < 2 || s1 <= s0 + 8 || (size_t) s1 + 2 >= y.n || (size_t) s0 < from || (size_t) s1 > to) continue;
                const double c = spliceCost ((size_t) s0, (size_t) s1);
                if (c < bestSplice - 1e-9) { bestSplice = c; bs = (size_t) s0; be = (size_t) s1; }
            }
        r.start = bs; r.end = be;
        r.cost = score (f, y, p->i, p->j, bs, be);
        r.quality = qualityOf (r.cost);
        out.push_back (r);
    }
    return out;
}

inline LoopResult findLoop (const Analysis& a, const AudioView& y, size_t from, size_t to, int minCycles = 4)
{
    auto v = findLoops (a, y, from, to, minCycles, 1);
    return v.empty() ? LoopResult() : v.front();
}

// Calidad de un loop puesto a mano, con el mismo criterio completo que el buscador.
inline double loopQuality (const Analysis& a, const AudioView& y, size_t start, size_t end)
{
    using namespace loopdetail;
    if (end <= start) return 0;
    {   // sonido que modula: la calidad es el parecido del tramo largo alrededor del empalme
        const auto sh = modloop::build (a, y, 0, y.n);
        if (modloop::isModulated (sh))
        {
            auto near = [&] (size_t v) {
                size_t best = 0, bd = (size_t) -1;
                for (size_t k = 0; k < sh.b.size(); ++k) { const size_t d = sh.b[k] > v ? sh.b[k] - v : v - sh.b[k]; if (d < bd) { bd = d; best = k; } }
                return best;
            };
            const size_t i = near (start), j = near (end);
            if (j > i)
                return std::max (0.0, std::min (1.0, modloop::context (sh, i, j, modloop::windowOf (sh, a.sampleRate),
                                                                       [&] (size_t p, size_t q) { return modloop::corr (sh, p, q); })));
        }
    }
    const auto f = build (a, y);
    // la onda entera más cercana a cada punta (con el ajuste fino pueden estar unas muestras corridas)
    auto nearest = [&] (size_t v) {
        size_t best = 0, bd = (size_t) -1;
        for (size_t k = 0; k < f.b.size(); ++k) { const size_t d = f.b[k] > v ? f.b[k] - v : v - f.b[k]; if (d < bd) { bd = d; best = k; } }
        return std::make_pair (best, bd);
    };
    const auto [i, di] = nearest (start);
    const auto [j, dj] = nearest (end);
    if (! f.b.empty() && di <= 8 && dj <= 8 && i >= 1 && j < f.F.size() && j > i)
        return qualityOf (score (f, y, i, j, start, end));
    // (no cae en ondas enteras, o en una punta del archivo: solo el empalme)
    return qualityOf (spliceError (y, start, end, std::max<size_t> (16, (end - start) / 8)));
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
