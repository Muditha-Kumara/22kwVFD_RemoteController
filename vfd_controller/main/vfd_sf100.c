/**
 * @file vfd_sf100.c
 * @brief Driver for the SF100 (travel / traversing motor).
 *
 * Register map distilled from docs/vfd_sf100_modbus.md:
 *  - Control mode      P0.00 = 2 (direct torque control)
 *  - Run cmd channel   P0.03 = 2 (serial)
 *  - Torque source     Pb.05 = 4 (RS485 comm)
 *  - Comm config       P3.09 = 163 (9600, 8N1, RTU)
 *  - Command word      2000H: bit0-1 (01 stop,10 run,11 jog),
 *                      bit4-5 (01 FWD,10 REV). FWD run=0x12, stop=0x01.
 *  - Freq/torque ref   2001H
 *  - Fault reset       2002H = 0x02
 *  - Fault code        2100H, status 2101H, freq 2103H, current 2104H,
 *                      bus 2105H, speed 2107H
 *
 * Parameter addressing: GGnnH = group letter<<8 | number.
 *   P0=0x00, P1=0x01, P2=0x02, P3=0x03, P4=0x04, Pb=0x0B ...
 */
#include "vfd.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"

#define TAG "sf100"

/* Fixed control/status registers */
#define SF_REG_CMD_WORD        0x2000
#define SF_REG_FREQ_REF        0x2001
#define SF_REG_FAULT_RESET     0x2002
#define SF_REG_FAULT_CODE      0x2100
#define SF_REG_STATUS          0x2101
#define SF_REG_SET_FREQ        0x2102
#define SF_REG_OUT_FREQ        0x2103
#define SF_REG_OUT_CURRENT     0x2104
#define SF_REG_BUS_VOLT        0x2105
#define SF_REG_MOTOR_SPEED     0x2107

/* Command word bit patterns */
#define SF_CMD_STOP            0x01
#define SF_CMD_RUN_FWD         0x12   /* run + forward */
#define SF_CMD_RUN_REV         0x22   /* run + reverse */

/* Status bits (2101H) */
#define SF_STATUS_RUNNING      (1u << 0)
#define SF_STATUS_UNDERVOLT    (1u << 1)
#define SF_STATUS_REVERSE      (1u << 2)

/* Parameter address helper: GGnnH */
#define SF_PARAM(grp, num)     (((uint16_t)(grp) << 8) | (num))

/* Group indices (letter -> index) */
#define SF_GRP_P0   0x00
#define SF_GRP_PB   0x0B
#define SF_GRP_P3   0x03

/* Key parameters */
#define SF_P0_00  SF_PARAM(SF_GRP_P0, 0)   /* control mode            */
#define SF_P0_03  SF_PARAM(SF_GRP_P0, 3)   /* run command channel     */
#define SF_PB_05  SF_PARAM(SF_GRP_PB, 5)   /* torque setting source   */
#define SF_P3_09  SF_PARAM(SF_GRP_P3, 9)   /* comm config             */
#define SF_P3_10  SF_PARAM(SF_GRP_P3, 10)  /* station address         */

/* ------------------------------------------------------------------ */
static esp_err_t sf_configure(vfd_dev_t *dev, const vfd_cfg_t *cfg)
{
    dev->cfg = *cfg;
    uint8_t a = cfg->slave_addr;
    esp_err_t err;

    /* NOTE: factory default is ASCII 9600 7N2. The very first commissioning
     * step must set P3.09=163 (RTU) — after that the bus speaks RTU.
     * We assume the drive is already reachable in RTU here (done at factory
     * bring-up), and configure the control behaviour. */

    /* Direct torque control mode */
    if ((err = modbus_write_reg(dev->bus, a, SF_P0_00, 2)) != ESP_OK) {
        ESP_LOGE(TAG, "set P0.00 failed"); return err;
    }
    /* Run command from serial port */
    if ((err = modbus_write_reg(dev->bus, a, SF_P0_03, 2)) != ESP_OK) {
        ESP_LOGE(TAG, "set P0.03 failed"); return err;
    }
    /* Torque reference from RS485 communication */
    if ((err = modbus_write_reg(dev->bus, a, SF_PB_05, 4)) != ESP_OK) {
        ESP_LOGE(TAG, "set Pb.05 failed"); return err;
    }

    ESP_LOGI(TAG, "configured addr=%d torque_limit=%d",
             a, cfg->torque_limit_pct);
    return ESP_OK;
}

static esp_err_t sf_probe(vfd_dev_t *dev)
{
    uint16_t status;
    return modbus_read_regs(dev->bus, dev->cfg.slave_addr,
                            SF_REG_STATUS, 1, &status);
}

static esp_err_t sf_run(vfd_dev_t *dev, vfd_dir_t dir)
{
    dev->dir = dir;
    uint16_t cmd = (dir == VFD_DIR_FORWARD) ? SF_CMD_RUN_FWD : SF_CMD_RUN_REV;
    esp_err_t err = modbus_write_reg(dev->bus, dev->cfg.slave_addr,
                                     SF_REG_CMD_WORD, cmd);
    if (err == ESP_OK) {
        dev->running = true;
    }
    return err;
}

static esp_err_t sf_stop(vfd_dev_t *dev)
{
    esp_err_t err = modbus_write_reg(dev->bus, dev->cfg.slave_addr,
                                     SF_REG_CMD_WORD, SF_CMD_STOP);
    if (err == ESP_OK) {
        dev->running = false;
    }
    return err;
}

static esp_err_t sf_coast_stop(vfd_dev_t *dev)
{
    /* SF100 has no explicit coast word; stop command is the safe path.
     * For a true emergency, controller must also cut the contactor. */
    return sf_stop(dev);
}

static esp_err_t sf_set_torque(vfd_dev_t *dev, int16_t torque_pct_x100)
{
    int16_t limit = (int16_t)dev->cfg.torque_limit_pct;
    if (torque_pct_x100 > limit)  torque_pct_x100 = limit;
    if (torque_pct_x100 < -limit) torque_pct_x100 = -limit;

    /* 2001H carries the serial reference. In torque mode (Pb.05=4) this is
     * the torque reference. Scale assumed signed 16-bit, 0.01 % units
     * (1000 = 100.0%) — VERIFY on hardware during commissioning. */
    return modbus_write_reg(dev->bus, dev->cfg.slave_addr,
                            SF_REG_FREQ_REF, (uint16_t)torque_pct_x100);
}

static esp_err_t sf_fault_reset(vfd_dev_t *dev)
{
    return modbus_write_reg(dev->bus, dev->cfg.slave_addr,
                            SF_REG_FAULT_RESET, 0x02);
}

static esp_err_t sf_read_status(vfd_dev_t *dev, vfd_status_t *st)
{
    uint8_t a = dev->cfg.slave_addr;
    memset(st, 0, sizeof(*st));

    uint16_t status;
    esp_err_t err = modbus_read_regs(dev->bus, a, SF_REG_STATUS, 1, &status);
    if (err != ESP_OK) {
        return err;
    }
    st->running = (status & SF_STATUS_RUNNING) != 0;

    uint16_t fault;
    if (modbus_read_regs(dev->bus, a, SF_REG_FAULT_CODE, 1, &fault) == ESP_OK) {
        st->fault_code = fault;
        st->fault = (fault != 0);
    }

    /* Monitoring registers are contiguous 2101H..2107H; read the block
     * 2103H..2107H (5 words): outFreq, current, busV, outV, speed. */
    uint16_t mon[5];
    if (modbus_read_regs(dev->bus, a, SF_REG_OUT_FREQ, 5, mon) == ESP_OK) {
        st->freq_hz_x100  = mon[0];
        st->current_a_x10 = mon[1];
        st->bus_v         = mon[2];
        st->speed_rpm     = mon[4];
    }

    /* SF100 does not expose output torque directly; the torque reference we
     * commanded is the best proxy. The control engine reads it back via the
     * set value register 2102H if needed. */
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
static const vfd_ops_t s_sf_ops = {
    .name        = "SF100",
    .configure   = sf_configure,
    .probe       = sf_probe,
    .run         = sf_run,
    .stop        = sf_stop,
    .coast_stop  = sf_coast_stop,
    .set_torque  = sf_set_torque,
    .fault_reset = sf_fault_reset,
    .read_status = sf_read_status,
};

vfd_dev_t *vfd_sf100_create(modbus_bus_t *bus)
{
    vfd_dev_t *dev = calloc(1, sizeof(*dev));
    if (!dev) {
        return NULL;
    }
    dev->ops = &s_sf_ops;
    dev->bus = bus;
    return dev;
}
