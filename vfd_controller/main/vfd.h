#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "modbus_rtu.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file vfd.h
 * @brief Common VFD driver interface (torque-control oriented).
 *
 * Both the main 22 kW motor (ZUKED D720) and the travel motor (SF100)
 * run in torque-control mode. The control engine talks only to this
 * abstract interface; device-specific register maps live in vfd_zuked720.c
 * and vfd_sf100.c.
 */

typedef enum {
    VFD_DIR_FORWARD = 0,
    VFD_DIR_REVERSE = 1,
} vfd_dir_t;

/** Live process values read back from the drive. */
typedef struct {
    bool     running;          /* drive reports run state            */
    bool     fault;            /* drive reports a fault              */
    uint16_t fault_code;       /* drive-specific fault register      */
    int16_t  torque_pct_x100;  /* output torque, 0.01 % units        */
    uint16_t freq_hz_x100;     /* output frequency, 0.01 Hz units    */
    uint16_t current_a_x10;    /* output current, 0.1 A units        */
    uint16_t bus_v;            /* DC bus voltage, volts              */
    uint16_t speed_rpm;        /* motor speed rpm (0 if unavailable) */
} vfd_status_t;

/** Configuration applied once at commissioning (written to EEPROM). */
typedef struct {
    uint8_t  slave_addr;       /* Modbus address on the shared bus   */
    uint16_t torque_limit_pct; /* safety torque ceiling (0.01 %)     */
    uint16_t max_freq_hz_x100; /* safety speed ceiling in torque mode*/
} vfd_cfg_t;

typedef struct vfd_dev vfd_dev_t;

/** Driver operations table (implemented per drive model). */
typedef struct {
    const char *name;

    /** One-time commissioning: put drive in comm + torque-control mode. */
    esp_err_t (*configure)(vfd_dev_t *dev, const vfd_cfg_t *cfg);

    /** Probe the drive (read a status register) to confirm it is alive. */
    esp_err_t (*probe)(vfd_dev_t *dev);

    /** Start running in the given direction at the current torque ref. */
    esp_err_t (*run)(vfd_dev_t *dev, vfd_dir_t dir);

    /** Controlled (decel) stop. */
    esp_err_t (*stop)(vfd_dev_t *dev);

    /** Coast / emergency stop (immediate, free-run). */
    esp_err_t (*coast_stop)(vfd_dev_t *dev);

    /** Set torque reference, 0.01 % units, signed (sign => direction). */
    esp_err_t (*set_torque)(vfd_dev_t *dev, int16_t torque_pct_x100);

    /** Clear a latched drive fault. */
    esp_err_t (*fault_reset)(vfd_dev_t *dev);

    /** Read live process values. */
    esp_err_t (*read_status)(vfd_dev_t *dev, vfd_status_t *st);
} vfd_ops_t;

struct vfd_dev {
    const vfd_ops_t *ops;
    modbus_bus_t    *bus;
    vfd_cfg_t        cfg;
    vfd_dir_t        dir;
    bool             running;
};

/** Constructors implemented by each driver. */
vfd_dev_t *vfd_zuked720_create(modbus_bus_t *bus);
vfd_dev_t *vfd_sf100_create(modbus_bus_t *bus);

/* ---- Thin inline wrappers so callers use dev-centric API ---- */
static inline esp_err_t vfd_configure(vfd_dev_t *d, const vfd_cfg_t *c) { return d->ops->configure(d, c); }
static inline esp_err_t vfd_probe(vfd_dev_t *d)                        { return d->ops->probe(d); }
static inline esp_err_t vfd_run(vfd_dev_t *d, vfd_dir_t dir)           { return d->ops->run(d, dir); }
static inline esp_err_t vfd_stop(vfd_dev_t *d)                         { return d->ops->stop(d); }
static inline esp_err_t vfd_coast_stop(vfd_dev_t *d)                   { return d->ops->coast_stop(d); }
static inline esp_err_t vfd_set_torque(vfd_dev_t *d, int16_t t)        { return d->ops->set_torque(d, t); }
static inline esp_err_t vfd_fault_reset(vfd_dev_t *d)                  { return d->ops->fault_reset(d); }
static inline esp_err_t vfd_read_status(vfd_dev_t *d, vfd_status_t *s) { return d->ops->read_status(d, s); }

#ifdef __cplusplus
}
#endif
