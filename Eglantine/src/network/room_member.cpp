// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: Copyright 2017 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include "multiplayer_logging.h"
#include "multiplayer_validation.h"
#include "multiplayer_send.h"
#include "multiplayer_nonblocking.h"
#include <list>
#include <mutex>
#include <set>
#include <thread>
#include "common/assert.h"
#include "common/polyfill_thread.h"
#include "common/socket_types.h"
#include "enet/enet.h"
#include "network/packet.h"
#include "network/room_member.h"

namespace Network {

constexpr u32 ConnectionTimeoutMs = 5000;

class RoomMember::RoomMemberImpl {
public:
    void SetState(const State new_state) noexcept {
        if (new_state != State::Joined && new_state != State::Moderator) ping_ms = -1;
        if (state != new_state) {
            state = new_state;
            Invoke<State>(state);
        }
    }

    void SetError(const Error new_error) noexcept {
        Invoke<Error>(new_error);
    }

    [[nodiscard]] bool IsConnected() const noexcept {
        return state == State::Joining || state == State::Joined || state == State::Moderator;
    }

    std::atomic<int> ping_ms{-1};
    ENetHost* client = nullptr; ///< ENet network interface.
    ENetPeer* server = nullptr; ///< The server peer the client is connected to

    /// Information about the clients connected to the same room as us.
    MemberList member_information;
    /// Information about the room we're connected to.
    RoomInformation room_information;

    /// The current game name, id and version
    GameInfo current_game_info;

    std::atomic<State> state{State::Idle}; ///< Current state of the RoomMember.

    std::string nickname; ///< The nickname of this member.

    std::string username;              ///< The username of this member.
    mutable std::mutex username_mutex; ///< Mutex for locking username.

    IPv4Address fake_ip; ///< The fake ip of this member.

    std::mutex network_mutex; ///< Mutex that controls access to the `client` variable.
    /// Thread that receives and dispatches network packets
    std::optional<std::jthread> loop_thread;
    std::mutex send_list_mutex;  ///< Mutex that controls access to the `send_list` variable.
    Eden::Multiplayer::RoomSendBudget send_list_budget, enet_send_budget;
    std::atomic<bool> send_failed{false};
    std::vector<Packet> send_list; ///< A list that stores all packets to send the async

    template <typename T>
    using CallbackSet = std::set<CallbackHandle<T>>;
    std::mutex callback_mutex; ///< The mutex used for handling callbacks

    class Callbacks {
    public:
        template <typename T>
        CallbackSet<T>& Get();

    private:
        CallbackSet<ProxyPacket> callback_set_proxy_packet;
        CallbackSet<LDNPacket> callback_set_ldn_packet;
        CallbackSet<ChatEntry> callback_set_chat_messages;
        CallbackSet<StatusMessageEntry> callback_set_status_messages;
        CallbackSet<RoomInformation> callback_set_room_information;
        CallbackSet<State> callback_set_state;
        CallbackSet<Error> callback_set_error;
        CallbackSet<Room::BanList> callback_set_ban_list;
    };
    Callbacks callbacks; ///< All CallbackSets to all events

    void StartLoop();

    /**
     * Sends data to the room. It will be send on channel 0 with flag RELIABLE
     * @param packet The data to send
     */
    void Send(Packet&& packet);

    /**
     * Sends a request to the server, asking for permission to join a room with the specified
     * nickname and preferred fake ip.
     * @params nickname The desired nickname.
     * @params preferred_fake_ip The preferred IP address to use in the room, the NoPreferredIP
     * tells
     * @params password The password for the room
     * the server to assign one for us.
     */
    void SendJoinRequest(const std::string& nickname_,
                         const IPv4Address& preferred_fake_ip = NoPreferredIP,
                         const std::string& password = "", const std::string& token = "");

    /**
     * Extracts a MAC Address from a received ENet packet.
     * @param event The ENet event that was received.
     */
    void HandleJoinPacket(const ENetEvent* event);
    /**
     * Extracts RoomInformation and MemberInformation from a received ENet packet.
     * @param event The ENet event that was received.
     */
    void HandleRoomInformationPacket(const ENetEvent* event);

    /**
     * Extracts a ProxyPacket from a received ENet packet.
     * @param event The ENet event that was received.
     */
    void HandleProxyPackets(const ENetEvent* event);

    /**
     * Extracts an LdnPacket from a received ENet packet.
     * @param event The ENet event that was received.
     */
    void HandleLdnPackets(const ENetEvent* event);

    /**
     * Extracts a chat entry from a received ENet packet and adds it to the chat queue.
     * @param event The ENet event that was received.
     */
    void HandleChatPacket(const ENetEvent* event);

    /**
     * Extracts a system message entry from a received ENet packet and adds it to the system message
     * queue.
     * @param event The ENet event that was received.
     */
    void HandleStatusMessagePacket(const ENetEvent* event);

    /**
     * Extracts a ban list request response from a received ENet packet.
     * @param event The ENet event that was received.
     */
    void HandleModBanListResponsePacket(const ENetEvent* event);

    /**
     * Disconnects the RoomMember from the Room
     */
    void Disconnect();

    template <typename T>
    void Invoke(const T& data);

    template <typename T>
    CallbackHandle<T> Bind(std::function<void(const T&)> callback);
};

// RoomMemberImpl
void RoomMember::RoomMemberImpl::StartLoop() {
    loop_thread.emplace([&](std::stop_token stoken) {
        auto next_diagnostic = std::chrono::steady_clock::time_point{};
        // Receive packets while the connection is open
        while (IsConnected()) {
            std::lock_guard lock(network_mutex);
            ENetEvent event;
            if (enet_host_service(client, &event, 5) > 0) {
                switch (event.type) {
                case ENET_EVENT_TYPE_RECEIVE:
                    if (!Eden::Multiplayer::ValidRoomPacket(
                            {event.packet->data, event.packet->dataLength})) {
                        Eden::Multiplayer::Debug("Rejected malformed room packet");
                        enet_packet_destroy(event.packet);
                        SetState(State::Idle);
                        SetError(Error::UnknownError);
                        break;
                    }
                    switch (event.packet->data[0]) {
                    case IdProxyPacket:
                        HandleProxyPackets(&event);
                        break;
                    case IdLdnPacket:
                        HandleLdnPackets(&event);
                        break;
                    case IdChatMessage:
                        HandleChatPacket(&event);
                        break;
                    case IdStatusMessage:
                        HandleStatusMessagePacket(&event);
                        break;
                    case IdRoomInformation:
                        HandleRoomInformationPacket(&event);
                        break;
                    case IdJoinSuccess:
                    case IdJoinSuccessAsMod:
                        // The join request was successful, we are now in the room.
                        // If we joined successfully, there must be at least one client in the room: us.
                        if (state != State::Joining || member_information.empty()) {
                            SetState(State::Idle);
                            SetError(Error::UnknownError);
                            break;
                        }
                        HandleJoinPacket(&event); // Get the MAC Address for the client
                        if (event.packet->data[0] == IdJoinSuccessAsMod) {
                            SetState(State::Moderator);
                        } else {
                            SetState(State::Joined);
                        }
                        break;
                    case IdModBanListResponse:
                        HandleModBanListResponsePacket(&event);
                        break;
                    case IdRoomIsFull:
                        SetState(State::Idle);
                        SetError(Error::RoomIsFull);
                        break;
                    case IdNameCollision:
                        SetState(State::Idle);
                        SetError(Error::NameCollision);
                        break;
                    case IdIpCollision:
                        SetState(State::Idle);
                        SetError(Error::IpCollision);
                        break;
                    case IdVersionMismatch:
                        SetState(State::Idle);
                        SetError(Error::WrongVersion);
                        break;
                    case IdWrongPassword:
                        SetState(State::Idle);
                        SetError(Error::WrongPassword);
                        break;
                    case IdCloseRoom:
                        SetState(State::Idle);
                        SetError(Error::LostConnection);
                        break;
                    case IdHostKicked:
                        SetState(State::Idle);
                        SetError(Error::HostKicked);
                        break;
                    case IdHostBanned:
                        SetState(State::Idle);
                        SetError(Error::HostBanned);
                        break;
                    case IdModPermissionDenied:
                        SetError(Error::PermissionDenied);
                        break;
                    case IdModNoSuchUser:
                        SetError(Error::NoSuchUser);
                        break;
                    }
                    enet_packet_destroy(event.packet);
                    break;
                case ENET_EVENT_TYPE_DISCONNECT:
                    if (IsConnected()) {
                        SetState(State::Idle);
                        SetError(Error::LostConnection);
                    }
                    break;
                case ENET_EVENT_TYPE_NONE:
                    break;
                case ENET_EVENT_TYPE_CONNECT:
                    // The ENET_EVENT_TYPE_CONNECT event can not possibly happen here because we're
                    // already connected
                    ASSERT_MSG(false, "Received unexpected connect event while already connected");
                    break;
                }
            }
            if (send_failed.exchange(false)) {
                SetState(State::Idle);
                SetError(Error::LostConnection);
            }
            if (!IsConnected()) break;
            std::vector<Packet> packets;
            {
                std::lock_guard send_lock(send_list_mutex);
                packets.swap(send_list);
                send_list_budget.Reset();
            }
            for (auto const& packet : packets) {
                const auto size = packet.GetDataSize();
                if (!enet_send_budget.Add(size)) {
                    Eden::Multiplayer::Debug("Outstanding ENet send budget exceeded");
                    SetState(State::Idle);
                    SetError(Error::LostConnection);
                    break;
                }
                ENetPacket* enetPacket = enet_packet_create(packet.GetData(), size, ENET_PACKET_FLAG_RELIABLE);
                if (!enetPacket) {
                    Eden::Multiplayer::Debug("Room packet allocation failed");
                    enet_send_budget.Release(size);
                    SetState(State::Idle);
                    SetError(Error::LostConnection);
                    break;
                }
                enetPacket->userData = &enet_send_budget;
                enetPacket->freeCallback = [](ENetPacket* sent) {
                    static_cast<Eden::Multiplayer::RoomSendBudget*>(sent->userData)->Release(sent->dataLength);
                };
                if (enet_peer_send(server, 0, enetPacket) != 0) {
                    Eden::Multiplayer::Debug("ENet send failed");
                    enet_packet_destroy(enetPacket);
                    SetState(State::Idle);
                    SetError(Error::LostConnection);
                    break;
                }
            }
            // Read ENet's measurement only on the thread that owns the peer.
            if (server && (state == State::Joined || state == State::Moderator)) {
                ping_ms = static_cast<int>(std::min(server->roundTripTime, enet_uint32{9999}));
                if (Eden::Multiplayer::detailed_logging.load()) {
                    const auto now = std::chrono::steady_clock::now();
                    if (now >= next_diagnostic) {
                        char detail[192];
                        std::snprintf(detail, sizeof(detail),
                            "Transport: rtt_ms=%u sent_packets=%u received_packets=%u enet_waiting_bytes=%zu",
                            server->roundTripTime, client->totalSentPackets,
                            client->totalReceivedPackets, server->totalWaitingData);
                        Eden::Multiplayer::Debug(detail);
                        next_diagnostic = now + std::chrono::seconds{5};
                    }
                }
            }
            enet_host_flush(client);
        }
        ping_ms = -1;
        Disconnect();
    });
}

void RoomMember::RoomMemberImpl::Send(Packet&& packet) {
    std::lock_guard lock(send_list_mutex);
    if (!IsConnected() || send_failed) return;
    if (!send_list_budget.Add(packet.GetDataSize())) {
        Eden::Multiplayer::Debug("Outgoing room queue limit exceeded");
        send_failed = true;
        return;
    }
    send_list.push_back(std::move(packet));
}

void RoomMember::RoomMemberImpl::SendJoinRequest(const std::string& nickname_,
                                                 const IPv4Address& preferred_fake_ip,
                                                 const std::string& password,
                                                 const std::string& token) {
    Packet packet;
    packet.Write(static_cast<u8>(IdJoinRequest));
    packet.Write(nickname_);
    packet.Write(preferred_fake_ip);
    packet.Write(network_version);
    packet.Write(password);
    packet.Write(token);
    Send(std::move(packet));
}

void RoomMember::RoomMemberImpl::HandleRoomInformationPacket(const ENetEvent* event) {
    Packet packet;
    packet.Append(event->packet->data, event->packet->dataLength);

    // Ignore the first byte, which is the message id.
    packet.IgnoreBytes(sizeof(u8)); // Ignore the message type

    RoomInformation info{};
    packet.Read(info.name);
    packet.Read(info.description);
    packet.Read(info.member_slots);
    packet.Read(info.port);
    packet.Read(info.preferred_game.name);
    packet.Read(info.host_username);
    room_information.name = info.name;
    room_information.description = info.description;
    room_information.member_slots = info.member_slots;
    room_information.port = info.port;
    room_information.preferred_game = info.preferred_game;
    room_information.host_username = info.host_username;

    u32 num_members;
    packet.Read(num_members);
    member_information.resize(num_members);

    for (auto& member : member_information) {
        packet.Read(member.nickname);
        packet.Read(member.fake_ip);
        packet.Read(member.game_info.name);
        packet.Read(member.game_info.id);
        packet.Read(member.game_info.version);
        packet.Read(member.username);
        packet.Read(member.display_name);
        packet.Read(member.avatar_url);

        {
            std::lock_guard lock(username_mutex);
            if (member.nickname == nickname) {
                username = member.username;
            }
        }
    }
    Invoke(room_information);
}

void RoomMember::RoomMemberImpl::HandleJoinPacket(const ENetEvent* event) {
    Packet packet;
    packet.Append(event->packet->data, event->packet->dataLength);

    // Ignore the first byte, which is the message id.
    packet.IgnoreBytes(sizeof(u8)); // Ignore the message type

    // Parse the MAC Address from the packet
    packet.Read(fake_ip);
}

void RoomMember::RoomMemberImpl::HandleProxyPackets(const ENetEvent* event) {
    ProxyPacket proxy_packet{};
    Packet packet;
    packet.Append(event->packet->data, event->packet->dataLength);

    // Ignore the first byte, which is the message id.
    packet.IgnoreBytes(sizeof(u8)); // Ignore the message type

    // Parse the ProxyPacket from the packet
    u8 local_family;
    packet.Read(local_family);
    proxy_packet.local_endpoint.family = static_cast<Domain>(local_family);
    packet.Read(proxy_packet.local_endpoint.ip);
    packet.Read(proxy_packet.local_endpoint.portno);

    u8 remote_family;
    packet.Read(remote_family);
    proxy_packet.remote_endpoint.family = static_cast<Domain>(remote_family);
    packet.Read(proxy_packet.remote_endpoint.ip);
    packet.Read(proxy_packet.remote_endpoint.portno);

    u8 protocol_type;
    packet.Read(protocol_type);
    proxy_packet.protocol = static_cast<Protocol>(protocol_type);

    packet.Read(proxy_packet.broadcast);
    packet.Read(proxy_packet.data);

    Invoke<ProxyPacket>(proxy_packet);
}

void RoomMember::RoomMemberImpl::HandleLdnPackets(const ENetEvent* event) {
    LDNPacket ldn_packet{};
    Packet packet;
    packet.Append(event->packet->data, event->packet->dataLength);

    // Ignore the first byte, which is the message id.
    packet.IgnoreBytes(sizeof(u8)); // Ignore the message type

    u8 packet_type;
    packet.Read(packet_type);
    ldn_packet.type = static_cast<LDNPacketType>(packet_type);

    packet.Read(ldn_packet.local_ip);
    packet.Read(ldn_packet.remote_ip);
    packet.Read(ldn_packet.broadcast);

    packet.Read(ldn_packet.data);

    Invoke<LDNPacket>(ldn_packet);
}

void RoomMember::RoomMemberImpl::HandleChatPacket(const ENetEvent* event) {
    Packet packet;
    packet.Append(event->packet->data, event->packet->dataLength);

    // Ignore the first byte, which is the message id.
    packet.IgnoreBytes(sizeof(u8));

    ChatEntry chat_entry{};
    packet.Read(chat_entry.nickname);
    packet.Read(chat_entry.username);
    packet.Read(chat_entry.message);
    Invoke<ChatEntry>(chat_entry);
}

void RoomMember::RoomMemberImpl::HandleStatusMessagePacket(const ENetEvent* event) {
    Packet packet;
    packet.Append(event->packet->data, event->packet->dataLength);

    // Ignore the first byte, which is the message id.
    packet.IgnoreBytes(sizeof(u8));

    StatusMessageEntry status_message_entry{};
    u8 type{};
    packet.Read(type);
    status_message_entry.type = static_cast<StatusMessageTypes>(type);
    packet.Read(status_message_entry.nickname);
    packet.Read(status_message_entry.username);
    Invoke<StatusMessageEntry>(status_message_entry);
}

void RoomMember::RoomMemberImpl::HandleModBanListResponsePacket(const ENetEvent* event) {
    Packet packet;
    packet.Append(event->packet->data, event->packet->dataLength);

    // Ignore the first byte, which is the message id.
    packet.IgnoreBytes(sizeof(u8));

    Room::BanList ban_list = {};
    packet.Read(ban_list.first);
    packet.Read(ban_list.second);
    Invoke<Room::BanList>(ban_list);
}

void RoomMember::RoomMemberImpl::Disconnect() {
    member_information.clear();
    room_information.member_slots = 0;
    room_information.name.clear();

    if (!server) {
        return;
    }
    enet_peer_disconnect(server, 0);

    ENetEvent event;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
    while (std::chrono::steady_clock::now() < deadline) {
        if (enet_host_service(client, &event, 20) <= 0) continue;
        switch (event.type) {
        case ENET_EVENT_TYPE_RECEIVE:
            enet_packet_destroy(event.packet); // Ignore all incoming data
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
            server = nullptr;
            return;
        case ENET_EVENT_TYPE_NONE:
        case ENET_EVENT_TYPE_CONNECT:
            break;
        }
    }
    // didn't disconnect gracefully force disconnect
    enet_peer_reset(server);
    server = nullptr;
}

template <>
RoomMember::RoomMemberImpl::CallbackSet<ProxyPacket>& RoomMember::RoomMemberImpl::Callbacks::Get() {
    return callback_set_proxy_packet;
}

template <>
RoomMember::RoomMemberImpl::CallbackSet<LDNPacket>& RoomMember::RoomMemberImpl::Callbacks::Get() {
    return callback_set_ldn_packet;
}

template <>
RoomMember::RoomMemberImpl::CallbackSet<RoomMember::State>&
RoomMember::RoomMemberImpl::Callbacks::Get() {
    return callback_set_state;
}

template <>
RoomMember::RoomMemberImpl::CallbackSet<RoomMember::Error>&
RoomMember::RoomMemberImpl::Callbacks::Get() {
    return callback_set_error;
}

template <>
RoomMember::RoomMemberImpl::CallbackSet<RoomInformation>&
RoomMember::RoomMemberImpl::Callbacks::Get() {
    return callback_set_room_information;
}

template <>
RoomMember::RoomMemberImpl::CallbackSet<ChatEntry>& RoomMember::RoomMemberImpl::Callbacks::Get() {
    return callback_set_chat_messages;
}

template <>
RoomMember::RoomMemberImpl::CallbackSet<StatusMessageEntry>&
RoomMember::RoomMemberImpl::Callbacks::Get() {
    return callback_set_status_messages;
}

template <>
RoomMember::RoomMemberImpl::CallbackSet<Room::BanList>&
RoomMember::RoomMemberImpl::Callbacks::Get() {
    return callback_set_ban_list;
}

template <typename T>
void RoomMember::RoomMemberImpl::Invoke(const T& data) {
    std::lock_guard lock(callback_mutex);
    CallbackSet<T> callback_set = callbacks.Get<T>();
    for (auto const& callback : callback_set) {
        (*callback)(data);
    }
}

template <typename T>
RoomMember::CallbackHandle<T> RoomMember::RoomMemberImpl::Bind(
    std::function<void(const T&)> callback) {
    std::lock_guard lock(callback_mutex);
    CallbackHandle<T> handle;
    handle = std::make_shared<std::function<void(const T&)>>(callback);
    callbacks.Get<T>().insert(handle);
    return handle;
}

// RoomMember
RoomMember::RoomMember() : room_member_impl{std::make_unique<RoomMemberImpl>()} {}

RoomMember::~RoomMember() {
    ASSERT_MSG(!IsConnected(), "RoomMember is being destroyed while connected");
    if (room_member_impl->loop_thread) {
        Leave();
    }
}

RoomMember::State RoomMember::GetState() const {
    return room_member_impl->state;
}

const RoomMember::MemberList& RoomMember::GetMemberInformation() const {
    return room_member_impl->member_information;
}

const std::string& RoomMember::GetNickname() const {
    return room_member_impl->nickname;
}

const std::string& RoomMember::GetUsername() const {
    std::lock_guard lock(room_member_impl->username_mutex);
    return room_member_impl->username;
}

const IPv4Address& RoomMember::GetFakeIpAddress() const {
    ASSERT_MSG(IsConnected(), "Tried to get fake ip address while not connected");
    return room_member_impl->fake_ip;
}

RoomInformation RoomMember::GetRoomInformation() const {
    return room_member_impl->room_information;
}

void RoomMember::Join(const std::string& nick, const char* server_addr, u16 server_port,
                      u16 client_port, const IPv4Address& preferred_fake_ip,
                      const std::string& password, const std::string& token, std::stop_token stop) {
    // If the member is connected, kill the connection first
    if (room_member_impl->loop_thread && room_member_impl->loop_thread->joinable()) {
        Leave();
    }
    // If the thread isn't running but the ptr still exists, reset it
    else if (room_member_impl->loop_thread) {
        room_member_impl->loop_thread.reset();
    }

    if (!room_member_impl->client) {
        room_member_impl->client = enet_host_create(nullptr, 1, NumChannels, 0, 0);
        if (!room_member_impl->client) {
            char detail[96];
            std::snprintf(detail, sizeof(detail), "ENet client creation failed: errno=%d", errno);
            Eden::Multiplayer::Debug(detail);
            room_member_impl->SetState(State::Idle);
            room_member_impl->SetError(Error::CouldNotConnect);
            return;
        }
        room_member_impl->client->maximumPacketSize = Eden::Multiplayer::MaxRoomPacketBytes;
        room_member_impl->client->maximumWaitingData = Eden::Multiplayer::MaxRoomPacketBytes;
        // ENet ignores a failed fcntl(O_NONBLOCK); a blocking socket stalls the member loop.
        if (!Eden::Multiplayer::MakeSocketNonBlocking(room_member_impl->client->socket)) {
            char detail[96];
            std::snprintf(detail, sizeof(detail),
                "Room socket could not be made non-blocking: errno=%d", errno);
            Eden::Multiplayer::Debug(detail);
            enet_host_destroy(room_member_impl->client);
            room_member_impl->client = nullptr;
            room_member_impl->SetState(State::Idle);
            room_member_impl->SetError(Error::CouldNotConnect);
            return;
        }
        Eden::Multiplayer::Debug("Room socket is non-blocking");
    }

    room_member_impl->SetState(State::Joining);

    ENetAddress address{};
    if (stop.stop_requested()) {
        room_member_impl->SetState(State::Idle);
        return;
    }
    // The controller already resolves IPv4; avoid a second platform DNS lookup.
    if (enet_address_set_host_ip(&address, server_addr) != 0 &&
        enet_address_set_host(&address, server_addr) != 0) {
        Eden::Multiplayer::Debug("ENet address conversion failed");
        room_member_impl->SetState(State::Idle);
        room_member_impl->SetError(Error::CouldNotConnect);
        return;
    }
    address.port = server_port;
    room_member_impl->server =
        enet_host_connect(room_member_impl->client, &address, NumChannels, 0);

    if (!room_member_impl->server) {
        room_member_impl->SetState(State::Idle);
        room_member_impl->SetError(Error::UnknownError);
        return;
    }

    ENetEvent event{};
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds{ConnectionTimeoutMs};
    int net = 0;
    while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
        net = enet_host_service(room_member_impl->client, &event, 20);
        if (net != 0) break;
    }
    if (net <= 0 && !stop.stop_requested()) {
        const int error = net < 0 ? errno : 0;
        char detail[192];
        std::snprintf(detail, sizeof(detail),
            "ENet handshake: result=%d errno=%d sent_packets=%u received_packets=%u",
            net, error, room_member_impl->client->totalSentPackets,
            room_member_impl->client->totalReceivedPackets);
        Eden::Multiplayer::Debug(detail);
    }
    if (stop.stop_requested()) {
        if (net > 0 && event.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(event.packet);
        enet_peer_reset(room_member_impl->server);
        room_member_impl->server = nullptr;
        room_member_impl->SetState(State::Idle);
        return;
    }
    if (net > 0 && event.type == ENET_EVENT_TYPE_CONNECT) {
        room_member_impl->nickname = nick;
        room_member_impl->StartLoop();
        room_member_impl->SendJoinRequest(nick, preferred_fake_ip, password, token);
        SendGameInfo(room_member_impl->current_game_info);
    } else {
        if (net > 0 && event.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(event.packet);
        enet_peer_reset(room_member_impl->server);
        room_member_impl->server = nullptr;
        room_member_impl->SetState(State::Idle);
        room_member_impl->SetError(Error::CouldNotConnect);
    }
}

bool RoomMember::IsConnected() const {
    return room_member_impl->IsConnected();
}

void RoomMember::SendProxyPacket(const ProxyPacket& proxy_packet) {
    Packet packet;
    packet.Write(static_cast<u8>(IdProxyPacket));

    packet.Write(static_cast<u8>(proxy_packet.local_endpoint.family));
    packet.Write(proxy_packet.local_endpoint.ip);
    packet.Write(proxy_packet.local_endpoint.portno);

    packet.Write(static_cast<u8>(proxy_packet.remote_endpoint.family));
    packet.Write(proxy_packet.remote_endpoint.ip);
    packet.Write(proxy_packet.remote_endpoint.portno);

    packet.Write(static_cast<u8>(proxy_packet.protocol));
    packet.Write(proxy_packet.broadcast);
    packet.Write(proxy_packet.data);

    room_member_impl->Send(std::move(packet));
}

void RoomMember::SendLdnPacket(const LDNPacket& ldn_packet) {
    Packet packet;
    packet.Write(static_cast<u8>(IdLdnPacket));

    packet.Write(static_cast<u8>(ldn_packet.type));

    packet.Write(ldn_packet.local_ip);
    packet.Write(ldn_packet.remote_ip);
    packet.Write(ldn_packet.broadcast);

    packet.Write(ldn_packet.data);

    room_member_impl->Send(std::move(packet));
}

void RoomMember::SendChatMessage(const std::string& message) {
    Packet packet;
    packet.Write(static_cast<u8>(IdChatMessage));
    packet.Write(message);
    room_member_impl->Send(std::move(packet));
}

void RoomMember::SendGameInfo(const GameInfo& game_info) {
    room_member_impl->current_game_info = game_info;
    if (!IsConnected())
        return;

    Packet packet;
    packet.Write(static_cast<u8>(IdSetGameInfo));
    packet.Write(game_info.name);
    packet.Write(game_info.id);
    packet.Write(game_info.version);
    room_member_impl->Send(std::move(packet));
}

void RoomMember::SendModerationRequest(RoomMessageTypes type, const std::string& nickname) {
    ASSERT_MSG(type == IdModKick || type == IdModBan || type == IdModUnban,
               "type is not a moderation request");
    if (!IsConnected())
        return;

    Packet packet;
    packet.Write(static_cast<u8>(type));
    packet.Write(nickname);
    room_member_impl->Send(std::move(packet));
}

void RoomMember::RequestBanList() {
    if (!IsConnected())
        return;

    Packet packet;
    packet.Write(static_cast<u8>(IdModGetBanList));
    room_member_impl->Send(std::move(packet));
}

RoomMember::CallbackHandle<RoomMember::State> RoomMember::BindOnStateChanged(
    std::function<void(const RoomMember::State&)> callback) {
    return room_member_impl->Bind(callback);
}

RoomMember::CallbackHandle<RoomMember::Error> RoomMember::BindOnError(
    std::function<void(const RoomMember::Error&)> callback) {
    return room_member_impl->Bind(callback);
}

RoomMember::CallbackHandle<ProxyPacket> RoomMember::BindOnProxyPacketReceived(
    std::function<void(const ProxyPacket&)> callback) {
    return room_member_impl->Bind(callback);
}

RoomMember::CallbackHandle<LDNPacket> RoomMember::BindOnLdnPacketReceived(
    std::function<void(const LDNPacket&)> callback) {
    return room_member_impl->Bind(std::move(callback));
}

RoomMember::CallbackHandle<RoomInformation> RoomMember::BindOnRoomInformationChanged(
    std::function<void(const RoomInformation&)> callback) {
    return room_member_impl->Bind(callback);
}

RoomMember::CallbackHandle<ChatEntry> RoomMember::BindOnChatMessageReceived(
    std::function<void(const ChatEntry&)> callback) {
    return room_member_impl->Bind(callback);
}

RoomMember::CallbackHandle<StatusMessageEntry> RoomMember::BindOnStatusMessageReceived(
    std::function<void(const StatusMessageEntry&)> callback) {
    return room_member_impl->Bind(callback);
}

RoomMember::CallbackHandle<Room::BanList> RoomMember::BindOnBanListReceived(
    std::function<void(const Room::BanList&)> callback) {
    return room_member_impl->Bind(callback);
}

template <typename T>
void RoomMember::Unbind(CallbackHandle<T> handle) {
    std::lock_guard lock(room_member_impl->callback_mutex);
    room_member_impl->callbacks.Get<T>().erase(handle);
}

void RoomMember::Leave() {
    room_member_impl->SetState(State::Idle);
    room_member_impl->loop_thread.reset();
    enet_host_destroy(room_member_impl->client);
    room_member_impl->client = nullptr;
    room_member_impl->server = nullptr;
    std::lock_guard lock(room_member_impl->send_list_mutex);
    room_member_impl->send_list.clear();
    room_member_impl->send_list_budget.Reset();
    room_member_impl->send_failed = false;
}

template void RoomMember::Unbind(CallbackHandle<ProxyPacket>);
template void RoomMember::Unbind(CallbackHandle<LDNPacket>);
template void RoomMember::Unbind(CallbackHandle<RoomMember::State>);
template void RoomMember::Unbind(CallbackHandle<RoomMember::Error>);
template void RoomMember::Unbind(CallbackHandle<RoomInformation>);
template void RoomMember::Unbind(CallbackHandle<ChatEntry>);
template void RoomMember::Unbind(CallbackHandle<StatusMessageEntry>);
template void RoomMember::Unbind(CallbackHandle<Room::BanList>);

} // namespace Network
namespace Network {
int RoomMember::GetPingMilliseconds() const {
    const auto state = GetState();
    return state == State::Joined || state == State::Moderator ?
        room_member_impl->ping_ms.load() : -1;
}
}
