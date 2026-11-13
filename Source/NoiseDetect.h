#pragma once
#include <vector>
#include <cmath>
#include <cstddef>
#include <algorithm>

namespace cyclenorm
{
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
