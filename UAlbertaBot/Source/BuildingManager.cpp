#include "Common.h"
#include "BuildingManager.h"
#include "Micro.h"
#include "ScoutManager.h"
#include <BWAPI/Constants.h>

using namespace UAlbertaBot;

BuildingManager::BuildingManager()
    : _debugMode(false)
    , _reservedMinerals(0)
    , _reservedGas(0)
{
	cannonsBuilt = 0;
	pylonAmount = BWAPI::Broodwar->self()->completedUnitCount(BWAPI::UnitTypes::Protoss_Pylon);
}

// gets called every frame from GameCommander
void BuildingManager::update()
{
    validateWorkersAndBuildings();          // check to see if assigned workers have died en route or while constructing
    assignWorkersToUnassignedBuildings();   // assign workers to the unassigned buildings and label them 'planned'    
    constructAssignedBuildings();           // for each planned building, if the worker isn't constructing, send the command    
    checkForStartedConstruction();          // check to see if any buildings have started construction and update data structures    
    checkForDeadTerranBuilders();           // if we are terran and a building is under construction without a worker, assign a new one    
    checkForCompletedBuildings();           // check to see if any buildings have completed and update data structures
}

bool BuildingManager::isBeingBuilt(BWAPI::UnitType type)
{
    for (auto & b : _buildings)
    {
        if (b.type == type)
        {
            return true;
        }
    }

    return false;
}

// STEP 1: DO BOOK KEEPING ON WORKERS WHICH MAY HAVE DIED
void BuildingManager::validateWorkersAndBuildings()
{
    // TODO: if a terran worker dies while constructing and its building
    //       is under construction, place unit back into buildingsNeedingBuilders

    std::vector<Building> toRemove;
    
    // find any buildings which have become obsolete
    for (auto & b : _buildings)
    {
        if (b.status != BuildingStatus::UnderConstruction)
        {
            continue;
        }

        if (b.buildingUnit == nullptr || !b.buildingUnit->getType().isBuilding() || b.buildingUnit->getHitPoints() <= 0)
        {
            toRemove.push_back(b);
        }
    }

    removeBuildings(toRemove);
}

// STEP 2: ASSIGN WORKERS TO BUILDINGS WITHOUT THEM
void BuildingManager::assignWorkersToUnassignedBuildings()
{
    // for each building that doesn't have a builder, assign one
    for (Building & b : _buildings)
    {
        if (b.status != BuildingStatus::Unassigned)
        {
            continue;
        }

        if (_debugMode) { BWAPI::Broodwar->printf("Assigning Worker To: %s",b.type.getName().c_str()); }

        // grab a worker unit from WorkerManager which is closest to this final position
        BWAPI::Unit workerToAssign = WorkerManager::Instance().getBuilder(b);

        if (workerToAssign)
        {
            //BWAPI::Broodwar->printf("VALID WORKER BEING ASSIGNED: %d", workerToAssign->getID());

            // TODO: special case of terran building whose worker died mid construction
            //       send the right click command to the buildingUnit to resume construction
            //		 skip the buildingsAssigned step and push it back into buildingsUnderConstruction

            b.builderUnit = workerToAssign;

            BWAPI::TilePosition testLocation = getBuildingLocation(b);
            if (!testLocation.isValid())
            {
                continue;
            }

            b.finalPosition = testLocation;

            // reserve this building's space
            BuildingPlacer::Instance().reserveTiles(b.finalPosition,b.type.tileWidth(),b.type.tileHeight());

            b.status = BuildingStatus::Assigned;
        }
    }
}

// STEP 3: ISSUE CONSTRUCTION ORDERS TO ASSIGN BUILDINGS AS NEEDED
void BuildingManager::constructAssignedBuildings()
{
    for (auto & b : _buildings)
    {
        if (b.status != BuildingStatus::Assigned)
        {
            continue;
        }

        // if that worker is not currently constructing
        if (!b.builderUnit->isConstructing())
        {
            // if we haven't explored the build position, go there
            if (!isBuildingPositionExplored(b))
            {
                Micro::SmartMove(b.builderUnit,BWAPI::Position(b.finalPosition));
            }
            // if this is not the first time we've sent this guy to build this
            // it must be the case that something was in the way of building
            else if (b.buildCommandGiven)
            {
                // tell worker manager the unit we had is not needed now, since we might not be able
                // to get a valid location soon enough
                WorkerManager::Instance().finishedWithWorker(b.builderUnit);

                // free the previous location in reserved
                BuildingPlacer::Instance().freeTiles(b.finalPosition,b.type.tileWidth(),b.type.tileHeight());

                // nullify its current builder unit
                b.builderUnit = nullptr;

                // reset the build command given flag
                b.buildCommandGiven = false;

                // add the building back to be assigned
                b.status = BuildingStatus::Unassigned;
            }
            else
            {
                // issue the build order!
                b.builderUnit->build(b.type,b.finalPosition);

                // set the flag to true
                b.buildCommandGiven = true;
            }
        }
    }
}

// STEP 4: UPDATE DATA STRUCTURES FOR BUILDINGS STARTING CONSTRUCTION
void BuildingManager::checkForStartedConstruction()
{
    // for each building unit which is being constructed
    for (auto & buildingStarted : BWAPI::Broodwar->self()->getUnits())
    {
        // filter out units which aren't buildings under construction
        if (!buildingStarted->getType().isBuilding() || !buildingStarted->isBeingConstructed())
        {
            continue;
        }

        // check all our building status objects to see if we have a match and if we do, update it
        for (auto & b : _buildings)
        {
            if (b.status != BuildingStatus::Assigned)
            {
                continue;
            }
        
            // check if the positions match
            if (b.finalPosition == buildingStarted->getTilePosition())
            {
                // the resources should now be spent, so unreserve them
                _reservedMinerals -= buildingStarted->getType().mineralPrice();
                _reservedGas      -= buildingStarted->getType().gasPrice();

                // flag it as started and set the buildingUnit
                b.underConstruction = true;
                b.buildingUnit = buildingStarted;

                // if we are zerg, the buildingUnit now becomes nullptr since it's destroyed
                if (BWAPI::Broodwar->self()->getRace() == BWAPI::Races::Zerg)
                {
                    b.builderUnit = nullptr;
                    // if we are protoss, give the worker back to worker manager
                }
                else if (BWAPI::Broodwar->self()->getRace() == BWAPI::Races::Protoss)
                {
                    // if this was the gas steal unit then it's the scout worker so give it back to the scout manager
                    if (b.isGasSteal)
                    {
                        ScoutManager::Instance().setWorkerScout(b.builderUnit);
                    }
                    // otherwise tell the worker manager we're finished with this unit
                    else
                    {
                        WorkerManager::Instance().finishedWithWorker(b.builderUnit);
                    }

                    b.builderUnit = nullptr;
                }

                // put it in the under construction vector
                b.status = BuildingStatus::UnderConstruction;

                // free this space
                BuildingPlacer::Instance().freeTiles(b.finalPosition,b.type.tileWidth(),b.type.tileHeight());

                // only one building will match
                break;
            }
        }
    }
}

// STEP 5: IF WE ARE TERRAN, THIS MATTERS, SO: LOL
void BuildingManager::checkForDeadTerranBuilders() {}

// STEP 6: CHECK FOR COMPLETED BUILDINGS
void BuildingManager::checkForCompletedBuildings()
{
    std::vector<Building> toRemove;

    // for each of our buildings under construction
    for (auto & b : _buildings)
    {
        if (b.status != BuildingStatus::UnderConstruction)
        {
            continue;       
        }

        // if the unit has completed
        if (b.buildingUnit->isCompleted())
        {
            // if we are terran, give the worker back to worker manager
            if (BWAPI::Broodwar->self()->getRace() == BWAPI::Races::Terran)
            {
                if (b.isGasSteal)
                {
                    ScoutManager::Instance().setWorkerScout(b.builderUnit);
                }
                // otherwise tell the worker manager we're finished with this unit
                else
                {
                    WorkerManager::Instance().finishedWithWorker(b.builderUnit);
                }
            }

            // remove this unit from the under construction vector
            toRemove.push_back(b);
        }
    }

    removeBuildings(toRemove);
}

// COMPLETED
bool BuildingManager::isEvolvedBuilding(BWAPI::UnitType type) 
{
    if (type == BWAPI::UnitTypes::Zerg_Sunken_Colony ||
        type == BWAPI::UnitTypes::Zerg_Spore_Colony ||
        type == BWAPI::UnitTypes::Zerg_Lair ||
        type == BWAPI::UnitTypes::Zerg_Hive ||
        type == BWAPI::UnitTypes::Zerg_Greater_Spire)
    {
        return true;
    }

    return false;
}

// add a new building to be constructed
void BuildingManager::addBuildingTask(BWAPI::UnitType type, BWAPI::TilePosition desiredLocation, bool isGasSteal)
{

	pylonAmount = BWAPI::Broodwar->self()->completedUnitCount(BWAPI::UnitTypes::Protoss_Pylon);

	_reservedMinerals += type.mineralPrice();
	_reservedGas += type.gasPrice();

	Building b(type, desiredLocation);
	if (b.type == BWAPI::UnitTypes::Protoss_Pylon && pylonAmount ==1){
	
		b.isPhotonPylon = true;
	
	}
	b.isGasSteal = isGasSteal;
	b.status = BuildingStatus::Unassigned;


	_buildings.push_back(b);
}

bool BuildingManager::isBuildingPositionExplored(const Building & b) const
{
    BWAPI::TilePosition tile = b.finalPosition;

    // for each tile where the building will be built
    for (int x=0; x<b.type.tileWidth(); ++x)
    {
        for (int y=0; y<b.type.tileHeight(); ++y)
        {
            if (!BWAPI::Broodwar->isExplored(tile.x + x,tile.y + y))
            {
                return false;
            }
        }
    }

    return true;
}


char BuildingManager::getBuildingWorkerCode(const Building & b) const
{
    return b.builderUnit == nullptr ? 'X' : 'W';
}

int BuildingManager::getReservedMinerals() 
{
    return _reservedMinerals;
}

int BuildingManager::getReservedGas() 
{
    return _reservedGas;
}

void BuildingManager::drawBuildingInformation(int x,int y)
{
    if (!Config::Debug::DrawBuildingInfo)
    {
        return;
    }

    for (auto & unit : BWAPI::Broodwar->self()->getUnits())
    {
        BWAPI::Broodwar->drawTextMap(unit->getPosition().x,unit->getPosition().y+5,"\x07%d",unit->getID());
    }

    BWAPI::Broodwar->drawTextScreen(x,y,"\x04 Building Information:");
    BWAPI::Broodwar->drawTextScreen(x,y+20,"\x04 Name");
    BWAPI::Broodwar->drawTextScreen(x+150,y+20,"\x04 State");

    int yspace = 0;

    for (const auto & b : _buildings)
    {
        if (b.status == BuildingStatus::Unassigned)
        {
            BWAPI::Broodwar->drawTextScreen(x,y+40+((yspace)*10),"\x03 %s",b.type.getName().c_str());
            BWAPI::Broodwar->drawTextScreen(x+150,y+40+((yspace++)*10),"\x03 Need %c",getBuildingWorkerCode(b));
        }
        else if (b.status == BuildingStatus::Assigned)
        {
            BWAPI::Broodwar->drawTextScreen(x,y+40+((yspace)*10),"\x03 %s %d",b.type.getName().c_str(),b.builderUnit->getID());
            BWAPI::Broodwar->drawTextScreen(x+150,y+40+((yspace++)*10),"\x03 A %c (%d,%d)",getBuildingWorkerCode(b),b.finalPosition.x,b.finalPosition.y);

            int x1 = b.finalPosition.x*32;
            int y1 = b.finalPosition.y*32;
            int x2 = (b.finalPosition.x + b.type.tileWidth())*32;
            int y2 = (b.finalPosition.y + b.type.tileHeight())*32;

            BWAPI::Broodwar->drawLineMap(b.builderUnit->getPosition().x,b.builderUnit->getPosition().y,(x1+x2)/2,(y1+y2)/2,BWAPI::Colors::Orange);
            BWAPI::Broodwar->drawBoxMap(x1,y1,x2,y2,BWAPI::Colors::Red,false);
        }
        else if (b.status == BuildingStatus::UnderConstruction)
        {
            BWAPI::Broodwar->drawTextScreen(x,y+40+((yspace)*10),"\x03 %s %d",b.type.getName().c_str(),b.buildingUnit->getID());
            BWAPI::Broodwar->drawTextScreen(x+150,y+40+((yspace++)*10),"\x03 Const %c",getBuildingWorkerCode(b));
        }
    }
}

BuildingManager & BuildingManager::Instance()
{
    static BuildingManager instance;
    return instance;
}

std::vector<BWAPI::UnitType> BuildingManager::buildingsQueued()
{
    std::vector<BWAPI::UnitType> buildingsQueued;

    for (const auto & b : _buildings)
    {
        if (b.status == BuildingStatus::Unassigned || b.status == BuildingStatus::Assigned)
        {
            buildingsQueued.push_back(b.type);
        }
    }

    return buildingsQueued;
}


BWAPI::TilePosition BuildingManager::getBuildingLocation(const Building & b)
{
    int numPylons = BWAPI::Broodwar->self()->completedUnitCount(BWAPI::UnitTypes::Protoss_Pylon);

	if (b.isPhotonPylon){

		PhotonPylons.push_back(b);

		BWTA::BaseLocation* base = InformationManager::Instance().getMainBaseLocation(BWAPI::Broodwar->self());
		BWTA::Chokepoint* choke = BWTA::getNearestChokepoint(base->getTilePosition());

		//find midpoint between base an choke point
		int newX;
		if (base->getPosition().x > choke->getCenter().x){

			newX = base->getPosition().x - ((base->getPosition().x - choke->getCenter().x) / 2);

		}

		else{

			newX = base->getPosition().x - ((base->getPosition().x - choke->getCenter().x) / 2);
		}


		int newY;

		if (base->getPosition().y > choke->getCenter().y){

			newY = base->getPosition().y - ((base->getPosition().y - choke->getCenter().y) / 2);

		}

		else{

			newY = base->getPosition().y - ((base->getPosition().y - choke->getCenter().y) / 2);
		}

		//have to divide by tile size because it wasnt changing it.
		BWAPI::TilePosition inbetween = BWAPI::TilePosition(newX / TILE_SIZE, newY / TILE_SIZE);

		Building c(b.type, inbetween);
		c.buildCommandGiven = b.buildCommandGiven;
		c.builderUnit = b.builderUnit;
		c.buildingUnit = c.buildingUnit;
		c.isGasSteal = b.isGasSteal;
		c.lastOrderFrame = b.lastOrderFrame;
		c.underConstruction = b.underConstruction;
		//desiredPosition gets set by the constructor
		return c.desiredPosition;
	}

    if (b.isGasSteal)
    {
        BWTA::BaseLocation * enemyBaseLocation = InformationManager::Instance().getMainBaseLocation(BWAPI::Broodwar->enemy());
        UAB_ASSERT(enemyBaseLocation,"Should have enemy base location before attempting gas steal");
        UAB_ASSERT(enemyBaseLocation->getGeysers().size() > 0,"Should have spotted an enemy geyser");

        for (auto & unit : enemyBaseLocation->getGeysers())
        {
            BWAPI::TilePosition tp(unit->getInitialTilePosition());
            return tp;
        }
    }

    if (b.type.requiresPsi() && numPylons == 0)
    {
        return BWAPI::TilePositions::None;
    }

    if (b.type.isRefinery())
    {
        return BuildingPlacer::Instance().getRefineryPosition();
    }

    if (b.type.isResourceDepot())
    {
        // get the location 

        BWAPI::TilePosition tile = MapTools::Instance().getNextExpansion();

        return tile;
    }

	if (b.type == BWAPI::UnitTypes::Protoss_Photon_Cannon){
	
		Building pylon = PhotonPylons[0];

		BWAPI::Broodwar->printf("no is pylon yet: %d", pylon.desiredPosition.x );
		
		BWTA::BaseLocation* base = InformationManager::Instance().getMainBaseLocation(BWAPI::Broodwar->self());
		BWTA::Chokepoint* choke = BWTA::getNearestChokepoint(base->getTilePosition());
		BWAPI::TilePosition chokeTile = BWAPI::TilePosition(choke->getCenter());

		BWAPI::TilePosition photonLocation = PhotonLocationFinder(pylon.desiredPosition, chokeTile);

		Building c(b.type , photonLocation);
		c.buildCommandGiven = b.buildCommandGiven;
		c.builderUnit = b.builderUnit;
		c.buildingUnit = c.buildingUnit;
		c.isGasSteal = b.isGasSteal;

		c.lastOrderFrame = b.lastOrderFrame;
		c.underConstruction = b.underConstruction;

		
		return BWAPI::Broodwar->getBuildLocation(c.type,c.desiredPosition);
	}
    // set the building padding specifically
    int distance = b.type == BWAPI::UnitTypes::Protoss_Photon_Cannon ? 0 : Config::Macro::BuildingSpacing;
    if (b.type == BWAPI::UnitTypes::Protoss_Pylon && (numPylons < 3))
    {
        distance = Config::Macro::PylonSpacing;
    }

    // get a position within our region
    return BuildingPlacer::Instance().getBuildLocationNear(b,distance,false);
}

void BuildingManager::removeBuildings(const std::vector<Building> & toRemove)
{
    for (auto & b : toRemove)
    {
        auto & it = std::find(_buildings.begin(), _buildings.end(), b);

        if (it != _buildings.end())
        {
            _buildings.erase(it);
        }
    }
}


BWAPI::TilePosition BuildingManager::PhotonLocationFinder(BWAPI::TilePosition pylon , BWAPI::TilePosition ChokePoint){


	int newX;
	if (pylon.x > ChokePoint.x){

		newX = pylon.x - ((pylon.x - ChokePoint.x) / 2);

	}

	else{

		newX = pylon.x - ((pylon.x -ChokePoint.x) / 2);
	}


	int newY;

	if (pylon.y > ChokePoint.y){

		newY = pylon.y - ((pylon.y - ChokePoint.y) / 2);

	}

	else{

		newY = pylon.y - ((pylon.y - ChokePoint.y) / 2);
	}


	return BWAPI::TilePosition(newX,newY);

}