/*
 * Lua RTOS, sensor driver
 *
 * Copyright (C) 2015 - 2017
 * IBEROXARXA SERVICIOS INTEGRALES, S.L.
 *
 * Author: Jaume Olivé (jolive@iberoxarxa.com / jolive@whitecatboard.org)
 *
 * All rights reserved.
 *
 * Permission to use, copy, modify, and distribute this software
 * and its documentation for any purpose and without fee is hereby
 * granted, provided that the above copyright notice appear in all
 * copies and that both that the copyright notice and this
 * permission notice and warranty disclaimer appear in supporting
 * documentation, and that the name of the author not be used in
 * advertising or publicity pertaining to distribution of the
 * software without specific, written prior permission.
 *
 * The author disclaim all warranties with regard to this
 * software, including all implied warranties of merchantability
 * and fitness.  In no event shall the author be liable for any
 * special, indirect or consequential damages or any damages
 * whatsoever resulting from loss of use, data or profits, whether
 * in an action of contract, negligence or other tortious action,
 * arising out of or in connection with the use or performance of
 * this software.
 */

#include "sdkconfig.h"

#if CONFIG_LUA_RTOS_LUA_USE_SENSOR

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <string.h>

#include <sys/status.h>
#include <sys/list.h>
#include <sys/driver.h>
#include <sys/syslog.h>

#include <drivers/sensor.h>
#include <drivers/adc.h>
#include <drivers/adc_internal.h>
#include <drivers/gpio.h>
#include <drivers/owire.h>
#include <drivers/i2c.h>
#include <drivers/uart.h>
#include <drivers/power_bus.h>

// This variable is defined at linker time
extern const sensor_t sensors[];

// Driver message errors
DRIVER_REGISTER_ERROR(SENSOR, sensor, CannotSetup, "can't setup", SENSOR_ERR_CANT_INIT);
DRIVER_REGISTER_ERROR(SENSOR, sensor, Timeout, "timeout", SENSOR_ERR_TIMEOUT);
DRIVER_REGISTER_ERROR(SENSOR, sensor, NotEnoughtMemory, "not enough memory", SENSOR_ERR_NOT_ENOUGH_MEMORY);
DRIVER_REGISTER_ERROR(SENSOR, sensor, SetupUndefined, "setup function is not defined", SENSOR_ERR_SETUP_UNDEFINED);
DRIVER_REGISTER_ERROR(SENSOR, sensor, AcquireUndefined, "acquire function is not defined", SENSOR_ERR_ACQUIRE_UNDEFINED);
DRIVER_REGISTER_ERROR(SENSOR, sensor, SetUndefined, "set function is not defined", SENSOR_ERR_SET_UNDEFINED);
DRIVER_REGISTER_ERROR(SENSOR, sensor, NotFound, "not found", SENSOR_ERR_NOT_FOUND);
DRIVER_REGISTER_ERROR(SENSOR, sensor, InterfaceNotSupported, "interface not supported", SENSOR_ERR_INTERFACE_NOT_SUPPORTED);
DRIVER_REGISTER_ERROR(SENSOR, sensor, NotSetup, "sensor is not setup", SENSOR_ERR_NOT_SETUP);
DRIVER_REGISTER_ERROR(SENSOR, sensor, InvalidAddress, "invalid address", SENSOR_ERR_INVALID_ADDRESS);
DRIVER_REGISTER_ERROR(SENSOR, sensor, NoMoreCallbacks, "no more callbacks available", SENSOR_ERR_NO_MORE_CALLBACKS);
DRIVER_REGISTER_ERROR(SENSOR, sensor, InvalidData, "invalid data", SENSOR_ERR_INVALID_DATA);
DRIVER_REGISTER_ERROR(SENSOR, sensor, NoCallbacksAlowed, "callbacks not allowed for this sensor", SENSOR_ERR_CALLBACKS_NOT_ALLOWED);

static xQueueHandle queue = NULL;
static TaskHandle_t task = NULL;
uint8_t attached = 0;

/*
 * Helper functions
 */

static void sensor_task(void *arg) {
	sensor_deferred_data_t data;

    for(;;) {
        xQueueReceive(queue, &data, portMAX_DELAY);
        data.callback(data.callback_id, data.instance, data.data, data.latch);
    }
}

static void IRAM_ATTR debouncing(void *arg, uint8_t val) {
	// Get sensor instance
	sensor_instance_t *unit = (sensor_instance_t *)arg;

	mtx_lock(&unit->mtx);

	// Latch current data
	memcpy(unit->latch, unit->data, sizeof(unit->data));

	// Store sensor data
	if (unit->sensor->flags & SENSOR_FLAG_ON_H) {
		unit->data[0].integerd.value = val;
	} else if (unit->sensor->flags & SENSOR_FLAG_ON_L) {
		unit->data[0].integerd.value = !val;
	} else {
		mtx_unlock(&unit->mtx);
		return;
	}

	sensor_queue_callbacks(unit);

	mtx_unlock(&unit->mtx);
};

static void IRAM_ATTR isr(void* arg) {
	// Get sensor instance
	sensor_instance_t *unit = (sensor_instance_t *)arg;

	// Get pin value
	uint8_t val = gpio_ll_pin_get(unit->setup[0].gpio.gpio);

	mtx_lock(&unit->mtx);

	// Latch current data
	memcpy(unit->latch, unit->data, sizeof(unit->data));

	// Store sensor data
	if (unit->sensor->flags & SENSOR_FLAG_ON_H) {
		unit->data[0].integerd.value = val;
	} else if (unit->sensor->flags & SENSOR_FLAG_ON_L) {
		unit->data[0].integerd.value = !val;
	} else {
		mtx_unlock(&unit->mtx);
		return;
	}

	sensor_queue_callbacks(unit);

	mtx_unlock(&unit->mtx);
}

static driver_error_t *sensor_adc_setup(uint8_t interface, sensor_instance_t *unit) {
	driver_unit_lock_error_t *lock_error = NULL;
	driver_error_t *error;

	// Lock ADC channel
    if ((lock_error = driver_lock(SENSOR_DRIVER, unit->unit, ADC_DRIVER, unit->setup[interface].adc.channel, DRIVER_ALL_FLAGS, unit->sensor->id))) {
    	// Revoked lock on ADC channel
    	return driver_lock_error(SENSOR_DRIVER, lock_error);
    }

	if (
			(error = adc_setup(
					unit->setup[interface].adc.unit, unit->setup[interface].adc.channel,
					unit->setup[interface].adc.devid, unit->setup[interface].adc.vrefp,
					unit->setup[interface].adc.vrefn, unit->setup[interface].adc.resolution,
					&unit->setup[interface].adc.h
			))
		) {
		return error;
	}

	return NULL;
}

static driver_error_t *sensor_gpio_setup(uint8_t interface, sensor_instance_t *unit) {
	driver_unit_lock_error_t *lock_error = NULL;

	// Lock gpio
    if ((lock_error = driver_lock(SENSOR_DRIVER, unit->unit, GPIO_DRIVER, unit->setup[interface].gpio.gpio, DRIVER_ALL_FLAGS, unit->sensor->id))) {
    	// Revoked lock on gpio
    	return driver_lock_error(SENSOR_DRIVER, lock_error);
    }

    if (unit->sensor->flags & SENSOR_FLAG_ON_OFF) {
    	if (unit->sensor->flags & SENSOR_FLAG_DEBOUNCING) {
    		driver_error_t *error;
    		uint16_t threshold = (unit->sensor->flags & 0xffff0000) >> 16;

    		if ((error = gpio_debouncing_register(1 << unit->setup[interface].gpio.gpio, threshold, debouncing, (void *)unit))) {
    			return error;
    		}
    	} else {
        	portDISABLE_INTERRUPTS();

            // Configure pins as input
            gpio_config_t io_conf;

            io_conf.intr_type = GPIO_INTR_ANYEDGE;
            io_conf.mode = GPIO_MODE_INPUT;
            io_conf.pin_bit_mask = (1ULL << unit->setup[interface].gpio.gpio);
            io_conf.pull_down_en = 0;
            io_conf.pull_up_en = 1;

            gpio_config(&io_conf);

        	// Configure interrupts
            if (!status_get(STATUS_ISR_SERVICE_INSTALLED)) {
            	gpio_install_isr_service(0);

            	status_set(STATUS_ISR_SERVICE_INSTALLED);
            }

            gpio_isr_handler_add(unit->setup[interface].gpio.gpio, isr, (void *)unit);

            portENABLE_INTERRUPTS();
    	}
    } else {
        gpio_pin_output(unit->setup[interface].gpio.gpio);
    	gpio_pin_set(unit->setup[interface].gpio.gpio);
    }

	return NULL;
}

static driver_error_t *sensor_owire_setup(uint8_t interface, sensor_instance_t *unit) {
	driver_error_t *error;

	// By default we always can get sensor data
    gettimeofday(&unit->next, NULL);

	#if CONFIG_LUA_RTOS_USE_POWER_BUS
	pwbus_on();
	#endif

	// Check if owire interface is setup on the given gpio
	int dev = owire_checkpin(unit->setup[interface].owire.gpio);
	if (dev < 0) {
		// setup new owire interface on given pin
		if ((error = owire_setup_pin(unit->setup[interface].owire.gpio))) {
		  	return error;
		}
		int dev = owire_checkpin(unit->setup[interface].owire.gpio);
		if (dev < 0) {
			return driver_error(SENSOR_DRIVER, SENSOR_ERR_CANT_INIT, NULL);
		}
		vTaskDelay(10 / portTICK_RATE_MS);
		owdevice_input(dev);
		ow_devices_init(dev);
		unit->setup[interface].owire.owdevice = dev;

		// Search for devices on owire bus
		TM_OneWire_Dosearch(dev);
	}
	else {
		unit->setup[interface].owire.owdevice = dev;
		TM_OneWire_Dosearch(dev);
	}

	// check if owire bus is setup
	if (ow_devices[unit->setup[interface].owire.owdevice].device.pin == 0) {
		return driver_error(SENSOR_DRIVER, SENSOR_ERR_CANT_INIT, NULL);
	}

	return NULL;
}

static driver_error_t *sensor_i2c_setup(uint8_t interface, sensor_instance_t *unit) {
	driver_error_t *error;

    driver_unit_lock_error_t *lock_error = NULL;
	if ((lock_error = driver_lock(SENSOR_DRIVER, unit->unit, I2C_DRIVER, unit->setup[interface].i2c.id, DRIVER_ALL_FLAGS, unit->sensor->id))) {
		return driver_lock_error(SENSOR_DRIVER, lock_error);
	}

	#if CONFIG_LUA_RTOS_USE_POWER_BUS
	pwbus_on();
	#endif

    if ((error = i2c_setup(unit->setup[interface].i2c.id, I2C_MASTER, unit->setup[interface].i2c.speed, 0, 0))) {
    	return error;
    }

	return NULL;
}

static driver_error_t *sensor_uart_setup(uint8_t interface, sensor_instance_t *unit) {
	driver_error_t *error;

    driver_unit_lock_error_t *lock_error = NULL;
	if ((lock_error = driver_lock(SENSOR_DRIVER, unit->unit, UART_DRIVER,unit->setup[interface].uart.id, DRIVER_ALL_FLAGS, unit->sensor->id))) {
		return driver_lock_error(SENSOR_DRIVER, lock_error);
	}

	if ((error = uart_init(
    		unit->setup[interface].uart.id, unit->setup[interface].uart.speed, unit->setup[interface].uart.data_bits,
			unit->setup[interface].uart.parity, unit->setup[interface].uart.stop_bits, DRIVER_ALL_FLAGS, 1024
	))) {
    	return error;
    }

    if ((error = uart_setup_interrupts(unit->setup[interface].uart.id))) {
        return error;
    }

	return NULL;
}

/*
 * Operation functions
 */
const sensor_t *get_sensor(const char *id) {
	const sensor_t *csensor;

	csensor = sensors;
	while (csensor->id) {
		if (strcmp(csensor->id,id) == 0) {
			return csensor;
		}
		csensor++;
	}

	return NULL;
}

const sensor_data_t *sensor_get_property(const sensor_t *sensor, const char *property) {
	int idx = 0;

	for(idx=0;idx <  SENSOR_MAX_PROPERTIES;idx++) {
		if (sensor->properties[idx].id) {
			if (strcmp(sensor->properties[idx].id,property) == 0) {
				return &(sensor->properties[idx]);
			}
		}
	}

	return NULL;
}

driver_error_t *sensor_setup(const sensor_t *sensor, sensor_setup_t *setup, sensor_instance_t **unit) {
	driver_error_t *error = NULL;
	sensor_instance_t *instance = NULL;
	int i = 0;

	// Sanity checks
	if (!sensor->acquire) {
		//return driver_error(SENSOR_DRIVER, SENSOR_ERR_ACQUIRE_UNDEFINED, NULL);
	}

	// Create a sensor instance
	if (!(instance = (sensor_instance_t *)calloc(1, sizeof(sensor_instance_t)))) {
		return driver_error(SENSOR_DRIVER, SENSOR_ERR_NOT_ENOUGH_MEMORY, NULL);
	}

	// Create mutex
	mtx_init(&instance->mtx, NULL, NULL, 0);

	// Store reference to sensor into instance
	instance->sensor = sensor;

	// Copy sensor setup configuration into instance
	memcpy(&instance->setup, setup, SENSOR_MAX_INTERFACES * sizeof(sensor_setup_t));

	// Initialize sensor data from sensor definition into instance
	for(i=0;i<SENSOR_MAX_DATA;i++) {
		instance->data[i].type = sensor->data[i].type;
	}

	memcpy(instance->latch, instance->data, sizeof(instance->data));

	// Initialize sensor properties from sensor definition into instance
	for(i=0;i<SENSOR_MAX_PROPERTIES;i++) {
		instance->properties[i].type = sensor->properties[i].type;
	}

	// Call to specific pre setup function, if any
	if (instance->sensor->presetup) {
		if ((error = instance->sensor->presetup(instance))) {
			mtx_destroy(&instance->mtx);
			free(instance);
			return error;
		}
	}

	// Setup sensor interfaces
	if (!(sensor->flags & SENSOR_FLAG_CUSTOM_INTERFACE_INIT)) {
		for(i=0;i<SENSOR_MAX_INTERFACES;i++) {
			if (sensor->interface[i]) {
				switch (sensor->interface[i]) {
					case ADC_INTERFACE: error = sensor_adc_setup(i, instance);break;
					case GPIO_INTERFACE: error = sensor_gpio_setup(i, instance);break;
					case OWIRE_INTERFACE: error = sensor_owire_setup(i, instance);break;
					case I2C_INTERFACE: error = sensor_i2c_setup(i, instance);break;
					case UART_INTERFACE: error = sensor_uart_setup(i, instance);break;
					default:
						return driver_error(SENSOR_DRIVER, SENSOR_ERR_INTERFACE_NOT_SUPPORTED, NULL);
						break;
				}

				if (error) {
					break;
				}
			}
		}
	}

	if (error) {
		mtx_destroy(&instance->mtx);
		free(instance);
		return error;
	}

	// Call to specific setup function
	if (instance->sensor->setup) {
		if ((error = instance->sensor->setup(instance))) {
			mtx_destroy(&instance->mtx);
			free(instance);
			return error;
		}
	}

	*unit = instance;

	attached++;

	return NULL;
}

driver_error_t *sensor_unsetup(sensor_instance_t *unit) {
	driver_error_t *error;
	int i;

	portDISABLE_INTERRUPTS();

	if (attached == 0) {
		// No sensors attached, nothing to do
		portENABLE_INTERRUPTS();
		return NULL;
	}

	// Call the the custom unsetup function, if any
	if (unit->sensor->unsetup) {
		error = unit->sensor->unsetup(unit);
		if (error) {
			portENABLE_INTERRUPTS();
			return error;
		}
	}

	// Remove interrupts
	if (unit->sensor->flags & SENSOR_FLAG_ON_OFF) {
		for(i=0; i < SENSOR_MAX_INTERFACES; i++) {
			if (unit->setup[i].gpio.gpio) {
				gpio_isr_handler_remove(unit->setup[i].gpio.gpio);
			}
		}
	}

	if (attached == 1) {
		if (task) {
			vTaskDelete(task);
			task = NULL;
		}

		if (queue) {
			vQueueDelete(queue);
			queue = NULL;
		}
	}

	attached--;

	mtx_destroy(&unit->mtx);
	free(unit);

	portENABLE_INTERRUPTS();

	return NULL;
}

driver_error_t *sensor_acquire(sensor_instance_t *unit) {
	driver_error_t *error = NULL;
	sensor_value_t *value = NULL;
	int i = 0;

	// Check if we can get data
	uint64_t next_available_data = unit->next.tv_sec * 1000000 +unit->next.tv_usec;

	struct timeval now;
    gettimeofday(&now, NULL);
	uint64_t now_usec = now.tv_sec * 1000000 + now.tv_usec;

    if (now_usec < next_available_data) {
    	return NULL;
    }

	#if CONFIG_LUA_RTOS_USE_POWER_BUS
	pwbus_on();
	#endif

	// Allocate space for sensor data
	// This is done only if sensor is not interrupt driven, because in
	// interrupt driven sensors the sensor value is set in the ISR
	if (!(unit->sensor->flags & (SENSOR_FLAG_ON_OFF | SENSOR_FLAG_AUTO_ACQ))) {
		if (!(value = calloc(1, sizeof(sensor_value_t) * SENSOR_MAX_DATA))) {
			return driver_error(SENSOR_DRIVER, SENSOR_ERR_NOT_ENOUGH_MEMORY, NULL);
		}
	}

	// Call to specific acquire function, if any
	if (unit->sensor->acquire) {
		if ((error = unit->sensor->acquire(unit, value))) {
			free(value);
			return error;
		}
	}

	if (!(unit->sensor->flags & (SENSOR_FLAG_ON_OFF | SENSOR_FLAG_AUTO_ACQ))) {
		mtx_lock(&unit->mtx);

		memcpy(unit->latch, unit->data, sizeof(unit->data));

		// Copy sensor values into instance
		// Note that we only copy raw values as value types are set in sensor_setup from sensor
		// definition
		for(i=0;i < SENSOR_MAX_DATA;i++) {
			unit->data[i].raw = value[i].raw;
		}

		mtx_unlock(&unit->mtx);

		free(value);
	}

	return NULL;
}

driver_error_t *sensor_read(sensor_instance_t *unit, const char *id, sensor_value_t **value) {
	int idx = 0;

	mtx_lock(&unit->mtx);

	*value = NULL;
	for(idx=0;idx <  SENSOR_MAX_DATA;idx++) {
		if (unit->sensor->data[idx].id) {
			if (strcmp(unit->sensor->data[idx].id,id) == 0) {
				*value = &unit->data[idx];

				mtx_unlock(&unit->mtx);
				return NULL;
			}
		}
	}

	mtx_unlock(&unit->mtx);

	return driver_error(SENSOR_DRIVER, SENSOR_ERR_NOT_FOUND, NULL);
}

driver_error_t *sensor_set(sensor_instance_t *unit, const char *id, sensor_value_t *value) {
	int idx = 0;

	// Sanity checks
	if (!unit->sensor->set) {
		return driver_error(SENSOR_DRIVER, SENSOR_ERR_SET_UNDEFINED, NULL);
	}

	mtx_lock(&unit->mtx);

	for(idx=0;idx < SENSOR_MAX_PROPERTIES;idx++) {
		if (unit->sensor->properties[idx].id) {
			if (strcmp(unit->sensor->properties[idx].id,id) == 0) {
				unit->sensor->set(unit, id, value);

				mtx_unlock(&unit->mtx);
				return NULL;
			}
		}
	}

	mtx_unlock(&unit->mtx);

	return driver_error(SENSOR_DRIVER, SENSOR_ERR_NOT_FOUND, NULL);
}

driver_error_t *sensor_get(sensor_instance_t *unit, const char *id, sensor_value_t **value) {
	int idx = 0;

	*value = NULL;

	// Sanity checks
	if (!unit->sensor->get) {
		return driver_error(SENSOR_DRIVER, SENSOR_ERR_SET_UNDEFINED, NULL);
	}

	mtx_lock(&unit->mtx);

	for(idx=0;idx < SENSOR_MAX_PROPERTIES;idx++) {
		if (unit->sensor->properties[idx].id) {
			if (strcmp(unit->sensor->properties[idx].id,id) == 0) {
				unit->sensor->get(unit, id, &unit->properties[idx]);

				*value = &unit->properties[idx];

				mtx_unlock(&unit->mtx);
				return NULL;
			}
		}
	}

	mtx_unlock(&unit->mtx);

	return driver_error(SENSOR_DRIVER, SENSOR_ERR_NOT_FOUND, NULL);
}

driver_error_t *sensor_register_callback(sensor_instance_t *unit, sensor_callback_t callback, int id, uint8_t deferred) {
	int i;

	// Sanity checks
	if (!(unit->sensor->flags & (SENSOR_FLAG_ON_OFF | SENSOR_FLAG_AUTO_ACQ))) {
		return driver_error(SENSOR_DRIVER, SENSOR_ERR_CALLBACKS_NOT_ALLOWED, NULL);
	}

	portDISABLE_INTERRUPTS();

	// Find for a free callback
	for(i=0;i < SENSOR_MAX_CALLBACKS;i++) {
		if (unit->callbacks[i].callback == NULL) {
			unit->callbacks[i].callback = callback;
			unit->callbacks[i].callback_id = id;
			break;
		}
	}

	if (i == SENSOR_MAX_CALLBACKS) {
		portENABLE_INTERRUPTS();
		return driver_error(SENSOR_DRIVER, SENSOR_ERR_NO_MORE_CALLBACKS, NULL);
	}

	// Create queue if needed
	if (!queue) {
		queue = xQueueCreate(10, sizeof(sensor_deferred_data_t));
		if (!queue) {
			portENABLE_INTERRUPTS();
			return driver_error(SENSOR_DRIVER, SENSOR_ERR_NOT_ENOUGH_MEMORY, NULL);
		}
	}

	// Create task if needed
	if (!task) {
		BaseType_t xReturn;

		xReturn = xTaskCreatePinnedToCore(sensor_task, "sensor", CONFIG_LUA_RTOS_LUA_THREAD_STACK_SIZE, NULL, CONFIG_LUA_RTOS_LUA_THREAD_PRIORITY, &task, xPortGetCoreID());
		if (xReturn != pdPASS) {
			portENABLE_INTERRUPTS();
			return driver_error(SENSOR_DRIVER, SENSOR_ERR_NOT_ENOUGH_MEMORY, NULL);
		}
	}

	portENABLE_INTERRUPTS();

	return NULL;
}

void IRAM_ATTR sensor_queue_callbacks(sensor_instance_t *unit) {
	// Queue callbacks
	sensor_deferred_data_t data;
	int i;

	for(i=0;i < SENSOR_MAX_CALLBACKS;i++) {
		if (unit->callbacks[i].callback) {
			data.instance = unit;
			data.callback = unit->callbacks[i].callback;
			data.callback_id = unit->callbacks[i].callback_id;
			memcpy(data.data, unit->data, sizeof(unit->data));
			memcpy(data.latch, unit->latch, sizeof(unit->latch));

			xQueueSendFromISR(queue, &data, NULL);
		}
	}
}

DRIVER_REGISTER(SENSOR,sensor,NULL,NULL,NULL);

#endif
