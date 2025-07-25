/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @page game_changelog Lists all changes / additions to the API.
 *
 * Only new / renamed / deleted api functions will be listed here. A list of
 * bug fixes can be found in the normal changelog. Note that removed API
 * functions may still be available if you return an older API version
 * in GetAPIVersion() in info.nut.
 *
 * \b 15.0
 *
 * This version is not yet released. The following changes are not set in stone yet.
 *
 * API additions:
 * \li GSEventVehicleCrashed::GetVictims
 * \li GSEventVehicleCrashed::GetVehicleOwner
 * \li GSEventCompanyRenamed
 * \li GSEventPresidentRenamed
 *
 * Other changes:
 * \li GSBridge::GetBridgeID renamed to GSBridge::GetBridgeType
 * \li GSWaypoint::GetWaypointID now returns the StationID of any type of waypoint
 *
 * \b 14.0
 *
 * API additions:
 * \li GSIndustry::GetConstructionDate
 * \li GSAsyncMode
 * \li GSCompanyMode::IsValid
 * \li GSCompanyMode::IsDeity
 * \li GSTown::ROAD_LAYOUT_RANDOM
 * \li GSVehicle::IsPrimaryVehicle
 * \li GSOrder::SetOrderJumpTo
 * \li GSOrder::SetOrderCondition
 * \li GSOrder::SetOrderCompareFunction
 * \li GSOrder::SetOrderCompareValue
 * \li GSOrder::SetStopLocation
 * \li GSOrder::SetOrderRefit
 * \li GSOrder::AppendOrder
 * \li GSOrder::AppendConditionalOrder
 * \li GSOrder::InsertOrder
 * \li GSOrder::InsertConditionalOrder
 * \li GSOrder::RemoveOrder
 * \li GSOrder::SetOrderFlags
 * \li GSOrder::MoveOrder
 * \li GSOrder::SkipToOrder
 * \li GSOrder::CopyOrders
 * \li GSOrder::ShareOrders
 * \li GSOrder::UnshareOrders
 * \li GSCompany::IsMine
 * \li GSCompany::SetPresidentGender
 * \li GSCompany::SetAutoRenewStatus
 * \li GSCompany::SetAutoRenewMonths
 * \li GSCompany::SetAutoRenewMoney
 * \li GSCompany::SetMaxLoanAmountForCompany
 * \li GSCompany::ResetMaxLoanAmountForCompany
 * \li GSGameSettings::IsDisabledVehicleType
 * \li GSGroup::GroupID
 * \li GSGroup::IsValidGroup
 * \li GSGroup::CreateGroup
 * \li GSGroup::DeleteGroup
 * \li GSGroup::GetVehicleType
 * \li GSGroup::SetName
 * \li GSGroup::GetName
 * \li GSGroup::SetParent
 * \li GSGroup::GetParent
 * \li GSGroup::EnableAutoReplaceProtection
 * \li GSGroup::GetAutoReplaceProtection
 * \li GSGroup::GetNumEngines
 * \li GSGroup::GetNumVehicles
 * \li GSGroup::MoveVehicle
 * \li GSGroup::EnableWagonRemoval
 * \li GSGroup::HasWagonRemoval
 * \li GSGroup::SetAutoReplace
 * \li GSGroup::GetEngineReplacement
 * \li GSGroup::StopAutoReplace
 * \li GSGroup::GetProfitThisYear
 * \li GSGroup::GetProfitLastYear
 * \li GSGroup::GetCurrentUsage
 * \li GSGroup::SetPrimaryColour
 * \li GSGroup::SetSecondaryColour
 * \li GSGroup::GetPrimaryColour
 * \li GSGroup::GetSecondaryColour
 * \li GSGroupList
 * \li GSVehicleList_Group
 * \li GSVehicleList_DefaultGroup
 * \li GSGoal::IsValidGoalDestination
 * \li GSGoal::SetDestination
 * \li GSIndustry::GetProductionLevel
 * \li GSIndustry::SetProductionLevel
 * \li GSStoryPage::IsValidStoryPageElementType
 * \li GSStoryPage::IsValidStoryPageButtonColour
 * \li GSStoryPage::IsValidStoryPageButtonFlags
 * \li GSStoryPage::IsValidStoryPageButtonCursor
 * \li GSTileList_StationCoverage
 * \li GSAirport::GetAirportNumHelipads
 *
 * API removals:
 * \li GSError::ERR_PRECONDITION_TOO_MANY_PARAMETERS, that error is never returned anymore.
 * \li GSInfo::CONFIG_RANDOM, no longer used.
 * \li GSInfo::AddSettings random_deviation is no longer used.
 *
 * Other changes:
 * \li GSGroupList accepts an optional filter function
 * \li GSIndustryList accepts an optional filter function
 * \li GSSignList accepts an optional filter function
 * \li GSSubsidyList accepts an optional filter function
 * \li GSTownList accepts an optional filter function
 * \li GSVehicleList accepts an optional filter function
 * \li GSInfo::AddSettings easy_value / medium_value / hard_value are replaced with default_value
 *
 * \b 13.0
 *
 * API additions:
 * \li GSCargo::GetWeight
 * \li GSIndustryType::ResolveNewGRFID
 * \li GSObjectType::ResolveNewGRFID
 * \li GSLeagueTable
 *
 * Other changes:
 * \li GSRoad::HasRoadType now correctly checks RoadType against RoadType
 *
 * \b 12.0
 *
 * API additions:
 * \li GSNewGRF
 * \li GSNewGRFList
 * \li GSMarine::BT_LOCK
 * \li GSMarine::BT_CANAL
 * \li GSTile::IsSeaTile
 * \li GSTile::IsRiverTile
 * \li GSTile::BT_CLEAR_WATER
 * \li GSObjectTypeList
 * \li GSObjectType
 *
 * \b 1.11.0
 *
 * API additions:
 * \li GSCargo::GetName
 * \li GSEventStoryPageButtonClick
 * \li GSEventStoryPageTileSelect
 * \li GSEventStoryPageVehicleSelect
 * \li GSIndustry::GetCargoLastAcceptedDate
 * \li GSIndustry::GetControlFlags
 * \li GSIndustry::GetExclusiveConsumer
 * \li GSIndustry::GetExclusiveSupplier
 * \li GSIndustry::GetLastProductionYear
 * \li GSIndustry::SetControlFlags
 * \li GSIndustry::SetExclusiveConsumer
 * \li GSIndustry::SetExclusiveSupplier
 * \li GSIndustry::SetText
 * \li GSStoryPage::MakePushButtonReference
 * \li GSStoryPage::MakeTileButtonReference
 * \li GSStoryPage::MakeVehicleButtonReference
 * \li GSPriorityQueue
 *
 * Other changes:
 * \li GSCompany::ChangeBankBalance takes one extra parameter to refer to a location to show text effect on
 * \li GSGoal::Question and GSGoal::QuestionClient no longer require to have any buttons except for the window type GSGoal.QT_QUESTION
 *
 * \b 1.10.0
 *
 * API additions:
 * \li GSVehicle::BuildVehicleWithRefit
 * \li GSVehicle::GetBuildWithRefitCapacity
 * \li GSRoad::GetName
 * \li GSRoad::RoadVehCanRunOnRoad
 * \li GSRoad::RoadVehHasPowerOnRoad
 * \li GSRoad::ConvertRoadType
 * \li GSRoad::GetMaxSpeed
 * \li GSEngine::EnableForCompany
 * \li GSEngine::DisableForCompany
 *
 * \b 1.9.0
 *
 * API additions:
 * \li GSAirport::GetMonthlyMaintenanceCost
 * \li GSClient
 * \li GSClientList
 * \li GSClientList_Company
 * \li GSViewport::ScrollEveryoneTo
 * \li GSViewport::ScrollCompanyClientsTo
 * \li GSViewport::ScrollClientTo
 * \li GSGoal::QuestionClient
 *
 * Other changes:
 * \li GSBridge::GetName takes one extra parameter to refer the vehicle type
 *
 * \b 1.8.0
 *
 * No changes
 *
 * API additions:
 * \li GSRoad::ERR_ROADTYPE_DISALLOWS_CROSSING
 *
 * \b 1.7.0 - 1.7.2
 *
 * No changes
 *
 * \b 1.6.1 - 1.6.0
 *
 * No changes
 *
 * \b 1.5.3 - 1.5.1
 *
 * No changes
 *
 * \b 1.5.0
 *
 * API additions:
 * \li GSList::SwapList
 * \li GSStation::GetCargoPlanned
 * \li GSStation::GetCargoPlannedFrom
 * \li GSStation::GetCargoPlannedFromVia
 * \li GSStation::GetCargoPlannedVia
 * \li GSStation::GetCargoWaitingFromVia
 * \li GSStationList_CargoPlannedByFrom
 * \li GSStationList_CargoPlannedByVia
 * \li GSStationList_CargoPlannedFromByVia
 * \li GSStationList_CargoPlannedViaByFrom
 * \li GSStationList_CargoWaitingByFrom
 * \li GSStationList_CargoWaitingByVia
 * \li GSStationList_CargoWaitingFromByVia
 * \li GSStationList_CargoWaitingViaByFrom
 *
 * Other changes:
 * \li GSNews::Create takes two extra parameters to refer to a location, station,
 *     industry, or town. The user can click at the news message to jump to the
 *     referred location.
 *
 * \b 1.4.4 - 1.4.3
 *
 * No changes
 *
 * \b 1.4.2
 *
 * Other changes:
 * \li GSCargoMonitor delivery and pickup monitor functions have improved boundary checking for
 *     their parameters, and return \c -1 if they are found out of bounds.
 *
 * \b 1.4.1
 *
 * No changes
 *
 * \b 1.4.0
 *
 * API additions:
 * \li AICargo::GetDistributionType
 * \li GSCompany::ChangeBankBalance
 * \li GSDate::DATE_INVALID
 * \li GSDate::IsValidDate
 * \li GSGoal::GT_STORY_PAGE
 * \li GSGoal::IsCompleted
 * \li GSGoal::SetCompleted
 * \li GSGoal::SetProgress
 * \li GSGoal::SetText
 * \li GSStation::HasCargoRating
 * \li GSStation::GetCargoWaitingFrom
 * \li GSStation::GetCargoWaitingVia
 * \li GSStoryPage
 * \li GSStoryPageList
 * \li GSStoryPageElementList
 * \li GSTile::GetTerrainType
 * \li GSTown::FoundTown
 * \li GSTown::GetFundBuildingsDuration
 * \li GSTown::SetName
 * \li GSTown::TOWN_GROWTH_NONE
 * \li GSTown::TOWN_GROWTH_NORMAL
 *
 * Other changes:
 * \li GSGoal::New can now create up to 64000 concurrent goals. The old limit was 256 goals.
 * \li GSStation::GetCargoRating does return -1 for cargo-station combinations that
 *     do not have a rating yet instead of returning 69.
 *
 * \b 1.3.3 - 1.3.2
 *
 * No changes
 *
 * \b 1.3.1
 *
 * API additions:
 * \li GSTile::GetTerrainType
 *
 * \b 1.3.0
 *
 * API additions:
 * \li GSCargoMonitor
 * \li GSEngine::IsValidEngine and GSEngine::IsBuildable when outside GSCompanyMode scope
 * \li GSEventExclusiveTransportRights
 * \li GSEventRoadReconstruction
 * \li GSNews::NewsType::Accident, GSNews::NewsType::CompanyInfo, GSNews::NewsType::Advice, GSNews::NewsType::Acceptance
 * \li GSIndustryType::IsProcessingIndustry
 * \li GSStation::IsAirportClosed
 * \li GSStation::OpenCloseAirport
 * \li GSController::Break
 * \li GSIndustryType::BuildIndustry, GSIndustryType::CanBuildIndustry, GSIndustryType::ProspectIndustry and GSIndustryType::CanProspectIndustry when outside GSCompanyMode scope
 *
 * Other changes:
 * \li Company specific goals are now removed when a company goes bankrupt or is taken over.
 *
 * \b 1.2.3 - 1.2.1
 *
 * No changes
 *
 * \b 1.2.0
 * \li First stable release with the NoGo framework.
 */

/**
 * @mainpage
 *
 * What's new?
 * \li \ref game_changelog
 *
 * Main classes:
 * \li \ref GSInfo
 * \li \ref GSController
 *
 * Detail topics:
 * \li \ref script_ids
 */
