UAlbertaBot - StarCraft AI Competition Bot

Author:  David Churchill (dave.churchill@gmail.com)

Please check out the [Wiki](https://github.com/davechurchill/ualbertabot/wiki) for full instructions and documentation!

Please consider the code in this repository as unstable research code. Stable releases can be found in the [stable release section](https://github.com/davechurchill/ualbertabot/tree/master/binary_stable_releases). Note that the instructions included in the wiki may not be correct for older binary releases.

#Modified Files:

WorkerManager.cpp:
    Attempted to add code to reassign idle mineral workers

MeleeManger.cpp:

    getTarget() function changed, moveToFront() function added
    Priority added to Terran Comsat Stations
    
RangedManager.cpp:

    getTarget() function changed
    Priority to flying detectors
    
BuildingManager.cpp:

    addBuildingTask(). function changed. 
    added function PhotonLocationFinder()
    
StrategyManager.cpp:
    
    Added goal build order, adjusted Nexus build requirements for lower mineral counts.
    Added protoss scouts to take down flying detectors, used Dark Templar for army.	
