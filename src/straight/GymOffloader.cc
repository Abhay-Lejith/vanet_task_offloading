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
    
    // Energy parameters
    cpuPowerVehicle = par("cpuPowerVehicle").doubleValue();
    txPowerDbmVehicle = par("txPowerDbmVehicle").doubleValue();
    rxPowerVehicle = par("rxPowerVehicle").doubleValue();
    batteryCapacity = par("batteryCapacity").doubleValue();
    idlePowerVehicle = par("idlePowerVehicle").doubleValue();
    rewardAlpha = par("rewardAlpha").doubleValue();
    rewardBeta = par("rewardBeta").doubleValue();
    remainingBattery = batteryCapacity;

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
        lastTaskLatency = (simTime() - taskStart).dbl();
        
        // Calculate vehicle energy for offloading
        double vehicleEnergy = 0.0;
        if (lastAction > 0) {
            // Vehicle energy = UL transmit + DL receive
            int rsuIdx = lastAction - 1;
            double distance = (getVehicleMobility(vehicleId)->getPositionAt(simTime()) - getRsuPositions()[rsuIdx]).length();
            double txPower = dbmToW(txPowerDbmVehicle);
            double t_ul = done->getTotalTime() * 0.4;  // Rough estimate: 40% of time in UL
            double t_dl = done->getTotalTime() * 0.1;  // Rough estimate: 10% of time in DL
            vehicleEnergy = txPower * t_ul + rxPowerVehicle * t_dl;
        }
        
        // Total system energy = vehicle + RSU
        double rsuEnergy = done->getRsuTotalEnergy();
        lastTaskEnergy = vehicleEnergy + rsuEnergy;
        
        // Update battery and counters
        remainingBattery -= vehicleEnergy;
        totalEnergyConsumed += lastTaskEnergy;
        taskCounter++;
        
        // Compute balanced reward: normalize both metrics to [0,1] then combine
        // Latency normalized: lower latency → higher score
        double maxLatency = 2.0;  // Typical max latency (s)
        double latencyScore = std::max(0.0, 1.0 - (lastTaskLatency / maxLatency));
        
        // Energy normalized: lower energy → higher score
        double maxEnergy = 50.0;  // Typical max energy per task (J)
        double energyScore = std::max(0.0, 1.0 - (lastTaskEnergy / maxEnergy));
        
        // Balanced combination: equal weight by default
        lastReward = rewardAlpha * latencyScore + rewardBeta * energyScore;
        
        EV_INFO << "Task completed for vehicle '" << done->getVehicleId() 
                << "' latency=" << lastTaskLatency << "s (score=" << latencyScore 
                << "), energy=" << lastTaskEnergy << "J (score=" << energyScore 
                << "), reward=" << lastReward << "\n";
        delete done;
        // Reschedule next tick (cancel if already scheduled)
        if (tick) cancelEvent(tick);
        scheduleAt(simTime() + pollInterval, tick);
        return;
    }
    if (msg->isSelfMessage() && msg != tick) {
        // Local processing done
        busy = false;
        lastTaskLatency = (simTime() - taskStart).dbl();
        
        // Calculate local processing energy
        lastTaskEnergy = cpuPowerVehicle * lastTaskLatency;
        
        // Update battery and counters
        remainingBattery -= lastTaskEnergy;
        totalEnergyConsumed += lastTaskEnergy;
        taskCounter++;
        
        // Compute balanced reward: normalize both metrics to [0,1] then combine
        // Latency normalized: lower latency → higher score
        double maxLatency = 2.0;  // Typical max latency (s)
        double latencyScore = std::max(0.0, 1.0 - (lastTaskLatency / maxLatency));
        
        // Energy normalized: lower energy → higher score
        double maxEnergy = 50.0;  // Typical max energy per task (J)
        double energyScore = std::max(0.0, 1.0 - (lastTaskEnergy / maxEnergy));
        
        // Balanced combination: equal weight by default
        lastReward = rewardAlpha * latencyScore + rewardBeta * energyScore;
        
        EV_INFO << "Local task completed latency=" << lastTaskLatency 
                << "s (score=" << latencyScore << "), energy=" << lastTaskEnergy 
                << "J (score=" << energyScore << "), reward=" << lastReward << "\n";
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

    // If a task is in progress, do NOT communicate with the agent.
    // This ensures env.step(action) only returns once the task completes
    // and the reward for that action is available.
    if (busy) {
        EV_INFO << "Task in progress; deferring agent communication until completion" << "\n";
        scheduleAt(simTime() + pollInterval, tick);
        return;
    }

    if (!busy && !hasPendingTask) {
        double inputMB = uniform(taskMinMB, taskMaxMB);
        pendingInputBytes = (int64_t) std::llround(inputMB * 1e6);
        pendingOutputBytes = (int64_t) std::llround(outputFactor * pendingInputBytes);
        pendingCycles = (int64_t) std::llround(cyclesPerByte * (double)pendingInputBytes);
        hasPendingTask = true;
        EV_INFO << "Sampled new pending task: inputMB=" << inputMB
                << " cycles=" << pendingCycles << "\n";
    }

    // Compute observation
    std::array<double, 14> obs;
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
    int action = 0;
    try {
        auto reply = gymCon->communicate(request);
        action = reply.action().discrete().value();
    } catch (const std::exception& e) {
        EV_WARN << "Gym communicate failed (" << e.what() << "), will retry next tick\n";
        scheduleAt(simTime() + pollInterval, tick);
        return;
    } catch (...) {
        EV_WARN << "Gym communicate failed with unknown error, will retry next tick\n";
        scheduleAt(simTime() + pollInterval, tick);
        return;
    }
    EV_INFO << "RL action received: " << action << " (0=no offload, 1=RSU0, 2=RSU1, 3=RSU2)\n";

    // If not currently processing a task but have a pending one, execute based on action
    if (!busy && hasPendingTask) {
        taskStart = simTime();
        busy = true;
        lastAction = action;

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

    // While the task is running, we avoid further agent communications.
    // The next communication (and reward delivery) will occur after completion.
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

std::array<double, 14> GymOffloader::computeObservation() const {
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

        // Busy if external busy or any active task exists (UL/CPU/DL)
        bool b = server->getExternallyBusy() || (server->getActiveTasks() > 0) || server->getCpuBusy();
        busy[i] = b ? 1.0 : 0.0;

        // Compute expected UL rate if a new UL starts now
        double bandwidthHz = server->par("bandwidthHz").doubleValue();
        double carrierHz = server->par("carrierHz").doubleValue();
        double noiseFigureDb = server->par("noiseFigureDb").doubleValue();
        double txPowerDbmVehicle = server->par("txPowerDbmVehicle").doubleValue();

        int shareDiv = std::max(1, server->getUlActive() + 1);
        double Beff = bandwidthHz / (double)shareDiv;
        double L = friisPathLossLin(carrierHz, d[i]);
        double Pt = dbmToW(txPowerDbmVehicle);
        double N = noisePowerW(Beff, noiseFigureDb);
        double Pr = Pt / L;
        double snr = Pr / std::max(N, 1e-18);
        double R_ul = shannonRate(Beff, snr); // bits/s
        ulMbps[i] = std::max(0.0, R_ul / 1e6);
    }
    
    // Battery level (normalized 0-1)
    double batteryLevel = remainingBattery / batteryCapacity;
    
    // Estimate energy for local processing (normalized by dividing by 100)
    double energyLocal = 0.0;
    if (hasPendingTask) {
        double t_local = (double)pendingCycles / cpuFreqVehicle;
        energyLocal = cpuPowerVehicle * t_local / 100.0;  // Normalized
    }
    
    // Estimate energy for offloading to nearest RSU (normalized)
    double energyOffload = 0.0;
    if (hasPendingTask) {
        int nearestRsu = (d[0] < d[1] && d[0] < d[2]) ? 0 : (d[1] < d[2] ? 1 : 2);
        double distance = d[nearestRsu];
        double txPower = dbmToW(txPowerDbmVehicle);
        double R_ul = ulMbps[nearestRsu] * 1e6;  // Convert Mbps to bps
        if (R_ul > 0) {
            double t_ul = (8.0 * pendingInputBytes) / R_ul;
            double t_dl = (8.0 * pendingOutputBytes) / R_ul;
            double vehicleEnergy = txPower * t_ul + rxPowerVehicle * t_dl;
            double rsuEnergy = 100.0 * ((double)pendingCycles / 10e9);  // Rough RSU estimate
            energyOffload = (vehicleEnergy + rsuEnergy) / 100.0;  // Normalized
        }
    }

    return {speed, d[0], d[1], d[2], inputMB, busy[0], busy[1], busy[2], 
            ulMbps[0], ulMbps[1], ulMbps[2], batteryLevel, energyLocal, energyOffload};
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

veinsgym::proto::Request GymOffloader::serializeObservation(const std::array<double, 14>& observation, double reward) const {
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
