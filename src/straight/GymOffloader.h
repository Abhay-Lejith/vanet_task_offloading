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

    // state
    omnetpp::cMessage* tick = nullptr;
    GymConnection* gymCon = nullptr;
    bool sentShutdown = false; 
    bool busy = false;       
    omnetpp::simtime_t taskStart; 
    double lastReward = 0.0;   

    // helpers
  veins::TraCIMobility* getVehicleMobility(const std::string& id) const;
  std::array<double, 11> computeObservation() const; // [speed, d0,d1,d2, inputMB, busy0,busy1,busy2, ul0,ul1,ul2]
  double computeReward() const; 
  veinsgym::proto::Request serializeObservation(const std::array<double, 11>& observation, double reward) const;
    double estimateBandwidth(double distance) const;  

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
