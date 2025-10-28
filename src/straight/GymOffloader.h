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
    double pollInterval = 0.1; // seconds

    // state
    omnetpp::cMessage* tick = nullptr;
    GymConnection* gymCon = nullptr;
    bool sentShutdown = false; // ensure we only try to shutdown once

    // helpers
  veins::TraCIMobility* getVehicleMobility(const std::string& id) const;
  std::array<double, 7> computeObservation() const; // [speed, d0,d1,d2, bw0,bw1,bw2]
  double computeReward() const; // placeholder reward (0.0)
  veinsgym::proto::Request serializeObservation(const std::array<double, 7>& observation, double reward) const;
    double estimateBandwidth(double distance) const;   // simple path-loss like estimate

    // fetch RSU positions from their mobility modules
    std::array<veins::Coord, 3> getRsuPositions() const;
};
