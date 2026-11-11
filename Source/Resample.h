// Re-sampleo de alta calidad (para afinar el 808, como hace un sampler al tocar otra tecla):
// el audio se reproduce más rápido para subir o más lento para bajar. Cambia el tono y el largo
// juntos, y conserva el carácter del sonido (el glide, la saturación, todo se mueve junto).
//
// Interpolación sinc con ventana Kaiser (32 cruces a cada lado). Al subir, el filtro se corta más
// abajo para que no aparezca aliasing.
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace cyclenorm
{

namespace resampledetail
{
inline double besselI0 (double x)
{
    double sum = 1.0, term = 1.0;
    const double q = x * x / 4.0;
    for (int k = 1; k < 50; ++k)
    {
        term *= q / ((double) k * (double) k);
        sum += term;
        if (term < sum * 1e-12) break;
    }
    return sum;
}
} // namespace resampledetail

// speed > 1: más agudo y más corto. speed < 1: más grave y más largo.
// Salida: muestra m = entrada en la posición m * speed.
inline std::vector<std::vector<float>> resample (const std::vector<std::vector<float>>& in, double speed)
{
    using namespace resampledetail;
    std::vector<std::vector<float>> out;
    if (in.empty() || speed <= 0.0) return out;
    const size_t n = in[0].size();
    const size_t outLen = (size_t) std::floor ((double) n / speed);
    out.assign (in.size(), std::vector<float> (outLen, 0.0f));
    if (n == 0) return out;

    const double cutoff = std::min (1.0, 1.0 / speed) * 0.97; // al subir, sin aliasing
    const int zeros = 32;
    const double halfWidth = (double) zeros / cutoff;          // muestras de entrada a cada lado
    const double beta = 9.0;
    const double i0beta = besselI0 (beta);

    // tabla de la ventana*sinc (muy fina) para no calcular senos y Bessel en cada muestra
    const int tableRes = 512;
    const int tableLen = (int) std::ceil (halfWidth * tableRes) + 2;
    std::vector<float> table ((size_t) tableLen);
    for (int t = 0; t < tableLen; ++t)
    {
        const double x = (double) t / tableRes;
        const double r = x / halfWidth;
        if (r >= 1.0) { table[(size_t) t] = 0.0f; continue; }
        const double w = besselI0 (beta * std::sqrt (1.0 - r * r)) / i0beta;
        const double arg = M_PI * x * cutoff;
        const double sinc = x < 1e-12 ? 1.0 : std::sin (arg) / arg;
        table[(size_t) t] = (float) (cutoff * sinc * w);
    }
    auto kernel = [&] (double x) {
        x = std::abs (x) * tableRes;
        const int i = (int) x;
        if (i + 1 >= tableLen) return 0.0f;
        const float f = (float) (x - i);
        return table[(size_t) i] + (table[(size_t) i + 1] - table[(size_t) i]) * f;
    };

    const long hw = (long) std::ceil (halfWidth);
    for (size_t m = 0; m < outLen; ++m)
    {
        const double pos = (double) m * speed;
        const long c = (long) std::floor (pos);
        const long a = std::max (0L, c - hw + 1), b = std::min ((long) n - 1, c + hw);
        for (size_t ch = 0; ch < in.size(); ++ch)
        {
            const float* src = in[ch].data();
            double acc = 0;
            for (long k = a; k <= b; ++k) acc += (double) src[k] * kernel (pos - (double) k);
            out[ch][m] = (float) acc;
        }
    }
    return out;
}

} // namespace cyclenorm
