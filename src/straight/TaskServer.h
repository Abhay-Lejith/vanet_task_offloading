#pragma once

#include <omnetpp.h>
#include <unordered_map>
#include <deque>
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
    void finish() override;
    void handleMessage(omnetpp::cMessage* msg) override;

    int getUlActive() const { return ulActive; }
    int getDlActive() const { return dlActive; }
    int getActiveTasks() const { return (int)tasks.size(); }
    bool getExternallyBusy() const { return externalBusy; }
    bool getCpuBusy() const { return cpuBusy; }

  private:
    // Parameters
    double bandwidthHz;     // total system bandwidth per RSU
    double carrierHz;       // carrier frequency
    double noiseFigureDb;   // noise figure in dB
    double txPowerDbmVehicle; // UL Tx power
    double txPowerDbmRsu;     // DL Tx power
    double cpuFreqRsu;      // RSU CPU cycles per second
    bool enableExternalBusy = false;
    double busyOnMean = 0.0;   // mean duration of busy ON state (s)
    double busyOffMean = 0.0;  // mean duration of busy OFF state (s)
    
    // Energy parameters
    double cpuPowerRsu;        // RSU CPU power (W)
    double idlePowerRsu;       // RSU idle power (W)
    double rxPowerRsu;         // RSU receive power (W)
    double staticEnergyPerTask; // Fixed overhead per task (J)
    double totalEnergyConsumed = 0.0; // Cumulative energy tracking (J)

    int ulActive = 0;
    int dlActive = 0;
    enum Kind { UL_COMPLETE = 1001, CPU_COMPLETE = 1002, DL_COMPLETE = 1003, BUSY_TOGGLE = 1100 };

    struct TaskCtx {
        std::string vehicleId;
        int64_t inputBytes = 0;
        int64_t outputBytes = 0;
        int64_t cycles = 0;
        omnetpp::cMessage* ulEvt = nullptr;
        omnetpp::cMessage* cpuEvt = nullptr;
        omnetpp::cMessage* dlEvt = nullptr;
        omnetpp::simtime_t startTime;
        omnetpp::simtime_t ulStartTime;
        omnetpp::simtime_t cpuStartTime;
        omnetpp::simtime_t dlStartTime;
        
        // Energy metrics
        double uplinkEnergy = 0.0;
        double computeEnergy = 0.0;
        double downlinkEnergy = 0.0;
        double totalEnergy = 0.0;
    };

    std::unordered_map<omnetpp::cMessage*, TaskCtx> tasks; 
    std::deque<TaskCtx> cpuQueue;
    bool cpuBusy = false;
    bool externalBusy = false;
    omnetpp::cMessage* busyEvt = nullptr;

    // Helpers
    veins::BaseMobility* getRsuMobility() const;
    veins::TraCIMobility* getVehicleMobility(const std::string& id) const;
    double distanceToVehicle(const std::string& id) const;

    static double dbmToW(double dbm);
    static double noisePowerW(double bandwidthHz, double noiseFigureDb);
    static double friisPathLossLin(double freqHz, double distanceMeters);
    static double shannonRate(double bandwidthHz, double snrLin);
    void tryStartCpu(TaskCtx&& ctx);
    void maybeStartNextCpu();
};
