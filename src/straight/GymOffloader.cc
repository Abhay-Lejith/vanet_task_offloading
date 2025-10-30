#include "straight/GymOffloader.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>

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
    cpuFreqVehicle = par("cpuFreqVehicle").doubleValue();
    cyclesPerByte = par("cyclesPerByte").doubleValue();
    taskMinMB = par("taskMinMB").doubleValue();
    taskMaxMB = par("taskMaxMB").doubleValue();
    outputFactor = par("outputFactor").doubleValue();

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
    // Handle task completion messages from RSU or local processing
    if (auto* done = dynamic_cast<straight::TaskDone*>(msg)) {
        busy = false;
        lastReward = 1.0 / std::max(1e-12, (simTime() - taskStart).dbl());
        EV_INFO << "Task completed for vehicle '" << done->getVehicleId() << "' totalTime=" << (simTime() - taskStart) << "s, reward=" << lastReward << "\n";
        delete done;
        // Reschedule next tick (cancel if already scheduled)
        if (tick) cancelEvent(tick);
        scheduleAt(simTime() + pollInterval, tick);
        return;
    }
    if (msg->isSelfMessage() && msg != tick) {
        // Local processing done
        busy = false;
        lastReward = 1.0 / std::max(1e-12, (simTime() - taskStart).dbl());
        EV_INFO << "Local task completed totalTime=" << (simTime() - taskStart) << "s, reward=" << lastReward << "\n";
        delete msg;
        if (tick) cancelEvent(tick);
        scheduleAt(simTime() + pollInterval, tick);
        return;
    }
    if (msg != tick) { delete msg; return; }

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

    // If idle and no pending task, sample one now so the agent decides based on it
    if (!busy && !hasPendingTask) {
        double inputMB = uniform(taskMinMB, taskMaxMB);
        pendingInputBytes = (int64_t) std::llround(inputMB * 1e6);
        pendingOutputBytes = (int64_t) std::llround(outputFactor * pendingInputBytes);
        pendingCycles = (int64_t) std::llround(cyclesPerByte * (double)pendingInputBytes);
        hasPendingTask = true;
        EV_INFO << "Sampled new pending task: inputMB=" << inputMB
                << " cycles=" << pendingCycles << "\n";
    }

    // Compute observation (includes task features if pending)
    std::array<double, 11> obs;
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

    // Log observation being sent to the agent (compact formatting)
    {
        std::ostringstream os;
        os.setf(std::ios::fixed);
        os << "[" << std::setprecision(2)
           << obs[0] << ", " << obs[1] << ", " << obs[2] << ", " << obs[3]
           << ", " << obs[4] << ", " << obs[5] << ", " << obs[6] << "]";
        EV_INFO << "Sending observation to agent: obs=" << os.str()
                << ", prevReward=" << std::setprecision(6) << reward << "\n";
    }

    auto request = serializeObservation(obs, reward);
    auto reply = gymCon->communicate(request);
    int action = reply.action().discrete().value();
    EV_INFO << "RL action received: " << action << " (0=no offload, 1=RSU0, 2=RSU1, 3=RSU2)\n";

    // If not currently processing a task but have a pending one, execute based on action
    if (!busy && hasPendingTask) {
        taskStart = simTime();
        busy = true;

        if (action == 0) {
            // Local processing for pending task
            double t_cpu = (double)pendingCycles / cpuFreqVehicle;
            cMessage* localDone = new cMessage("localDone");
            scheduleAt(simTime() + t_cpu, localDone);
            EV_INFO << "Starting LOCAL compute: inputBytes=" << pendingInputBytes << " cycles=" << pendingCycles << " t_cpu=" << t_cpu << "s\n";
        } else {
            int rsuIdx = action - 1;
            if (rsuIdx < 0 || rsuIdx >= 3) {
                EV_WARN << "Invalid action " << action << ", defaulting to local processing\n";
                double t_cpu = (double)pendingCycles / cpuFreqVehicle;
                cMessage* localDone = new cMessage("localDone");
                scheduleAt(simTime() + t_cpu, localDone);
            } else {
                auto* req = new straight::TaskRequest();
                req->setVehicleId(vehicleId.c_str());
                req->setInputBytes(pendingInputBytes);
                req->setOutputBytes(pendingOutputBytes);
                req->setCycles(pendingCycles);
                send(req, "out", rsuIdx);
                EV_INFO << "Offloading to RSU[" << rsuIdx << "]: input=" << pendingInputBytes << "B output=" << pendingOutputBytes << "B cycles=" << pendingCycles << "\n";
            }
        }
        // consume pending
        hasPendingTask = false;
    }

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

std::array<double, 11> GymOffloader::computeObservation() const {
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

    // task feature: input size [MB]
    double inputMB = hasPendingTask ? (double)pendingInputBytes / 1e6 : 0.0;

    // RSU busy flags and UL rates (Mbps) accounting for sharing if we start a new UL now)
    std::array<double, 3> busy{};
    std::array<double, 3> ulMbps{};
    for (int i = 0; i < 3; ++i) {
        std::stringstream serverPath;
        serverPath << "rsuServer[" << i << "]";
        cModule* m = getModuleByPath(serverPath.str().c_str());
        auto* server = dynamic_cast<TaskServer*>(m);
        if (!server) throw cRuntimeError("TaskServer not found at %s", serverPath.str().c_str());

        // Busy if any active task exists (UL/CPU/DL)
        busy[i] = server->getActiveTasks() > 0 ? 1.0 : 0.0;

        // Compute expected UL rate if a new UL starts now
        double bandwidthHz = server->par("bandwidthHz").doubleValue();
        double carrierHz = server->par("carrierHz").doubleValue();
        double noiseFigureDb = server->par("noiseFigureDb").doubleValue();
        double txPowerDbmVehicle = server->par("txPowerDbmVehicle").doubleValue();

        int shareDiv = std::max(1, server->getUlActive() + 1); // include this flow
        double Beff = bandwidthHz / (double)shareDiv;
        double L = friisPathLossLin(carrierHz, d[i]);
        double Pt = dbmToW(txPowerDbmVehicle);
        double N = noisePowerW(Beff, noiseFigureDb);
        double Pr = Pt / L;
        double snr = Pr / std::max(N, 1e-18);
        double R_ul = shannonRate(Beff, snr); // bits/s
        ulMbps[i] = std::max(0.0, R_ul / 1e6);
    }

    return {speed, d[0], d[1], d[2], inputMB, busy[0], busy[1], busy[2], ulMbps[0], ulMbps[1], ulMbps[2]};
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
    // Return lastReward once, then reset to 0 for subsequent steps
    double r = lastReward;
    const_cast<GymOffloader*>(this)->lastReward = 0.0;
    return r;
}

veinsgym::proto::Request GymOffloader::serializeObservation(const std::array<double, 11>& observation, double reward) const {
    veinsgym::proto::Request request;
    request.set_id(1);
    auto* values = request.mutable_step()->mutable_observation()->mutable_box()->mutable_values();
    values->Reserve(observation.size());
    for (double v : observation) values->Add(v);
    request.mutable_step()->mutable_reward()->mutable_box()->mutable_values()->Add();
    request.mutable_step()->mutable_reward()->mutable_box()->set_values(0, reward);
    return request;
}

// Helpers duplicated from TaskServer to keep consistent PHY math
double GymOffloader::dbmToW(double dbm) { return std::pow(10.0, dbm / 10.0) / 1000.0; }
double GymOffloader::noisePowerW(double bandwidthHz, double noiseFigureDb) {
    const double N0_mW_per_Hz = std::pow(10.0, -174.0 / 10.0);
    double N_mW = N0_mW_per_Hz * bandwidthHz * std::pow(10.0, noiseFigureDb / 10.0);
    return N_mW / 1000.0;
}
double GymOffloader::friisPathLossLin(double freqHz, double dMeters) {
    const double c = 299792458.0;
    if (dMeters <= 1e-3) return 1.0;
    double lambda = c / freqHz;
    double L = std::pow(4.0 * M_PI * dMeters / lambda, 2.0);
    return std::max(L, 1.0);
}
double GymOffloader::shannonRate(double bandwidthHz, double snrLin) {
    return bandwidthHz * std::log2(1.0 + std::max(0.0, snrLin));
}
