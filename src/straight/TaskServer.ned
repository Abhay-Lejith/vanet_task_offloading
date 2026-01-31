package straight;

simple TaskServer
{
    parameters:
        @class(TaskServer);
        double bandwidthHz @unit(Hz) = default(10e6Hz); // RSU system bandwidth
        double carrierHz @unit(Hz) = default(5.9e9Hz);   // carrier frequency for path loss
        double noiseFigureDb = default(9);               // receiver noise figure in dB
        double txPowerDbmVehicle = default(24);          // UL Tx power from vehicle
        double txPowerDbmRsu = default(24);              // DL Tx power from RSU
        double cpuFreqRsu @unit(Hz) = default(10e9Hz);   // RSU CPU frequency
        bool enableExternalBusy = default(true);         // enable stochastic busy periods
        double busyOnMean @unit(s) = default(3s);        // mean busy ON duration
        double busyOffMean @unit(s) = default(7s);       // mean busy OFF duration
        
        // Energy parameters
        double cpuPowerRsu @unit(W) = default(100.0W);      // RSU CPU power consumption
        double idlePowerRsu @unit(W) = default(20.0W);      // RSU idle power
        double rxPowerRsu @unit(W) = default(5.0W);         // RSU receive power
        double staticEnergyPerTask @unit(J) = default(0.1J); // Fixed overhead per task
    gates:
        input in;
        output out;
}
