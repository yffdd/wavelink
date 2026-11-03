#include "demodatagenerator.h"
#include <QtMath>
#include <QRandomGenerator>

double DemoDataGenerator::generate(WaveType type, double t_sec, const Parameters &params)
{
    double val = 0.0;
    double t = t_sec * params.frequency; 
    // Normalized time in cycle (0 to 1)
    double phase = t - qFloor(t); 

    switch (type) {
    case Sine:
        val = qSin(2.0 * M_PI * phase) * (params.vpp / 2.0);
        break;
    case Triangle:
        if (phase < 0.25) val = phase * 4.0;
        else if (phase < 0.75) val = 1.0 - (phase - 0.25) * 4.0;
        else val = -1.0 + (phase - 0.75) * 4.0;
        val *= (params.vpp / 2.0);
        break;
    case Square:
        val = (phase < (params.dutyCycle / 100.0)) ? (params.vpp / 2.0) : -(params.vpp / 2.0);
        break;
    case Pulse:
        // Pulse width is in ms, we convert it to phase fraction based on frequency
        {
            double period_ms = (1.0 / params.frequency) * 1000.0;
            double pulseFraction = params.pulseWidth / period_ms;
            val = (phase < pulseFraction) ? (params.vpp / 2.0) : -(params.vpp / 2.0);
        }
        break;
    case ECG:
        val = generateECG(phase, params);
        break;
    case EMG:
        val = generateEMG(t_sec, params);
        break;
    case EEG:
        val = generateEEG(t_sec, params);
        break;
    case EOG:
        val = generateEOG(t_sec, params);
        break;
    case PPG:
        val = generatePPG(phase, params);
        break;
    case CBF:
        val = generateCBF(t_sec, params);
        break;
    }

    val += params.offset;
    if (params.noiseLevel > 0) {
        bool isPhysiological = (type >= ECG && type <= CBF);
        val = addNoise(val, params.noiseLevel, t_sec, isPhysiological);
    }
    return val;
}

double DemoDataGenerator::addNoise(double val, double noiseLevel, double t_sec, bool isPhysiological)
{
    if (isPhysiological) {
        double interference = qSin(2.0 * M_PI * 50.0 * t_sec) * (noiseLevel / 2.0);
        return val + interference;
    } else {
        double noise = (QRandomGenerator::global()->generateDouble() * 2.0 - 1.0) * noiseLevel;
        return val + noise;
    }
}

double DemoDataGenerator::generateECG(double phase, const Parameters &params)
{
    static const double ECG_LUT[256] = {
    0.0000, 0.0005, 0.0010, 0.0015, 0.0020, 0.0025, 0.0029, 0.0034, 0.0039, 0.0044, 0.0049, 0.0054, 0.0058, 0.0063, 0.0068, 0.0072,
    0.0077, 0.0081, 0.0086, 0.0090, 0.0095, 0.0099, 0.0103, 0.0107, 0.0111, 0.0116, 0.0120, 0.0123, 0.0127, 0.0131, 0.0135, 0.0139,
    0.0143, 0.0148, 0.0154, 0.0163, 0.0175, 0.0193, 0.0219, 0.0258, 0.0313, 0.0389, 0.0488, 0.0613, 0.0762, 0.0930, 0.1109, 0.1286,
    0.1447, 0.1576, 0.1660, 0.1690, 0.1673, 0.1621, 0.1537, 0.1427, 0.1299, 0.1160, 0.1019, 0.0881, 0.0753, 0.0638, 0.0538, 0.0455,
    0.0388, 0.0334, 0.0294, 0.0264, 0.0242, 0.0226, 0.0215, 0.0208, 0.0203, 0.0199, 0.0196, 0.0194, 0.0192, 0.0190, 0.0188, 0.0186,
    0.0184, 0.0182, 0.0178, 0.0165, 0.0123, 0.0008, -0.0232, -0.0594, -0.0931, -0.0967, -0.0453, 0.0680, 0.2328, 0.4317, 0.6436, 0.8370,
    0.9720, 1.0139, 0.9506, 0.7986, 0.5923, 0.3670, 0.1494, -0.0357, -0.1587, -0.1992, -0.1672, -0.1021, -0.0430, -0.0070, 0.0092, 0.0149,
    0.0167, 0.0174, 0.0179, 0.0184, 0.0189, 0.0194, 0.0198, 0.0202, 0.0205, 0.0208, 0.0210, 0.0211, 0.0211, 0.0210, 0.0208, 0.0205,
    0.0201, 0.0197, 0.0191, 0.0186, 0.0180, 0.0175, 0.0171, 0.0169, 0.0168, 0.0171, 0.0178, 0.0190, 0.0208, 0.0233, 0.0268, 0.0312,
    0.0368, 0.0436, 0.0519, 0.0616, 0.0728, 0.0856, 0.0999, 0.1157, 0.1329, 0.1512, 0.1704, 0.1903, 0.2104, 0.2303, 0.2497, 0.2681,
    0.2850, 0.3000, 0.3126, 0.3226, 0.3296, 0.3334, 0.3337, 0.3270, 0.3122, 0.2904, 0.2630, 0.2318, 0.1987, 0.1653, 0.1333, 0.1038,
    0.0775, 0.0550, 0.0363, 0.0213, 0.0095, 0.0007, -0.0059, -0.0106, -0.0139, -0.0161, -0.0176, -0.0185, -0.0191, -0.0195, -0.0197, -0.0199,
    -0.0199, -0.0199, -0.0199, -0.0199, -0.0199, -0.0198, -0.0197, -0.0196, -0.0195, -0.0194, -0.0193, -0.0192, -0.0190, -0.0189, -0.0187, -0.0185,
    -0.0183, -0.0181, -0.0179, -0.0177, -0.0174, -0.0172, -0.0169, -0.0167, -0.0164, -0.0161, -0.0158, -0.0155, -0.0152, -0.0149, -0.0145, -0.0142,
    -0.0138, -0.0135, -0.0131, -0.0127, -0.0123, -0.0120, -0.0116, -0.0111, -0.0107, -0.0103, -0.0099, -0.0095, -0.0090, -0.0086, -0.0081, -0.0077,
    -0.0072, -0.0068, -0.0063, -0.0058, -0.0054, -0.0049, -0.0044, -0.0039, -0.0034, -0.0029, -0.0025, -0.0020, -0.0015, -0.0010, -0.0005, -0.0000,
};

    // phase is [0.0, 1.0)
    if (phase < 0.0) phase = 0.0;
    if (phase >= 1.0) phase = 0.9999;
    
    double idx_exact = phase * 255.0;
    int idx_low = static_cast<int>(idx_exact);
    int idx_high = idx_low + 1;
    if (idx_high > 255) idx_high = 255;
    
    double frac = idx_exact - idx_low;
    
    // Linear interpolation
    double val = ECG_LUT[idx_low] * (1.0 - frac) + ECG_LUT[idx_high] * frac;
    
    return val * params.vpp;
}

double DemoDataGenerator::generatePPG(double phase, const Parameters &params)
{
    // PPG composed of systolic peak and diastolic peak (dicrotic notch)
    auto gaussian = [](double x, double a, double b, double c) {
        return a * qExp(-((x - b) * (x - b)) / (2.0 * c * c));
    };

    double sys = gaussian(phase, 1.0, 0.3, 0.1);
    double dia = gaussian(phase, 0.4, 0.55, 0.12);
    
    double val = sys + dia;
    // adding some baseline wander
    val += 0.05 * qSin(2 * M_PI * phase);
    
    return val * params.vpp;
}

double DemoDataGenerator::generateEEG(double t_sec, const Parameters &params)
{
    // EEG is usually a mix of different bands + noise
    // Alpha (8-13Hz), Beta (13-30Hz)
    double alpha = qSin(2.0 * M_PI * 10.0 * t_sec) * 0.5;
    double beta = qSin(2.0 * M_PI * 20.0 * t_sec) * 0.2;
    double theta = qSin(2.0 * M_PI * 5.0 * t_sec) * 0.3;
    
    // Add randomness for complexity
    double noise = (QRandomGenerator::global()->generateDouble() * 2.0 - 1.0) * 0.5;
    
    // Envelope for burst-like behavior in Alpha
    double envelope = qMax(0.1, qSin(2.0 * M_PI * 0.5 * t_sec));
    
    double val = (alpha * envelope) + beta + theta + noise;
    return val * (params.vpp / 2.0);
}

double DemoDataGenerator::generateEMG(double t_sec, const Parameters &params)
{
    // EMG is mostly high frequency bursts on muscle contraction
    double contractionFreq = 0.5; // Contraction every 2 seconds
    double envelope = qMax(0.0, qSin(2.0 * M_PI * contractionFreq * t_sec));
    envelope = qPow(envelope, 4.0); // Make bursts sharper
    
    // High frequency noise (50-150Hz)
    double hf = (QRandomGenerator::global()->generateDouble() * 2.0 - 1.0);
    
    double val = hf * envelope;
    
    // Add small baseline noise
    val += (QRandomGenerator::global()->generateDouble() * 2.0 - 1.0) * 0.05;
    
    return val * params.vpp;
}

double DemoDataGenerator::generateEOG(double t_sec, const Parameters &params)
{
    // EOG contains slow rolling eye movements and sharp blinks
    double blinkFreq = 0.3; // roughly a blink every 3.3 seconds
    double phase = t_sec * blinkFreq - qFloor(t_sec * blinkFreq);
    
    double blink = 0.0;
    if (phase > 0.9) {
        // Model a sharp blink (looks like a fast positive then negative pulse)
        double b_phase = (phase - 0.9) * 10.0; // 0 to 1
        blink = qSin(2.0 * M_PI * b_phase) * qExp(-b_phase * 5.0);
    }
    
    // Slow drift
    double drift = qSin(2.0 * M_PI * 0.1 * t_sec) * 0.2;
    
    double val = blink + drift;
    return val * (params.vpp / 2.0);
}

double DemoDataGenerator::generateCBF(double t_sec, const Parameters &params)
{
    // Cerebral Blood Flow (CBF) waveform
    // Modeled using fundamental heart rate frequency and harmonics to create a skewed pulse
    double theta = 2.0 * M_PI * params.frequency * t_sec;
    
    // Fundamental frequency (e.g. 1.2Hz) + 2nd Harmonic + 3rd Harmonic
    double val = 1.0 * qSin(theta) 
               + 0.35 * qSin(2.0 * theta - 0.5) 
               + 0.15 * qSin(3.0 * theta - 0.2);
               
    // Add a very slight low frequency respiratory wander (e.g. 0.2 Hz)
    double resp = 0.05 * qSin(2.0 * M_PI * 0.2 * t_sec);
    val += resp;
    
    // Normalize amplitude so that peak is around 1.0
    val = val * 0.6; 
    
    return val * (params.vpp / 2.0);
}
