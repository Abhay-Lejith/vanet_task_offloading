#pragma once

#include <omnetpp.h>
#include <array>
#include <string>

#include "veins/base/modules/BaseMobility.h"
#include "veins/modules/mobility/traci/TraCIMobility.h"
#include "veins/modules/mobility/traci/TraCIScenarioManager.h"
#include "veins/base/utils/FindModule.h"

#include "serpentine/GymConnection.h"
#include "protobuf/veinsgym.pb.h"
#include "straight/TaskMsg_m.h"
#include "straight/TaskServer.h"

class GymOffloader : public omnetpp::cSimpleModule {
  public:
    GymOffloader() = default;
    virtual ~GymOffloader();

    void initialize() override;
    void finish() override;
    void handleMessage(omnetpp::cMessage* msg) override;

  private:
    // parameters
    std::string vehicleId;
    double pollInterval = 0.1; 
    double cpuFreqVehicle = 0.6e9; 
    double cyclesPerByte = 1900.0;
    double taskMinMB = 10.0;
    double taskMaxMB = 20.0;
    double outputFactor = 0.2;
    
    // Energy parameters
    double cpuPowerVehicle = 2.0;       // Vehicle CPU power (W)
    double txPowerDbmVehicle = 24.0;    // Vehicle transmit power (dBm)
    double rxPowerVehicle = 0.3;        // Vehicle receive power (W)
    double batteryCapacity = 3600.0;    // Battery capacity (J)
    double idlePowerVehicle = 0.5;      // Idle power drain (W)
    double rewardAlpha = 1.0;           // Latency weight in reward
    double rewardBeta = 0.001;          // Energy weight in reward

    // state
    omnetpp::cMessage* tick = nullptr;
    GymConnection* gymCon = nullptr;
    bool sentShutdown = false; 
    bool busy = false;       
    omnetpp::simtime_t taskStart; 
    double lastReward = 0.0;
    int lastAction = 0;                 // Last action taken
    double lastTaskEnergy = 0.0;        // Energy consumed by last task (J)
    double lastTaskLatency = 0.0;       // Latency of last task (s)
    double remainingBattery = 3600.0;   // Remaining battery (J)
    double totalEnergyConsumed = 0.0;   // Cumulative energy (J)
    int taskCounter = 0;                // Total tasks completed   

    // helpers
  veins::TraCIMobility* getVehicleMobility(const std::string& id) const;
  std::array<double, 14> computeObservation() const; // [speed, d0,d1,d2, inputMB, busy0,busy1,busy2, ul0,ul1,ul2, battery, energyLocal, energyRsu]
  double computeReward() const; 
  veinsgym::proto::Request serializeObservation(const std::array<double, 14>& observation, double reward) const;
    double estimateBandwidth(double distance) const;
    double computeEnergyForAction(int action, int64_t inputBytes, int64_t outputBytes, int64_t cycles, int rsuIdx) const;  

    std::array<veins::Coord, 3> getRsuPositions() const;

  bool hasPendingTask = false;
  int64_t pendingInputBytes = 0;
  int64_t pendingOutputBytes = 0;
  int64_t pendingCycles = 0;

    static double dbmToW(double dbm);
    static double noisePowerW(double bandwidthHz, double noiseFigureDb);
    static double friisPathLossLin(double freqHz, double distanceMeters);
    static double shannonRate(double bandwidthHz, double snrLin);
};
