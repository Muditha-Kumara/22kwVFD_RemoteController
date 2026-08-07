#include "vfd_protocol.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* CRC16 (Modbus): poly 0xA001 reflected, init 0xFFFF                  */
/* ------------------------------------------------------------------ */
uint16_t vfd_crc16(const uint8_t *data, size_t len)
{
    return vfd_crc16_continue(0xFFFF, data, len);
}

uint16_t vfd_crc16_continue(uint16_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

/* ------------------------------------------------------------------ */
/* Packet build / parse                                                */
/* ------------------------------------------------------------------ */
int vfd_pkt_build(uint8_t *out_buf, size_t out_cap,
                  vfd_packet_type_t type,
                  vfd_device_type_t src, vfd_device_type_t dst,
                  uint8_t seq,
                  const void *payload, size_t payload_len)
{
    if (out_buf == NULL) {
        return -1;
    }
    if (payload_len > 0 && payload == NULL) {
        return -1;
    }
    if (payload_len > UINT8_MAX) {
        return -1;
    }

    const size_t total = sizeof(vfd_pkt_hdr_t) + payload_len;
    if (total > out_cap || total > VFD_NOW_MAX_PAYLOAD) {
        return -1;
    }

    vfd_pkt_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic   = VFD_PROTO_MAGIC;
    hdr.version = VFD_PROTO_VERSION;
    hdr.type    = (uint8_t)type;
    hdr.src     = (uint8_t)src;
    hdr.dst     = (uint8_t)dst;
    hdr.seq     = seq;
    hdr.len     = (uint8_t)payload_len;
    hdr.crc16   = 0;

    memcpy(out_buf, &hdr, sizeof(hdr));
    if (payload_len > 0) {
        memcpy(out_buf + sizeof(hdr), payload, payload_len);
    }

    /* CRC over everything except the crc16 field itself.
     * Layout: [magic..len][crc16][payload] -> hash [0..offsetof(crc16))
     * then [offsetof(crc16)+2 .. total). */
    const size_t crc_off = offsetof(vfd_pkt_hdr_t, crc16);
    uint16_t crc = 0xFFFF;
    crc = vfd_crc16(out_buf, crc_off);
    /* feed the two crc bytes as zero, then payload */
    uint8_t zero2[2] = {0, 0};
    crc = vfd_crc16_continue(crc, zero2, 2);
    if (payload_len > 0) {
        crc = vfd_crc16_continue(crc, out_buf + sizeof(hdr), payload_len);
    }

    memcpy(out_buf + crc_off, &crc, sizeof(crc));
    return (int)total;
}

bool vfd_pkt_parse(const uint8_t *buf, size_t len,
                   vfd_pkt_hdr_t *hdr_out,
                   const uint8_t **payload_ptr, size_t *payload_len)
{
    if (buf == NULL || len < sizeof(vfd_pkt_hdr_t)) {
        return false;
    }

    vfd_pkt_hdr_t hdr;
    memcpy(&hdr, buf, sizeof(hdr));

    if (hdr.magic != VFD_PROTO_MAGIC) {
        return false;
    }
    if (hdr.version != VFD_PROTO_VERSION) {
        return false;
    }
    if ((size_t)hdr.len + sizeof(vfd_pkt_hdr_t) != len) {
        return false;
    }

    /* recompute CRC with the crc field zeroed */
    const size_t crc_off = offsetof(vfd_pkt_hdr_t, crc16);
    uint16_t crc = vfd_crc16(buf, crc_off);
    uint8_t zero2[2] = {0, 0};
    crc = vfd_crc16_continue(crc, zero2, 2);
    if (hdr.len > 0) {
        crc = vfd_crc16_continue(crc, buf + sizeof(hdr), hdr.len);
    }
    if (crc != hdr.crc16) {
        return false;
    }

    if (hdr_out != NULL) {
        *hdr_out = hdr;
    }
    if (payload_ptr != NULL) {
        *payload_ptr = buf + sizeof(hdr);
    }
    if (payload_len != NULL) {
        *payload_len = hdr.len;
    }
    return true;
}

bool vfd_pkt_for_me(const vfd_pkt_hdr_t *hdr, vfd_device_type_t me)
{
    if (hdr == NULL) {
        return false;
    }
    return hdr->dst == (uint8_t)me || hdr->dst == 0xFF; /* 0xFF = broadcast */
}
