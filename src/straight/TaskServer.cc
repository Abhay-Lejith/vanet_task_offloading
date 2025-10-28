#include "straight/TaskServer.h"

#include <cmath>

using namespace omnetpp;
using veins::TraCIMobility;
using veins::TraCIMobilityAccess;
using veins::TraCIScenarioManagerAccess;

Define_Module(TaskServer);

void TaskServer::initialize() {
    bandwidthHz = par("bandwidthHz").doubleValue();
    carrierHz = par("carrierHz").doubleValue();
    noiseFigureDb = par("noiseFigureDb").doubleValue();
    txPowerDbmVehicle = par("txPowerDbmVehicle").doubleValue();
    txPowerDbmRsu = par("txPowerDbmRsu").doubleValue();
    cpuFreqRsu = par("cpuFreqRsu").doubleValue();
}

void TaskServer::handleMessage(cMessage* msg) {
    if (auto* req = dynamic_cast<straight::TaskRequest*>(msg)) {
        // New UL starts now; include this flow in sharing
        ulActive++;

        TaskCtx ctx;
        ctx.vehicleId = req->getVehicleId();
        ctx.inputBytes = req->getInputBytes();
        ctx.outputBytes = req->getOutputBytes();
        ctx.cycles = req->getCycles();
        ctx.startTime = simTime();

        // Compute UL SNR and rate (equal share among ulActive)
        double d = distanceToVehicle(ctx.vehicleId);
        double L = friisPathLossLin(carrierHz, d);
        double Pt = dbmToW(txPowerDbmVehicle);
        double N = noisePowerW(bandwidthHz / std::max(1, ulActive), noiseFigureDb); // noise scales with bandwidth share
        double Pr = Pt / L;
        double snr = Pr / N;
        double Beff = bandwidthHz / std::max(1, ulActive);
        double R_ul = shannonRate(Beff, snr); // bits/s
        double t_ul = (8.0 * (double)ctx.inputBytes) / std::max(R_ul, 1e-9);

        ctx.ulEvt = new cMessage("ulComplete", UL_COMPLETE);
        tasks[ctx.ulEvt] = ctx;
        scheduleAt(simTime() + t_ul, ctx.ulEvt);
        delete req; // done with request
        return;
    }

    // Progress existing task via internal events
    auto it = tasks.find(msg);
    if (it == tasks.end()) {
        delete msg;
        return;
    }
    TaskCtx ctx = it->second;
    tasks.erase(it);

    if (msg->getKind() == UL_COMPLETE) {
        // UL finished
        if (ulActive > 0) ulActive--; // release UL share

        // Schedule CPU processing
        double t_cpu = (double)ctx.cycles / cpuFreqRsu;
        ctx.cpuEvt = new cMessage("cpuComplete", CPU_COMPLETE);
        tasks[ctx.cpuEvt] = ctx;
        scheduleAt(simTime() + t_cpu, ctx.cpuEvt);
    }
    else if (msg->getKind() == CPU_COMPLETE) {
        // CPU finished, start DL
        dlActive++;
        double d = distanceToVehicle(ctx.vehicleId);
        double L = friisPathLossLin(carrierHz, d);
        double Pt = dbmToW(txPowerDbmRsu);
        double Beff = bandwidthHz / std::max(1, dlActive);
        double N = noisePowerW(Beff, noiseFigureDb);
        double Pr = Pt / L;
        double snr = Pr / N;
        double R_dl = shannonRate(Beff, snr);
        double t_dl = (8.0 * (double)ctx.outputBytes) / std::max(R_dl, 1e-9);

        ctx.dlEvt = new cMessage("dlComplete", DL_COMPLETE);
        tasks[ctx.dlEvt] = ctx;
        scheduleAt(simTime() + t_dl, ctx.dlEvt);
    }
    else if (msg->getKind() == DL_COMPLETE) {
        if (dlActive > 0) dlActive--; // release DL share
        // Send completion back to vehicle
    auto* done = new straight::TaskDone();
        done->setVehicleId(ctx.vehicleId.c_str());
        done->setInputBytes(ctx.inputBytes);
        done->setOutputBytes(ctx.outputBytes);
        done->setTotalTime((simTime() - ctx.startTime).dbl());
        send(done, "out");
    }

    delete msg;
}

veins::BaseMobility* TaskServer::getRsuMobility() const {
    std::stringstream path;
    // TaskServer is instantiated as rsuServer[index] at top-level
    path << "rsu[" << getIndex() << "].mobility";
    cModule* m = getModuleByPath(path.str().c_str());
    auto* mob = dynamic_cast<veins::BaseMobility*>(m);
    if (!mob) throw cRuntimeError("RSU mobility not found at %s", path.str().c_str());
    return mob;
}

TraCIMobility* TaskServer::getVehicleMobility(const std::string& id) const {
    auto manager = TraCIScenarioManagerAccess().get();
    if (!manager) throw cRuntimeError("TraCIScenarioManager not found");
    const auto& hosts = manager->getManagedHosts();
    auto it = hosts.find(id);
    if (it == hosts.end()) throw cRuntimeError("Vehicle %s not managed yet", id.c_str());
    cModule* host = it->second;
    return TraCIMobilityAccess().get(host);
}

double TaskServer::distanceToVehicle(const std::string& id) const {
    auto* veh = getVehicleMobility(id);
    auto* rsu = getRsuMobility();
    veins::Coord vpos = veh->getPositionAt(simTime());
    veins::Coord rpos = rsu->getPositionAt(simTime());
    return (vpos - rpos).length();
}

double TaskServer::dbmToW(double dbm) {
    return std::pow(10.0, dbm / 10.0) / 1000.0;
}

double TaskServer::noisePowerW(double bandwidthHz_, double noiseFigureDb_) {
    // Thermal noise density: -174 dBm/Hz -> 10^(-174/10) mW/Hz
    const double N0_mW_per_Hz = std::pow(10.0, -174.0 / 10.0);
    double N_mW = N0_mW_per_Hz * bandwidthHz_ * std::pow(10.0, noiseFigureDb_ / 10.0);
    return N_mW / 1000.0; // to Watts
}

double TaskServer::friisPathLossLin(double freqHz_, double dMeters) {
    const double c = 299792458.0; // m/s
    if (dMeters <= 1e-3) return 1.0; // avoid singularity
    double lambda = c / freqHz_;
    double L = std::pow(4.0 * M_PI * dMeters / lambda, 2.0);
    return std::max(L, 1.0);
}

double TaskServer::shannonRate(double bandwidthHz_, double snrLin_) {
    return bandwidthHz_ * std::log2(1.0 + std::max(0.0, snrLin_));
}
