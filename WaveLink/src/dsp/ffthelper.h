#ifndef FFTHELPER_H
#define FFTHELPER_H

#include <complex>
#include <vector>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

class FftHelper {
public:
    static int powerOfTwoFloor(int count) {
        if (count <= 0) return 0;
        int result = 1;
        while (result <= count / 2) result *= 2;
        return result;
    }
    // Requires x.size() to be a power of 2
    static void computeFFT(std::vector<std::complex<double>>& x) {
        size_t N = x.size();
        if (N <= 1) return;

        // Separate even and odd elements
        std::vector<std::complex<double>> even(N / 2);
        std::vector<std::complex<double>> odd(N / 2);
        for (size_t i = 0; i < N / 2; ++i) {
            even[i] = x[i * 2];
            odd[i] = x[i * 2 + 1];
        }

        // Conquer
        computeFFT(even);
        computeFFT(odd);

        // Combine
        for (size_t k = 0; k < N / 2; ++k) {
            std::complex<double> t = std::polar(1.0, -2.0 * M_PI * k / N) * odd[k];
            x[k] = even[k] + t;
            x[k + N / 2] = even[k] - t;
        }
    }
};

#endif // FFTHELPER_H
