/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file network_admin.cpp Server part of the admin network protocol. */

#include "../stdafx.h"
#include "../debug.h"
#include "../strings_func.h"
#include "../date_func.h"
#include "core/network_game_info.h"
#include "network_admin.h"
#include "network_base.h"
#include "network_server.h"
#include "../command_func.h"
#include "../company_base.h"
#include "../console_func.h"
#include "../core/pool_func.hpp"
#include "../map_func.h"
#include "../rev.h"
#include "../game/game.hpp"

#include <numeric>

#include "../safeguards.h"


/* This file handles all the admin network commands. */

/** Redirection of the (remote) console to the admin. */
AdminID _redirect_console_to_admin = INVALID_ADMIN_ID;

/** The pool with sockets/clients. */
NetworkAdminSocketPool _networkadminsocket_pool("NetworkAdminSocket");
INSTANTIATE_POOL_METHODS(NetworkAdminSocket)

static NetworkAuthenticationDefaultPasswordProvider _admin_password_provider(_settings_client.network.admin_password); ///< Provides the password validation for the game's password.
static NetworkAuthenticationDefaultAuthorizedKeyHandler _admin_authorized_key_handler(_settings_client.network.admin_authorized_keys); ///< Provides the authorized key handling for the game authentication.

/** The timeout for authorisation of the client. */
static const std::chrono::seconds ADMIN_AUTHORISATION_TIMEOUT(10);


/** Frequencies, which may be registered for a certain update type. */
static const AdminUpdateFrequency _admin_update_type_frequencies[] = {
	ADMIN_FREQUENCY_POLL | ADMIN_FREQUENCY_DAILY | ADMIN_FREQUENCY_WEEKLY | ADMIN_FREQUENCY_MONTHLY | ADMIN_FREQUENCY_QUARTERLY | ADMIN_FREQUENCY_ANUALLY, ///< ADMIN_UPDATE_DATE
	ADMIN_FREQUENCY_POLL | ADMIN_FREQUENCY_AUTOMATIC,                                                                                                      ///< ADMIN_UPDATE_CLIENT_INFO
	ADMIN_FREQUENCY_POLL | ADMIN_FREQUENCY_AUTOMATIC,                                                                                                      ///< ADMIN_UPDATE_COMPANY_INFO
	ADMIN_FREQUENCY_POLL |                         ADMIN_FREQUENCY_WEEKLY | ADMIN_FREQUENCY_MONTHLY | ADMIN_FREQUENCY_QUARTERLY | ADMIN_FREQUENCY_ANUALLY, ///< ADMIN_UPDATE_COMPANY_ECONOMY
	ADMIN_FREQUENCY_POLL |                         ADMIN_FREQUENCY_WEEKLY | ADMIN_FREQUENCY_MONTHLY | ADMIN_FREQUENCY_QUARTERLY | ADMIN_FREQUENCY_ANUALLY, ///< ADMIN_UPDATE_COMPANY_STATS
	                       ADMIN_FREQUENCY_AUTOMATIC,                                                                                                      ///< ADMIN_UPDATE_CHAT
	                       ADMIN_FREQUENCY_AUTOMATIC,                                                                                                      ///< ADMIN_UPDATE_CONSOLE
	ADMIN_FREQUENCY_POLL,                                                                                                                                  ///< ADMIN_UPDATE_CMD_NAMES
	                       ADMIN_FREQUENCY_AUTOMATIC,                                                                                                      ///< ADMIN_UPDATE_CMD_LOGGING
	                       ADMIN_FREQUENCY_AUTOMATIC,                                                                                                      ///< ADMIN_UPDATE_GAMESCRIPT
};
/** Sanity check. */
static_assert(lengthof(_admin_update_type_frequencies) == ADMIN_UPDATE_END);

/**
 * Create a new socket for the server side of the admin network.
 * @param s The socket to connect with.
 */
ServerNetworkAdminSocketHandler::ServerNetworkAdminSocketHandler(SOCKET s) : NetworkAdminSocketHandler(s)
{
	this->status = ADMIN_STATUS_INACTIVE;
	this->connect_time = std::chrono::steady_clock::now();
}

/**
 * Clear everything related to this admin.
 */
ServerNetworkAdminSocketHandler::~ServerNetworkAdminSocketHandler()
{
	Debug(net, 3, "[admin] '{}' ({}) has disconnected", this->admin_name, this->admin_version);
	if (_redirect_console_to_admin == this->index) _redirect_console_to_admin = INVALID_ADMIN_ID;

	if (this->update_frequency[ADMIN_UPDATE_CONSOLE] & ADMIN_FREQUENCY_AUTOMATIC) {
		this->update_frequency[ADMIN_UPDATE_CONSOLE] = (AdminUpdateFrequency)0;
		DebugReconsiderSendRemoteMessages();
	}
}

/**
 * Whether a connection is allowed or not at this moment.
 * @return Whether the connection is allowed.
 */
/* static */ bool ServerNetworkAdminSocketHandler::AllowConnection()
{
	return _settings_client.network.AdminAuthenticationConfigured() && ServerNetworkAdminSocketHandler::CanAllocateItem();
}

/** Send the packets for the server sockets. */
/* static */ void ServerNetworkAdminSocketHandler::Send()
{
	for (ServerNetworkAdminSocketHandler *as : ServerNetworkAdminSocketHandler::Iterate()) {
		if (as->status <= ADMIN_STATUS_AUTHENTICATE && std::chrono::steady_clock::now() > as->connect_time + ADMIN_AUTHORISATION_TIMEOUT) {
			Debug(net, 2, "[admin] Admin did not send its authorisation within {} seconds", std::chrono::duration_cast<std::chrono::seconds>(ADMIN_AUTHORISATION_TIMEOUT).count());
			as->CloseConnection(true);
			continue;
		}
		if (as->writable) {
			as->SendPackets();
		}
	}
}

/**
 * Handle the acception of a connection.
 * @param s The socket of the new connection.
 * @param address The address of the peer.
 */
/* static */ void ServerNetworkAdminSocketHandler::AcceptConnection(SOCKET s, const NetworkAddress &address)
{
	ServerNetworkAdminSocketHandler *as = new ServerNetworkAdminSocketHandler(s);
	as->address = address; // Save the IP of the client
}

/***********
 * Sending functions for admin network
 ************/

/**
 * Send an error to the admin.
 * @param error The error to send.
 */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendError(NetworkErrorCode error)
{
	/* Whatever the error might be, authentication (keys) must be released as soon as possible. */
	this->authentication_handler = nullptr;

	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_ERROR);

	p->Send_uint8(error);
	this->SendPacket(std::move(p));

	std::string error_message = GetString(GetNetworkErrorMsg(error));

	Debug(net, 1, "[admin] The admin '{}' ({}) made an error and has been disconnected: '{}'", this->admin_name, this->admin_version, error_message);

	return this->CloseConnection(true);
}

/** Send the protocol version to the admin. */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendProtocol()
{
	this->status = ADMIN_STATUS_ACTIVE;

	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_PROTOCOL);

	/* announce the protocol version */
	p->Send_uint8(NETWORK_GAME_ADMIN_VERSION);

	for (int i = 0; i < ADMIN_UPDATE_END; i++) {
		p->Send_bool  (true);
		p->Send_uint16(i);
		p->Send_uint16(_admin_update_type_frequencies[i]);
	}

	p->Send_bool(false);
	this->SendPacket(std::move(p));

	return this->SendWelcome();
}

/** Send a welcome message to the admin. */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendWelcome()
{
	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_WELCOME);

	p->Send_string(_settings_client.network.server_name);
	p->Send_string(_openttd_revision);
	p->Send_bool  (_network_dedicated);

	p->Send_string(""); // Used to be map-name.
	p->Send_uint32(_settings_game.game_creation.generation_seed);
	p->Send_uint8 (to_underlying(_settings_game.game_creation.landscape));
	p->Send_uint32(CalTime::ConvertYMDToDate(_settings_game.game_creation.starting_year, 0, 1).base());
	p->Send_uint16(Map::SizeX());
	p->Send_uint16(Map::SizeY());

	this->SendPacket(std::move(p));

	return NETWORK_RECV_STATUS_OKAY;
}

/** Tell the admin we started a new game. */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendNewGame()
{
	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_NEWGAME);
	this->SendPacket(std::move(p));
	return NETWORK_RECV_STATUS_OKAY;
}

/** Tell the admin we're shutting down. */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendShutdown()
{
	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_SHUTDOWN);
	this->SendPacket(std::move(p));
	return NETWORK_RECV_STATUS_OKAY;
}

/** Tell the admin the date. */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendDate()
{
	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_DATE);

	p->Send_uint32(CalTime::CurDate().base());
	this->SendPacket(std::move(p));

	return NETWORK_RECV_STATUS_OKAY;
}

/**
 * Tell the admin that a client joined.
 * @param client_id The client that joined.
 */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendClientJoin(ClientID client_id)
{
	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_CLIENT_JOIN);

	p->Send_uint32(client_id);
	this->SendPacket(std::move(p));

	return NETWORK_RECV_STATUS_OKAY;
}

/**
 * Send an initial set of data from some client's information.
 * @param cs The socket of the client.
 * @param ci The information about the client.
 */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendClientInfo(const NetworkClientSocket *cs, const NetworkClientInfo *ci)
{
	/* Only send data when we're a proper client, not just someone trying to query the server. */
	if (ci == nullptr) return NETWORK_RECV_STATUS_OKAY;

	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_CLIENT_INFO);

	p->Send_uint32(ci->client_id);
	p->Send_string(cs == nullptr ? "" : const_cast<NetworkAddress &>(cs->client_address).GetHostname());
	p->Send_string(ci->client_name);
	p->Send_uint8 (0); // Used to be language
	p->Send_uint32(ci->join_date.base());
	p->Send_uint16 (ci->client_playas);

	this->SendPacket(std::move(p));

	return NETWORK_RECV_STATUS_OKAY;
}


/**
 * Send an update for some client's information.
 * @param ci The information about a client.
 */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendClientUpdate(const NetworkClientInfo *ci)
{
	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_CLIENT_UPDATE);

	p->Send_uint32(ci->client_id);
	p->Send_string(ci->client_name);
	p->Send_uint16 (ci->client_playas);

	this->SendPacket(std::move(p));

	return NETWORK_RECV_STATUS_OKAY;
}

/**
 * Tell the admin that a client quit.
 * @param client_id The client that quit.
 */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendClientQuit(ClientID client_id)
{
	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_CLIENT_QUIT);

	p->Send_uint32(client_id);
	this->SendPacket(std::move(p));

	return NETWORK_RECV_STATUS_OKAY;
}

/**
 * Tell the admin that a client made an error.
 * @param client_id The client that made the error.
 * @param error The error that was made.
 */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendClientError(ClientID client_id, NetworkErrorCode error)
{
	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_CLIENT_ERROR);

	p->Send_uint32(client_id);
	p->Send_uint8 (error);
	this->SendPacket(std::move(p));

	return NETWORK_RECV_STATUS_OKAY;
}

/**
 * Tell the admin that a new company was founded.
 * @param company_id The company that was founded.
 */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendCompanyNew(CompanyID company_id)
{
	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_COMPANY_NEW);
	p->Send_uint16 (company_id);

	this->SendPacket(std::move(p));

	return NETWORK_RECV_STATUS_OKAY;
}

/**
 * Send the admin some information about a company.
 * @param c The company to send the information about.
 */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendCompanyInfo(const Company *c)
{
	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_COMPANY_INFO);

	p->Send_uint16 (c->index);
	SetDParam(0, c->index);
	p->Send_string(GetString(STR_COMPANY_NAME));
	SetDParam(0, c->index);
	p->Send_string(GetString(STR_PRESIDENT_NAME));
	p->Send_uint8 (c->colour);
	p->Send_bool  (NetworkCompanyIsPassworded(c->index));
	p->Send_uint32(c->InauguratedDisplayYear());
	p->Send_bool  (c->is_ai);
	p->Send_uint8 (CeilDiv(c->months_of_bankruptcy, 3)); // send as quarters_of_bankruptcy

	this->SendPacket(std::move(p));

	return NETWORK_RECV_STATUS_OKAY;
}


/**
 * Send an update about a company.
 * @param c The company to send the update of.
 */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendCompanyUpdate(const Company *c)
{
	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_COMPANY_UPDATE);

	p->Send_uint16 (c->index);
	SetDParam(0, c->index);
	p->Send_string(GetString(STR_COMPANY_NAME));
	SetDParam(0, c->index);
	p->Send_string(GetString(STR_PRESIDENT_NAME));
	p->Send_uint8 (c->colour);
	p->Send_bool  (NetworkCompanyIsPassworded(c->index));
	p->Send_uint8 (CeilDiv(c->months_of_bankruptcy, 3)); // send as quarters_of_bankruptcy

	this->SendPacket(std::move(p));

	return NETWORK_RECV_STATUS_OKAY;
}

/**
 * Tell the admin that a company got removed.
 * @param company_id The company that got removed.
 * @param acrr The reason for removal, e.g. bankruptcy or merger.
 */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendCompanyRemove(CompanyID company_id, AdminCompanyRemoveReason acrr)
{
	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_COMPANY_REMOVE);

	p->Send_uint16(company_id);
	p->Send_uint8(acrr);

	this->SendPacket(std::move(p));

	return NETWORK_RECV_STATUS_OKAY;
}

/** Send economic information of all companies. */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendCompanyEconomy()
{
	for (const Company *company : Company::Iterate()) {
		/* Get the income. */
		Money income = -std::reduce(std::begin(company->yearly_expenses[0]), std::end(company->yearly_expenses[0]));

		auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_COMPANY_ECONOMY);

		p->Send_uint16(company->index);

		/* Current information. */
		p->Send_uint64(company->money);
		p->Send_uint64(company->current_loan);
		p->Send_uint64(income);
		p->Send_uint16(static_cast<uint16_t>(std::min<uint64_t>(UINT16_MAX, company->cur_economy.delivered_cargo.GetSum<OverflowSafeInt64>())));

		/* Send stats for the last 2 quarters. */
		for (uint i = 0; i < 2; i++) {
			p->Send_uint64(company->old_economy[i].company_value);
			p->Send_uint16(company->old_economy[i].performance_history);
			p->Send_uint16(static_cast<uint16_t>(std::min<uint64_t>(UINT16_MAX, company->old_economy[i].delivered_cargo.GetSum<OverflowSafeInt64>())));
		}

		this->SendPacket(std::move(p));
	}


	return NETWORK_RECV_STATUS_OKAY;
}

/** Send statistics about the companies. */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendCompanyStats()
{
	/* Fetch the latest version of the stats. */
	NetworkCompanyStats company_stats[MAX_COMPANIES];
	NetworkPopulateCompanyStats(company_stats);

	/* Go through all the companies. */
	for (const Company *company : Company::Iterate()) {
		auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_COMPANY_STATS);

		/* Send the information. */
		p->Send_uint16(company->index);

		for (uint i = 0; i < NETWORK_VEH_END; i++) {
			p->Send_uint16(company_stats[company->index].num_vehicle[i]);
		}

		for (uint i = 0; i < NETWORK_VEH_END; i++) {
			p->Send_uint16(company_stats[company->index].num_station[i]);
		}

		this->SendPacket(std::move(p));
	}

	return NETWORK_RECV_STATUS_OKAY;
}

/**
 * Send a chat message.
 * @param action The action associated with the message.
 * @param desttype The destination type.
 * @param client_id The origin of the chat message.
 * @param msg The actual message.
 * @param data Arbitrary extra data.
 */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendChat(NetworkAction action, DestType desttype, ClientID client_id, const std::string &msg, NetworkTextMessageData data)
{
	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_CHAT);

	p->Send_uint8 (action);
	p->Send_uint8 (desttype);
	p->Send_uint32(client_id);
	p->Send_string(msg);
	data.send(*p);

	this->SendPacket(std::move(p));
	return NETWORK_RECV_STATUS_OKAY;
}

/**
 * Send a notification indicating the rcon command has completed.
 * @param command The original command sent.
 */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendRconEnd(const std::string_view command)
{
	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_RCON_END);

	p->Send_string(command);
	this->SendPacket(std::move(p));

	return NETWORK_RECV_STATUS_OKAY;
}

/**
 * Send the reply of an rcon command.
 * @param colour The colour of the text.
 * @param result The result of the command.
 */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendRcon(uint16_t colour, const std::string_view result)
{
	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_RCON);

	p->Send_uint16(colour);
	p->Send_string(result);
	this->SendPacket(std::move(p));

	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ServerNetworkAdminSocketHandler::Receive_ADMIN_RCON(Packet &p)
{
	if (this->status <= ADMIN_STATUS_AUTHENTICATE) return this->SendError(NETWORK_ERROR_NOT_EXPECTED);

	std::string command = p.Recv_string(NETWORK_RCONCOMMAND_LENGTH);

	Debug(net, 3, "[admin] Rcon command from '{}' ({}): {}", this->admin_name, this->admin_version, command);

	_redirect_console_to_admin = this->index;
	IConsoleCmdExec(command);
	_redirect_console_to_admin = INVALID_ADMIN_ID;
	return this->SendRconEnd(command);
}

NetworkRecvStatus ServerNetworkAdminSocketHandler::Receive_ADMIN_GAMESCRIPT(Packet &p)
{
	if (this->status <= ADMIN_STATUS_AUTHENTICATE) return this->SendError(NETWORK_ERROR_NOT_EXPECTED);

	std::string json = p.Recv_string(NETWORK_GAMESCRIPT_JSON_LENGTH);

	Debug(net, 6, "[admin] GameScript JSON from '{}' ({}): {}", this->admin_name, this->admin_version, json);

	Game::NewEvent(new ScriptEventAdminPort(json));
	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ServerNetworkAdminSocketHandler::Receive_ADMIN_PING(Packet &p)
{
	if (this->status <= ADMIN_STATUS_AUTHENTICATE) return this->SendError(NETWORK_ERROR_NOT_EXPECTED);

	uint32_t d1 = p.Recv_uint32();

	Debug(net, 6, "[admin] Ping from '{}' ({}): {}", this->admin_name, this->admin_version, d1);

	return this->SendPong(d1);
}

/**
 * Send console output of other clients.
 * @param origin The origin of the string.
 * @param string The string that's put on the console.
 */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendConsole(const std::string_view origin, const std::string_view string)
{
	/* If the length of both strings, plus the 2 '\0' terminations and 3 bytes of the packet
	 * are bigger than the MTU, just ignore the message. Better safe than sorry. It should
	 * never occur though as the longest strings are chat messages, which are still 30%
	 * smaller than COMPAT_MTU. */
	if (origin.size() + string.size() + 2 + 3 >= COMPAT_MTU) return NETWORK_RECV_STATUS_OKAY;

	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_CONSOLE);

	p->Send_string(origin);
	p->Send_string(string);
	this->SendPacket(std::move(p));

	return NETWORK_RECV_STATUS_OKAY;
}

/**
 * Send GameScript JSON output.
 * @param json The JSON string.
 */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendGameScript(const std::string_view json)
{
	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_GAMESCRIPT);

	p->Send_string(json);
	this->SendPacket(std::move(p));

	return NETWORK_RECV_STATUS_OKAY;
}

/** Send ping-reply (pong) to admin **/
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendPong(uint32_t d1)
{
	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_PONG);

	p->Send_uint32(d1);
	this->SendPacket(std::move(p));

	return NETWORK_RECV_STATUS_OKAY;
}

/** Send the names of the commands. */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendCmdNames()
{
	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_CMD_NAMES);

	for (uint i = 0; i < CMD_END; i++) {
		const char *cmdname = GetCommandName(static_cast<Commands>(i));

		/* Should COMPAT_MTU be exceeded, start a new packet
		 * (magic 5: 1 bool "more data" and one uint16_t "command id", one
		 * byte for string '\0' termination and 1 bool "no more data" */
		if (!p->CanWriteToPacket(strlen(cmdname) + 5)) {
			p->Send_bool(false);
			this->SendPacket(std::move(p));

			p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_CMD_NAMES);
		}

		p->Send_bool(true);
		p->Send_uint16(i);
		p->Send_string(cmdname);
	}

	/* Marker to notify the end of the packet has been reached. */
	p->Send_bool(false);
	this->SendPacket(std::move(p));

	return NETWORK_RECV_STATUS_OKAY;
}

/**
 * Send a command for logging purposes.
 * @param client_id The client executing the command.
 * @param cp The command that would be executed.
 */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendCmdLogging(ClientID client_id, const CommandPacket &cp)
{
	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_CMD_LOGGING);

	p->Send_uint32(client_id);
	p->Send_uint16 (cp.company);
	p->Send_uint16(cp.command_container.cmd);

	size_t payload_pos = p->GetSendOffset();
	p->Send_uint16(0);
	p->Send_uint32(cp.command_container.tile.base());
	cp.command_container.payload->Serialise(p->AsBufferSerialisationRef());
	p->SendAtOffset_uint16(payload_pos, (uint16_t)(p->GetSendOffset() - payload_pos - 2));

	p->Send_uint32(cp.frame);

	this->SendPacket(std::move(p));

	return NETWORK_RECV_STATUS_OKAY;
}

/***********
 * Receiving functions
 ************/

NetworkRecvStatus ServerNetworkAdminSocketHandler::Receive_ADMIN_JOIN(Packet &p)
{
	if (this->status != ADMIN_STATUS_INACTIVE) return this->SendError(NETWORK_ERROR_NOT_EXPECTED);

	if (!_settings_client.network.allow_insecure_admin_login) {
		/* You're not authorized to login using this method. */
		return this->SendError(NETWORK_ERROR_NOT_AUTHORIZED);
	}

	std::string password = p.Recv_string(NETWORK_PASSWORD_LENGTH);

	if (_settings_client.network.admin_password.empty() ||
			_settings_client.network.admin_password.compare(password) != 0) {
		/* Password is invalid */
		return this->SendError(NETWORK_ERROR_WRONG_PASSWORD);
	}

	this->admin_name = p.Recv_string(NETWORK_CLIENT_NAME_LENGTH);
	this->admin_version = p.Recv_string(NETWORK_REVISION_LENGTH);

	if (this->admin_name.empty() || this->admin_version.empty()) {
		/* no name or version supplied */
		return this->SendError(NETWORK_ERROR_ILLEGAL_PACKET);
	}

	Debug(net, 3, "[admin] '{}' ({}) has connected", this->admin_name, this->admin_version);

	return this->SendProtocol();
}

NetworkRecvStatus ServerNetworkAdminSocketHandler::Receive_ADMIN_QUIT(Packet &)
{
	/* The admin is leaving nothing else to do */
	return this->CloseConnection();
}

NetworkRecvStatus ServerNetworkAdminSocketHandler::Receive_ADMIN_UPDATE_FREQUENCY(Packet &p)
{
	if (this->status <= ADMIN_STATUS_AUTHENTICATE) return this->SendError(NETWORK_ERROR_NOT_EXPECTED);

	AdminUpdateType type = (AdminUpdateType)p.Recv_uint16();
	AdminUpdateFrequency freq = (AdminUpdateFrequency)p.Recv_uint16();

	if (type >= ADMIN_UPDATE_END || (_admin_update_type_frequencies[type] & freq) != freq) {
		/* The server does not know of this UpdateType. */
		Debug(net, 1, "[admin] Not supported update frequency {} ({}) from '{}' ({})", type, freq, this->admin_name, this->admin_version);
		return this->SendError(NETWORK_ERROR_ILLEGAL_PACKET);
	}

	this->update_frequency[type] = freq;

	if (type == ADMIN_UPDATE_CONSOLE) DebugReconsiderSendRemoteMessages();

	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ServerNetworkAdminSocketHandler::Receive_ADMIN_POLL(Packet &p)
{
	if (this->status <= ADMIN_STATUS_AUTHENTICATE) return this->SendError(NETWORK_ERROR_NOT_EXPECTED);

	AdminUpdateType type = (AdminUpdateType)p.Recv_uint8();
	uint32_t d1 = p.Recv_uint32();

	switch (type) {
		case ADMIN_UPDATE_DATE:
			/* The admin is requesting the current date. */
			this->SendDate();
			break;

		case ADMIN_UPDATE_CLIENT_INFO:
			/* The admin is requesting client info. */
			if (d1 == UINT32_MAX) {
				this->SendClientInfo(nullptr, NetworkClientInfo::GetByClientID(CLIENT_ID_SERVER));
				for (const NetworkClientSocket *cs : NetworkClientSocket::Iterate()) {
					this->SendClientInfo(cs, cs->GetInfo());
				}
			} else {
				if (d1 == CLIENT_ID_SERVER) {
					this->SendClientInfo(nullptr, NetworkClientInfo::GetByClientID(CLIENT_ID_SERVER));
				} else {
					const NetworkClientSocket *cs = NetworkClientSocket::GetByClientID((ClientID)d1);
					if (cs != nullptr) this->SendClientInfo(cs, cs->GetInfo());
				}
			}
			break;

		case ADMIN_UPDATE_COMPANY_INFO:
			/* The admin is asking for company info. */
			if (d1 == UINT32_MAX) {
				for (const Company *company : Company::Iterate()) {
					this->SendCompanyInfo(company);
				}
			} else {
				const Company *company = Company::GetIfValid(d1);
				if (company != nullptr) this->SendCompanyInfo(company);
			}
			break;

		case ADMIN_UPDATE_COMPANY_ECONOMY:
			/* The admin is requesting economy info. */
			this->SendCompanyEconomy();
			break;

		case ADMIN_UPDATE_COMPANY_STATS:
			/* the admin is requesting company stats. */
			this->SendCompanyStats();
			break;

		case ADMIN_UPDATE_CMD_NAMES:
			/* The admin is requesting the names of DoCommands. */
			this->SendCmdNames();
			break;

		default:
			/* An unsupported "poll" update type. */
			Debug(net, 1, "[admin] Not supported poll {} ({}) from '{}' ({}).", type, d1, this->admin_name, this->admin_version);
			return this->SendError(NETWORK_ERROR_ILLEGAL_PACKET);
	}

	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ServerNetworkAdminSocketHandler::Receive_ADMIN_CHAT(Packet &p)
{
	if (this->status <= ADMIN_STATUS_AUTHENTICATE) return this->SendError(NETWORK_ERROR_NOT_EXPECTED);

	NetworkAction action = (NetworkAction)p.Recv_uint8();
	DestType desttype = (DestType)p.Recv_uint8();
	int dest = p.Recv_uint32();

	std::string msg = p.Recv_string(NETWORK_CHAT_LENGTH);

	switch (action) {
		case NETWORK_ACTION_CHAT:
		case NETWORK_ACTION_CHAT_CLIENT:
		case NETWORK_ACTION_CHAT_COMPANY:
		case NETWORK_ACTION_SERVER_MESSAGE:
			NetworkServerSendChat(action, desttype, dest, msg, _network_own_client_id, 0, true);
			break;

		default:
			Debug(net, 1, "[admin] Invalid chat action {} from admin '{}' ({}).", action, this->admin_name, this->admin_version);
			return this->SendError(NETWORK_ERROR_ILLEGAL_PACKET);
	}

	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ServerNetworkAdminSocketHandler::Receive_ADMIN_EXTERNAL_CHAT(Packet &p)
{
	if (this->status <= ADMIN_STATUS_AUTHENTICATE) return this->SendError(NETWORK_ERROR_NOT_EXPECTED);

	std::string source = p.Recv_string(NETWORK_CHAT_LENGTH);
	TextColour colour = (TextColour)p.Recv_uint16();
	std::string user = p.Recv_string(NETWORK_CHAT_LENGTH);
	std::string msg = p.Recv_string(NETWORK_CHAT_LENGTH);

	if (!IsValidConsoleColour(colour)) {
		Debug(net, 1, "[admin] Not supported chat colour {} ({}, {}, {}) from '{}' ({}).", (uint16_t)colour, source, user, msg, this->admin_name, this->admin_version);
		return this->SendError(NETWORK_ERROR_ILLEGAL_PACKET);
	}

	NetworkServerSendExternalChat(source, colour, user, msg);

	return NETWORK_RECV_STATUS_OKAY;
}

/*
 * Secure authentication send and receive methods.
 */

NetworkRecvStatus ServerNetworkAdminSocketHandler::Receive_ADMIN_JOIN_SECURE(Packet &p)
{
	if (this->status != ADMIN_STATUS_INACTIVE) return this->SendError(NETWORK_ERROR_NOT_EXPECTED);

	this->admin_name = p.Recv_string(NETWORK_CLIENT_NAME_LENGTH);
	this->admin_version = p.Recv_string(NETWORK_REVISION_LENGTH);
	NetworkAuthenticationMethodMask method_mask{p.Recv_uint16()};

	/* Always exclude key exchange only, as that provides no credential checking. */
	method_mask.Reset(NetworkAuthenticationMethod::X25519_KeyExchangeOnly);

	if (this->admin_name.empty() || this->admin_version.empty()) {
		/* No name or version supplied. */
		return this->SendError(NETWORK_ERROR_ILLEGAL_PACKET);
	}

	auto handler = NetworkAuthenticationServerHandler::Create(&_admin_password_provider, &_admin_authorized_key_handler, method_mask);
	if (!handler->CanBeUsed()) return this->SendError(NETWORK_ERROR_NO_AUTHENTICATION_METHOD_AVAILABLE);

	this->authentication_handler = std::move(handler);
	Debug(net, 3, "[admin] '{}' ({}) has connected", this->admin_name, this->admin_version);

	return this->SendAuthRequest();
}

NetworkRecvStatus ServerNetworkAdminSocketHandler::SendAuthRequest()
{
	this->status = ADMIN_STATUS_AUTHENTICATE;

	Debug(net, 6, "[admin] '{}' ({}) authenticating using {}", this->admin_name, this->admin_version, this->authentication_handler->GetName());

	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_AUTH_REQUEST);
	this->authentication_handler->SendRequest(*p);

	this->SendPacket(std::move(p));

	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ServerNetworkAdminSocketHandler::SendEnableEncryption()
{
	if (this->status != ADMIN_STATUS_AUTHENTICATE) return this->SendError(NETWORK_ERROR_NOT_EXPECTED);

	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_ENABLE_ENCRYPTION);
	this->authentication_handler->SendEnableEncryption(*p);
	this->SendPacket(std::move(p));

	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ServerNetworkAdminSocketHandler::Receive_ADMIN_AUTH_RESPONSE(Packet &p)
{
	if (this->status != ADMIN_STATUS_AUTHENTICATE) return this->SendError(NETWORK_ERROR_NOT_EXPECTED);

	switch (this->authentication_handler->ReceiveResponse(p)) {
		case NetworkAuthenticationServerHandler::ResponseResult::Authenticated:
			Debug(net, 3, "[admin] '{}' ({}) authenticated", this->admin_name, this->admin_version);

			this->SendEnableEncryption();

			this->receive_encryption_handler = this->authentication_handler->CreateClientToServerEncryptionHandler();
			this->send_encryption_handler = this->authentication_handler->CreateServerToClientEncryptionHandler();
			this->authentication_handler = nullptr;
			return this->SendProtocol();

		case NetworkAuthenticationServerHandler::ResponseResult::RetryNextMethod:
			Debug(net, 6, "[admin] '{}' ({}) authentication failed, trying next method", this->admin_name, this->admin_version);
			return this->SendAuthRequest();

		case NetworkAuthenticationServerHandler::ResponseResult::NotAuthenticated:
		default:
			Debug(net, 3, "[admin] '{}' ({}) authentication failed", this->admin_name, this->admin_version);
			return this->SendError(NETWORK_ERROR_WRONG_PASSWORD);
	}
}

/*
 * Useful wrapper functions
 */

/**
 * Notify the admin network of a new client (if they did opt in for the respective update).
 * @param cs the client info.
 * @param new_client if this is a new client, send the respective packet too.
 */
void NetworkAdminClientInfo(const NetworkClientSocket *cs, bool new_client)
{
	for (ServerNetworkAdminSocketHandler *as : ServerNetworkAdminSocketHandler::IterateActive()) {
		if (as->update_frequency[ADMIN_UPDATE_CLIENT_INFO] & ADMIN_FREQUENCY_AUTOMATIC) {
			as->SendClientInfo(cs, cs->GetInfo());
			if (new_client) {
				as->SendClientJoin(cs->client_id);
			}
		}
	}
}

/**
 * Notify the admin network of a client update (if they did opt in for the respective update).
 * @param ci the client info.
 */
void NetworkAdminClientUpdate(const NetworkClientInfo *ci)
{
	for (ServerNetworkAdminSocketHandler *as : ServerNetworkAdminSocketHandler::IterateActive()) {
		if (as->update_frequency[ADMIN_UPDATE_CLIENT_INFO] & ADMIN_FREQUENCY_AUTOMATIC) {
			as->SendClientUpdate(ci);
		}
	}
}

/**
 * Notify the admin network that a client quit (if they have opt in for the respective update).
 * @param client_id of the client that quit.
 */
void NetworkAdminClientQuit(ClientID client_id)
{
	for (ServerNetworkAdminSocketHandler *as : ServerNetworkAdminSocketHandler::IterateActive()) {
		if (as->update_frequency[ADMIN_UPDATE_CLIENT_INFO] & ADMIN_FREQUENCY_AUTOMATIC) {
			as->SendClientQuit(client_id);
		}
	}
}

/**
 * Notify the admin network of a client error (if they have opt in for the respective update).
 * @param client_id the client that made the error.
 * @param error_code the error that was caused.
 */
void NetworkAdminClientError(ClientID client_id, NetworkErrorCode error_code)
{
	for (ServerNetworkAdminSocketHandler *as : ServerNetworkAdminSocketHandler::IterateActive()) {
		if (as->update_frequency[ADMIN_UPDATE_CLIENT_INFO] & ADMIN_FREQUENCY_AUTOMATIC) {
			as->SendClientError(client_id, error_code);
		}
	}
}

/**
 * Notify the admin network of a new company.
 * @param company the company of which details will be sent into the admin network.
 */
void NetworkAdminCompanyNew(const Company *company)
{
	if (company == nullptr) {
		Debug(net, 1, "[admin] Empty company given for update");
		return;
	}

	for (ServerNetworkAdminSocketHandler *as : ServerNetworkAdminSocketHandler::IterateActive()) {
		if (as->update_frequency[ADMIN_UPDATE_COMPANY_INFO] != ADMIN_FREQUENCY_AUTOMATIC) continue;

		as->SendCompanyNew(company->index);
		as->SendCompanyInfo(company);
	}
}

/**
 * Notify the admin network of company updates.
 * @param company company of which updates are going to be sent into the admin network.
 */
void NetworkAdminCompanyUpdate(const Company *company)
{
	if (company == nullptr) return;

	for (ServerNetworkAdminSocketHandler *as : ServerNetworkAdminSocketHandler::IterateActive()) {
		if (as->update_frequency[ADMIN_UPDATE_COMPANY_INFO] != ADMIN_FREQUENCY_AUTOMATIC) continue;

		as->SendCompanyUpdate(company);
	}
}

/**
 * Notify the admin network of a company to be removed (including the reason why).
 * @param company_id ID of the company that got removed.
 * @param bcrr the reason why the company got removed (e.g. bankruptcy).
 */
void NetworkAdminCompanyRemove(CompanyID company_id, AdminCompanyRemoveReason bcrr)
{
	for (ServerNetworkAdminSocketHandler *as : ServerNetworkAdminSocketHandler::IterateActive()) {
		as->SendCompanyRemove(company_id, bcrr);
	}
}


/**
 * Send chat to the admin network (if they did opt in for the respective update).
 */
void NetworkAdminChat(NetworkAction action, DestType desttype, ClientID client_id, const std::string &msg, NetworkTextMessageData data, bool from_admin)
{
	if (from_admin) return;

	for (ServerNetworkAdminSocketHandler *as : ServerNetworkAdminSocketHandler::IterateActive()) {
		if (as->update_frequency[ADMIN_UPDATE_CHAT] & ADMIN_FREQUENCY_AUTOMATIC) {
			as->SendChat(action, desttype, client_id, msg, data);
		}
	}
}

/**
 * Pass the rcon reply to the admin.
 * @param admin_index The admin to give the reply.
 * @param colour_code The colour of the string.
 * @param string      The string to show.
 */
void NetworkServerSendAdminRcon(AdminID admin_index, TextColour colour_code, const std::string_view string)
{
	ServerNetworkAdminSocketHandler::Get(admin_index)->SendRcon(colour_code, string);
}

/**
 * Send console to the admin network (if they did opt in for the respective update).
 * @param origin the origin of the message.
 * @param string the message as printed on the console.
 */
void NetworkAdminConsole(const std::string_view origin, const std::string_view string)
{
	for (ServerNetworkAdminSocketHandler *as : ServerNetworkAdminSocketHandler::IterateActive()) {
		if (as->update_frequency[ADMIN_UPDATE_CONSOLE] & ADMIN_FREQUENCY_AUTOMATIC) {
			as->SendConsole(origin, string);
		}
	}
}

/**
 * Send GameScript JSON to the admin network (if they did opt in for the respective update).
 * @param json The JSON data as received from the GameScript.
 */
void NetworkAdminGameScript(const std::string_view json)
{
	for (ServerNetworkAdminSocketHandler *as : ServerNetworkAdminSocketHandler::IterateActive()) {
		if (as->update_frequency[ADMIN_UPDATE_GAMESCRIPT] & ADMIN_FREQUENCY_AUTOMATIC) {
			as->SendGameScript(json);
		}
	}
}

/**
 * Distribute CommandPacket details over the admin network for logging purposes.
 * @param owner The owner of the CommandPacket (who sent us the CommandPacket).
 * @param cp    The CommandPacket to be distributed.
 */
void NetworkAdminCmdLogging(const NetworkClientSocket *owner, const CommandPacket &cp)
{
	ClientID client_id = owner == nullptr ? _network_own_client_id : owner->client_id;

	for (ServerNetworkAdminSocketHandler *as : ServerNetworkAdminSocketHandler::IterateActive()) {
		if (as->update_frequency[ADMIN_UPDATE_CMD_LOGGING] & ADMIN_FREQUENCY_AUTOMATIC) {
			as->SendCmdLogging(client_id, cp);
		}
	}
}

/**
 * Send a Welcome packet to all connected admins
 */
void ServerNetworkAdminSocketHandler::WelcomeAll()
{
	for (ServerNetworkAdminSocketHandler *as : ServerNetworkAdminSocketHandler::IterateActive()) {
		as->SendWelcome();
	}
}

/**
 * Send (push) updates to the admin network as they have registered for these updates.
 * @param freq the frequency to be processed.
 */
void NetworkAdminUpdate(AdminUpdateFrequency freq)
{
	for (ServerNetworkAdminSocketHandler *as : ServerNetworkAdminSocketHandler::IterateActive()) {
		for (int i = 0; i < ADMIN_UPDATE_END; i++) {
			if (as->update_frequency[i] & freq) {
				/* Update the admin for the required details */
				switch (i) {
					case ADMIN_UPDATE_DATE:
						as->SendDate();
						break;

					case ADMIN_UPDATE_COMPANY_ECONOMY:
						as->SendCompanyEconomy();
						break;

					case ADMIN_UPDATE_COMPANY_STATS:
						as->SendCompanyStats();
						break;

					default: NOT_REACHED();
				}
			}
		}
	}
}
