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
                                         size_t loopStart = 0, size_t loopEnd = 0,
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
        bool startClick = false, endClick = false; // (el principio y el final no se cuentan: sólo el salto del loop)
        if (loop) // del fin del loop al principio
            endClick = seam ({ p[loopEnd - 3], p[loopEnd - 2], p[loopEnd - 1] }, { p[loopStart], p[loopStart + 1], p[loopStart + 2] },
                             std::max (edgeLevel (p, loopEnd), edgeLevel (p, loopStart)),
                             std::max (smoothRef (o, loopEnd), smoothRef (o, loopStart)));
        (void) o;
        if (startClick && (out.empty() || out.front() > from + grp)) out.insert (out.begin(), from);
        if (endClick && (out.empty() || out.back() + grp < to)) out.push_back (to - 1);
    }
    std::sort (out.begin(), out.end());
    out.erase (std::unique (out.begin(), out.end()), out.end());
    return out;
}

// ---------------------------------------------------------------------------------------------
// Ruido: lo que el proceso le agregó al sonido además de cambiarle el volumen (la distorsión de
// subir y bajar la ganancia dentro de cada ciclo). Por ventanas: el espectro del procesado contra
// el del original llevado al mismo nivel (con lo grave, donde está la nota); lo que sobra arriba
// de 250 Hz es agregado. Se pondera como el oído (curva A: los agudos se oyen mucho más que el
// grave del 808) y se da como % de la energía (también ponderada) del procesado: 0 = limpio.
namespace noisedetail
{
    inline void fft (std::vector<double>& re, std::vector<double>& im)
    {
        const size_t n = re.size();
        for (size_t i = 1, j = 0; i < n; ++i)
        {
            size_t bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) { std::swap (re[i], re[j]); std::swap (im[i], im[j]); }
        }
        for (size_t len = 2; len <= n; len <<= 1)
        {
            const double ang = -2.0 * M_PI / (double) len;
            const double wr = std::cos (ang), wi = std::sin (ang);
            for (size_t i = 0; i < n; i += len)
            {
                double cr = 1.0, ci = 0.0;
                for (size_t k = 0; k < len / 2; ++k)
                {
                    const size_t a = i + k, b = i + k + len / 2;
                    const double xr = re[b] * cr - im[b] * ci, xi = re[b] * ci + im[b] * cr;
                    re[b] = re[a] - xr; im[b] = im[a] - xi;
                    re[a] += xr; im[a] += xi;
                    const double t = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = t;
                }
            }
        }
    }
    inline double aWeight2 (double f) // la curva A (en potencia), 1 en 1 kHz
    {
        auto ra = [] (double x) {
            const double f2 = x * x;
            return (12194.0 * 12194.0 * f2 * f2)
                 / ((f2 + 20.6 * 20.6) * std::sqrt ((f2 + 107.7 * 107.7) * (f2 + 737.9 * 737.9)) * (f2 + 12194.0 * 12194.0));
        };
        const double r = ra (std::max (1.0, f)) / ra (1000.0);
        return r * r;
    }
}

inline double measureNoise (const std::vector<const float*>& proc, const std::vector<const float*>& orig,
                            size_t n, size_t from, size_t to, double sr, double cutHz = 250.0)
{
    using namespace noisedetail;
    to = std::min (to, n);
    const size_t W = sr > 60000.0 ? 4096 : 2048;
    if (proc.empty() || orig.size() != proc.size() || to < from + W) return 0.0;
    std::vector<double> win (W), aw (W / 2 + 1);
    for (size_t i = 0; i < W; ++i) win[i] = 0.5 - 0.5 * std::cos (2.0 * M_PI * (double) i / (double) (W - 1));
    for (size_t k = 0; k <= W / 2; ++k) aw[k] = aWeight2 ((double) k * sr / (double) W);
    const size_t cutBin = (size_t) std::ceil (cutHz * (double) W / sr);
    std::vector<double> pr (W), pi (W), orr (W), oi (W);
    double added = 0.0, total = 0.0;
    for (size_t ch = 0; ch < proc.size(); ++ch)
        for (size_t s0 = from; s0 + W <= to; s0 += W)
        {
            for (size_t i = 0; i < W; ++i)
            {
                pr[i] = (double) proc[ch][s0 + i] * win[i]; pi[i] = 0.0;
                orr[i] = (double) orig[ch][s0 + i] * win[i]; oi[i] = 0.0;
            }
            fft (pr, pi); fft (orr, oi);
            double pl = 0.0, ol = 0.0;
            for (size_t k = 0; k < cutBin; ++k) { pl += pr[k] * pr[k] + pi[k] * pi[k]; ol += orr[k] * orr[k] + oi[k] * oi[k]; }
            const double g2 = ol > 1e-20 ? pl / ol : 0.0;
            for (size_t k = 0; k <= W / 2; ++k)
            {
                const double P = pr[k] * pr[k] + pi[k] * pi[k];
                total += P * aw[k];
                if (k >= cutBin) added += std::max (0.0, P - g2 * (orr[k] * orr[k] + oi[k] * oi[k])) * aw[k];
            }
        }
    return total > 1e-20 ? 100.0 * added / total : 0.0;
}
}
