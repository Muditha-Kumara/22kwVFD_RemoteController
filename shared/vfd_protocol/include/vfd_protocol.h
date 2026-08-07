#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file vfd_protocol.h
 * @brief Single source of truth for the ESP-NOW link between the
 *        vfd_controller (ESP32-S3) and remote_control (ESP32).
 *
 * Design constraints:
 *  - ESP-NOW max payload is 250 bytes -> every packet must fit.
 *  - 10 Hz telemetry cadence; loss > 0.5 s (5 packets) => safe stop.
 *  - All structs packed, fixed-width, little-endian (ESP32 native).
 *  - Every packet carries magic + version + CRC16 for forward compat / OTA.
 */

/* ------------------------------------------------------------------ */
/* Protocol constants                                                  */
/* ------------------------------------------------------------------ */

/** Magic number: bump on any breaking protocol change. Used by OTA to
 *  verify firmware/protocol compatibility before accepting an update. */
#define VFD_PROTO_MAGIC        0x224B5746u   /* "22KW" ascii-ish */

/** Protocol version (independent of firmware version). */
#define VFD_PROTO_VERSION      0x0001u

/** Maximum ESP-NOW payload we will ever send (bytes). */
#define VFD_NOW_MAX_PAYLOAD    250

/** Telemetry cadence. */
#define VFD_TELEM_HZ           10
#define VFD_TELEM_PERIOD_MS    (1000 / VFD_TELEM_HZ)

/** Communication-loss timeout: machine must safe-stop after this. */
#define VFD_LINK_TIMEOUT_MS    500

/* ------------------------------------------------------------------ */
/* Device / packet identifiers                                         */
/* ------------------------------------------------------------------ */

typedef enum {
    VFD_DEV_CONTROLLER = 0x01,   /* machine-side ESP32-S3            */
    VFD_DEV_REMOTE     = 0x02,   /* handheld remote ESP32            */
} vfd_device_type_t;

typedef enum {
    /* controller -> remote */
    VFD_PKT_TELEMETRY      = 0x10,  /* live machine state @10 Hz     */
    VFD_PKT_EVENT          = 0x11,  /* discrete event / alarm         */

    /* remote -> controller */
    VFD_PKT_CMD            = 0x20,  /* operator command               */
    VFD_PKT_TORQUE_SET     = 0x21,  /* encoder torque setpoint        */
    VFD_PKT_HEARTBEAT      = 0x22,  /* remote alive + battery         */
    VFD_PKT_CHANNEL_SWITCH = 0x23,  /* controller: move to new channel*/

    /* bidirectional / OTA */
    VFD_PKT_OTA_HDR        = 0x30,  /* OTA handshake (magic/version)  */
    VFD_PKT_ACK            = 0x40,  /* generic ack                    */
} vfd_packet_type_t;

/* ------------------------------------------------------------------ */
/* Machine state (mirrored to remote for display)                      */
/* ------------------------------------------------------------------ */

typedef enum {
    VFD_STATE_IDLE          = 0,  /* nothing running                  */
    VFD_STATE_CONNECTING    = 1,  /* wifi / link bring-up             */
    VFD_STATE_READY         = 2,  /* VFDs online, awaiting command    */
    VFD_STATE_STARTING      = 3,  /* ramping up (soft start)          */
    VFD_STATE_CUTTING       = 4,  /* normal cutting                   */
    VFD_STATE_FRESH_CUT     = 5,  /* fresh-cut / wire-freeing cycle   */
    VFD_STATE_REVERSING     = 6,  /* stuck-wire reverse vibration     */
    VFD_STATE_STOPPING      = 7,  /* controlled decel stop            */
    VFD_STATE_ESTOP         = 8,  /* emergency stop engaged           */
    VFD_STATE_FAULT         = 9,  /* latched fault                    */
} vfd_machine_state_t;

/* Alarm / event codes (VFD_PKT_EVENT) */
typedef enum {
    VFD_EVT_NONE            = 0,
    VFD_EVT_COMM_LOSS       = 1,  /* link lost -> safe stop           */
    VFD_EVT_VFD_FAULT_MAIN  = 2,  /* main VFD reported fault          */
    VFD_EVT_VFD_FAULT_TRAVEL= 3,  /* travel VFD reported fault        */
    VFD_EVT_OVER_TORQUE     = 4,  /* torque exceeded limit            */
    VFD_EVT_STUCK_WIRE      = 5,  /* fresh-cut start torque too high  */
    VFD_EVT_ESTOP           = 6,
    VFD_EVT_WIFI_CONNECTED  = 7,
    VFD_EVT_WIFI_LOST       = 8,
    VFD_EVT_OTA_AVAILABLE   = 9,
} vfd_event_code_t;

/* ------------------------------------------------------------------ */
/* Packet header (common to every packet)                              */
/* ------------------------------------------------------------------ */

typedef struct __attribute__((packed)) {
    uint32_t magic;        /* VFD_PROTO_MAGIC                          */
    uint16_t version;      /* VFD_PROTO_VERSION                        */
    uint8_t  type;         /* vfd_packet_type_t                        */
    uint8_t  src;          /* vfd_device_type_t sender                 */
    uint8_t  dst;          /* vfd_device_type_t receiver               */
    uint8_t  seq;          /* rolling sequence (detect loss)           */
    uint8_t  len;          /* payload bytes following this header      */
    uint16_t crc16;        /* CRC16 over header(excl crc)+payload      */
} vfd_pkt_hdr_t;

/* ------------------------------------------------------------------ */
/* Telemetry: controller -> remote @10 Hz                             */
/* ------------------------------------------------------------------ */

typedef struct __attribute__((packed)) {
    /* machine */
    uint8_t  machine_state;     /* vfd_machine_state_t                */
    uint8_t  alarm;             /* vfd_event_code_t (last/latched)    */
    uint16_t fault_code_main;   /* main VFD fault register            */
    uint16_t fault_code_travel; /* travel VFD fault register          */

    /* main (22 kW) VFD */
    int16_t  main_torque_pct;   /* output torque, 0.01 %  (x100)      */
    int16_t  main_torque_set;   /* commanded torque, 0.01 %           */
    uint16_t main_freq_hz;      /* output freq, 0.01 Hz               */
    uint16_t main_current_a;    /* output current, 0.1 A              */
    uint16_t main_bus_v;        /* DC bus voltage, 1 V                */

    /* travel VFD */
    int16_t  travel_torque_pct; /* output torque, 0.01 %              */
    int16_t  travel_torque_set; /* commanded torque, 0.01 %           */
    uint16_t travel_freq_hz;    /* output freq, 0.01 Hz               */
    uint16_t travel_current_a;  /* output current, 0.1 A              */

    /* cutting / link */
    uint16_t wire_tension_raw;  /* derived tension proxy (torque)     */
    uint16_t cut_depth_mm;      /* accumulated cut depth, 0.1 mm      */
    uint8_t  link_quality;      /* 0..100 (rx success ratio)          */
    uint32_t uptime_ms;         /* controller uptime                  */
} vfd_telemetry_t;

/* ------------------------------------------------------------------ */
/* Commands: remote -> controller                                      */
/* ------------------------------------------------------------------ */

typedef enum {
    VFD_CMD_NONE           = 0,
    VFD_CMD_START_CUT      = 1,  /* safe soft start -> cutting        */
    VFD_CMD_FRESH_CUT      = 2,  /* dedicated fresh-cut start         */
    VFD_CMD_STOP           = 3,  /* controlled stop                   */
    VFD_CMD_ESTOP          = 4,  /* emergency stop                    */
    VFD_CMD_TENSION_TEST   = 5,  /* travel-only torque to tighten wire*/
    VFD_CMD_FAULT_RESET    = 6,
    VFD_CMD_PAUSE          = 7,
    VFD_CMD_RESUME         = 8,
} vfd_cmd_code_t;

typedef struct __attribute__((packed)) {
    uint8_t  cmd;               /* vfd_cmd_code_t                     */
    uint8_t  arg0;              /* command-specific                   */
    uint16_t arg1;              /* command-specific                   */
    uint32_t arg2;              /* command-specific                   */
} vfd_cmd_t;

/* ------------------------------------------------------------------ */
/* Torque setpoint (encoder) : remote -> controller                    */
/* ------------------------------------------------------------------ */

typedef struct __attribute__((packed)) {
    int16_t main_torque_set;    /* 0.01 % of rated                    */
    int16_t travel_torque_set;  /* 0.01 % of rated                    */
    uint8_t flags;              /* bit0 main enabled, bit1 travel en  */
} vfd_torque_set_t;

/* ------------------------------------------------------------------ */
/* Heartbeat : remote -> controller (carries battery)                  */
/* ------------------------------------------------------------------ */

typedef struct __attribute__((packed)) {
    uint16_t battery_mv;        /* battery voltage, mV                */
    uint8_t  battery_pct;       /* 0..100                             */
    uint8_t  flags;             /* bit0 charging                      */
} vfd_heartbeat_t;

/* ------------------------------------------------------------------ */
/* Channel switch : controller -> remote (VFD_PKT_CHANNEL_SWITCH)      */
/* ------------------------------------------------------------------ */

typedef struct __attribute__((packed)) {
    uint8_t channel;            /* new WiFi channel (1..13)           */
    uint8_t reserved[3];
} vfd_channel_switch_t;

/* ------------------------------------------------------------------ */
/* OTA handshake                                                       */
/* ------------------------------------------------------------------ */

typedef struct __attribute__((packed)) {
    uint32_t magic;             /* must equal VFD_PROTO_MAGIC         */
    uint16_t proto_version;     /* VFD_PROTO_VERSION                  */
    uint16_t fw_version;        /* firmware build version             */
    uint32_t fw_size;           /* image size bytes                   */
    uint32_t fw_crc32;          /* image crc32                        */
} vfd_ota_hdr_t;

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/** Standard Modbus/CCITT CRC16 (poly 0xA001, init 0xFFFF), low byte first
 *  on the wire. Used for both ESP-NOW packets and RS-485 frames. */
uint16_t vfd_crc16(const uint8_t *data, size_t len);

/** Continue a running CRC16 (feed a previous crc as the starting value). */
uint16_t vfd_crc16_continue(uint16_t crc, const uint8_t *data, size_t len);

/**
 * @brief Build a packet into out_buf. Fills header, computes CRC.
 * @return total bytes written (header + payload), or -1 on error.
 */
int vfd_pkt_build(uint8_t *out_buf, size_t out_cap,
                  vfd_packet_type_t type,
                  vfd_device_type_t src, vfd_device_type_t dst,
                  uint8_t seq,
                  const void *payload, size_t payload_len);

/**
 * @brief Validate a received packet: magic, version, length, CRC.
 * @param[out] payload_ptr set to start of payload within buf (or NULL).
 * @param[out] payload_len set to payload length.
 * @return true if valid.
 */
bool vfd_pkt_parse(const uint8_t *buf, size_t len,
                   vfd_pkt_hdr_t *hdr_out,
                   const uint8_t **payload_ptr, size_t *payload_len);

/** Convenience: is this packet addressed to `me` (or broadcast)? */
bool vfd_pkt_for_me(const vfd_pkt_hdr_t *hdr, vfd_device_type_t me);

#ifdef __cplusplus
}
#endif
