#pragma once

#include "expected.hpp"
#include <algorithm>
#include <asio.hpp>
#include <asio/experimental/as_tuple.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/serial_port.hpp>
#include <asio/write.hpp>
#include <chrono>
#include <cmath>
#include <mavlink/common/mavlink.h>
#include <mavlink/mavlink_helpers.h>
#include <memory>
#include <span>
#include <spdlog/spdlog.h>

enum class MavlinkErrc
{
  NoHeartbeat = 1, // System Failure
  NoCommandAck,
  FailedWrite,
  FailedRead,
  TransmitTimeout = 10, // Timeouts
  ReceiveTimeout,
};
struct MavlinkErrCategory : std::error_category
{
  const char* name() const noexcept override
  {
    return "AutopilotCommunication";
  }
  std::string message(int ev) const override
  {
    switch (static_cast<MavlinkErrc>(ev)) {
      case MavlinkErrc::NoHeartbeat:
        return "no heartbeat received from autopilot";
      case MavlinkErrc::NoCommandAck:
      return "no ack received after command";
      case MavlinkErrc::FailedWrite:
        return "could not write, asio error";
      case MavlinkErrc::FailedRead:
        return "could not read, asio error";
      case MavlinkErrc::ReceiveTimeout:
        return "did not get response, timed out";
      case MavlinkErrc::TransmitTimeout:
        return "could not send message, timed out";
      default:
        return "(unrecognized error)";
    }
  }
};
const MavlinkErrCategory mavlinkerrc_category;
std::error_code
make_error_code(MavlinkErrc e)
{
  return { static_cast<int>(e), mavlinkerrc_category };
}
namespace std {
template<>
struct is_error_code_enum<MavlinkErrc> : true_type
{};
}

template<typename T>
using tResult = tl::expected<T, std::error_code>;
using tl::make_unexpected;
using namespace std::chrono;
using namespace asio::experimental::awaitable_operators;
constexpr auto use_nothrow_awaitable =
  asio::experimental::as_tuple(asio::use_awaitable);
const float INVALID = 0.0f;

class MavlinkInterface
{
  asio::serial_port m_uart;
  time_point<steady_clock> m_start;

  uint8_t m_heartbeat_channel = 0;
  uint8_t m_targets_channel = 1;
  uint8_t m_positions_channel = 2;

  // Guidance computer shares the system id with the autopilot => same system
  uint8_t m_system_id = 1;
  uint8_t m_component_id = 1;
  uint8_t m_my_id = 5;

  // Use field masks
  uint32_t USE_POSITION = 0x0DFC;
  uint32_t USE_VELOCITY = 0x0DE7;
  uint32_t USE_YAW = 0x09FF;

  inline auto get_uptime() -> uint32_t
  {
    return duration_cast<milliseconds>(steady_clock::now() - m_start).count();
  }
  auto send_message(const mavlink_message_t& msg)
    -> asio::awaitable<std::tuple<asio::error_code, std::size_t>>
  {
    uint8_t buffer[MAVLINK_MAX_PACKET_LEN];
    auto len = mavlink_msg_to_send_buffer(buffer, &msg);
    return asio::async_write(
      m_uart, asio::buffer(buffer, len), use_nothrow_awaitable);
  }
  auto wait_for_next_message(uint8_t channel) -> asio::awaitable<tResult<void>>
  {
    mavlink_status_t* status = mavlink_get_channel_status(channel);
    auto msgs_before = status->msg_received;

    std::vector<uint8_t> buffer(8);
    while (status->msg_received == msgs_before) {
      auto [error, len] = co_await m_uart.async_read_some(
        asio::buffer(buffer), use_nothrow_awaitable);
      if (error)
        co_return make_unexpected(error);

      std::for_each_n(cbegin(buffer), len, [channel, status](const uint8_t c) {
        mavlink_message_t msg;
        mavlink_parse_char(channel, c, &msg, status);
      });
    }
  }
  auto set_guided_mode() {}
  auto arm_autopilot() {}
  auto disarm_autopilot() {}

public:
  MavlinkInterface(asio::serial_port&& sp)
    : m_uart{ std::move(sp) }
    , m_start{ steady_clock::now() }
  {
  }
  auto init() -> asio::awaitable<tResult<void>>
  {
    using std::tie;
    using std::ignore;

    std::error_code error;
    mavlink_message_t msg;
    auto set_param_curried = std::bind_front(mavlink_msg_param_set_pack_chan,
                                             m_system_id,
                                             m_my_id,
                                             m_heartbeat_channel,
                                             &msg,
                                             m_system_id,
                                             m_component_id);
    bool ran_once = false;
    while (not ran_once) // RUN THIS ONLY ONCE
    {
      ran_once = true;
      set_param_curried("SR0_RAW_SENS", 0, MAV_PARAM_TYPE_INT16);
      tie(error, ignore) = co_await send_message(msg);
      if (error)
        break;

      set_param_curried("SR0_EXT_STAT", 0, MAV_PARAM_TYPE_INT16);
      tie(error, ignore) = co_await send_message(msg);
      if (error)
        break;

      set_param_curried("SR0_RC_CHAN", 0, MAV_PARAM_TYPE_INT16);
      tie(error, ignore) = co_await send_message(msg);
      if (error)
        break;

      set_param_curried("SR0_RAW_CTRL", 0, MAV_PARAM_TYPE_INT16);
      tie(error, ignore) = co_await send_message(msg);
      if (error)
        break;

      set_param_curried("SR0_POSITION", 0, MAV_PARAM_TYPE_INT16);
      tie(error, ignore) = co_await send_message(msg);
      if (error)
        break;

      set_param_curried("SR0_EXTRA1", 0, MAV_PARAM_TYPE_INT16);
      tie(error, ignore) = co_await send_message(msg);
      if (error)
        break;

      set_param_curried("SR0_EXTRA2", 0, MAV_PARAM_TYPE_INT16);
      tie(error, ignore) = co_await send_message(msg);
      if (error)
        break;

      set_param_curried("SR0_EXTRA3", 0, MAV_PARAM_TYPE_INT16);
      tie(error, ignore) = co_await send_message(msg);
      if (error)
        break;

      set_param_curried("SR0_PARAMS", 0, MAV_PARAM_TYPE_INT16);
      tie(error, ignore) = co_await send_message(msg);
      if (error)
        break;

      set_param_curried("SR0_ADSB", 0, MAV_PARAM_TYPE_INT16);
      // mavlink_msg_param_set_pack_chan(m_system_id, m_my_id,
      // m_heartbeat_channel, &msg, m_system_id, m_component_id, "SR0_ADSB", 0,
      // MAV_PARAM_TYPE_INT16);
      tie(error, ignore) = co_await send_message(msg);
      if (error)
        break;

      const uint16_t mav_cmd_preflight_reboot_shutdown = 246;
      mavlink_msg_command_int_pack_chan(m_system_id,
                                        m_my_id,
                                        m_heartbeat_channel,
                                        &msg,
                                        m_system_id,
                                        m_component_id,
                                        MAV_FRAME_LOCAL_NED,
                                        mav_cmd_preflight_reboot_shutdown,
                                        0, // Unused
                                        0, // Unused
                                        1, // Reboot autopilot
                                        0, // Don't reboot companion computer
                                        0, // Don't reboot componets
                                        0, // For all components attached
                                        0, // Unused
                                        0, // Unused
                                        0);// Unused
      tie(error, ignore) = co_await send_message(msg);
      if (error) break;
      co_await wait_for_next_message(m_heartbeat_channel);
      const mavlink_message_t* new_msg = mavlink_get_channel_buffer(m_heartbeat_channel);
      spdlog::info("Sent reboot, got reply msgid: {}", new_msg->msgid);
      if (new_msg->msgid != MAVLINK_MSG_ID_COMMAND_ACK) co_return make_unexpected(MavlinkErrc::NoCommandAck);
    }
    if (error) {
      spdlog::error("Could not initialize ardupilot params, asio error: {}",
                    error.message());
      co_return make_unexpected(MavlinkErrc::FailedWrite);
    }
  }
  auto set_target_position_local(std::span<float, 3> xyz)
    -> asio::awaitable<tResult<void>>
  {
    mavlink_message_t msg;
    mavlink_msg_set_position_target_local_ned_pack_chan(
      m_system_id,
      m_my_id,
      m_targets_channel,
      &msg,
      get_uptime(),
      m_system_id,
      m_component_id,
      MAV_FRAME_BODY_OFFSET_NED,
      USE_POSITION,
      xyz[0],
      xyz[1],
      xyz[2],
      INVALID,
      INVALID,
      INVALID,
      INVALID,
      INVALID,
      INVALID,
      INVALID,
      INVALID);
    auto [error, written] = co_await send_message(msg);
    // TODO: add cancellation and time out here
    if (error) {
      spdlog::error("Could not send set_target, asio error: {}",
                    error.message());
      co_return make_unexpected(MavlinkErrc::FailedWrite);
    }
  }
  auto set_target_velocity_local() {}
  auto set_target_heading_local() {}
  auto get_position_global() {}
  auto heartbeat() -> asio::awaitable<tResult<void>>
  {
    mavlink_message_t msg;
    mavlink_msg_heartbeat_pack_chan(m_system_id,
                                    m_my_id,
                                    m_heartbeat_channel,
                                    &msg,
                                    MAV_TYPE_ONBOARD_CONTROLLER,
                                    MAV_AUTOPILOT_INVALID,
                                    MAV_MODE_FLAG_GUIDED_ENABLED,
                                    INVALID,
                                    MAV_STATE_STANDBY);
    auto [error, written] = co_await send_message(msg);
    if (error) {
      spdlog::error("Could not send heartbeat, asio error: {}",
                    error.message());
      co_return make_unexpected(MavlinkErrc::FailedWrite);
    }
    spdlog::info("Sent heartbeat");
  }
};

awaitable<void> heartbeat_loop(MavlinkInterface& mi) {
  while(true) {
    auto [error, v] = co_await mi.heartbeat();
    if (error) break;
  }
}