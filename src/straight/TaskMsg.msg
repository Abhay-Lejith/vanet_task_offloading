namespace straight;

// Message representing a task offload request from a vehicle to an RSU
message TaskRequest {
    string vehicleId;
    int64 inputBytes;   // input data size in bytes
    int64 outputBytes;  // output data size in bytes
    int64 cycles;       // total CPU cycles required
}

// Message representing task completion notification back to the vehicle
message TaskDone {
    string vehicleId;
    int64 inputBytes;
    int64 outputBytes;
    double totalTime;   // total end-to-end time experienced for this task (s)
}
