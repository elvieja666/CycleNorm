#pragma once
// Detector de clicks: busca en el procesado los golpes de agudos (discontinuidades) que el original
// no tenía. Un 808 casi no tiene agudos, así que la segunda diferencia (un pasa-altos muy fuerte)
// de una onda redonda es chiquita; un click (un salto en el valor o en la pendiente) la dispara.
// Se compara contra el original en el mismo lugar (normalizado por el nivel de cada uno), así el
// ataque natural o un transitorio que ya venía en el sample no cuentan.
#include <vector>
#include <cmath>
#include <cstddef>
#include <algorithm>

namespace cyclenorm
{
struct ClickSettings
{
    double absThreshold = 0.01;  // segunda diferencia / nivel RMS local: arriba de esto es un golpe de agudos
                                 // (un salto de ~0,7 % del nivel en una onda redonda)
    double relThreshold = 4.0;   // y tantas veces más que lo que tenía el original ahí
    double levelWinSec  = 0.005; // ventana del nivel local (a cada lado)
    double refWinSec    = 0.001; // a cada lado: el original se mira en esta vecindad
    double groupSec     = 0.010; // golpes más cerca que esto son el mismo click
    double gateDb       = -70.0; // más bajo que esto no se mira (inaudible)
};

namespace clickdetail
{
    // RMS de x en [i-w, i+w] para cada i de [a, b), con sumas acumuladas
    inline std::vector<double> localRms (const float* x, size_t a, size_t b, size_t w, size_t n)
    {
        const size_t lo = a > w ? a - w : 0, hi = std::min (n, b + w);
        std::vector<double> cum (hi - lo + 1, 0.0);
        for (size_t i = lo; i < hi; ++i) cum[i - lo + 1] = cum[i - lo] + (double) x[i] * x[i];
        std::vector<double> r (b - a);
        for (size_t i = a; i < b; ++i)
        {
            const size_t s = i > lo + w ? i - w : lo, e = std::min (hi, i + w + 1);
            r[i - a] = std::sqrt ((cum[e - lo] - cum[s - lo]) / (double) std::max<size_t> (1, e - s));
        }
        return r;
    }
    inline double d2 (const float* x, size_t i) { return (double) x[i] - 2.0 * x[i - 1] + (double) x[i - 2]; }
}

// proc y orig: los canales (mismo largo, misma línea de tiempo: proc = orig × ganancia).
// [from, to): lo que suena. Con loop (loopEnd > loopStart): se revisa también el salto del fin
// del loop al principio, y lo que suena termina en loopEnd. Devuelve dónde está cada click (muestra).
inline std::vector<size_t> detectClicks (const std::vector<const float*>& proc, const std::vector<const float*>& orig,
                                         size_t n, size_t from, size_t to, double sr,
                                         size_t loopStart = 0, size_t loopEnd = 0, bool fadeIn = false, bool fadeOut = false,
                                         const ClickSettings& cs = {})
{
    using namespace clickdetail;
    std::vector<size_t> out;
    const bool loop = loopEnd > loopStart && loopEnd <= n;
    if (loop) to = std::min (to, loopEnd);
    to = std::min (to, n);
    if (proc.empty() || orig.size() != proc.size() || to < from + 8) return out;

    const size_t wLev = std::max<size_t> (8, (size_t) (cs.levelWinSec * sr));
    const size_t wRef = std::max<size_t> (2, (size_t) (cs.refWinSec * sr));
    const double gate = std::pow (10.0, cs.gateDb / 20.0);
    const size_t a = from + 2, b = to;

    // el puntaje de cada muestra: el máximo de los canales
    std::vector<double> score (b - a, 0.0);
    for (size_t ch = 0; ch < proc.size(); ++ch)
    {
        const float* p = proc[ch]; const float* o = orig[ch];
        const auto ap = localRms (p, a, b, wLev, n);
        const auto ao = localRms (o, a, b, wLev, n);
        for (size_t i = a; i < b; ++i)
        {
            const double lev = ap[i - a];
            if (lev < gate) continue;
            const double sp = std::abs (d2 (p, i)) / lev;
            if (sp <= cs.absThreshold) continue;
            // (pocos llegan acá) lo que tenía el original en la vecindad, normalizado con su nivel
            const size_t s0 = std::max (a, i > wRef ? i - wRef : 0), e0 = std::min (b, i + wRef + 1);
            double m = 0;
            for (size_t j = s0; j < e0; ++j) m = std::max (m, std::abs (d2 (o, j)));
            const double ro = ao[i - a] > 1e-9 ? m / ao[i - a] : 0.0;
            if (sp > cs.relThreshold * ro) score[i - a] = std::max (score[i - a], sp);
        }
    }
    // los golpes cercanos son un mismo click
    const size_t grp = std::max<size_t> (1, (size_t) (cs.groupSec * sr));
    size_t last = 0; bool any = false; double best = 0;
    for (size_t i = 0; i < score.size(); ++i)
    {
        if (score[i] <= 0) continue;
        if (any && i - last <= grp)
        {
            if (score[i] > best) { best = score[i]; out.back() = a + i; }
        }
        else { out.push_back (a + i); best = score[i]; }
        last = i; any = true;
    }

    // los bordes: lo que suena arranca en from y termina en to (o vuelve al principio del loop).
    // Se arma el pedacito que se escucha ahí y se mide igual (contra el nivel del lugar)
    auto seamD2 = [] (const std::vector<double>& left, const std::vector<double>& right) {
        std::vector<float> s;
        for (double v : left) s.push_back ((float) v);
        for (double v : right) s.push_back ((float) v);
        double m = 0;
        for (size_t i = 2; i < s.size(); ++i) m = std::max (m, std::abs (d2 (s.data(), i)));
        return m;
    };
    auto seam = [&] (const std::vector<double>& left, const std::vector<double>& right, double lev, double ref) {
        const double m = seamD2 (left, right);
        return lev > gate && m / lev > cs.absThreshold && m / lev > cs.relThreshold * ref;
    };
    // los bordes del archivo (el principio y el final de verdad): si el original ya arrancaba o
    // terminaba así, no es del proceso; cuenta sólo si el procesado salta bastante más (p. ej. la cola
    // subida de nivel que termina de golpe)
    auto naturalEdge = [&] (double mp, double mo, double lev) {
        return lev > gate && mp / lev > cs.absThreshold && mp > cs.relThreshold * mo;
    };
    auto edgeLevel = [&] (const float* x, size_t at) {
        const size_t s = at > wLev ? at - wLev : 0, e = std::min (n, at + wLev);
        double acc = 0; for (size_t i = s; i < e; ++i) acc += (double) x[i] * x[i];
        return std::sqrt (acc / (double) std::max<size_t> (1, e - s));
    };
    auto smoothRef = [&] (const float* o, size_t at) { // lo "normal" del original ahí
        const size_t s = std::max<size_t> (2, at > wRef ? at - wRef : 0), e = std::min (n, at + wRef);
        const double lev = edgeLevel (o, at);
        double m = 0; for (size_t i = s; i < e; ++i) m = std::max (m, std::abs (d2 (o, i)));
        return lev > 1e-9 ? m / lev : 0.0;
    };
    for (size_t ch = 0; ch < proc.size(); ++ch)
    {
        const float* p = proc[ch]; const float* o = orig[ch];
        bool startClick = false, endClick = false;
        if (! fadeIn && from == 0)
            startClick = naturalEdge (seamD2 ({ 0.0, 0.0 }, { p[0], p[1], p[2] }), seamD2 ({ 0.0, 0.0 }, { o[0], o[1], o[2] }), edgeLevel (p, wLev));
        else if (! fadeIn) // arranca de golpe en Start (antes, silencio)
            startClick = seam ({ 0.0, 0.0 }, { p[from], p[from + 1], p[from + 2] }, edgeLevel (p, from + wLev), smoothRef (o, from + wLev));
        if (loop) // del fin del loop al principio
            endClick = seam ({ p[loopEnd - 3], p[loopEnd - 2], p[loopEnd - 1] }, { p[loopStart], p[loopStart + 1], p[loopStart + 2] },
                             std::max (edgeLevel (p, loopEnd), edgeLevel (p, loopStart)),
                             std::max (smoothRef (o, loopEnd), smoothRef (o, loopStart)));
        else if (! fadeOut && to == n)
            endClick = naturalEdge (seamD2 ({ p[n - 3], p[n - 2], p[n - 1] }, { 0.0, 0.0 }), seamD2 ({ o[n - 3], o[n - 2], o[n - 1] }, { 0.0, 0.0 }),
                                    edgeLevel (p, n - wLev));
        else if (! fadeOut) // se corta de golpe en End (después, silencio)
            endClick = seam ({ p[to - 3], p[to - 2], p[to - 1] }, { 0.0, 0.0 }, edgeLevel (p, to - wLev), smoothRef (o, to - wLev));
        if (startClick && (out.empty() || out.front() > from + grp)) out.insert (out.begin(), from);
        if (endClick && (out.empty() || out.back() + grp < to)) out.push_back (to - 1);
    }
    std::sort (out.begin(), out.end());
    out.erase (std::unique (out.begin(), out.end()), out.end());
    return out;
}
}
