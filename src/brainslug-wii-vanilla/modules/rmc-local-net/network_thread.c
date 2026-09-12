
#include "network_thread.h"

#include <bslug.h>
#include <fcntl.h>
#include <rvl/dwc.h>
#include <rvl/OSTime.h>
#include <rvl/so.h>
#include <rvl/vi.h>
#include <stdbool.h>

#include "log.h"
#include "mkw.h"
#include "port_addresses.h"
#include "processor.h"

/* Keep 27900 as the first port for Wii consoles. Extra ports allow Dolphin
 * instances on one PC to bind separate sockets without changing the packet. */
#define BROADCAST_PORT 27900
#define BROADCAST_INTERVAL 60
#define BROADCAST_TIMEOUT (BROADCAST_INTERVAL * 5)
#define BROADCAST_MAGIC (('R' << 24) | ('M' << 16) | ('C' << 8) | 'X')

/* The original friend list has 30 slots. This is a discovery limit,
 * not the race limit: a race still has at most 12 players. */
#define MAX_LAN_PEERS 30
#define BROADCAST_PORT_COUNT MAX_LAN_PEERS
#define BROADCAST_TRACK_MAX MAX_LAN_PEERS

#define STATUS_MAGIC 0xbb49cc4d
#define STATUS_SIZE_MAX 512

uint8_t network_thread_stack[NETWORK_THREAD_SIZE];
OSThread_t network_thread;
uint32_t my_fake_pid;
uint32_t local_ip;
bool network_thread_running;

static uint64_t timestamp;
static so_fd_t broadcast_socket = -1;
static int broadcast_timer;
static uint16_t broadcast_port;
static int8_t game_state; // 1 is idling
static uint16_t communication_port;
static bool initialising_game_socket;
static const uint8_t *my_mii;
static uint16_t my_longitude;
static uint16_t my_latitude;
static uint8_t my_country;
static uint8_t my_state;
static uint64_t time_in_state_1;

static so_ret_t set_nonblock(so_fd_t fd) {
	int fcntl = SOFcntl(fd, F_GETFL, 0);
	so_ret_t r = fcntl < 0 ? fcntl : SOFcntl(fd, F_SETFL, fcntl | O_NONBLOCK);
	return r;
}

static struct tracked_peer {
	uint64_t last_update;
	uint32_t real_pid;
	uint16_t control_port;
	struct peer_info info;
	uint16_t longitude;
	uint16_t latitude;
	uint8_t country;
	uint8_t state;
	uint8_t mii[MII_SIZE];
} tracked_peers[BROADCAST_TRACK_MAX];

/* DWC picks a random game port. On a PC that port may already belong to
 * another application, or Windows may reserve it without opening a socket.
 * Retry the failed bind with port 0: IOS/Dolphin chooses and binds a free port
 * in one operation. No separate "probe then close" race, no DWC reinitialising.
 * Only the game-socket setup on our own thread is affected. In particular,
 * discovery must stay inside 27900..27929 so the other consoles can find us.
 * BSLUG calls to SOBind below refer to the original function, not this hook. */
static so_ret_t lan_game_SOBind(so_fd_t fd, const so_addr_t *addr) {
	so_ret_t result = SOBind(fd, addr);
	if (result >= 0 || !initialising_game_socket ||
		OSGetCurrentThread() != &network_thread || fd == broadcast_socket ||
		addr == NULL || addr->sa_family != AF_INET || addr->sa_port == 0)
		return result;

	so_addr_t fallback = *addr;
	fallback.sa_port = 0;
	LOG_WARN("game port %u bind failed (%d); requesting a free port",
		(unsigned)addr->sa_port, (int)result);
	result = SOBind(fd, &fallback);
	if (result < 0)
		LOG_ERROR("automatic game port bind failed (%d)", (int)result);
	return result;
}

static bool initialise(void) {
	if (game_state != 0)
		LOG_INFO("game_state %d -> 0", (int)game_state);
	game_state = 0;
	broadcast_timer = BROADCAST_INTERVAL;
	local_ip = SOGetHostID();
	if (local_ip == 0) {
		LOG_ERROR("SOGetHostID returned 0");
		return false;
	}

	broadcast_socket = SOSocket(PF_INET, SOCK_DGRAM, PROTO_UDP);
	if (broadcast_socket < 0) {
		LOG_ERROR("SOSocket returned %d", broadcast_socket);
		return false;
	}

	so_addr_t baddr = {
		.sa_len = sizeof(so_addr_t),
		.sa_family = AF_INET,
		.sa_port = 0,
		.sa_addr = 0
	};

	so_ret_t r = -1;

	for (int i = 0; i < BROADCAST_PORT_COUNT; i++) {
		baddr.sa_port = BROADCAST_PORT + i;

		r = SOBind(broadcast_socket, &baddr);

		if (r >= 0) {
			broadcast_port = baddr.sa_port;
			LOG_INFO("broadcast socket bound to port %u",
				(unsigned)broadcast_port);
			break;
		}
	}

	if (r < 0) {
		LOG_ERROR("could not bind any broadcast port");
		return false;
	}

	r = set_nonblock(broadcast_socket);
	if (r < 0) {
		LOG_ERROR("set_nonblock returned %d", r);
		return false;
	}

	/* The bound port distinguishes simultaneous instances on the same PC.
	 * Set the final PID before creating the game socket or advertising it. */
	mkw_setup_lan_pid(broadcast_port);
	LOG_INFO("local peer pid=%u ip=%x control=%u", (unsigned)my_fake_pid,
		(unsigned)local_ip, (unsigned)broadcast_port);

	initialising_game_socket = true;
	r = dwc_init_gt2_socket();
	initialising_game_socket = false;
	if (r != 0) {
		LOG_ERROR("dwc_init_gt2_socket returned %d", r);
		return false;
	}

	struct dwc_gamedata *gamedata = get_dwc_gamedata();
	/* Always advertise the port the socket actually owns, including fallback.
	 * GT2 also obtains its endpoint with SOGetSockName during socket creation. */
	so_addr_t game_addr = { .sa_len = sizeof(so_addr_t) };
	r = SOGetSockName(gamedata->gamedata2->socket->socket, &game_addr);
	if (r < 0 || game_addr.sa_port == 0) {
		LOG_ERROR("could not read bound game port (%d)", (int)r);
		return false;
	}
	communication_port = game_addr.sa_port;
	gamedata->gamedata2->socket->local_port = communication_port;
	LOG_INFO("game socket ready on port %u", (unsigned)communication_port);
	gamedata->gamedata2->online_status = 3;

	return true;
}

struct broadcast_packet {
	uint32_t magic;
	uint32_t pid;
	uint8_t mii[MII_SIZE];
	uint16_t communication_port;
	uint16_t longitude;
	uint16_t latitude;
	uint8_t country;
	uint8_t state;
	int8_t status;
} __attribute__((packed));

static void check_for_stuck_friend_join(struct dwc_gamedata *gamedata) {
	int isr;
	_CPU_ISR_Disable(isr);
	/* This code is looking to DC friend rooms if they get stuck on joining. */
	void *menu_data = *(void **)get_port_address(9);
	menu_data = menu_data ? *(void **)menu_data : 0;
	void *panel9b = menu_data ? ((void **)menu_data)[0x9b + 2] : 0;
	uint32_t *state = panel9b ? (uint32_t *)panel9b + (0x35c / 4) : 0;
	uint32_t start_state = state ? *state : 0;
	if (
		/* if the network has put us back on the menu OR */
		gamedata->gamedata2->online_status == 3 ||
		/* we're still joining AND */
		((game_state == 4 || game_state == 13) && (state && start_state == 1) && (
			/* we've somehow ended up as host OR */
			gamedata->dwc_match_state == DWC_MATCH_STATE_SV_WAITING ||
			/* we've taken too long. */
			ticks_to_secs(OSGetTime() - time_in_state_1) >= 65
		))
	) {
		LOG_INFO("game_state %d -> 0", (int)game_state);
		game_state = 0;
		/* prevent any state transition if a dialogue (panel4d) is open - it's probably a dc message :) */
		void *panel4d = menu_data ? ((void **)menu_data)[0x4d + 2] : 0;
		if (state && panel4d && ((int *)panel4d)[2] == 1) {
			switch (start_state) {
			case 1: *state = 2; break;
			case 3: *state = 2; break;
			default: break;
			}
			LOG_INFO("panel9b.state %u -> %u", (unsigned)start_state, (unsigned)*state);
		}
	}
	_CPU_ISR_Restore(isr);
}

static void update_game_state(struct dwc_gamedata *gamedata) {
	if (game_state != 0) {
		check_for_stuck_friend_join(gamedata);
		// Check if suspend has changed.
		int8_t old_state = game_state;
		switch (game_state) {
		case  3: if ( gamedata->dwc_suspend) game_state = 11; break;
		case  4: if ( gamedata->dwc_suspend) game_state = 13; break;
		case 11: if (!gamedata->dwc_suspend) game_state =  3; break;
		case 13: if (!gamedata->dwc_suspend) game_state =  4; break;
		}
		if (old_state != game_state)
			LOG_INFO("game_state %d -> %d", (int)old_state, (int)game_state);
	}
}

static bool compute_and_broadcast_status(so_fd_t fd) {
	struct dwc_gamedata *gamedata = get_dwc_gamedata();
	if (gamedata == NULL)
		// DCed from wifi
		return false;
	update_game_state(gamedata);

	struct broadcast_packet packet = {
		.magic = BROADCAST_MAGIC,
		.pid = my_fake_pid,
		.communication_port = communication_port,
		.status = game_state,
		.country = 0xff,
		.state = 0xff
	};
	if (my_mii) {
		for (int i = 0; i < MII_SIZE; i++)
			packet.mii[i] = my_mii[i];
		packet.longitude = my_longitude;
		packet.latitude = my_latitude;
		packet.country = my_country;
		packet.state = my_state;
	}
	so_addr_t baddr = {
		.sa_len = sizeof(so_addr_t),
		.sa_family = AF_INET,
		.sa_port = BROADCAST_PORT,
		.sa_addr = 0xffffffff
	};

	for (int i = 0; i < BROADCAST_PORT_COUNT; i++) {
		baddr.sa_port = BROADCAST_PORT + i;
		SOSendTo(fd, &packet, sizeof(packet), 0, &baddr);
	}
	return true;
}

static int find_or_alloc_peer(const so_addr_t *ip, uint32_t pid) {
    /*
     * Ignore only our own advertisement.
     *
     * The original LAN mod ignored every packet coming from our own IP.
     * That works for separate Wiis, but prevents multiple Dolphin instances
     * on the same PC from discovering each other.
     */
    if (pid == 0 || pid == my_fake_pid)
        return -1;

    /*
     * Identify an existing peer by its PID rather than its IP address.
     * Multiple Dolphin instances can share the same host IP.
     */
    for (int i = 0; i < BROADCAST_TRACK_MAX; i++) {
        if (tracked_peers[i].real_pid == pid) {
            tracked_peers[i].info.ip = ip->sa_addr;
            return i;
        }
    }

    /*
     * Allocate a new peer slot.
     */
    for (int i = 0; i < BROADCAST_TRACK_MAX; i++) {
        if (tracked_peers[i].info.ip == 0) {
            tracked_peers[i].info.ip = ip->sa_addr;
            tracked_peers[i].real_pid = pid;
            LOG_INFO("discovered peer pid=%u ip=%x control=%u", (unsigned)pid,
                (unsigned)ip->sa_addr, (unsigned)ip->sa_port);
            return i;
        }
    }

    return -1;
}
static int find_peer_by_fake_pid(uint32_t fake_pid) {
	if (fake_pid == 0) return -1;
	if (fake_pid < BROADCAST_TRACK_MAX + 1) return fake_pid - 1;
	return -1;
}
static int find_peer_by_pid(uint32_t pid) {
	if (pid == 0) return -1;
	if (pid < BROADCAST_TRACK_MAX + 1) return pid - 1;
	for (int i = 0; i < BROADCAST_TRACK_MAX; i++) {
		if (tracked_peers[i].real_pid == pid) {
			return i;
		}
	}
	return -1;
}

static void process_broadcast(const struct broadcast_packet buffer, const so_addr_t raddr) {
	if (buffer.magic != BROADCAST_MAGIC) return;
	/* IDs 1..30 are reserved for local friend-list aliases. */
	if (buffer.pid <= BROADCAST_TRACK_MAX || raddr.sa_port == 0 ||
		buffer.communication_port == 0) return;
	int peer = find_or_alloc_peer(&raddr, buffer.pid);
	if (peer == -1) return;
	tracked_peers[peer].control_port = raddr.sa_port;
	tracked_peers[peer].last_update = timestamp;
	tracked_peers[peer].real_pid = buffer.pid;
	tracked_peers[peer].info.communication_port = buffer.communication_port;
	tracked_peers[peer].info.status = buffer.status + 1;
	tracked_peers[peer].longitude = buffer.longitude;
	tracked_peers[peer].latitude = buffer.latitude;
	tracked_peers[peer].country = buffer.country;
	tracked_peers[peer].state = buffer.state;
	for (int i = 0; i < MII_SIZE; i++)
		tracked_peers[peer].mii[i] = buffer.mii[i];

	int isr;
	_CPU_ISR_Disable(isr);
	void *menu_data = *(void **)get_port_address(9);
	menu_data = menu_data ? *(void **)menu_data : 0;
	struct menu_friend_list_friend_full *ff = menu_data ? ((void **)menu_data)[0x404 / 4] : 0;
	struct menu_friend_list_friend *f = ff ? &ff[peer].f : 0;
	if (f && !f->mii_known) {
		f->longitude = tracked_peers[peer].longitude;
		f->latitude = tracked_peers[peer].latitude;
		f->country = tracked_peers[peer].country;
		f->state = tracked_peers[peer].state;
		f->mii_known = 1;
		for (int i = 0; i < MII_SIZE; i++)
			f->mii[i] = tracked_peers[peer].mii[i];
		f->flags |= 1;
	}
	_CPU_ISR_Restore(isr);
}

static void process_status(uint32_t *buffer, int32_t length, const so_addr_t raddr) {
	LOG_INFO("receive size %u from %x:%u", (unsigned)length, (unsigned)raddr.sa_addr, (unsigned)raddr.sa_port);
	if (length < 0x14) return;
	if (length < 0x14 + ((buffer[2] >> 16) & 0xff)) return;
	if (buffer[0] != STATUS_MAGIC) return;
	LOG_INFO("receive status message %u from %x:%u", (unsigned)(buffer[2] >> 24), (unsigned)raddr.sa_addr, (unsigned)raddr.sa_port);
	struct dwc_gamedata *gamedata = get_dwc_gamedata();
	if (gamedata == NULL) return;
	gamedata->public_ip = FAKE_PUBLIC_IP;
	dwc_process_status_record(buffer[2] >> 24, __builtin_bswap32(buffer[4]), raddr.sa_addr, raddr.sa_port, buffer + 5, (buffer[2] >> 18) & 0x3f);
}

bool network_thread_get_fake_pid_status(uint32_t pid, uint8_t *status) {
	struct ls {
		uint32_t gid;
		uint8_t region;
		uint8_t state;
		uint8_t player_count;
		uint8_t race_number;
	};
	int peer = find_peer_by_fake_pid(pid);
	if (peer == -1) return false;
	if (tracked_peers[peer].info.status == 0) return false;
	*(struct ls *)status = (struct ls){
		.gid = pid,
		.region = 0xff,
		.state = (tracked_peers[peer].info.status - 1) & 0x3f,
		.player_count = 12,
		.race_number = tracked_peers[peer].info.status >> 6
	};
	return true;
}

static void check_for_status_or_broadcast() {
	so_ret_t r;
	do {
		uint32_t buffer[STATUS_SIZE_MAX / 4];
		so_addr_t raddr = { .sa_len = sizeof(so_addr_t) };
		r = SORecvFrom(broadcast_socket, &buffer, sizeof(buffer), 0, &raddr);
		if (r <= 0) break;
		if (r < 0x4) {
			LOG_ERROR("received overly short message. %u length", (unsigned)r);
			continue;
		}
		if (buffer[0] == STATUS_MAGIC) {
			process_status(buffer, r, raddr);
		} else if (buffer[0] == BROADCAST_MAGIC) {
			if (r != sizeof(struct broadcast_packet)) continue;
			process_broadcast(*(const struct broadcast_packet *)buffer, raddr);
		} else {
			LOG_ERROR("received unknown message. %u length %x header", (unsigned)r, (unsigned)buffer[0]);
		}
	} while (r > 0);
	for (int i = 0; i < BROADCAST_TRACK_MAX; i++) {
		if (tracked_peers[i].last_update + BROADCAST_TIMEOUT < timestamp) {
			tracked_peers[i].info.status = 0;
		}
	}
}

void* network_thread_main(void *arg) {
	network_thread_running = true;
	LOG_INFO("network_thread startup");
	if (!initialise()) {
		mkw_net_state->connection_state = 5; // idk lol
		goto cleanup;
	}
	LOG_INFO("network_thread initialised");
	mkw_net_state->connection_state = 5;
	while (1) {
		timestamp++;
		broadcast_timer++;
		if (broadcast_timer >= BROADCAST_INTERVAL) {
			broadcast_timer = 0;
			if (!compute_and_broadcast_status(broadcast_socket))
				break;
		}
		check_for_status_or_broadcast();
		VIWaitForRetrace();
	}
cleanup:
	initialising_game_socket = false;
	communication_port = 0;
	/* Release the control port on normal exit AND on partial startup failure.
	 * The GT2 socket belongs to DWC and is not closed here. */
	if (broadcast_socket >= 0) {
		SOClose(broadcast_socket);
		broadcast_socket = -1;
	}
	broadcast_port = 0;
	LOG_INFO("network_thread exit");
	my_mii = 0;
	for (int i = 0; i < BROADCAST_TRACK_MAX; i++)
		tracked_peers[i] = (struct tracked_peer){ };
	network_thread_running = false;
	return NULL;
}

static void fill_in_peer_0(struct dwc_gamedata *gamedata) {
	gamedata->public_port = communication_port;
	gamedata->public_ip = FAKE_PUBLIC_IP;
	gamedata->peers[0].player_id = gamedata->my_pid;
	gamedata->peers[0].public_ip = FAKE_PUBLIC_IP;
	gamedata->peers[0].local_ip = local_ip;
	gamedata->peers[0].public_port = communication_port;
	gamedata->peers[0].private_port = communication_port;
	gamedata->peers[0].communication_ip = 0;
	gamedata->peers[0].communication_port = 0;
	gamedata->peers[0]._unk17[0] = 1;
	gamedata->peers[0].players_at_console_little_endian = gamedata->players_at_console_little_endian;
}

void network_thread_after_server_setup(struct dwc_gamedata *gamedata) {
	LOG_INFO("network_thread_after_server_setup()");
	fill_in_peer_0(gamedata);
	gamedata->peers[0].peer_index = 0;
	gamedata->player_count = 1;
	gamedata->dwc_match_state = DWC_MATCH_STATE_SV_WAITING;
	LOG_INFO("game_state %d -> 3", (int)game_state);
	game_state = 3;
}

void network_thread_on_server_join(struct dwc_gamedata *gamedata) {
	LOG_INFO("network_thread_on_server_join()");
	fill_in_peer_0(gamedata);
	gamedata->peers[0].peer_index = 0xff;
	gamedata->player_count = 1;
	gamedata->sb_self_lookup_complete = 1;
	LOG_INFO("game_state %d -> 4", (int)game_state);
	game_state = 4;
	time_in_state_1 = OSGetTime();
}

void network_thread_after_server_join(struct dwc_gamedata *gamedata) {
	LOG_INFO("network_thread_after_server_join()");
	int peer = find_peer_by_pid(gamedata->host_pid);
	if (peer != -1) {
		gamedata->host_pid = tracked_peers[peer].real_pid;
		LOG_INFO("host_pid set to %d", (int)tracked_peers[peer].real_pid);
	} else {
		LOG_ERROR("could not set host_pid");
	}
}

bool network_thread_send_status_record(int type, uint32_t pid, uint32_t ip, uint16_t port, const uint32_t *packet, uint32_t packet_len_in_words) {
	/* The wire header has one byte for the payload length (in bytes).
	 * Bound the stack allocation too; never silently wrap that length. */
	if (packet_len_in_words > 63 || (packet_len_in_words != 0 && packet == NULL) ||
		broadcast_socket < 0) return false;
	if (type == 2 || type == 6) {
		struct dwc_gamedata *gamedata = get_dwc_gamedata();
		gamedata->last_enter_type = type;
		gamedata->last_enter_port = port;
		gamedata->last_enter_ip = ip;
		gamedata->last_enter_pid = pid;
		gamedata->last_enter_length = packet_len_in_words;
		gamedata->last_enter_timestamp = OSGetTime();
	}

	int peer = find_peer_by_pid(pid);
	if (peer == -1) {
		LOG_ERROR("network_thread_send_status_record to unknown peer %u", (unsigned)pid);
		return false;
	}
	if (tracked_peers[peer].info.ip == 0) {
		LOG_ERROR("network_thread_send_status_record to peer with no ip");
		return false;
	}
	if (tracked_peers[peer].control_port == 0) {
		LOG_ERROR("network_thread_send_status_record to peer with no control port");
		return false;
	}
	LOG_INFO("network_thread_send_status_record %d to %x:%u", type, (unsigned)tracked_peers[peer].info.ip, (unsigned)tracked_peers[peer].control_port);

	uint32_t full_packet[packet_len_in_words + 5];
	for (int i = 0; i < packet_len_in_words; i++)
		full_packet[i + 5] = packet[i];
	full_packet[0] = STATUS_MAGIC;
	full_packet[1] = __builtin_bswap32(90);
	full_packet[2] = (type << 24) | ((packet_len_in_words * 4) << 16) | communication_port;
	full_packet[3] = FAKE_PUBLIC_IP;
	full_packet[4] = __builtin_bswap32(my_fake_pid);

	const so_addr_t daddr = {
		.sa_len = sizeof(so_addr_t),
		.sa_family = AF_INET,
		.sa_port = tracked_peers[peer].control_port,
		.sa_addr = tracked_peers[peer].info.ip
	};
	so_ret_t r = SOSendTo(broadcast_socket, &full_packet, sizeof(full_packet), 0, &daddr);
	if (r < 0) {
		LOG_ERROR("SOSendTo returned %d", r);
	}
	return r >= 0;
}

void network_thread_set_mii(const uint8_t mii[MII_SIZE], uint16_t longitude, uint16_t latitude, uint8_t country, uint8_t state) {
	my_mii = mii;
	my_longitude = longitude;
	my_latitude = latitude;
	my_country = country;
	my_state = state;
}

void network_thread_set_friend(int index, struct menu_friend_list_friend *f) {
	int isr;
	_CPU_ISR_Disable(isr);
	f->friend_code = index + 1;
	f->mii_known = tracked_peers[index].info.status != 0;
	f->longitude = tracked_peers[index].longitude;
	f->latitude = tracked_peers[index].latitude;
	f->country = tracked_peers[index].country;
	f->state = tracked_peers[index].state;
	if (f->mii_known)
		for (int i = 0; i < MII_SIZE; i++)
			f->mii[i] = tracked_peers[index].mii[i];
	f->flags = 0x1388 | f->mii_known;
	_CPU_ISR_Restore(isr);
}

struct peer_info network_get_peer_info(unsigned int index) {
	if (index >= BROADCAST_TRACK_MAX) return (struct peer_info) { };
	return tracked_peers[index].info;
}

BSLUG_MUST_REPLACE(SOBind, lan_game_SOBind);
