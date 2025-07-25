/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file network_client.cpp Client part of the network protocol. */

#include "../stdafx.h"
#include "network_gui.h"
#include "../sl/saveload.h"
#include "../sl/saveload_filter.h"
#include "../command_func.h"
#include "../console_func.h"
#include "../strings_func.h"
#include "../window_func.h"
#include "../company_func.h"
#include "../company_base.h"
#include "../company_cmd.h"
#include "../company_gui.h"
#include "../core/random_func.hpp"
#include "../date_func.h"
#include "../gfx_func.h"
#include "../error.h"
#include "../rev.h"
#include "network.h"
#include "network_base.h"
#include "network_client.h"
#include "network_gamelist.h"
#include "../core/backup_type.hpp"
#include "../thread.h"
#include "../social_integration.h"
#include "../crashlog.h"
#include "../core/checksum_func.hpp"
#include "../core/alloc_func.hpp"
#include "../core/alloc_type.hpp"
#include "../fileio_func.h"
#include "../debug_settings.h"
#include "../3rdparty/monocypher/monocypher.h"

#include <tuple>

#include "table/strings.h"

#include "../safeguards.h"

/* This file handles all the client-commands */

static void ResetClientConnectionKeyStates();

/** Read some packets, and when do use that data as initial load filter. */
struct PacketReader : LoadFilter {
	static const size_t CHUNK = 32 * 1024;  ///< 32 KiB chunks of memory.

	std::vector<std::unique_ptr<char, FreeDeleter>> blocks; ///< Buffer with blocks of allocated memory.
	char *buf;                                              ///< Buffer we're going to write to/read from.
	char *bufe;                                             ///< End of the buffer we write to/read from.
	size_t current_block;                                   ///< Index of current block
	size_t written_bytes;                                   ///< The total number of bytes we've written.
	size_t read_bytes;                                      ///< The total number of read bytes.

	/** Initialise everything. */
	PacketReader() : LoadFilter(nullptr), buf(nullptr), bufe(nullptr), current_block(0), written_bytes(0), read_bytes(0) {}

	/**
	 * Simple wrapper around fwrite to be able to pass it to Packet's TransferOut.
	 * @param destination The reader to add the data to.
	 * @param source      The buffer to read data from.
	 * @param amount      The number of bytes to copy.
	 * @return The number of bytes that were copied.
	 */
	static inline ssize_t TransferOutMemCopy(PacketReader *destination, const char *source, size_t amount)
	{
		memcpy(destination->buf, source, amount);
		destination->buf += amount;
		destination->written_bytes += amount;
		return amount;
	}

	/**
	 * Add a packet to this buffer.
	 * @param p The packet to add.
	 */
	void AddPacket(Packet &p)
	{
		assert(this->read_bytes == 0);
		p.TransferOutWithLimit(TransferOutMemCopy, this->bufe - this->buf, this);

		/* Did everything fit in the current chunk, then we're done. */
		if (p.RemainingBytesToTransfer() == 0) return;

		/* Allocate a new chunk and add the remaining data. */
		this->buf = MallocT<char>(CHUNK);
		this->bufe = this->buf + CHUNK;
		this->blocks.emplace_back(this->buf);

		p.TransferOutWithLimit(TransferOutMemCopy, this->bufe - this->buf, this);
	}

	size_t Read(uint8_t *rbuf, size_t size) override
	{
		/* Limit the amount to read to whatever we still have. */
		size_t ret_size = size = std::min(this->written_bytes - this->read_bytes, size);
		this->read_bytes += ret_size;
		const uint8_t *rbufe = rbuf + ret_size;

		while (rbuf != rbufe) {
			if (this->buf == this->bufe) {
				this->current_block++;
				this->buf = this->blocks[this->current_block].get();
				this->bufe = this->buf + CHUNK;
			}

			size_t to_write = std::min(this->bufe - this->buf, rbufe - rbuf);
			memcpy(rbuf, this->buf, to_write);
			rbuf += to_write;
			this->buf += to_write;
		}

		return ret_size;
	}

	void Reset() override
	{
		this->read_bytes = 0;

		this->current_block = 0;
		this->buf = this->blocks[this->current_block].get();
		this->bufe = this->buf + CHUNK;
	}
};


/**
 * Create an emergency savegame when the network connection is lost.
 */
void ClientNetworkEmergencySave()
{
	if (!_settings_client.gui.autosave_on_network_disconnect) return;
	if (!_networking) return;
	if (!ClientNetworkGameSocketHandler::EmergencySavePossible()) return;

	static FiosNumberedSaveName _netsave_ctr("netsave");
	DoAutoOrNetsave(_netsave_ctr, false);
}


/**
 * Create a new socket for the client side of the game connection.
 * @param s The socket to connect with.
 */
ClientNetworkGameSocketHandler::ClientNetworkGameSocketHandler(SOCKET s, std::string connection_string)
	: NetworkGameSocketHandler(s), connection_string(std::move(connection_string))
{
	assert(ClientNetworkGameSocketHandler::my_client == nullptr);
	ClientNetworkGameSocketHandler::my_client = this;
}

/** Clear whatever we assigned. */
ClientNetworkGameSocketHandler::~ClientNetworkGameSocketHandler()
{
	assert(ClientNetworkGameSocketHandler::my_client == this);
	ClientNetworkGameSocketHandler::my_client = nullptr;
	_network_settings_access = false;

	delete this->GetInfo();

	if (this->desync_log_file.has_value()) {
		if (!this->server_desync_log.empty()) {
			fwrite("\n", 1, 1, *this->desync_log_file);
			fwrite(this->server_desync_log.data(), 1, this->server_desync_log.size(), *this->desync_log_file);
		}
		this->desync_log_file.reset();
	}

	ResetClientConnectionKeyStates();
}

NetworkRecvStatus ClientNetworkGameSocketHandler::CloseConnection(NetworkRecvStatus status)
{
	assert(status != NETWORK_RECV_STATUS_OKAY);
	if (this->IsPendingDeletion()) return status;

	assert(this->sock != INVALID_SOCKET);
	if (this->status == STATUS_CLOSING) return status;

	if (!this->HasClientQuit()) {
		Debug(net, 3, "Closed client connection {}", this->client_id);

		SetBlocking(this->sock);

		this->SendPackets(true);

		ShutdownSocket(this->sock, false, true, 2);

		/* Wait a number of ticks so our leave message can reach the server.
		 * This is especially needed for Windows servers as they seem to get
		 * the "socket is closed" message before receiving our leave message,
		 * which would trigger the server to close the connection as well. */
		CSleep(3 * MILLISECONDS_PER_TICK);
	}

	Debug(net, 1, "Shutdown client connection {}", this->client_id);

	if (status == NETWORK_RECV_STATUS_DESYNC) {
		this->status = STATUS_CLOSING;
		this->ignore_close = true;
		this->ReceivePackets();
	}

	this->DeferDeletion();

	return status;
}

/**
 * Handle an error coming from the client side.
 * @param res The "error" that happened.
 */
void ClientNetworkGameSocketHandler::ClientError(NetworkRecvStatus res)
{
	if (this->IsPendingDeletion()) return;

	/* First, send a CLIENT_ERROR to the server, so it knows we are
	 *  disconnected (and why!) */
	NetworkErrorCode errorno;

	/* We just want to close the connection.. */
	if (res == NETWORK_RECV_STATUS_CLOSE_QUERY) {
		this->NetworkSocketHandler::MarkClosed();
		this->CloseConnection(res);
		_networking = false;

		CloseWindowById(WC_NETWORK_STATUS_WINDOW, WN_NETWORK_STATUS_WINDOW_JOIN);
		return;
	}

	switch (res) {
		case NETWORK_RECV_STATUS_DESYNC:          errorno = NETWORK_ERROR_DESYNC; break;
		case NETWORK_RECV_STATUS_SAVEGAME:        errorno = NETWORK_ERROR_SAVEGAME_FAILED; break;
		case NETWORK_RECV_STATUS_NEWGRF_MISMATCH: errorno = NETWORK_ERROR_NEWGRF_MISMATCH; break;
		default:                                  errorno = NETWORK_ERROR_GENERAL; break;
	}

	if (res == NETWORK_RECV_STATUS_SERVER_ERROR || res == NETWORK_RECV_STATUS_SERVER_FULL ||
			res == NETWORK_RECV_STATUS_SERVER_BANNED) {
		/* This means the server closed the connection. Emergency save is
		 * already created if this was appropriate during handling of the
		 * disconnect. */
		this->SendPackets(true);
		this->CloseConnection(res);
	} else {
		/* This means we as client made a boo-boo. */
		SendError(errorno, res);

		/* Close connection before we make an emergency save, as the save can
		 * take a bit of time; better that the server doesn't stall while we
		 * are doing the save, and already disconnects us. */
		this->SendPackets(true);
		this->CloseConnection(res);
		ClientNetworkEmergencySave();
	}

	CloseNetworkClientWindows();
	CloseWindowById(WC_NETWORK_STATUS_WINDOW, WN_NETWORK_STATUS_WINDOW_JOIN);

	if (_game_mode != GM_MENU) _switch_mode = SM_MENU;
	_networking = false;
}


/**
 * Check whether we received/can send some data from/to the server and
 * when that's the case handle it appropriately.
 * @return true when everything went okay.
 */
/* static */ bool ClientNetworkGameSocketHandler::Receive()
{
	if (my_client->CanSendReceive()) {
		NetworkRecvStatus res = my_client->ReceivePackets();
		if (res != NETWORK_RECV_STATUS_OKAY) {
			/* The client made an error of which we can not recover.
			 * Close the connection and drop back to the main menu. */
			my_client->ClientError(res);
			return false;
		}
	}
	return _networking;
}

/** Send the packets of this socket handler. */
/* static */ void ClientNetworkGameSocketHandler::Send()
{
	my_client->SendPackets();
	if (my_client != nullptr) my_client->CheckConnection();
}

/**
 * Actual game loop for the client.
 * @return Whether everything went okay, or not.
 */
/* static */ bool ClientNetworkGameSocketHandler::GameLoop()
{
	_frame_counter++;

	const size_t total_sync_records = _network_sync_records.size();
	_network_sync_records.push_back({ _frame_counter, _random.state[0], _state_checksum.state });
	_record_sync_records = true;

	NetworkExecuteLocalCommandQueue();

	StateGameLoop();

	_network_sync_records.push_back({ NSRE_FRAME_DONE, _random.state[0], _state_checksum.state });
	_network_sync_record_counts.push_back((uint)(_network_sync_records.size() - total_sync_records));
	_record_sync_records = false;

	/* Check if we are in sync! */
	if (_sync_frame != 0) {
		if (_sync_frame == _frame_counter) {
			if (_sync_seed_1 != _random.state[0] || (_sync_state_checksum != _state_checksum.state && !HasChickenBit(DCBF_MP_NO_STATE_CSUM_CHECK))) {
				DesyncExtraInfo info;
				if (_sync_seed_1 != _random.state[0]) info.flags |= DesyncExtraInfo::DEIF_RAND;
				if (_sync_state_checksum != _state_checksum.state) info.flags |= DesyncExtraInfo::DEIF_STATE;

				ShowNetworkError(STR_NETWORK_ERROR_DESYNC);
				Debug(desync, 1, "sync_err: {} {{{:X}, {:X}}} != {{{:X}, {:X}}}",
						debug_date_dumper().HexDate(), _sync_seed_1, _sync_state_checksum, _random.state[0], _state_checksum.state);
				Debug(net, 0, "Sync error detected!");

				std::string desync_log;
				DesyncDeferredSaveInfo deferred_save;
				info.log_file = &(my_client->desync_log_file);
				info.defer_savegame_write = &deferred_save;
				CrashLog::DesyncCrashLog(nullptr, &desync_log, info);
				my_client->SendDesyncLog(desync_log);
				my_client->SendDesyncSyncData();
				my_client->ClientError(NETWORK_RECV_STATUS_DESYNC);
				CrashLog::WriteDesyncSavegame(desync_log.c_str(), deferred_save.name_buffer.c_str());
				return false;
			}
			_last_sync_date = EconTime::CurDate();
			_last_sync_date_fract = EconTime::CurDateFract();
			_last_sync_tick_skip_counter = TickSkipCounter();
			_last_sync_frame_counter = _sync_frame;
			_network_sync_records.clear();
			_network_sync_record_counts.clear();

			/* If this is the first time we have a sync-frame, we
			 *   need to let the server know that we are ready and at the same
			 *   frame as it is.. so we can start playing! */
			if (_network_first_time) {
				_network_first_time = false;
				SendAck();
			}

			_sync_frame = 0;
		} else if (_sync_frame < _frame_counter) {
			Debug(net, 1, "Missed frame for sync-test: {} / {}", _sync_frame, _frame_counter);
			_sync_frame = 0;
		}
	}

	if (_network_sync_record_counts.size() >= 128) {
		/* Remove records from start of queue */
		_network_sync_records.erase(_network_sync_records.begin(), _network_sync_records.begin() + _network_sync_record_counts[0]);
		_network_sync_record_counts.pop_front();
	}

	return true;
}

/* static */ bool ClientNetworkGameSocketHandler::EmergencySavePossible()
{
	if (!my_client) return false;
	if (my_client->emergency_save_done) return false;
	my_client->emergency_save_done = true;
	return true;
}


/** Our client's connection. */
ClientNetworkGameSocketHandler * ClientNetworkGameSocketHandler::my_client = nullptr;

/** Last frame we performed an ack. */
static uint32_t last_ack_frame;

/** One bit of 'entropy' used to generate a salt for the company passwords. */
static uint32_t _company_password_game_seed;
/** Network server's x25519 public key, used for key derivation */
static std::array<uint8_t, 32> _server_x25519_pub_key;
/** Key message ID counter */
static uint64_t _next_key_message_id;
/** The other bit of 'entropy' used to generate a salt for the server, rcon, and settings passwords. */
static std::string _password_server_id;
/** The other bit of 'entropy' used to generate a salt for the company passwords. */
static std::string _company_password_server_id;

/** Maximum number of companies of the currently joined server. */
static uint16_t _network_server_max_companies;
/** The current name of the server you are on. */
std::string _network_server_name;

/** Information about the game to join to. */
NetworkJoinInfo _network_join;

/** Make sure the server ID length is the same as a md5 hash. */
static_assert(NETWORK_SERVER_ID_LENGTH == MD5_HASH_BYTES * 2 + 1);

NetworkRecvStatus ClientNetworkGameSocketHandler::SendKeyPasswordPacket(PacketType packet_type, NetworkSharedSecrets &ss, const std::string &password, const std::string *payload)
{
	const NetworkGameKeys &keys = this->GetKeys();

	std::array<uint8_t, 32> shared_secret; // Shared secret
	crypto_x25519(shared_secret.data(), keys.x25519_priv_key.data(), _server_x25519_pub_key.data());
	if (std::all_of(shared_secret.begin(), shared_secret.end(), [](auto v) { return v == 0; })) {
		/* Secret is all 0 because public key is all 0, just give up at this point */
		return NETWORK_RECV_STATUS_MALFORMED_PACKET;
	}

	crypto_blake2b_ctx ctx;
	crypto_blake2b_init  (&ctx, ss.shared_data.size());
	crypto_blake2b_update(&ctx, shared_secret.data(),          shared_secret.size());          // Shared secret
	crypto_blake2b_update(&ctx, keys.x25519_pub_key.data(),    keys.x25519_pub_key.size());    // Client pub key
	crypto_blake2b_update(&ctx, _server_x25519_pub_key.data(), _server_x25519_pub_key.size()); // Server pub key
	crypto_blake2b_update(&ctx, (const uint8_t *)password.data(), password.size());               // Password
	crypto_blake2b_final (&ctx, ss.shared_data.data());

	/* NetworkSharedSecrets::shared_data now contains 2 keys worth of hash, first key is used for up direction, second key for down direction (if any) */

	crypto_wipe(shared_secret.data(), shared_secret.size());

	std::vector<uint8_t> message;
	BufferSerialisationRef buffer(message);

	/* Put monotonically increasing counter in message */
	buffer.Send_uint64(_next_key_message_id);

	/* Put actual payload in message, if there is one */
	if (payload != nullptr) buffer.Send_string(*payload);

	/* Message authentication code */
	std::array<uint8_t, 16> mac;

	/* Use only once per key: random */
	std::array<uint8_t, 24> nonce;
	RandomBytesWithFallback(nonce);

	/* Encrypt in place, use first half of hash as key */
	static_assert(std::tuple_size<decltype(ss.shared_data)>::value == 64);
	crypto_aead_lock(message.data(), mac.data(), ss.shared_data.data(), nonce.data(), keys.x25519_pub_key.data(), keys.x25519_pub_key.size(), message.data(), message.size());

	auto p = std::make_unique<Packet>(my_client, packet_type, TCP_MTU);
	static_assert(std::tuple_size<decltype(keys.x25519_pub_key)>::value == 32);
	p->Send_binary(keys.x25519_pub_key);
	p->Send_binary(nonce);
	p->Send_binary(mac);
	p->Send_binary(message);

	_next_key_message_id++;

	my_client->SendPacket(std::move(p));
	return NETWORK_RECV_STATUS_OKAY;
}

/***********
 * Sending functions
 ************/

/** Tell the server we would like to join. */
NetworkRecvStatus ClientNetworkGameSocketHandler::SendJoin()
{
	my_client->status = STATUS_JOIN;
	_network_join_status = NETWORK_JOIN_STATUS_AUTHORIZING;
	SetWindowDirty(WC_NETWORK_STATUS_WINDOW, WN_NETWORK_STATUS_WINDOW_JOIN);

	auto p = std::make_unique<Packet>(my_client, PACKET_CLIENT_JOIN);
	p->Send_string(GetNetworkRevisionString());
	p->Send_uint32(_openttd_newgrf_version);
	my_client->SendPacket(std::move(p));

	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ClientNetworkGameSocketHandler::SendIdentify()
{
	auto p = std::make_unique<Packet>(my_client, PACKET_CLIENT_IDENTIFY, TCP_MTU);
	p->Send_string(_settings_client.network.client_name); // Client name
	p->Send_uint16 (_network_join.company);     // PlayAs
	p->Send_uint8 (0); // Used to be language
	my_client->SendPacket(std::move(p));
	return NETWORK_RECV_STATUS_OKAY;
}

/** Tell the server we got all the NewGRFs. */
NetworkRecvStatus ClientNetworkGameSocketHandler::SendNewGRFsOk()
{
	auto p = std::make_unique<Packet>(my_client, PACKET_CLIENT_NEWGRFS_CHECKED, TCP_MTU);
	my_client->SendPacket(std::move(p));
	return NETWORK_RECV_STATUS_OKAY;
}

/**
 * Set the game password as requested.
 * @param password The game password.
 */
NetworkRecvStatus ClientNetworkGameSocketHandler::SendAuthResponse()
{
	auto p = std::make_unique<Packet>(my_client, PACKET_CLIENT_AUTH_RESPONSE, TCP_MTU);
	my_client->authentication_handler->SendResponse(*p);
	my_client->SendPacket(std::move(p));

	return NETWORK_RECV_STATUS_OKAY;
}

/**
 * Set the company password as requested.
 * @param password The company password.
 */
NetworkRecvStatus ClientNetworkGameSocketHandler::SendCompanyPassword(const std::string &password)
{
	auto p = std::make_unique<Packet>(my_client, PACKET_CLIENT_COMPANY_PASSWORD, TCP_MTU);
	p->Send_string(GenerateCompanyPasswordHash(password, _company_password_server_id, _company_password_game_seed));
	my_client->SendPacket(std::move(p));
	return NETWORK_RECV_STATUS_OKAY;
}

/**
 * Set the game password as requested.
 * @param password The game password.
 */
NetworkRecvStatus ClientNetworkGameSocketHandler::SendSettingsPassword(const std::string &password)
{
	if (password.empty()) {
		auto p = std::make_unique<Packet>(my_client, PACKET_CLIENT_SETTINGS_PASSWORD, TCP_MTU);
		my_client->SendPacket(std::move(p));
		return NETWORK_RECV_STATUS_OKAY;
	} else {
		NetworkSharedSecrets ss;
		return my_client->SendKeyPasswordPacket(PACKET_CLIENT_SETTINGS_PASSWORD, ss, password, nullptr);
	}
}

/** Request the map from the server. */
NetworkRecvStatus ClientNetworkGameSocketHandler::SendGetMap()
{
	my_client->status = STATUS_MAP_WAIT;

	auto p = std::make_unique<Packet>(my_client, PACKET_CLIENT_GETMAP, TCP_MTU);
#if defined(WITH_ZSTD)
	p->Send_bool(true);
#else
	p->Send_bool(false);
#endif
	my_client->SendPacket(std::move(p));
	return NETWORK_RECV_STATUS_OKAY;
}

/** Tell the server we received the complete map. */
NetworkRecvStatus ClientNetworkGameSocketHandler::SendMapOk()
{
	my_client->status = STATUS_ACTIVE;

	auto p = std::make_unique<Packet>(my_client, PACKET_CLIENT_MAP_OK, TCP_MTU);
	my_client->SendPacket(std::move(p));
	return NETWORK_RECV_STATUS_OKAY;
}

/** Send an acknowledgement from the server's ticks. */
NetworkRecvStatus ClientNetworkGameSocketHandler::SendAck()
{
	auto p = std::make_unique<Packet>(my_client, PACKET_CLIENT_ACK, TCP_MTU);

	p->Send_uint32(_frame_counter);
	p->Send_uint8 (my_client->token);
	my_client->SendPacket(std::move(p));
	return NETWORK_RECV_STATUS_OKAY;
}

/**
 * Send a command to the server.
 * @param cp The command to send.
 */
NetworkRecvStatus ClientNetworkGameSocketHandler::SendCommand(const OutgoingCommandPacket &cp)
{
	auto p = std::make_unique<Packet>(my_client, PACKET_CLIENT_COMMAND, TCP_MTU);
	my_client->NetworkGameSocketHandler::SendCommand(*p, cp);

	my_client->SendPacket(std::move(p));
	return NETWORK_RECV_STATUS_OKAY;
}

/** Send a chat-packet over the network */
NetworkRecvStatus ClientNetworkGameSocketHandler::SendChat(NetworkAction action, DestType type, int dest, const std::string &msg, NetworkTextMessageData data)
{
	if (!my_client) return NETWORK_RECV_STATUS_CLIENT_QUIT;
	auto p = std::make_unique<Packet>(my_client, PACKET_CLIENT_CHAT, TCP_MTU);

	p->Send_uint8 (action);
	p->Send_uint8 (type);
	p->Send_uint32(dest);
	p->Send_string(msg);
	data.send(*p);

	my_client->SendPacket(std::move(p));
	return NETWORK_RECV_STATUS_OKAY;
}

/** Send an error-packet over the network */
NetworkRecvStatus ClientNetworkGameSocketHandler::SendError(NetworkErrorCode errorno, NetworkRecvStatus recvstatus)
{
	auto p = std::make_unique<Packet>(my_client, PACKET_CLIENT_ERROR, TCP_MTU);

	p->Send_uint8(errorno);
	p->Send_uint8(recvstatus);
	p->Send_uint8(my_client->status);
	p->Send_uint8(my_client->last_pkt_type);
	my_client->SendPacket(std::move(p));
	return NETWORK_RECV_STATUS_OKAY;
}

/** Send an error-packet over the network */
NetworkRecvStatus ClientNetworkGameSocketHandler::SendDesyncLog(const std::string &log)
{
	for (size_t offset = 0; offset < log.size();) {
		auto p = std::make_unique<Packet>(my_client, PACKET_CLIENT_DESYNC_LOG, TCP_MTU);
		size_t size = std::min<size_t>(log.size() - offset, TCP_MTU - 2 - p->Size());
		p->Send_uint16((uint16_t)size);
		p->Send_binary((const uint8_t *)(log.data() + offset), size);
		my_client->SendPacket(std::move(p));

		offset += size;
	}
	return NETWORK_RECV_STATUS_OKAY;
}

/** Send an error-packet over the network */
NetworkRecvStatus ClientNetworkGameSocketHandler::SendDesyncMessage(const char *msg)
{
	auto p = std::make_unique<Packet>(my_client, PACKET_CLIENT_DESYNC_MSG, TCP_MTU);
	p->Send_uint32(EconTime::CurDate().base());
	p->Send_uint16(EconTime::CurDateFract());
	p->Send_uint8(TickSkipCounter());
	p->Send_string(msg);
	my_client->SendPacket(std::move(p));
	return NETWORK_RECV_STATUS_OKAY;
}

/** Send an error-packet over the network */
NetworkRecvStatus ClientNetworkGameSocketHandler::SendDesyncSyncData()
{
	if (_network_sync_record_counts.empty()) return NETWORK_RECV_STATUS_OKAY;

	uint total = 0;
	for (uint32_t count : _network_sync_record_counts) {
		total += count;
	}

	if ((size_t)total != _network_sync_records.size()) {
		Debug(net, 0, "Network sync record error");
		return NETWORK_RECV_STATUS_OKAY;
	}

	auto p = std::make_unique<Packet>(my_client, PACKET_CLIENT_DESYNC_SYNC_DATA, TCP_MTU);
	p->Send_uint32((uint32_t)_network_sync_record_counts.size());
	uint32_t offset = 0;
	for (uint32_t count : _network_sync_record_counts) {
		p->Send_uint32(count);
		for (uint i = 0; i < count; i++) {
			const NetworkSyncRecord &record = _network_sync_records[offset + i];
			p->Send_uint32(record.frame);
			p->Send_uint32(record.seed_1);
			p->Send_uint64(record.state_checksum);
		}
		offset += count;
	}
	my_client->SendPacket(std::move(p));
	return NETWORK_RECV_STATUS_OKAY;
}

/**
 * Tell the server that we like to change the password of the company.
 * @param password The new password.
 */
NetworkRecvStatus ClientNetworkGameSocketHandler::SendSetPassword(const std::string &password)
{
	auto p = std::make_unique<Packet>(my_client, PACKET_CLIENT_SET_PASSWORD, TCP_MTU);

	p->Send_string(GenerateCompanyPasswordHash(password, _company_password_server_id, _company_password_game_seed));
	my_client->SendPacket(std::move(p));
	return NETWORK_RECV_STATUS_OKAY;
}

/**
 * Tell the server that we like to change the name of the client.
 * @param name The new name.
 */
NetworkRecvStatus ClientNetworkGameSocketHandler::SendSetName(const std::string &name)
{
	auto p = std::make_unique<Packet>(my_client, PACKET_CLIENT_SET_NAME, TCP_MTU);

	p->Send_string(name);
	my_client->SendPacket(std::move(p));
	return NETWORK_RECV_STATUS_OKAY;
}

/**
 * Tell the server we would like to quit.
 */
NetworkRecvStatus ClientNetworkGameSocketHandler::SendQuit()
{
	auto p = std::make_unique<Packet>(my_client, PACKET_CLIENT_QUIT, TCP_MTU);

	my_client->SendPacket(std::move(p));
	return NETWORK_RECV_STATUS_OKAY;
}

/**
 * Send a console command.
 * @param pass The password for the remote command.
 * @param command The actual command.
 */
NetworkRecvStatus ClientNetworkGameSocketHandler::SendRCon(const std::string &pass, const std::string &command)
{
	return my_client->SendKeyPasswordPacket(PACKET_CLIENT_RCON, my_client->last_rcon_shared_secrets, pass, &command);
}

/**
 * Ask the server to move us.
 * @param company The company to move to.
 * @param password The password of the company to move to.
 */
NetworkRecvStatus ClientNetworkGameSocketHandler::SendMove(CompanyID company, const std::string &password)
{
	auto p = std::make_unique<Packet>(my_client, PACKET_CLIENT_MOVE, TCP_MTU);
	p->Send_uint16(company);
	p->Send_string(GenerateCompanyPasswordHash(password, _company_password_server_id, _company_password_game_seed));
	my_client->SendPacket(std::move(p));
	return NETWORK_RECV_STATUS_OKAY;
}

/**
 * Check whether the client is actually connected (and in the game).
 * @return True when the client is connected.
 */
bool ClientNetworkGameSocketHandler::IsConnected()
{
	return my_client != nullptr && my_client->status == STATUS_ACTIVE;
}


/***********
 * Receiving functions
 ************/

extern bool SafeLoad(const std::string &filename, SaveLoadOperation fop, DetailedFileType dft, GameMode newgm, Subdirectory subdir,
		std::shared_ptr<struct LoadFilter> lf = nullptr, std::string *error_detail = nullptr);

NetworkRecvStatus ClientNetworkGameSocketHandler::Receive_SERVER_FULL(Packet &)
{
	/* We try to join a server which is full */
	ShowErrorMessage(STR_NETWORK_ERROR_SERVER_FULL, INVALID_STRING_ID, WL_CRITICAL);

	return NETWORK_RECV_STATUS_SERVER_FULL;
}

NetworkRecvStatus ClientNetworkGameSocketHandler::Receive_SERVER_BANNED(Packet &)
{
	/* We try to join a server where we are banned */
	ShowErrorMessage(STR_NETWORK_ERROR_SERVER_BANNED, INVALID_STRING_ID, WL_CRITICAL);

	return NETWORK_RECV_STATUS_SERVER_BANNED;
}

/* This packet contains info about the client (playas and name)
 *  as client we save this in NetworkClientInfo, linked via 'client_id'
 *  which is always an unique number on a server. */
NetworkRecvStatus ClientNetworkGameSocketHandler::Receive_SERVER_CLIENT_INFO(Packet &p)
{
	NetworkClientInfo *ci;
	ClientID client_id = (ClientID)p.Recv_uint32();
	CompanyID playas = (CompanyID)p.Recv_uint16();

	std::string name = p.Recv_string(NETWORK_NAME_LENGTH);
	//std::string public_key = p.Recv_string(NETWORK_PUBLIC_KEY_LENGTH);

	if (this->status < STATUS_AUTHORIZED) return NETWORK_RECV_STATUS_MALFORMED_PACKET;
	if (this->HasClientQuit()) return NETWORK_RECV_STATUS_CLIENT_QUIT;
	/* The server validates the name when receiving it from clients, so when it is wrong
	 * here something went really wrong. In the best case the packet got malformed on its
	 * way too us, in the worst case the server is broken or compromised. */
	if (!NetworkIsValidClientName(name)) return NETWORK_RECV_STATUS_MALFORMED_PACKET;

	ci = NetworkClientInfo::GetByClientID(client_id);
	if (ci != nullptr) {
		if (playas == ci->client_playas && name.compare(ci->client_name) != 0) {
			/* Client name changed, display the change */
			NetworkTextMessage(NETWORK_ACTION_NAME_CHANGE, CC_DEFAULT, false, ci->client_name, name);
		} else if (playas != ci->client_playas) {
			/* The client changed from client-player..
			 * Do not display that for now */
		}

		/* Make sure we're in the company the server tells us to be in,
		 * for the rare case that we get moved while joining. */
		if (client_id == _network_own_client_id) SetLocalCompany(!Company::IsValidID(playas) ? COMPANY_SPECTATOR : playas);

		ci->client_playas = playas;
		ci->client_name = name;
		//ci->public_key = public_key;

		InvalidateWindowData(WC_CLIENT_LIST, 0);

		return NETWORK_RECV_STATUS_OKAY;
	}

	/* There are at most as many ClientInfo as ClientSocket objects in a
	 * server. Having more info than a server can have means something
	 * has gone wrong somewhere, i.e. the server has more info than it
	 * has actual clients. That means the server is feeding us an invalid
	 * state. So, bail out! This server is broken. */
	if (!NetworkClientInfo::CanAllocateItem()) return NETWORK_RECV_STATUS_MALFORMED_PACKET;

	/* We don't have this client_id yet, find an empty client_id, and put the data there */
	ci = new NetworkClientInfo(client_id);
	ci->client_playas = playas;
	if (client_id == _network_own_client_id) this->SetInfo(ci);

	ci->client_name = name;
	//ci->public_key = public_key;

	InvalidateWindowData(WC_CLIENT_LIST, 0);

	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ClientNetworkGameSocketHandler::Receive_SERVER_ERROR(Packet &p)
{
	static const StringID network_error_strings[] = {
		STR_NETWORK_ERROR_LOSTCONNECTION,      // NETWORK_ERROR_GENERAL
		STR_NETWORK_ERROR_LOSTCONNECTION,      // NETWORK_ERROR_DESYNC
		STR_NETWORK_ERROR_LOSTCONNECTION,      // NETWORK_ERROR_SAVEGAME_FAILED
		STR_NETWORK_ERROR_LOSTCONNECTION,      // NETWORK_ERROR_CONNECTION_LOST
		STR_NETWORK_ERROR_LOSTCONNECTION,      // NETWORK_ERROR_ILLEGAL_PACKET
		STR_NETWORK_ERROR_LOSTCONNECTION,      // NETWORK_ERROR_NEWGRF_MISMATCH
		STR_NETWORK_ERROR_SERVER_ERROR,        // NETWORK_ERROR_NOT_AUTHORIZED
		STR_NETWORK_ERROR_SERVER_ERROR,        // NETWORK_ERROR_NOT_EXPECTED
		STR_NETWORK_ERROR_WRONG_REVISION,      // NETWORK_ERROR_WRONG_REVISION
		STR_NETWORK_ERROR_LOSTCONNECTION,      // NETWORK_ERROR_NAME_IN_USE
		STR_NETWORK_ERROR_WRONG_PASSWORD,      // NETWORK_ERROR_WRONG_PASSWORD
		STR_NETWORK_ERROR_SERVER_ERROR,        // NETWORK_ERROR_COMPANY_MISMATCH
		STR_NETWORK_ERROR_KICKED,              // NETWORK_ERROR_KICKED
		STR_NETWORK_ERROR_CHEATER,             // NETWORK_ERROR_CHEATER
		STR_NETWORK_ERROR_SERVER_FULL,         // NETWORK_ERROR_FULL
		STR_NETWORK_ERROR_TOO_MANY_COMMANDS,   // NETWORK_ERROR_TOO_MANY_COMMANDS
		STR_NETWORK_ERROR_TIMEOUT_PASSWORD,    // NETWORK_ERROR_TIMEOUT_PASSWORD
		STR_NETWORK_ERROR_TIMEOUT_COMPUTER,    // NETWORK_ERROR_TIMEOUT_COMPUTER
		STR_NETWORK_ERROR_TIMEOUT_MAP,         // NETWORK_ERROR_TIMEOUT_MAP
		STR_NETWORK_ERROR_TIMEOUT_JOIN,        // NETWORK_ERROR_TIMEOUT_JOIN
		STR_NETWORK_ERROR_INVALID_CLIENT_NAME, // NETWORK_ERROR_INVALID_CLIENT_NAME
		STR_NETWORK_ERROR_NOT_ON_ALLOW_LIST,   // NETWORK_ERROR_NOT_ON_ALLOW_LIST
		STR_NETWORK_ERROR_SERVER_ERROR,        // NETWORK_ERROR_NO_AUTHENTICATION_METHOD_AVAILABLE
	};
	static_assert(lengthof(network_error_strings) == NETWORK_ERROR_END);

	NetworkErrorCode error = (NetworkErrorCode)p.Recv_uint8();

	StringID err = STR_NETWORK_ERROR_LOSTCONNECTION;
	if (error < (ptrdiff_t)lengthof(network_error_strings)) err = network_error_strings[error];
	/* In case of kicking a client, we assume there is a kick message in the packet if we can read one byte */
	if (error == NETWORK_ERROR_KICKED && p.CanReadFromPacket(1)) {
		SetDParamStr(0, p.Recv_string(NETWORK_CHAT_LENGTH));
		ShowErrorMessage(err, STR_NETWORK_ERROR_KICK_MESSAGE, WL_CRITICAL);
	} else {
		ShowErrorMessage(err, INVALID_STRING_ID, WL_CRITICAL);
	}

	/* Perform an emergency save if we had already entered the game */
	if (this->status == STATUS_ACTIVE) ClientNetworkEmergencySave();

	return NETWORK_RECV_STATUS_SERVER_ERROR;
}

NetworkRecvStatus ClientNetworkGameSocketHandler::Receive_SERVER_CHECK_NEWGRFS(Packet &p)
{
	if (this->status != STATUS_ENCRYPTED) return NETWORK_RECV_STATUS_MALFORMED_PACKET;

	uint grf_count = p.Recv_uint32();
	if (grf_count > MAX_NON_STATIC_GRF_COUNT) return NETWORK_RECV_STATUS_MALFORMED_PACKET;
	NetworkRecvStatus ret = NETWORK_RECV_STATUS_OKAY;

	/* Check all GRFs */
	for (; grf_count > 0; grf_count--) {
		GRFIdentifier c;
		DeserializeGRFIdentifier(p, c);

		/* Check whether we know this GRF */
		const GRFConfig *f = FindGRFConfig(c.grfid, FGCM_EXACT, &c.md5sum);
		if (f == nullptr) {
			/* We do not know this GRF, bail out of initialization */
			Debug(grf, 0, "NewGRF {:08X} not found; checksum {}", std::byteswap(c.grfid), c.md5sum);
			ret = NETWORK_RECV_STATUS_NEWGRF_MISMATCH;
		}
	}

	if (ret == NETWORK_RECV_STATUS_OKAY) {
		/* Start receiving the map */
		return SendNewGRFsOk();
	}

	/* NewGRF mismatch, bail out */
	ShowErrorMessage(STR_NETWORK_ERROR_NEWGRF_MISMATCH, INVALID_STRING_ID, WL_CRITICAL);
	return ret;
}

class ClientGamePasswordRequestHandler : public NetworkAuthenticationPasswordRequestHandler {
	virtual void SendResponse() override { MyClient::SendAuthResponse(); }
	virtual void AskUserForPassword(std::shared_ptr<NetworkAuthenticationPasswordRequest> request) override
	{
		if (!_network_join.server_password.empty()) {
			request->Reply(_network_join.server_password);
		} else {
			ShowNetworkNeedPassword(NETWORK_GAME_PASSWORD, request);
		}
	}
};

NetworkRecvStatus ClientNetworkGameSocketHandler::Receive_SERVER_AUTH_REQUEST(Packet &p)
{
	if (this->status != STATUS_JOIN && this->status != STATUS_AUTH_GAME) return NETWORK_RECV_STATUS_MALFORMED_PACKET;
	this->status = STATUS_AUTH_GAME;

	if (this->authentication_handler == nullptr) {
		this->authentication_handler = NetworkAuthenticationClientHandler::Create(std::make_shared<ClientGamePasswordRequestHandler>(),
				_settings_client.network.client_secret_key, _settings_client.network.client_public_key);
	}
	switch (this->authentication_handler->ReceiveRequest(p)) {
		case NetworkAuthenticationClientHandler::RequestResult::ReadyForResponse:
			return SendAuthResponse();

		case NetworkAuthenticationClientHandler::RequestResult::AwaitUserInput:
			return NETWORK_RECV_STATUS_OKAY;

		case NetworkAuthenticationClientHandler::RequestResult::Invalid:
		default:
			return NETWORK_RECV_STATUS_MALFORMED_PACKET;
	}
}

NetworkRecvStatus ClientNetworkGameSocketHandler::Receive_SERVER_ENABLE_ENCRYPTION(Packet &p)
{
	if (this->status != STATUS_AUTH_GAME || this->authentication_handler == nullptr) return NETWORK_RECV_STATUS_MALFORMED_PACKET;

	if (!this->authentication_handler->ReceiveEnableEncryption(p)) return NETWORK_RECV_STATUS_MALFORMED_PACKET;

	this->receive_encryption_handler = this->authentication_handler->CreateServerToClientEncryptionHandler();
	this->send_encryption_handler = this->authentication_handler->CreateClientToServerEncryptionHandler();
	this->authentication_handler = nullptr;

	this->status = STATUS_ENCRYPTED;

	return this->SendIdentify();
}

class CompanyPasswordRequest : public NetworkAuthenticationPasswordRequest {
	virtual void Reply(const std::string &password) override
	{
		MyClient::SendCompanyPassword(password);
	}
};

NetworkRecvStatus ClientNetworkGameSocketHandler::Receive_SERVER_NEED_COMPANY_PASSWORD(Packet &p)
{
	if (this->status < STATUS_ENCRYPTED || this->status >= STATUS_AUTH_COMPANY) return NETWORK_RECV_STATUS_MALFORMED_PACKET;
	this->status = STATUS_AUTH_COMPANY;

	_company_password_game_seed = p.Recv_uint32();
	_company_password_server_id = p.Recv_string(NETWORK_SERVER_ID_LENGTH);
	if (this->HasClientQuit()) return NETWORK_RECV_STATUS_MALFORMED_PACKET;

	if (!_network_join.company_password.empty()) {
		return SendCompanyPassword(_network_join.company_password);
	}

	ShowNetworkNeedPassword(NETWORK_COMPANY_PASSWORD, std::make_shared<CompanyPasswordRequest>());

	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ClientNetworkGameSocketHandler::Receive_SERVER_WELCOME(Packet &p)
{
	if (this->status < STATUS_ENCRYPTED || this->status >= STATUS_AUTHORIZED) return NETWORK_RECV_STATUS_MALFORMED_PACKET;
	this->status = STATUS_AUTHORIZED;

	_network_own_client_id = (ClientID)p.Recv_uint32();

	/* Initialize the password hash salting variables, even if they were previously. */
	_company_password_game_seed = p.Recv_uint32();
	static_assert(_server_x25519_pub_key.size() == 32);
	p.Recv_binary(_server_x25519_pub_key);
	_password_server_id = p.Recv_string(NETWORK_SERVER_ID_LENGTH);
	_company_password_server_id = p.Recv_string(NETWORK_SERVER_ID_LENGTH);

	/* Start receiving the map */
	return SendGetMap();
}

NetworkRecvStatus ClientNetworkGameSocketHandler::Receive_SERVER_WAIT(Packet &p)
{
	/* We set the internal wait state when requesting the map. */
	if (this->status != STATUS_MAP_WAIT) return NETWORK_RECV_STATUS_MALFORMED_PACKET;

	/* But... only now we set the join status to waiting, instead of requesting. */
	_network_join_status = NETWORK_JOIN_STATUS_WAITING;
	_network_join_waiting = p.Recv_uint8();
	SetWindowDirty(WC_NETWORK_STATUS_WINDOW, WN_NETWORK_STATUS_WINDOW_JOIN);

	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ClientNetworkGameSocketHandler::Receive_SERVER_MAP_BEGIN(Packet &p)
{
	if (this->status < STATUS_AUTHORIZED || this->status >= STATUS_MAP) return NETWORK_RECV_STATUS_MALFORMED_PACKET;
	this->status = STATUS_MAP;

	if (this->savegame != nullptr) return NETWORK_RECV_STATUS_MALFORMED_PACKET;

	this->savegame = std::make_shared<PacketReader>();

	_frame_counter = _frame_counter_server = _frame_counter_max = p.Recv_uint32();

	_network_join_bytes = 0;
	_network_join_bytes_total = 0;

	_network_join_status = NETWORK_JOIN_STATUS_DOWNLOADING;
	SetWindowDirty(WC_NETWORK_STATUS_WINDOW, WN_NETWORK_STATUS_WINDOW_JOIN);

	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ClientNetworkGameSocketHandler::Receive_SERVER_MAP_SIZE(Packet &p)
{
	if (this->status != STATUS_MAP) return NETWORK_RECV_STATUS_MALFORMED_PACKET;
	if (this->savegame == nullptr) return NETWORK_RECV_STATUS_MALFORMED_PACKET;

	_network_join_bytes_total = p.Recv_uint32();
	SetWindowDirty(WC_NETWORK_STATUS_WINDOW, WN_NETWORK_STATUS_WINDOW_JOIN);

	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ClientNetworkGameSocketHandler::Receive_SERVER_MAP_DATA(Packet &p)
{
	if (this->status != STATUS_MAP) return NETWORK_RECV_STATUS_MALFORMED_PACKET;
	if (this->savegame == nullptr) return NETWORK_RECV_STATUS_MALFORMED_PACKET;

	/* We are still receiving data, put it to the file */
	this->savegame->AddPacket(p);

	_network_join_bytes = (uint32_t)this->savegame->written_bytes;
	SetWindowDirty(WC_NETWORK_STATUS_WINDOW, WN_NETWORK_STATUS_WINDOW_JOIN);

	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ClientNetworkGameSocketHandler::Receive_SERVER_MAP_DONE(Packet &)
{
	if (this->status != STATUS_MAP) return NETWORK_RECV_STATUS_MALFORMED_PACKET;
	if (this->savegame == nullptr) return NETWORK_RECV_STATUS_MALFORMED_PACKET;

	_network_join_status = NETWORK_JOIN_STATUS_PROCESSING;
	SetWindowDirty(WC_NETWORK_STATUS_WINDOW, WN_NETWORK_STATUS_WINDOW_JOIN);

	this->savegame->Reset();

	/* The map is done downloading, load it */
	ClearErrorMessages();

	/* Set the abstract filetype. This is read during savegame load. */
	_file_to_saveload.SetMode(SLO_LOAD, FT_SAVEGAME, DFT_GAME_FILE);

	std::string error_detail;
	bool load_success = SafeLoad({}, SLO_LOAD, DFT_GAME_FILE, GM_NORMAL, NO_DIRECTORY, std::move(this->savegame), &error_detail);
	this->savegame = nullptr;

	/* Long savegame loads shouldn't affect the lag calculation! */
	this->last_packet = std::chrono::steady_clock::now();

	if (!load_success) {
		StringID detail = INVALID_STRING_ID;
		if (!error_detail.empty()) {
			detail = STR_JUST_RAW_STRING;
			SetDParamStr(0, error_detail.c_str());
		}
		ShowErrorMessage(STR_NETWORK_ERROR_SAVEGAMEERROR, detail, WL_CRITICAL);
		return NETWORK_RECV_STATUS_SAVEGAME;
	}
	/* If the savegame has successfully loaded, ALL windows have been removed,
	 * only toolbar/statusbar and gamefield are visible */

	/* Say we received the map and loaded it correctly! */
	SendMapOk();

	/* As we skipped switch-mode, update the time we "switched". */
	_game_session_stats.start_time = std::chrono::steady_clock::now();
	_game_session_stats.savegame_size = std::nullopt;

	ShowClientList();

	/* New company/spectator (invalid company) or company we want to join is not active
	 * Switch local company to spectator and await the server's judgement */
	if (_network_join.company == COMPANY_NEW_COMPANY || !Company::IsValidID(_network_join.company)) {
		SetLocalCompany(COMPANY_SPECTATOR);

		if (_network_join.company != COMPANY_SPECTATOR) {
			/* We have arrived and ready to start playing; send a command to make a new company;
			 * the server will give us a client-id and let us in */
			_network_join_status = NETWORK_JOIN_STATUS_REGISTERING;
			ShowJoinStatusWindow();
			NetworkSendCommand<CMD_COMPANY_CTRL>({}, CmdCompanyCtrlData::Make(CCA_NEW, {}, {}, {}, {}), (StringID)0, CommandCallback::None, 0, _local_company);
		}
	} else {
		/* take control over an existing company */
		SetLocalCompany(_network_join.company);
	}

	SocialIntegration::EventEnterMultiplayer(Map::SizeX(), Map::SizeY());

	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ClientNetworkGameSocketHandler::Receive_SERVER_FRAME(Packet &p)
{
	if (this->status == STATUS_CLOSING) return NETWORK_RECV_STATUS_OKAY;
	if (this->status != STATUS_ACTIVE) return NETWORK_RECV_STATUS_MALFORMED_PACKET;

	_frame_counter_server = p.Recv_uint32();
	_frame_counter_max = p.Recv_uint32();
#ifdef ENABLE_NETWORK_SYNC_EVERY_FRAME
	/* Test if the server supports this option
	 *  and if we are at the frame the server is */
	if (p.CanReadFromPacket(4 + 8)) {
		_sync_frame = _frame_counter_server;
		_sync_seed_1 = p.Recv_uint32();
		_sync_state_checksum = p.Recv_uint64();
	}
#endif
	/* Receive the token. */
	if (p.CanReadFromPacket(sizeof(uint8_t))) this->token = p.Recv_uint8();

	Debug(net, 7, "Received FRAME {}", _frame_counter_server);

	/* Let the server know that we received this frame correctly
	 *  We do this only once per day, to save some bandwidth ;) */
	if (!_network_first_time && last_ack_frame < _frame_counter) {
		last_ack_frame = _frame_counter + DAY_TICKS;
		Debug(net, 7, "Sent ACK at {}", _frame_counter);
		SendAck();
	}

	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ClientNetworkGameSocketHandler::Receive_SERVER_SYNC(Packet &p)
{
	if (this->status == STATUS_CLOSING) return NETWORK_RECV_STATUS_OKAY;
	if (this->status != STATUS_ACTIVE) return NETWORK_RECV_STATUS_MALFORMED_PACKET;

	_sync_frame = p.Recv_uint32();
	_sync_seed_1 = p.Recv_uint32();
	_sync_state_checksum = p.Recv_uint64();

	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ClientNetworkGameSocketHandler::Receive_SERVER_COMMAND(Packet &p)
{
	if (this->status == STATUS_CLOSING) return NETWORK_RECV_STATUS_OKAY;
	if (this->status != STATUS_ACTIVE) return NETWORK_RECV_STATUS_MALFORMED_PACKET;

	CommandPacket cp;
	const char *err = this->ReceiveCommand(p, cp);
	cp.frame    = p.Recv_uint32();
	cp.my_cmd   = p.Recv_bool();

	if (err != nullptr) {
		IConsolePrint(CC_ERROR, "WARNING: {} from server, dropping...", err);
		return NETWORK_RECV_STATUS_MALFORMED_PACKET;
	}

	this->incoming_queue.push_back(std::move(cp));

	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ClientNetworkGameSocketHandler::Receive_SERVER_CHAT(Packet &p)
{
	if (this->status == STATUS_CLOSING) return NETWORK_RECV_STATUS_OKAY;
	if (this->status != STATUS_ACTIVE) return NETWORK_RECV_STATUS_MALFORMED_PACKET;

	std::string name;
	const NetworkClientInfo *ci = nullptr, *ci_to;

	NetworkAction action = (NetworkAction)p.Recv_uint8();
	ClientID client_id = (ClientID)p.Recv_uint32();
	bool self_send = p.Recv_bool();
	std::string msg = p.Recv_string(NETWORK_CHAT_LENGTH);
	NetworkTextMessageData data;
	data.recv(p);

	ci_to = NetworkClientInfo::GetByClientID(client_id);
	if (ci_to == nullptr) return NETWORK_RECV_STATUS_OKAY;

	/* Did we initiate the action locally? */
	if (self_send) {
		switch (action) {
			case NETWORK_ACTION_CHAT_CLIENT:
				/* For speaking to client we need the client-name */
				name = ci_to->client_name;
				ci = NetworkClientInfo::GetByClientID(_network_own_client_id);
				break;

			/* For speaking to company or giving money, we need the company-name */
			case NETWORK_ACTION_GIVE_MONEY:
				if (!Company::IsValidID(ci_to->client_playas)) return NETWORK_RECV_STATUS_OKAY;
				[[fallthrough]];

			case NETWORK_ACTION_CHAT_COMPANY: {
				StringID str = Company::IsValidID(ci_to->client_playas) ? STR_COMPANY_NAME : STR_NETWORK_SPECTATORS;
				SetDParam(0, ci_to->client_playas);

				name = GetString(str);
				ci = NetworkClientInfo::GetByClientID(_network_own_client_id);
				break;
			}

			default: return NETWORK_RECV_STATUS_MALFORMED_PACKET;
		}
	} else {
		/* Display message from somebody else */
		name = ci_to->client_name;
		ci = ci_to;
	}

	if (ci != nullptr) {
		NetworkTextMessage(action, GetDrawStringCompanyColour(ci->client_playas), self_send, name, msg, data);
	}
	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ClientNetworkGameSocketHandler::Receive_SERVER_EXTERNAL_CHAT(Packet &p)
{
	if (this->status != STATUS_ACTIVE) return NETWORK_RECV_STATUS_MALFORMED_PACKET;

	std::string source = p.Recv_string(NETWORK_CHAT_LENGTH);
	TextColour colour = (TextColour)p.Recv_uint16();
	std::string user = p.Recv_string(NETWORK_CHAT_LENGTH);
	std::string msg = p.Recv_string(NETWORK_CHAT_LENGTH);

	if (!IsValidConsoleColour(colour)) return NETWORK_RECV_STATUS_MALFORMED_PACKET;

	NetworkTextMessage(NETWORK_ACTION_EXTERNAL_CHAT, colour, false, user, msg, 0, source.c_str());

	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ClientNetworkGameSocketHandler::Receive_SERVER_ERROR_QUIT(Packet &p)
{
	if (this->status < STATUS_AUTHORIZED) return NETWORK_RECV_STATUS_MALFORMED_PACKET;

	ClientID client_id = (ClientID)p.Recv_uint32();
	if (client_id == _network_own_client_id) return NETWORK_RECV_STATUS_OKAY; // do not try to clear our own client info

	NetworkClientInfo *ci = NetworkClientInfo::GetByClientID(client_id);
	if (ci != nullptr) {
		NetworkTextMessage(NETWORK_ACTION_LEAVE, CC_DEFAULT, false, ci->client_name, "", GetNetworkErrorMsg((NetworkErrorCode)p.Recv_uint8()));
		delete ci;
	}

	InvalidateWindowData(WC_CLIENT_LIST, 0);

	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ClientNetworkGameSocketHandler::Receive_SERVER_DESYNC_LOG(Packet &p)
{
	uint size = p.Recv_uint16();
	this->server_desync_log.resize(this->server_desync_log.size() + size);
	p.Recv_binary((uint8_t *)(this->server_desync_log.data() + this->server_desync_log.size() - size), size);
	Debug(net, 2, "Received {} bytes of server desync log", size);
	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ClientNetworkGameSocketHandler::Receive_SERVER_QUIT(Packet &p)
{
	if (this->status < STATUS_AUTHORIZED) return NETWORK_RECV_STATUS_MALFORMED_PACKET;

	ClientID client_id = (ClientID)p.Recv_uint32();

	NetworkClientInfo *ci = NetworkClientInfo::GetByClientID(client_id);
	if (ci != nullptr) {
		NetworkTextMessage(NETWORK_ACTION_LEAVE, CC_DEFAULT, false, ci->client_name, "", STR_NETWORK_MESSAGE_CLIENT_LEAVING);
		delete ci;
	} else {
		Debug(net, 1, "Unknown client ({}) is leaving the game", client_id);
	}

	InvalidateWindowData(WC_CLIENT_LIST, 0);

	/* If we come here it means we could not locate the client.. strange :s */
	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ClientNetworkGameSocketHandler::Receive_SERVER_JOIN(Packet &p)
{
	if (this->status < STATUS_AUTHORIZED) return NETWORK_RECV_STATUS_MALFORMED_PACKET;

	ClientID client_id = (ClientID)p.Recv_uint32();

	NetworkClientInfo *ci = NetworkClientInfo::GetByClientID(client_id);
	if (ci != nullptr) {
		NetworkTextMessage(NETWORK_ACTION_JOIN, CC_DEFAULT, false, ci->client_name);
	}

	InvalidateWindowData(WC_CLIENT_LIST, 0);

	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ClientNetworkGameSocketHandler::Receive_SERVER_SHUTDOWN(Packet &)
{
	/* Only when we're trying to join we really
	 * care about the server shutting down. */
	if (this->status >= STATUS_JOIN) {
		ShowErrorMessage(STR_NETWORK_MESSAGE_SERVER_SHUTDOWN, INVALID_STRING_ID, WL_CRITICAL);
	}

	if (this->status == STATUS_ACTIVE) ClientNetworkEmergencySave();

	return NETWORK_RECV_STATUS_SERVER_ERROR;
}

NetworkRecvStatus ClientNetworkGameSocketHandler::Receive_SERVER_NEWGAME(Packet &)
{
	/* Only when we're trying to join we really
	 * care about the server shutting down. */
	if (this->status >= STATUS_JOIN) {
		/* To throttle the reconnects a bit, every clients waits its
		 * Client ID modulo 16 + 1 (value 0 means no reconnect).
		 * This way reconnects should be spread out a bit. */
		_network_reconnect = _network_own_client_id % 16 + 1;
		ShowErrorMessage(STR_NETWORK_MESSAGE_SERVER_REBOOT, INVALID_STRING_ID, WL_CRITICAL);
	}

	if (this->status == STATUS_ACTIVE) ClientNetworkEmergencySave();

	return NETWORK_RECV_STATUS_SERVER_ERROR;
}

NetworkRecvStatus ClientNetworkGameSocketHandler::Receive_SERVER_RCON(Packet &p)
{
	if (this->status < STATUS_AUTHORIZED) return NETWORK_RECV_STATUS_MALFORMED_PACKET;

	if (!p.CanReadFromPacket(1)) {
		IConsolePrint(CC_ERROR, "Access Denied");
		return NETWORK_RECV_STATUS_OKAY;
	}

	std::array<uint8_t, 24> nonce;
	std::array<uint8_t, 16> mac;
	p.Recv_binary(nonce);
	p.Recv_binary(mac);

	std::vector<uint8_t> message = p.Recv_binary(p.RemainingBytesToTransfer());

	static_assert(std::tuple_size<decltype(NetworkSharedSecrets::shared_data)>::value == 64);
	if (crypto_aead_unlock(message.data(), mac.data(), this->last_rcon_shared_secrets.shared_data.data() + 32, nonce.data(), nullptr, 0, message.data(), message.size()) == 0) {
		SubPacketDeserialiser spd(p, message);
		TextColour colour_code = (TextColour)spd.Recv_uint16();
		if (!IsValidConsoleColour(colour_code)) return NETWORK_RECV_STATUS_MALFORMED_PACKET;

		std::string rcon_out = spd.Recv_string(NETWORK_RCONCOMMAND_LENGTH);
		IConsolePrint(colour_code, rcon_out.c_str());
	}

	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ClientNetworkGameSocketHandler::Receive_SERVER_MOVE(Packet &p)
{
	if (this->status < STATUS_AUTHORIZED) return NETWORK_RECV_STATUS_MALFORMED_PACKET;

	/* Nothing more in this packet... */
	ClientID client_id   = (ClientID)p.Recv_uint32();
	CompanyID company_id = (CompanyID)p.Recv_uint16();

	if (client_id == 0) {
		/* definitely an invalid client id, debug message and do nothing. */
		Debug(net, 1, "Received invalid client index = 0");
		return NETWORK_RECV_STATUS_MALFORMED_PACKET;
	}

	const NetworkClientInfo *ci = NetworkClientInfo::GetByClientID(client_id);
	/* Just make sure we do not try to use a client_index that does not exist */
	if (ci == nullptr) return NETWORK_RECV_STATUS_OKAY;

	/* if not valid player, force spectator, else check player exists */
	if (!Company::IsValidID(company_id)) company_id = COMPANY_SPECTATOR;

	if (client_id == _network_own_client_id) {
		SetLocalCompany(company_id);
	}

	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ClientNetworkGameSocketHandler::Receive_SERVER_CONFIG_UPDATE(Packet &p)
{
	if (this->status < STATUS_ACTIVE) return NETWORK_RECV_STATUS_MALFORMED_PACKET;

	_network_server_max_companies = p.Recv_uint16();
	_network_server_name = p.Recv_string(NETWORK_NAME_LENGTH);

	InvalidateWindowData(WC_CLIENT_LIST, 0);

	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ClientNetworkGameSocketHandler::Receive_SERVER_COMPANY_UPDATE(Packet &p)
{
	if (this->status < STATUS_ACTIVE) return NETWORK_RECV_STATUS_MALFORMED_PACKET;

	static_assert(sizeof(_network_company_passworded) <= sizeof(uint16_t));
	_network_company_passworded.edit_base() = p.Recv_uint16();
	SetWindowClassesDirty(WC_COMPANY);

	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ClientNetworkGameSocketHandler::Receive_SERVER_SETTINGS_ACCESS(Packet &p)
{
	if (this->status < STATUS_ACTIVE) return NETWORK_RECV_STATUS_MALFORMED_PACKET;

	_network_settings_access = p.Recv_bool();

	CloseWindowById(WC_CHEATS, 0);
	ReInitAllWindows(false);

	return NETWORK_RECV_STATUS_OKAY;
}

/**
 * Check the connection's state, i.e. is the connection still up?
 */
void ClientNetworkGameSocketHandler::CheckConnection()
{
	/* Only once we're authorized we can expect a steady stream of packets. */
	if (this->status < STATUS_AUTHORIZED) return;

	/* 5 seconds are roughly twice the server's "you're slow" threshold (1 game day). */
	std::chrono::steady_clock::duration lag = std::chrono::steady_clock::now() - this->last_packet;
	if (lag < std::chrono::seconds(5)) return;

	/* 20 seconds are (way) more than 4 game days after which
	 * the server will forcefully disconnect you. */
	if (lag > std::chrono::seconds(20)) {
		this->NetworkGameSocketHandler::CloseConnection();
		return;
	}

	/* Prevent showing the lag message every tick; just update it when needed. */
	static std::chrono::steady_clock::duration last_lag = {};
	if (std::chrono::duration_cast<std::chrono::seconds>(last_lag) == std::chrono::duration_cast<std::chrono::seconds>(lag)) return;

	last_lag = lag;
	SetDParam(0, std::chrono::duration_cast<std::chrono::seconds>(lag).count());
	ShowErrorMessage(STR_NETWORK_ERROR_CLIENT_GUI_LOST_CONNECTION_CAPTION, STR_NETWORK_ERROR_CLIENT_GUI_LOST_CONNECTION, WL_INFO);
}

const char *ClientNetworkGameSocketHandler::GetServerStatusName(ServerStatus status)
{
	static const char* _server_status_names[] {
		"INACTIVE",
		"JOIN",
		"AUTH_GAME",
		"ENCRYPTED",
		"NEWGRFS_CHECK",
		"AUTH_COMPANY",
		"AUTHORIZED",
		"MAP_WAIT",
		"MAP",
		"ACTIVE",
		"CLOSING",
	};
	static_assert(lengthof(_server_status_names) == STATUS_END);
	return status < STATUS_END ? _server_status_names[status] : "[invalid status]";
}

std::string ClientNetworkGameSocketHandler::GetDebugInfo() const
{
	return fmt::format("status: {} ({})", this->status, GetServerStatusName(this->status));
}

static void ResetClientConnectionKeyStates()
{
	_next_key_message_id = 0;
	crypto_wipe(_server_x25519_pub_key.data(), _server_x25519_pub_key.size());
}


/** Is called after a client is connected to the server */
void NetworkClient_Connected()
{
	/* Set the frame-counter to 0 so nothing happens till we are ready */
	_frame_counter = 0;
	_frame_counter_server = 0;
	last_ack_frame = 0;
	ResetClientConnectionKeyStates();
	/* Request the game-info */
	MyClient::SendJoin();
}

/**
 * Send a remote console command.
 * @param password The password.
 * @param command The command to execute.
 */
void NetworkClientSendRcon(const std::string &password, const std::string &command)
{
	MyClient::SendRCon(password, command);
}

/**
 * Send settings password.
 * @param password The password.
 * @param command The command to execute.
 */
void NetworkClientSendSettingsPassword(const std::string &password)
{
	MyClient::SendSettingsPassword(password);
}

/**
 * Notify the server of this client wanting to be moved to another company.
 * @param company_id id of the company the client wishes to be moved to.
 * @param pass the password, is only checked on the server end if a password is needed.
 * @return void
 */
void NetworkClientRequestMove(CompanyID company_id, const std::string &pass)
{
	MyClient::SendMove(company_id, pass);
}

/**
 * Move the clients of a company to the spectators.
 * @param cid The company to move the clients of.
 */
void NetworkClientsToSpectators(CompanyID cid)
{
	Backup<CompanyID> cur_company(_current_company, FILE_LINE);
	/* If our company is changing owner, go to spectators */
	if (cid == _local_company) SetLocalCompany(COMPANY_SPECTATOR);

	for (NetworkClientInfo *ci : NetworkClientInfo::Iterate()) {
		if (ci->client_playas != cid) continue;
		NetworkTextMessage(NETWORK_ACTION_COMPANY_SPECTATOR, CC_DEFAULT, false, ci->client_name);
		ci->client_playas = COMPANY_SPECTATOR;
	}

	cur_company.Restore();
}

/**
 * Check whether the given client name is deemed valid for use in network games.
 * An empty name (null or '') is not valid as that is essentially no name at all.
 * A name starting with white space is not valid for tab completion purposes.
 * @param client_name The client name to check for validity.
 * @return True iff the name is valid.
 */
bool NetworkIsValidClientName(const std::string_view client_name)
{
	if (client_name.empty()) return false;
	if (client_name[0] == ' ') return false;
	return true;
}

/**
 * Trim the given client name in place, i.e. remove leading and trailing spaces.
 * After the trim check whether the client name is valid. A client name is valid
 * whenever the name is not empty and does not start with spaces. This check is
 * done via \c NetworkIsValidClientName.
 * When the client name is valid, this function returns true.
 * When the client name is not valid a GUI error message is shown telling the
 * user to set the client name and this function returns false.
 *
 * This function is not suitable for ensuring a valid client name at the server
 * as the error message will then be shown to the host instead of the client.
 * @param client_name The client name to validate. It will be trimmed of leading
 *                    and trailing spaces.
 * @return True iff the client name is valid.
 */
bool NetworkValidateClientName(std::string &client_name)
{
	StrTrimInPlace(client_name);
	if (NetworkIsValidClientName(client_name)) return true;

	ShowErrorMessage(STR_NETWORK_ERROR_BAD_PLAYER_NAME, INVALID_STRING_ID, WL_ERROR);
	return false;
}

/**
 * Convenience method for NetworkValidateClientName on _settings_client.network.client_name.
 * It trims the client name and checks whether it is empty. When it is empty
 * an error message is shown to the GUI user.
 * See \c NetworkValidateClientName(char*) for details about the functionality.
 * @return True iff the client name is valid.
 */
bool NetworkValidateOurClientName()
{
	return NetworkValidateClientName(_settings_client.network.client_name);
}

/**
 * Send the server our name as callback from the setting.
 * @param newname The new client name.
 */
void NetworkUpdateClientName(const std::string &client_name)
{
	NetworkClientInfo *ci = NetworkClientInfo::GetByClientID(_network_own_client_id);
	if (ci == nullptr) return;

	/* Don't change the name if it is the same as the old name */
	if (client_name.compare(ci->client_name) != 0) {
		if (!_network_server) {
			MyClient::SendSetName(client_name);
		} else {
			/* Copy to a temporary buffer so no #n gets added after our name in the settings when there are duplicate names. */
			std::string temporary_name = client_name;
			if (NetworkMakeClientNameUnique(temporary_name)) {
				NetworkTextMessage(NETWORK_ACTION_NAME_CHANGE, CC_DEFAULT, false, ci->client_name, temporary_name);
				ci->client_name = temporary_name;
				NetworkUpdateClientInfo(CLIENT_ID_SERVER);
			}
		}
	}
}

/**
 * Send a chat message.
 * @param action The action associated with the message.
 * @param type The destination type.
 * @param dest The destination index, be it a company index or client id.
 * @param msg The actual message.
 * @param data Arbitrary extra data.
 */
void NetworkClientSendChat(NetworkAction action, DestType type, int dest, const std::string &msg, NetworkTextMessageData data)
{
	MyClient::SendChat(action, type, dest, msg, data);
}


void NetworkClientSendDesyncMsg(const char *msg)
{
	MyClient::SendDesyncMessage(msg);
}

/**
 * Set/Reset company password on the client side.
 * @param password Password to be set.
 */
void NetworkClientSetCompanyPassword(const std::string &password)
{
	MyClient::SendSetPassword(password);
}

/**
 * Tell whether the client has team members who they can chat to.
 * @param cio client to check members of.
 * @return true if there is at least one team member.
 */
bool NetworkClientPreferTeamChat(const NetworkClientInfo *cio)
{
	/* Only companies actually playing can speak to team. Eg spectators cannot */
	if (!_settings_client.gui.prefer_teamchat || !Company::IsValidID(cio->client_playas)) return false;

	for (const NetworkClientInfo *ci : NetworkClientInfo::Iterate()) {
		if (ci->client_playas == cio->client_playas && ci != cio) return true;
	}

	return false;
}

/**
 * Get the maximum number of companies that are allowed by the server.
 * @return The number of companies allowed.
 */
uint NetworkMaxCompaniesAllowed()
{
	return _network_server ? _settings_client.network.max_companies : _network_server_max_companies;
}

/**
 * Check if max_companies has been reached on the server (local check only).
 * @return true if the max value has been reached or exceeded, false otherwise.
 */
bool NetworkMaxCompaniesReached()
{
	return Company::GetNumItems() >= NetworkMaxCompaniesAllowed();
}
