#ifndef DEMODATAGENERATOR_H
#define DEMODATAGENERATOR_H

#include <QString>
#include <QVector>

class DemoDataGenerator
{
public:
    enum WaveType {
        Sine = 0,
        Triangle,
        Square,
        Pulse,
        ECG,
        EMG,
        EEG,
        EOG,
        PPG,
        CBF
    };

    struct Parameters {
        double frequency = 1.0;     // Hz
        double vpp = 1.0;           // Peak-to-Peak Amplitude
        double offset = 0.0;        // DC Offset
        double dutyCycle = 50.0;    // Percentage 0-100 (For Square)
        double pulseWidth = 10.0;   // ms (For Pulse)
        double noiseLevel = 0.0;    // Random noise amplitude
    };

    static double generate(WaveType type, double t_sec, const Parameters &params);
    
private:
    static double generateECG(double t_sec, const Parameters &params);
    static double generatePPG(double t_sec, const Parameters &params);
    static double generateEEG(double t_sec, const Parameters &params);
    static double generateEMG(double t_sec, const Parameters &params);
    static double generateEOG(double t_sec, const Parameters &params);
    static double generateCBF(double t_sec, const Parameters &params);
    static double addNoise(double val, double noiseLevel, double t_sec, bool isPhysiological);
};

#endif // DEMODATAGENERATOR_H
