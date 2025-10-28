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
    gates:
        input in[3];
        output out[3];
}
