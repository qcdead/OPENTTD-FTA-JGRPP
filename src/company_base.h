/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file company_base.h Definition of stuff that is very close to a company, like the company struct itself. */

#ifndef COMPANY_BASE_H
#define COMPANY_BASE_H

#include "road_type.h"
#include "livery.h"
#include "autoreplace_type.h"
#include "tile_type.h"
#include "settings_type.h"
#include "group.h"
#include <array>
#include <numeric>
#include <string>

static const Money COMPANY_MAX_LOAN_DEFAULT = INT64_MIN;

/** Statistics about the economy. */
struct CompanyEconomyEntry {
	Money income;                 ///< The amount of income.
	Money expenses;               ///< The amount of expenses.
	CargoArray delivered_cargo{}; ///< The amount of delivered cargo.
	int32_t performance_history;  ///< Company score (scale 0-1000)
	Money company_value;          ///< The value of the company.
};

struct CompanyInfrastructure {
	std::array<uint32_t, RAILTYPE_END> rail{}; ///< Count of company owned track bits for each rail type.
	std::array<uint32_t, ROADTYPE_END> road{}; ///< Count of company owned track bits for each road type.
	uint32_t signal{};                         ///< Count of company owned signals.
	uint32_t water{};                          ///< Count of company owned track bits for canals.
	uint32_t station{};                        ///< Count of company owned station tiles.
	uint32_t airport{};                        ///< Count of company owned airports.

	/** Get total sum of all owned track bits. */
	uint32_t GetRailTotal() const
	{
		return std::accumulate(std::begin(this->rail), std::end(this->rail), 0U);
	}

	uint32_t GetRoadTotal() const;
	uint32_t GetTramTotal() const;

	void Dump(struct format_target &buffer) const;

	bool operator==(const CompanyInfrastructure &) const = default;
};

class FreeUnitIDGenerator {
public:
	UnitID NextID() const;
	UnitID UseID(UnitID index);
	void ReleaseID(UnitID index);

private:
	using BitmapStorage = size_t;
	static constexpr size_t BITMAP_SIZE = std::numeric_limits<BitmapStorage>::digits;

	std::vector<BitmapStorage> used_bitmap;
};

enum CompanyBankruptcyFlags : uint8_t {
	CBRF_NONE      =   0x0,
	CBRF_SALE      =   0x1, ///< the company has been marked for sale
	CBRF_SALE_ONLY =   0x2, ///< the company has been marked for sale without being in a bankruptcy state first
};
DECLARE_ENUM_AS_BIT_SET(CompanyBankruptcyFlags)

typedef Pool<Company, CompanyID, 1, MAX_COMPANIES> CompanyPool;
extern CompanyPool _company_pool;

/** Statically loadable part of Company pool item */
struct CompanyProperties {
	uint32_t name_2;                 ///< Parameter of #name_1.
	StringID name_1;                 ///< Name of the company if the user did not change it.
	std::string name;                ///< Name of the company if the user changed it.

	StringID president_name_1;       ///< Name of the president if the user did not change it.
	uint32_t president_name_2;       ///< Parameter of #president_name_1
	std::string president_name;      ///< Name of the president if the user changed it.

	NetworkAuthorizedKeys allow_list; ///< Public keys of clients that are allowed to join this company.

	CompanyManagerFace face;         ///< Face description of the president.

	Money money;                     ///< Money owned by the company.
	uint8_t money_fraction;          ///< Fraction of money of the company, too small to represent in #money.
	Money current_loan;              ///< Amount of money borrowed from the bank.
	Money max_loan;                  ///< Max allowed amount of the loan or COMPANY_MAX_LOAN_DEFAULT.

	uint16_t colour;                  ///< Company colour.

	uint8_t block_preview;           ///< Number of quarters that the company is not allowed to get new exclusive engine previews (see CompaniesGenStatistics).

	TileIndex location_of_HQ;        ///< Northern tile of HQ; #INVALID_TILE when there is none.
	TileIndex last_build_coordinate; ///< Coordinate of the last build thing by this company.

	std::array<Owner, MAX_COMPANY_SHARE_OWNERS> share_owners; ///< Owners of the shares of the company. #INVALID_OWNER if nobody has bought them yet.

	CalTime::Year inaugurated_year;  ///< Year of starting the company.
	int32_t display_inaugurated_period;///< Wallclock display period of starting the company.
	EconTime::YearDelta age_years;   ///< Number of economy years that the company has been operational.

	uint8_t months_empty = 0;        ///< NOSAVE: Number of months this company has not had a client in multiplayer.
	uint8_t months_of_bankruptcy;    ///< Number of months that the company is unable to pay its debts
	CompanyID bankrupt_last_asked;   ///< Which company was most recently asked about buying it?
	CompanyBankruptcyFlags bankrupt_flags; ///< bankruptcy flags
	CompanyMask bankrupt_asked;      ///< which companies were asked about buying it?
	int16_t bankrupt_timeout;        ///< If bigger than \c 0, amount of time to wait for an answer on an offer to buy this company.
	Money bankrupt_value;

	uint32_t terraform_limit;        ///< Amount of tileheights we can (still) terraform (times 65536).
	uint32_t clear_limit;            ///< Amount of tiles we can (still) clear (times 65536).
	uint32_t tree_limit;             ///< Amount of trees we can (still) plant (times 65536).
	uint32_t purchase_land_limit;    ///< Amount of tiles we can (still) purchase (times 65536).
	uint32_t build_object_limit;     ///< Amount of tiles we can (still) build objects on (times 65536).

	/**
	 * If \c true, the company is (also) controlled by the computer (a NoAI program).
	 * @note It is possible that the user is also participating in such a company.
	 */
	bool is_ai;

	std::array<Expenses, 3> yearly_expenses{}; ///< Expenses of the company for the last three years.
	CompanyEconomyEntry cur_economy;                       ///< Economic data of the company of this quarter.
	CompanyEconomyEntry old_economy[MAX_HISTORY_QUARTERS]; ///< Economic data of the company of the last #MAX_HISTORY_QUARTERS quarters.
	uint8_t num_valid_stat_ent;                            ///< Number of valid statistical entries in #old_economy.

	Livery livery[LS_END];

	EngineRenewList engine_renew_list; ///< Engine renewals of this company.
	CompanySettings settings;          ///< settings specific for each company

	// TODO: Change some of these member variables to use relevant INVALID_xxx constants
	CompanyProperties()
		: name_2(0), name_1(0), president_name_1(0), president_name_2(0),
		  face(0), money(0), money_fraction(0), current_loan(0), max_loan(COMPANY_MAX_LOAN_DEFAULT), colour(COLOUR_BEGIN),
		  block_preview(0), location_of_HQ(0), last_build_coordinate(0), share_owners(), inaugurated_year(0), display_inaugurated_period(0), age_years(0),
		  months_of_bankruptcy(0), bankrupt_last_asked(INVALID_COMPANY), bankrupt_flags(CBRF_NONE)/*, bankrupt_asked()*/, bankrupt_timeout(0), bankrupt_value(0),
		  terraform_limit(0), clear_limit(0), tree_limit(0), purchase_land_limit(0), build_object_limit(0), is_ai(false), engine_renew_list(nullptr) {}

	int32_t InauguratedDisplayYear() const
	{
		return EconTime::UsingWallclockUnits() ? this->display_inaugurated_period : this->inaugurated_year.base();
	}
};

struct Company : CompanyPool::PoolItem<&_company_pool>, CompanyProperties {
	Company(StringID name_1 = StringID{0}, bool is_ai = false);
	~Company();

	RailTypes avail_railtypes;         ///< Rail types available to this company.
	RoadTypes avail_roadtypes;         ///< Road types available to this company.

	std::unique_ptr<class AIInstance> ai_instance;
	class AIInfo *ai_info;
	std::unique_ptr<class AIConfig> ai_config;

	GroupStatistics group_all[VEH_COMPANY_END];      ///< NOSAVE: Statistics for the ALL_GROUP group.
	GroupStatistics group_default[VEH_COMPANY_END];  ///< NOSAVE: Statistics for the DEFAULT_GROUP group.

	CompanyInfrastructure infrastructure; ///< NOSAVE: Counts of company owned infrastructure.

	FreeUnitIDGenerator freeunits[VEH_COMPANY_END];
	FreeUnitIDGenerator freegroups;

	Money GetMaxLoan() const;

	/**
	 * Is this company a valid company, controlled by the computer (a NoAI program)?
	 * @param index Index in the pool.
	 * @return \c true if it is a valid, computer controlled company, else \c false.
	 */
	static inline bool IsValidAiID(size_t index)
	{
		const Company *c = Company::GetIfValid(index);
		return c != nullptr && c->is_ai;
	}

	/**
	 * Is this company a valid company, not controlled by a NoAI program?
	 * @param index Index in the pool.
	 * @return \c true if it is a valid, human controlled company, else \c false.
	 * @note If you know that \a index refers to a valid company, you can use #IsHumanID() instead.
	 */
	static inline bool IsValidHumanID(size_t index)
	{
		const Company *c = Company::GetIfValid(index);
		return c != nullptr && !c->is_ai;
	}

	/**
	 * Is this company a company not controlled by a NoAI program?
	 * @param index Index in the pool.
	 * @return \c true if it is a human controlled company, else \c false.
	 * @pre \a index must be a valid CompanyID.
	 * @note If you don't know whether \a index refers to a valid company, you should use #IsValidHumanID() instead.
	 */
	static inline bool IsHumanID(size_t index)
	{
		return !Company::Get(index)->is_ai;
	}

	static void PostDestructor(size_t index);
};

Money CalculateCompanyValue(const Company *c, bool including_loan = true);
Money CalculateCompanyValueExcludingShares(const Company *c, bool including_loan = true);
Money CalculateHostileTakeoverValue(const Company *c);

extern uint _cur_company_tick_index;

#endif /* COMPANY_BASE_H */
