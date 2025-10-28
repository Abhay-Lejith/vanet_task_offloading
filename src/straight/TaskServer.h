#pragma once

#include <omnetpp.h>
#include <unordered_map>
#include <string>

#include "veins/base/modules/BaseMobility.h"
#include "veins/modules/mobility/traci/TraCIMobility.h"
#include "veins/modules/mobility/traci/TraCIScenarioManager.h"
#include "veins/base/utils/FindModule.h"

#include "straight/TaskMsg_m.h"

class TaskServer : public omnetpp::cSimpleModule {
  public:
    TaskServer() = default;
    ~TaskServer() override = default;

    void initialize() override;
    void handleMessage(omnetpp::cMessage* msg) override;

  private:
    // Parameters
    double bandwidthHz;     // total system bandwidth per RSU
    double carrierHz;       // carrier frequency
    double noiseFigureDb;   // noise figure in dB
    double txPowerDbmVehicle; // UL Tx power
    double txPowerDbmRsu;     // DL Tx power
    double cpuFreqRsu;      // RSU CPU cycles per second

    // Active flow counters for bandwidth sharing
    int ulActive = 0;
    int dlActive = 0;

    // Internal message kinds
    enum Kind { UL_COMPLETE = 1001, CPU_COMPLETE = 1002, DL_COMPLETE = 1003 };

    struct TaskCtx {
        std::string vehicleId;
        int64_t inputBytes = 0;
        int64_t outputBytes = 0;
        int64_t cycles = 0;
        omnetpp::cMessage* ulEvt = nullptr;
        omnetpp::cMessage* cpuEvt = nullptr;
        omnetpp::cMessage* dlEvt = nullptr;
        omnetpp::simtime_t startTime;
    };

    std::unordered_map<omnetpp::cMessage*, TaskCtx> tasks; // keyed by current event

    // Helpers
    veins::BaseMobility* getRsuMobility() const;
    veins::TraCIMobility* getVehicleMobility(const std::string& id) const;
    double distanceToVehicle(const std::string& id) const;

    static double dbmToW(double dbm);
    static double noisePowerW(double bandwidthHz, double noiseFigureDb);
    static double friisPathLossLin(double freqHz, double distanceMeters);
    static double shannonRate(double bandwidthHz, double snrLin);
};
