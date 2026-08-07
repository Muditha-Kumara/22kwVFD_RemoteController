/**
 * @file vfd_zuked720.c
 * @brief Driver for the ZUKED D720 (main 22 kW cutting motor).
 *
 * Register map distilled from docs/vfd_zuked720_modbus.md:
 *  - Run command channel  P0-02 = 2 (serial)
 *  - Torque mode          d0-00 = 1
 *  - Torque ref source    d0-01 = 5 (communication)
 *  - Control word         2000H (1=FWD,2=REV,6=decel stop,7=fault reset)
 *  - Status               3000H (1=FWD run,2=REV run,3=stopped)
 *  - Fault code           8000H
 *  - Comm set value       1000H (-10000..10000 = -100.00%..100.00%)
 *  - Output torque        1006H, current 1004H, freq 1001H, bus 1002H
 *
 * Parameter addressing: Pn-mm -> 0xF000 + n*0x100 + mm
 *                       dn-mm -> 0xA000 + n*0x100 + mm
 */
#include "vfd.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"

#define TAG "zuked720"

/* Fixed control/status registers */
#define ZK_REG_COMM_SETVALUE   0x1000
#define ZK_REG_CONTROL_WORD    0x2000
#define ZK_REG_RUN_STATUS      0x3000
#define ZK_REG_FAULT_CODE      0x8000
#define ZK_REG_OUT_FREQ        0x1001
#define ZK_REG_BUS_VOLT        0x1002
#define ZK_REG_OUT_CURRENT     0x1004
#define ZK_REG_OUT_TORQUE      0x1006
#define ZK_REG_RUN_SPEED       0x1007

/* Control word values */
#define ZK_CMD_FWD_RUN         0x0001
#define ZK_CMD_REV_RUN         0x0002
#define ZK_CMD_DECEL_STOP      0x0006
#define ZK_CMD_FAULT_RESET     0x0007

/* Run status values */
#define ZK_STATUS_FWD          0x0001
#define ZK_STATUS_REV          0x0002
#define ZK_STATUS_STOPPED      0x0003

/* Parameter address helpers */
#define ZK_PARAM_P(grp, num)   (0xF000u + ((uint16_t)(grp) << 8) + (num))
#define ZK_PARAM_D(grp, num)   (0xA000u + ((uint16_t)(grp) << 8) + (num))

/* Key parameters */
#define ZK_P0_02  ZK_PARAM_P(0, 2)   /* run command channel        */
#define ZK_P0_03  ZK_PARAM_P(0, 3)   /* main frequency source      */
#define ZK_D0_00  ZK_PARAM_D(0, 0)   /* speed/torque select        */
#define ZK_D0_01  ZK_PARAM_D(0, 1)   /* torque reference source    */
#define ZK_D0_05  ZK_PARAM_D(0, 5)   /* fwd max freq torque mode   */
#define ZK_D0_06  ZK_PARAM_D(0, 6)   /* rev max freq torque mode   */

/* ------------------------------------------------------------------ */
static esp_err_t zk_configure(vfd_dev_t *dev, const vfd_cfg_t *cfg)
{
    dev->cfg = *cfg;
    uint8_t a = cfg->slave_addr;
    esp_err_t err;

    /* Run command from serial communication */
    if ((err = modbus_write_reg(dev->bus, a, ZK_P0_02, 2)) != ESP_OK) {
        ESP_LOGE(TAG, "set P0-02 failed"); return err;
    }
    /* Torque control mode */
    if ((err = modbus_write_reg(dev->bus, a, ZK_D0_00, 1)) != ESP_OK) {
        ESP_LOGE(TAG, "set d0-00 failed"); return err;
    }
    /* Torque reference from communication */
    if ((err = modbus_write_reg(dev->bus, a, ZK_D0_01, 5)) != ESP_OK) {
        ESP_LOGE(TAG, "set d0-01 failed"); return err;
    }
    /* Safety speed ceilings in torque mode (0.01 Hz units) */
    if ((err = modbus_write_reg(dev->bus, a, ZK_D0_05, cfg->max_freq_hz_x100)) != ESP_OK) {
        ESP_LOGE(TAG, "set d0-05 failed"); return err;
    }
    if ((err = modbus_write_reg(dev->bus, a, ZK_D0_06, cfg->max_freq_hz_x100)) != ESP_OK) {
        ESP_LOGE(TAG, "set d0-06 failed"); return err;
    }

    ESP_LOGI(TAG, "configured addr=%d torque_limit=%d max_freq=%d",
             a, cfg->torque_limit_pct, cfg->max_freq_hz_x100);
    return ESP_OK;
}

static esp_err_t zk_probe(vfd_dev_t *dev)
{
    uint16_t status;
    return modbus_read_regs(dev->bus, dev->cfg.slave_addr,
                            ZK_REG_RUN_STATUS, 1, &status);
}

static esp_err_t zk_run(vfd_dev_t *dev, vfd_dir_t dir)
{
    dev->dir = dir;
    uint16_t cmd = (dir == VFD_DIR_FORWARD) ? ZK_CMD_FWD_RUN : ZK_CMD_REV_RUN;
    esp_err_t err = modbus_write_reg(dev->bus, dev->cfg.slave_addr,
                                     ZK_REG_CONTROL_WORD, cmd);
    if (err == ESP_OK) {
        dev->running = true;
    }
    return err;
}

static esp_err_t zk_stop(vfd_dev_t *dev)
{
    esp_err_t err = modbus_write_reg(dev->bus, dev->cfg.slave_addr,
                                     ZK_REG_CONTROL_WORD, ZK_CMD_DECEL_STOP);
    if (err == ESP_OK) {
        dev->running = false;
    }
    return err;
}

static esp_err_t zk_coast_stop(vfd_dev_t *dev)
{
    /* ZUKED: 5 = coast to stop */
    esp_err_t err = modbus_write_reg(dev->bus, dev->cfg.slave_addr,
                                     ZK_REG_CONTROL_WORD, 0x0005);
    if (err == ESP_OK) {
        dev->running = false;
    }
    return err;
}

static esp_err_t zk_set_torque(vfd_dev_t *dev, int16_t torque_pct_x100)
{
    /* Clamp to safety torque ceiling */
    int16_t limit = (int16_t)dev->cfg.torque_limit_pct;
    if (torque_pct_x100 > limit)  torque_pct_x100 = limit;
    if (torque_pct_x100 < -limit) torque_pct_x100 = -limit;

    /* 1000H: -10000..10000 = -100.00%..100.00% (0.01 % units).
     * torque_pct_x100 is already in 0.01 % units. */
    return modbus_write_reg(dev->bus, dev->cfg.slave_addr,
                            ZK_REG_COMM_SETVALUE, (uint16_t)torque_pct_x100);
}

static esp_err_t zk_fault_reset(vfd_dev_t *dev)
{
    return modbus_write_reg(dev->bus, dev->cfg.slave_addr,
                            ZK_REG_CONTROL_WORD, ZK_CMD_FAULT_RESET);
}

static esp_err_t zk_read_status(vfd_dev_t *dev, vfd_status_t *st)
{
    uint8_t a = dev->cfg.slave_addr;
    memset(st, 0, sizeof(*st));

    uint16_t run_status;
    esp_err_t err = modbus_read_regs(dev->bus, a, ZK_REG_RUN_STATUS, 1, &run_status);
    if (err != ESP_OK) {
        return err;
    }
    st->running = (run_status == ZK_STATUS_FWD || run_status == ZK_STATUS_REV);

    uint16_t fault;
    if (modbus_read_regs(dev->bus, a, ZK_REG_FAULT_CODE, 1, &fault) == ESP_OK) {
        st->fault_code = fault;
        st->fault = (fault != 0);
    }

    /* Read contiguous monitoring block 1001H..1007H (7 words) in one frame.
     * 1001 freq, 1002 bus, 1003 outV, 1004 current, 1005 power,
     * 1006 torque, 1007 speed. */
    uint16_t mon[7];
    if (modbus_read_regs(dev->bus, a, ZK_REG_OUT_FREQ, 7, mon) == ESP_OK) {
        st->freq_hz_x100    = mon[0];
        st->bus_v           = mon[1];
        st->current_a_x10   = mon[3];
        st->torque_pct_x100 = (int16_t)mon[5];
        st->speed_rpm       = mon[6];
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
static const vfd_ops_t s_zk_ops = {
    .name        = "ZUKED D720",
    .configure   = zk_configure,
    .probe       = zk_probe,
    .run         = zk_run,
    .stop        = zk_stop,
    .coast_stop  = zk_coast_stop,
    .set_torque  = zk_set_torque,
    .fault_reset = zk_fault_reset,
    .read_status = zk_read_status,
};

vfd_dev_t *vfd_zuked720_create(modbus_bus_t *bus)
{
    vfd_dev_t *dev = calloc(1, sizeof(*dev));
    if (!dev) {
        return NULL;
    }
    dev->ops = &s_zk_ops;
    dev->bus = bus;
    return dev;
}
