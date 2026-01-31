// RL offloading agent that talks to Gym via existing serpentine.GymConnection
package straight;

simple GymOffloader
{
    parameters:
        @class(GymOffloader);
        string vehicleId = default("leader"); // which vehicle externalId to control (e.g., "leader" or "follower")
        double pollInterval @unit(s) = default(0.1s); // cadence to query the RL agent
        double cpuFreqVehicle @unit(Hz) = default(0.6e9Hz); // vehicle CPU frequency
        double cyclesPerByte = default(1900); // required CPU cycles per byte of input
        double taskMinMB = default(10); // min input size [MB]
        double taskMaxMB = default(20); // max input size [MB]
        double outputFactor = default(0.2); // output size = factor * input size
        
        // Energy parameters
        double cpuPowerVehicle @unit(W) = default(2.0W);    // Vehicle CPU power
        double txPowerDbmVehicle = default(24);             // Vehicle transmit power (dBm)
        double rxPowerVehicle @unit(W) = default(0.3W);     // Vehicle receive power
        double batteryCapacity @unit(J) = default(3600.0J); // 1 Wh battery capacity
        double idlePowerVehicle @unit(W) = default(0.5W);   // Idle power drain
        
        // Reward function weights
        double rewardAlpha = default(1.0);     // Latency weight
        double rewardBeta = default(1.0);    // Energy weight
    gates:
        input in[3];
        output out[3];
}
