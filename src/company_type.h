/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file company_type.h Types related to companies. */

#ifndef COMPANY_TYPE_H
#define COMPANY_TYPE_H

#include "core/enum_type.hpp"
#include "core/bitmath_func.hpp"

/**
 * Enum for all companies/owners.
 */
enum Owner : uint16_t {
	/* All companies below MAX_COMPANIES are playable
	 * companies, above, they are special, computer controlled 'companies' */
	OWNER_BEGIN     = 0x000, ///< First owner
	COMPANY_FIRST   = 0x000, ///< First company, same as owner
	MAX_COMPANIES   = 0x1F5, ///< Maximum number of companies
	OLD_MAX_COMPANIES   = 0x0F, ///< Maximum number of companies
	OWNER_TOWN      = 0x20F, ///< A town owns the tile, or a town is expanding
	OLD_OWNER_TOWN      = 0x0F,
	OWNER_NONE      = 0x210, ///< The tile has no ownership
	OWNER_WATER     = 0x211, ///< The tile/execution is done by "water"
	OWNER_DEITY     = 0x212, ///< The object is owned by a superuser / goal script
	OWNER_END,              ///< Last + 1 owner
	INVALID_OWNER   = 0x2FF, ///< An invalid owner
	INVALID_COMPANY = 0x2FF, ///< An invalid company

	/* 'Fake' companies used for networks */
	COMPANY_INACTIVE_CLIENT = 0x2FD, ///< The client is joining
	COMPANY_NEW_COMPANY     = 0x2FE, ///< The client wants a new company
	COMPANY_SPECTATOR       = 0x2FF, ///< The client is spectating
};
DECLARE_INCREMENT_DECREMENT_OPERATORS(Owner)
DECLARE_ENUM_AS_ADDABLE(Owner)

static const uint MAX_LENGTH_PRESIDENT_NAME_CHARS = 32; ///< The maximum length of a president name in characters including '\0'
static const uint MAX_LENGTH_COMPANY_NAME_CHARS   = 32; ///< The maximum length of a company name in characters including '\0'

static const uint MAX_HISTORY_QUARTERS            = 24; ///< The maximum number of quarters kept as performance's history
static const uint MAX_COMPANY_SHARE_OWNERS        =  10; ///< The maximum number of shares of a company that can be owned by another company.

static const uint MIN_COMPETITORS_INTERVAL = 0;   ///< The minimum interval (in minutes) between competitors.
static const uint MAX_COMPETITORS_INTERVAL = 500; ///< The maximum interval (in minutes) between competitors.

typedef Owner CompanyID;

class CompanyMask : public BaseBitSet<CompanyMask, CompanyID, uint32_t> {
public:
	constexpr CompanyMask() : BaseBitSet<CompanyMask, CompanyID, uint32_t>() {}
	static constexpr size_t DecayValueType(CompanyID value) { return to_underlying(value); }

	constexpr auto operator <=>(const CompanyMask &) const noexcept = default;
};

struct Company;
typedef uint32_t CompanyManagerFace; ///< Company manager face bits, info see in company_manager_face.h

/** The reason why the company was removed. */
enum CompanyRemoveReason : uint16_t {
	CRR_MANUAL,    ///< The company is manually removed.
	CRR_AUTOCLEAN, ///< The company is removed due to autoclean.
	CRR_BANKRUPT,  ///< The company went belly-up.

	CRR_END,       ///< Sentinel for end.

	CRR_NONE = CRR_MANUAL, ///< Dummy reason for actions that don't need one.
};

/** The action to do with CMD_COMPANY_CTRL. */
enum CompanyCtrlAction : uint16_t {
	CCA_NEW,    ///< Create a new company.
	CCA_NEW_AI, ///< Create a new AI company.
	CCA_DELETE, ///< Delete a company.
	CCA_SALE,   ///< Offer a company for sale.
	CCA_MERGE,  ///< Merge companies.

	CCA_END,    ///< Sentinel for end.
};

/** The action to do with CMD_COMPANY_ALLOW_LIST_CTRL. */
enum CompanyAllowListCtrlAction : uint16_t {
	CALCA_ADD, ///< Create a public key.
	CALCA_REMOVE, ///< Remove a public key.

	CALCA_END,    ///< Sentinel for end.
};

#endif /* COMPANY_TYPE_H */
