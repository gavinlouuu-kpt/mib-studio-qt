#pragma once

#include <cstdint>
#include <string>

namespace backend::recording {

// What the RF sort generator (SIGLENT SSG3021X) was set to when the run
// started, read back over SCPI — never assumed from the profile. Stored as
// rf_generator_* attributes on the run-info group (schema version 1) so a
// file says what the sorter actually did per pulse: the TTL edge from the
// grabber reaches TRIG IN, the RF burst starts `triggerDelayS` later and
// lasts `pulseWidthS`. Portable (Qt-free, no service types) so Hdf5Service
// in mib_processing can write it.
struct RfGeneratorProvenance {
    static constexpr uint64_t kSchemaVersion = 1;
    std::string identity;      // raw *IDN? reply
    std::string link;          // transport description ("usb /dev/usbtmc0")
    bool rfOutputOn{false};    // :OUTPut?
    bool pulseModOn{false};    // :PULM:STATe?
    std::string pulseSource;   // :PULM:SOURce?  INTernal|EXTernal
    std::string pulseMode;     // :PULM:MODE?    SINGle|DOUBle|PTRain
    std::string triggerMode;   // :PULM:TRIGger:MODE? AUTO|KEY|EXTernal|EGATe
    std::string triggerSlope;  // :PULM:TRIGger:EXTernal:SLOPe? POSitive|NEGative
    double triggerDelayS{0.0}; // :PULM:DELay?  (external trigger -> first RF pulse)
    double pulseWidthS{0.0};   // :PULM:WIDTh?
    double pulsePeriodS{0.0};  // :PULM:PERiod?
    bool pulseOutOn{false};    // :PULM:OUT:STATe? (PULSE OUT envelope available for loopback)
    double frequencyHz{0.0};   // :FREQuency?
    double powerDbm{0.0};      // :POWer?
    uint64_t sampledHostUs{0}; // Tools::getTimestamp when the readback finished
};

} // namespace backend::recording
