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
    gates:
        input in;
        output out;
}
