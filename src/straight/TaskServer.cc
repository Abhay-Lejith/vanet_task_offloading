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
    enableExternalBusy = par("enableExternalBusy").boolValue();
    busyOnMean = par("busyOnMean").doubleValue();
    busyOffMean = par("busyOffMean").doubleValue();
    
    // Energy parameters
    cpuPowerRsu = par("cpuPowerRsu").doubleValue();
    idlePowerRsu = par("idlePowerRsu").doubleValue();
    rxPowerRsu = par("rxPowerRsu").doubleValue();
    staticEnergyPerTask = par("staticEnergyPerTask").doubleValue();
    totalEnergyConsumed = 0.0;

    if (enableExternalBusy && (busyOnMean > 0.0 || busyOffMean > 0.0)) {
        busyEvt = new cMessage("busyToggle", BUSY_TOGGLE);
        // start from OFF state, schedule first ON
        externalBusy = false;
        simtime_t dt = busyOffMean > 0.0 ? exponential(busyOffMean) : 0.0;
        scheduleAt(simTime() + dt, busyEvt);
    }
}

void TaskServer::handleMessage(cMessage* msg) {
    if (msg->isSelfMessage() && msg->getKind() == BUSY_TOGGLE) {
        // Toggle external busy state and schedule next toggle
        externalBusy = !externalBusy;
        if (!externalBusy) {
            // Just became idle: try to start next CPU task if any
            maybeStartNextCpu();
        }
        simtime_t dt;
        if (externalBusy) {
            dt = busyOnMean > 0.0 ? exponential(busyOnMean) : 0.0;
        } else {
            dt = busyOffMean > 0.0 ? exponential(busyOffMean) : 0.0;
        }
        scheduleAt(simTime() + dt, msg);
        return;
    }

    if (auto* req = dynamic_cast<straight::TaskRequest*>(msg)) {

        ulActive++;

        TaskCtx ctx;
        ctx.vehicleId = req->getVehicleId();
        ctx.inputBytes = req->getInputBytes();
        ctx.outputBytes = req->getOutputBytes();
        ctx.cycles = req->getCycles();
        ctx.startTime = simTime();
        ctx.ulStartTime = simTime();

        double d = distanceToVehicle(ctx.vehicleId);
        double L = friisPathLossLin(carrierHz, d);
        double Pt = dbmToW(txPowerDbmVehicle);
        double N = noisePowerW(bandwidthHz / std::max(1, ulActive), noiseFigureDb); 
        double Pr = Pt / L;
        double snr = Pr / N;
        double Beff = bandwidthHz / std::max(1, ulActive);
        double R_ul = shannonRate(Beff, snr); 
        double t_ul = (8.0 * (double)ctx.inputBytes) / std::max(R_ul, 1e-9);

        ctx.ulEvt = new cMessage("ulComplete", UL_COMPLETE);
        tasks[ctx.ulEvt] = ctx;
        scheduleAt(simTime() + t_ul, ctx.ulEvt);
        delete req; 
        return;
    }

    auto it = tasks.find(msg);
    if (it == tasks.end()) {
        delete msg;
        return;
    }
    TaskCtx ctx = it->second;
    tasks.erase(it);

    if (msg->getKind() == UL_COMPLETE) {

        if (ulActive > 0) ulActive--; 
        
        // Calculate uplink energy (RSU receiving)
        double t_ul = (simTime() - ctx.ulStartTime).dbl();
        ctx.uplinkEnergy = rxPowerRsu * t_ul;
        ctx.cpuStartTime = simTime();

        // Either start CPU immediately or enqueue if external busy or CPU already busy
        tryStartCpu(std::move(ctx));
    }
    else if (msg->getKind() == CPU_COMPLETE) {
        // CPU stage finished; mark CPU idle and maybe start next before DL
        cpuBusy = false;
        maybeStartNextCpu();
        
        // Calculate compute energy
        double t_cpu = (simTime() - ctx.cpuStartTime).dbl();
        ctx.computeEnergy = cpuPowerRsu * t_cpu;
        ctx.dlStartTime = simTime();

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
        if (dlActive > 0) dlActive--; 
        
        // Calculate downlink energy (RSU transmitting)
        double t_dl = (simTime() - ctx.dlStartTime).dbl();
        double txPower = dbmToW(txPowerDbmRsu);
        ctx.downlinkEnergy = txPower * t_dl;
        
        // Calculate total RSU energy for this task
        ctx.totalEnergy = ctx.uplinkEnergy + ctx.computeEnergy + 
                         ctx.downlinkEnergy + staticEnergyPerTask;
        totalEnergyConsumed += ctx.totalEnergy;

        auto* done = new straight::TaskDone();
        done->setVehicleId(ctx.vehicleId.c_str());
        done->setInputBytes(ctx.inputBytes);
        done->setOutputBytes(ctx.outputBytes);
        done->setTotalTime((simTime() - ctx.startTime).dbl());
        
        // Add energy metrics
        done->setRsuUplinkEnergy(ctx.uplinkEnergy);
        done->setRsuComputeEnergy(ctx.computeEnergy);
        done->setRsuDownlinkEnergy(ctx.downlinkEnergy);
        done->setRsuTotalEnergy(ctx.totalEnergy);
        
        send(done, "out");
    }

    delete msg;
}

void TaskServer::finish() {
    if (busyEvt) {
        try { cancelAndDelete(busyEvt); } catch (...) {}
        busyEvt = nullptr;
    }
    
    // Record energy statistics
    recordScalar("totalEnergyConsumed", totalEnergyConsumed);
}

veins::BaseMobility* TaskServer::getRsuMobility() const {
    std::stringstream path;
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
    const double N0_mW_per_Hz = std::pow(10.0, -174.0 / 10.0);
    double N_mW = N0_mW_per_Hz * bandwidthHz_ * std::pow(10.0, noiseFigureDb_ / 10.0);
    return N_mW / 1000.0; 
}

double TaskServer::friisPathLossLin(double freqHz_, double dMeters) {
    const double c = 299792458.0; 
    if (dMeters <= 1e-3) return 1.0; 
    double lambda = c / freqHz_;
    double L = std::pow(4.0 * M_PI * dMeters / lambda, 2.0);
    return std::max(L, 1.0);
}

double TaskServer::shannonRate(double bandwidthHz_, double snrLin_) {
    return bandwidthHz_ * std::log2(1.0 + std::max(0.0, snrLin_));
}

void TaskServer::tryStartCpu(TaskCtx&& ctx) {
    if (externalBusy || cpuBusy) {
        cpuQueue.emplace_back(std::move(ctx));
        return;
    }
    cpuBusy = true;
    double t_cpu = (double)ctx.cycles / cpuFreqRsu;
    ctx.cpuEvt = new cMessage("cpuComplete", CPU_COMPLETE);
    tasks[ctx.cpuEvt] = ctx;
    scheduleAt(simTime() + t_cpu, ctx.cpuEvt);
}

void TaskServer::maybeStartNextCpu() {
    if (cpuBusy || externalBusy) return;
    if (cpuQueue.empty()) return;
    TaskCtx ctx = std::move(cpuQueue.front());
    cpuQueue.pop_front();
    cpuBusy = true;
    double t_cpu = (double)ctx.cycles / cpuFreqRsu;
    ctx.cpuEvt = new cMessage("cpuComplete", CPU_COMPLETE);
    tasks[ctx.cpuEvt] = ctx;
    scheduleAt(simTime() + t_cpu, ctx.cpuEvt);
}
