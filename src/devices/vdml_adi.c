/**
 * \file vdml_adi.c
 *
 * \brief VDML ADI functionality.
 *
 * This file ensure thread saftey for operations on motors by maintaining
 * an array of RTOS Mutexes and implementing functions to take and give them.
 *
 * \copyright (c) 2018, Purdue University ACM SIGBots.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include <errno.h>
#include <math.h>
#include <stdio.h>

#include "ifi/v5_api.h"
#include "kapi.h"
#include "vdml/registry.h"
#include "vdml/vdml.h"

#define INTERNAL_ADI_PORT 21

#define ADI_MOTOR_MAX_SPEED 127
#define ADI_MOTOR_MIN_SPEED -128

#define NUM_MAX_TWOWIRE 4

typedef enum analog_type_e { E_ANALOG_IN = 0, E_ANALOG_GYRO } analog_type_e_t;

/**
 * Defined as a struct to reduce the amount of code that will need to be
 * rewritten if additional info is needed.
 */
typedef struct adi_analog {
	int32_t calib, mult;
	volatile int32_t value;
	analog_type_e_t type;
} adi_analog_t;

adi_analog_t analog_registry[NUM_ADI_PORTS];

bool encoder_reversed[NUM_MAX_TWOWIRE];

#define transform_adi_port(port)                                                                                       \
	if (port >= 'a' && port <= 'h')                                                                                \
		port -= 'a';                                                                                           \
	else if (port >= 'A' && port <= 'H')                                                                           \
		port -= 'A';                                                                                           \
	else                                                                                                           \
		port--;                                                                                                \
	if (port > 7 || port < 0) {                                                                                    \
		errno = EINVAL;                                                                                        \
		return PROS_ERR;                                                                                       \
	}

#define validate_type(port, type)                                                                                      \
	adi_port_config_e_t config = adi_port_config_get(port);                                                        \
	if (config != type) {                                                                                          \
		return PROS_ERR;                                                                                       \
	}

#define validate_analog(port)                                                                                          \
	adi_port_config_e_t config = adi_port_config_get(port);                                                        \
	if (config != E_ADI_ANALOG_IN && config != E_ADI_LEGACY_POT && config != E_ADI_LEGACY_LINE_SENSOR &&           \
	    config != E_ADI_LEGACY_LIGHT_SENSOR && config != E_ADI_LEGACY_ACCELEROMETER &&                             \
	    config != E_ADI_SMART_POT) {                                                                               \
		errno = EINVAL;                                                                                        \
		return PROS_ERR;                                                                                       \
	}

#define validate_digital_in(port)                                                                                      \
	adi_port_config_e_t config = adi_port_config_get(port);                                                        \
	if (config != E_ADI_DIGITAL_IN && config != E_ADI_LEGACY_BUTTON && config != E_ADI_SMART_BUTTON) {             \
		errno = EINVAL;                                                                                        \
		return PROS_ERR;                                                                                       \
	}

#define validate_motor(port)                                                                                           \
	adi_port_config_e_t config = adi_port_config_get(port);                                                        \
	if (config != E_ADI_LEGACY_PWM && config != E_ADI_LEGACY_SERVO) {                                              \
		errno = EINVAL;                                                                                        \
		return PROS_ERR;                                                                                       \
	}

#define validate_twowire(port_top, port_bottom)                                                                        \
	if (abs(port_top - port_bottom) > 1) {                                                                         \
		errno = EINVAL;                                                                                        \
		return PROS_ERR;                                                                                       \
	}                                                                                                              \
	int port;                                                                                                      \
	if (port_top < port_bottom)                                                                                    \
		port = port_top;                                                                                       \
	else if (port_bottom < port_top)                                                                               \
		port = port_bottom;                                                                                    \
	else                                                                                                           \
		return PROS_ERR;                                                                                       \
	if (!(port % 2)) {                                                                                             \
		return PROS_ERR;                                                                                       \
	}

int32_t adi_port_config_set(int port, adi_port_config_e_t type) {
	transform_adi_port(port);
	claim_port(INTERNAL_ADI_PORT, E_DEVICE_ADI);
	vexDeviceAdiPortConfigSet(device.device_info, port, type);
	return_port(INTERNAL_ADI_PORT, 1);
}

adi_port_config_e_t adi_port_config_get(int port) {
	transform_adi_port(port);
	claim_port(INTERNAL_ADI_PORT, E_DEVICE_ADI);
	adi_port_config_e_t rtn = (adi_port_config_e_t)vexDeviceAdiPortConfigGet(device.device_info, port);
	return_port(INTERNAL_ADI_PORT, rtn);
}

int32_t adi_value_set(int port, int32_t value) {
	transform_adi_port(port);
	claim_port(INTERNAL_ADI_PORT, E_DEVICE_ADI);
	vexDeviceAdiValueSet(device.device_info, port, value);
	return_port(INTERNAL_ADI_PORT, 1);
}

int32_t adi_value_get(int port) {
	transform_adi_port(port);
	claim_port(INTERNAL_ADI_PORT, E_DEVICE_ADI);
	int32_t rtn = vexDeviceAdiValueGet(device.device_info, port);
	return_port(INTERNAL_ADI_PORT, rtn);
}

int32_t adi_analog_calibrate(int port) {
	validate_analog(port);
	uint32_t total = 0, i;
	for (i = 0; i < 512; i++) {
		total += adi_value_get(port);
		task_delay(1);
	}
	analog_registry[port - 1].calib = (int32_t)((total + 16) >> 5);
	return ((int32_t)((total + 256) >> 9));
}

int32_t adi_analog_read(int port) {
	validate_analog(port);
	return adi_value_get(port);
}

int32_t adi_analog_read_calibrated(int port) {
	validate_analog(port);
	return (adi_value_get(port) - (analog_registry[port - 1].calib >> 4));
}

int32_t adi_analog_read_calibrated_HR(int port) {
	validate_analog(port);
	return ((adi_value_get(port) << 4) - analog_registry[port - 1].calib);
}

int32_t adi_digital_read(int port) {
	validate_digital_in(port);
	return adi_value_get(port);
}

int32_t adi_digital_write(int port, bool value) {
	validate_type(port, E_ADI_DIGITAL_OUT);
	return adi_value_set(port, (int32_t)value);
}

int32_t adi_pin_mode(int port, unsigned char mode) {
	switch (mode) {
	case INPUT:
		adi_port_config_set(port, E_ADI_DIGITAL_IN);
		break;
	case OUTPUT:
		adi_port_config_set(port, E_ADI_DIGITAL_OUT);
		break;
	case INPUT_ANALOG:
		adi_port_config_set(port, E_ADI_ANALOG_IN);
		break;
	case OUTPUT_ANALOG:
		adi_port_config_set(port, E_ADI_ANALOG_OUT);
		break;
	default:
		errno = EINVAL;
		return PROS_ERR;
	};
	return 1;
}

int32_t adi_motor_set(int port, int speed) {
	validate_motor(port);
	if (speed > ADI_MOTOR_MAX_SPEED)
		speed = ADI_MOTOR_MAX_SPEED;
	else if (speed < ADI_MOTOR_MIN_SPEED)
		speed = ADI_MOTOR_MIN_SPEED;

	return adi_value_set(port, speed);
}

int32_t adi_motor_get(int port) {
	validate_motor(port);
	return (adi_value_get(port) - ADI_MOTOR_MAX_SPEED);
}

int32_t adi_motor_stop(int port) {
	validate_motor(port);
	return adi_value_set(port, 0);
}

adi_encoder_t adi_encoder_init(int port_top, int port_bottom, bool reverse) {
	validate_twowire(port_top, port_bottom);
	encoder_reversed[(port - 1) / 2] = reverse;

	return adi_port_config_set(port, E_ADI_LEGACY_ENCODER);
}

int32_t adi_encoder_get(adi_encoder_t enc) {
	validate_type(enc, E_ADI_LEGACY_ENCODER);
	if (encoder_reversed[(enc - 1) / 2]) return (-adi_value_get(enc));
	return adi_value_get(enc);
}

int32_t adi_encoder_reset(adi_encoder_t enc) {
	validate_type(enc, E_ADI_LEGACY_ENCODER);
	return adi_value_set(enc, 0);
}

int32_t adi_encoder_shutdown(adi_encoder_t enc) {
	validate_type(enc, E_ADI_LEGACY_ENCODER);
	return adi_port_config_set(enc, E_ADI_TYPE_UNDEFINED);
}

adi_ultrasonic_t adi_ultrasonic_init(int port_echo, int port_ping) {
	validate_twowire(port_echo, port_ping);
	if (port != port_echo) return PROS_ERR;

	return adi_port_config_set(port_echo, E_ADI_LEGACY_ULTRASONIC);
}

int32_t adi_ultrasonic_get(adi_ultrasonic_t ult) {
	validate_type(ult, E_ADI_LEGACY_ULTRASONIC);
	return adi_value_get(ult);
}

int32_t adi_ultrasonic_shutdown(adi_ultrasonic_t ult) {
	validate_type(ult, E_ADI_LEGACY_ULTRASONIC);
	return adi_port_config_set(ult, E_ADI_TYPE_UNDEFINED);
}
