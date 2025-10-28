#include "straight/GymOffloader.h"

#include <algorithm>
#include <cmath>
#include <sstream>

using namespace omnetpp;
using veins::TraCIMobility;
using veins::TraCIMobilityAccess;
using veins::TraCIScenarioManagerAccess;

Define_Module(GymOffloader);

GymOffloader::~GymOffloader() {
    // Ensure timer is cleaned up
    if (tick) {
        try { cancelAndDelete(tick); } catch (...) {}
        tick = nullptr;
    }
    // Best-effort shutdown; swallow any ZMQ errors during teardown
    if (gymCon && !sentShutdown) {
        try {
            veinsgym::proto::Request request;
            request.set_id(1);
            *(request.mutable_shutdown()) = {};
            (void)gymCon->communicate(request);
            sentShutdown = true;
        } catch (const std::exception& e) {
            EV_WARN << "Ignoring exception during GymOffloader destructor shutdown: " << e.what() << "\n";
        } catch (...) {
            EV_WARN << "Ignoring unknown exception during GymOffloader destructor shutdown" << "\n";
        }
    }
}

void GymOffloader::initialize() {
    vehicleId = par("vehicleId").stdstringValue();
    pollInterval = par("pollInterval").doubleValue();

    // find global GymConnection module (existing serpentine.GymConnection)
    gymCon = veins::FindModule<GymConnection*>::findGlobalModule();
    if (!gymCon) throw cRuntimeError("GymConnection module not found. Ensure 'gym_connection: GymConnection {}' exists in the network.");

    tick = new cMessage("tick");
    scheduleAt(simTime() + pollInterval, tick);
}

void GymOffloader::finish() {
    // Cancel future polling first
    if (tick) {
        try { cancelAndDelete(tick); } catch (...) {}
        tick = nullptr;
    }
    // Send a single Shutdown while sockets are still likely valid
    if (gymCon && !sentShutdown) {
        try {
            veinsgym::proto::Request request;
            request.set_id(1);
            *(request.mutable_shutdown()) = {};
            (void)gymCon->communicate(request);
            sentShutdown = true;
        } catch (const std::exception& e) {
            EV_WARN << "Ignoring exception during GymOffloader finish() shutdown: " << e.what() << "\n";
        } catch (...) {
            EV_WARN << "Ignoring unknown exception during GymOffloader finish() shutdown" << "\n";
        }
    }
}

void GymOffloader::handleMessage(cMessage* msg) {
    if (msg != tick) {
        delete msg;
        return;
    }

    // Ensure TraCI and scenario objects are ready before computing observations
    auto manager = TraCIScenarioManagerAccess().get();
    if (!manager || !manager->isUsable()) {
        EV_INFO << "TraCI manager not ready yet, retrying in " << pollInterval << "s\n";
        scheduleAt(simTime() + pollInterval, tick);
        return;
    }

    const auto& hosts = manager->getManagedHosts();
    if (hosts.find(vehicleId) == hosts.end()) {
        EV_INFO << "Vehicle '" << vehicleId << "' not managed yet, retrying in " << pollInterval << "s\n";
        scheduleAt(simTime() + pollInterval, tick);
        return;
    }

    // Check RSU mobility modules exist
    for (int i = 0; i < 3; ++i) {
        std::stringstream path;
        path << "rsu[" << i << "].mobility";
        cModule* m = getModuleByPath(path.str().c_str());
        if (!m) {
            EV_INFO << "RSU mobility module '" << path.str() << "' not found yet, retrying in " << pollInterval << "s\n";
            scheduleAt(simTime() + pollInterval, tick);
            return;
        }
    }

    // Compute observation and send step to Gym
    std::array<double, 7> obs;
    try {
        obs = computeObservation();
    } catch (const cRuntimeError& e) {
        // Likely TraCI has not yet created the managed hosts/RSUs; try again shortly
        EV_WARN << "Offloader not ready yet (" << e.what() << "), retrying in " << pollInterval << "s\n";
        scheduleAt(simTime() + pollInterval, tick);
        return;
    }

    // Validate observation contains only finite numbers to satisfy Gym Box.contains
    for (double v : obs) {
        if (!std::isfinite(v)) {
            EV_WARN << "Computed observation contains non-finite value (" << v << "), retrying in " << pollInterval << "s\n";
            scheduleAt(simTime() + pollInterval, tick);
            return;
        }
    }

    double reward = computeReward();
    auto request = serializeObservation(obs, reward);
    auto reply = gymCon->communicate(request);
    int action = reply.action().discrete().value();
    EV_INFO << "RL action received: " << action << " (0=no offload, 1=RSU0, 2=RSU1, 3=RSU2)\n";

    // TODO: enact task offloading based on action (e.g., send message to RSU), not implemented yet

    scheduleAt(simTime() + pollInterval, tick);
}

TraCIMobility* GymOffloader::getVehicleMobility(const std::string& id) const {
    auto manager = TraCIScenarioManagerAccess().get();
    if (!manager) throw cRuntimeError("TraCIScenarioManager not found");
    const auto& hosts = manager->getManagedHosts();
    auto it = hosts.find(id);
    if (it == hosts.end()) throw cRuntimeError("Vehicle with externalId '%s' not managed yet", id.c_str());
    cModule* host = it->second;
    return TraCIMobilityAccess().get(host);
}

std::array<veins::Coord, 3> GymOffloader::getRsuPositions() const {
    std::array<veins::Coord, 3> pos{};
    cSimulation* sim = getSimulation();
    for (int i = 0; i < 3; ++i) {
        std::stringstream path;
        path << "rsu[" << i << "].mobility";
        cModule* m = getModuleByPath(path.str().c_str());
        if (!m) throw cRuntimeError("RSU mobility module '%s' not found", path.str().c_str());
    auto* mob = dynamic_cast<veins::BaseMobility*>(m);
    if (!mob) throw cRuntimeError("RSU mobility module is not BaseMobility at '%s'", path.str().c_str());
        pos[i] = mob->getPositionAt(simTime());
    }
    return pos;
}

std::array<double, 7> GymOffloader::computeObservation() const {
    // get ego vehicle mobility
    auto* ego = getVehicleMobility(vehicleId);
    const auto egoPos = ego->getPositionAt(simTime());
    const double speed = ego->getSpeed(); // m/s

    // distances to three RSUs
    auto rsuPos = getRsuPositions();
    std::array<double, 3> d{};
    for (int i = 0; i < 3; ++i) {
        d[i] = (egoPos - rsuPos[i]).length();
    }

    // simple bandwidth estimates (Mbps) as a decaying function of distance
    std::array<double, 3> bw{};
    for (int i = 0; i < 3; ++i) bw[i] = estimateBandwidth(d[i]);

    return {speed, d[0], d[1], d[2], bw[0], bw[1], bw[2]};
}

double GymOffloader::estimateBandwidth(double distance) const {
    // Rough placeholder: base 6 Mbps with exponential decay over 300 m scale
    const double base = 6.0; // Mbps
    const double scale = 300.0; // meters
    double val = base * std::exp(-distance / scale);
    // clamp to small floor
    return std::max(0.05, val);
}

double GymOffloader::computeReward() const {
    // Placeholder reward identical in spirit to GymSplitter's ability to compute reward,
    // but returning 0.0 until a proper task/offload signal is wired.
    return 0.0;
}

veinsgym::proto::Request GymOffloader::serializeObservation(const std::array<double, 7>& observation, double reward) const {
    veinsgym::proto::Request request;
    request.set_id(1);
    auto* values = request.mutable_step()->mutable_observation()->mutable_box()->mutable_values();
    values->Reserve(observation.size());
    for (double v : observation) values->Add(v);
    request.mutable_step()->mutable_reward()->mutable_box()->mutable_values()->Add();
    request.mutable_step()->mutable_reward()->mutable_box()->set_values(0, reward);
    return request;
}
