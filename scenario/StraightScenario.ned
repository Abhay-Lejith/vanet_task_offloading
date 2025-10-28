import org.car2x.veins.base.connectionManager.ConnectionManager;
import org.car2x.veins.base.modules.BaseWorldUtility;
import org.car2x.veins.modules.mobility.traci.TraCIScenarioManager*;
import org.car2x.veins.modules.obstacle.ObstacleControl;
import org.car2x.veins.modules.obstacle.VehicleObstacleControl;
import org.car2x.veins.modules.world.annotations.AnnotationManager;
import org.car2x.veins.nodes.Car;
import org.car2x.veins.nodes.RSU;
import org.car2x.veinsvlc.VlcConnectionManager;

import serpentine.GymConnection;
import straight.GymOffloader;
import straight.TaskServer;


network StraightScenario
{

    parameters:
        double playgroundSizeX @unit(m); // x size of the area the nodes are in (in meters)
        double playgroundSizeY @unit(m); // y size of the area the nodes are in (in meters)
        double playgroundSizeZ @unit(m); // z size of the area the nodes are in (in meters)
        @display("bgb=$playgroundSizeX,$playgroundSizeY");
    submodules:
        obstacles: ObstacleControl {
            @display("p=240,50");
        }
        vehicleObstacles: VehicleObstacleControl {}
        annotations: AnnotationManager {
            @display("p=260,50");
        }
        connectionManager: ConnectionManager {
            parameters:
                @display("p=150,0;i=abstract/multicast");
        }
        vlcConnectionManager: VlcConnectionManager {}
        world: BaseWorldUtility {
            parameters:
                playgroundSizeX = playgroundSizeX;
                playgroundSizeY = playgroundSizeY;
                playgroundSizeZ = playgroundSizeZ;
                @display("p=30,0;i=misc/globe");
        }
        manager: TraCIScenarioManagerForker {
            parameters:
                @display("p=512,128");
        }
        gym_connection: GymConnection {}
        offloader: GymOffloader {}
        node[0]: Car {}
        rsu[3]: RSU {}
        rsuServer[3]: TaskServer {}

    connections allowunconnected:
        for i=0..2 {
            offloader.out[i] --> rsuServer[i].in;
            rsuServer[i].out --> offloader.in[i];
        }
}
