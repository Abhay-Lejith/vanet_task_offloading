// RL offloading agent that talks to Gym via existing serpentine.GymConnection
package straight;

simple GymOffloader
{
    parameters:
        @class(GymOffloader);
        string vehicleId = default("leader"); // which vehicle externalId to control (e.g., "leader" or "follower")
        double pollInterval @unit(s) = default(0.1s); // cadence to query the RL agent
}
