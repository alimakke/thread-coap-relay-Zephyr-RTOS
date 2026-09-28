/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * Modifié : client à un seul bouton et deux LEDs
 *  - pas encore appairé : appui  -> provisioning
 *  - appairé, simple clic        -> commande selon le mode
 *        mode 1 (SERVER) : PUT /light  -> LED du serveur
 *        mode 2 (RELAY)  : PUT /relay  -> LED de l'autre client via le serveur
 *  - appairé, double clic        -> changement de mode
 *
 *  LED rouge (STATUS_LED) : éteinte = pas connecté Thread
 *                           fixe    = connecté, mode 1
 *                           clignote= connecté, mode 2
 *  LED bleue (LIGHT_LED)  : lumière de ce client (pilotée par l'autre client)
 */

#include <zephyr/kernel.h>
#include <dk_buttons_and_leds.h>
#include <zephyr/logging/log.h>
#include <ram_pwrdn.h>
#include <zephyr/device.h>
#include <zephyr/pm/device.h>

#include "coap_client_utils.h"
#include <coap_server_client_interface.h>

#if CONFIG_BT_NUS
#include "ble_utils.h"
#endif

LOG_MODULE_REGISTER(coap_client, CONFIG_COAP_CLIENT_LOG_LEVEL);

/* Vérifiez sur votre carte laquelle est rouge et laquelle est bleue,
 * et inversez DK_LED1 / DK_LED2 si besoin. */
#define STATUS_LED DK_LED1        /* rouge */
#define LIGHT_LED  DK_LED2        /* bleue */

#define USER_BUTTON_MSK DK_BTN1_MSK
#define DOUBLE_CLICK_MS 400       /* délai max entre deux appuis */
#define MODE2_BLINK_MS  500       /* clignotement de la LED rouge en mode 2 */

enum client_mode {
	MODE_SERVER = 0,   /* mode 1 : contrôle la LED du serveur      */
	MODE_RELAY  = 1,   /* mode 2 : contrôle l'autre client (relay) */
};

static volatile enum client_mode mode = MODE_SERVER;
static volatile bool ot_connected;

/* ------------------------------------------------------------------ */
/* LED rouge : état connexion + mode                                   */
/* ------------------------------------------------------------------ */

static void on_status_blink(struct k_timer *timer)
{
	static uint8_t on;

	ARG_UNUSED(timer);

	on = !on;
	dk_set_led(STATUS_LED, on);
}

K_TIMER_DEFINE(status_blink_timer, on_status_blink, NULL);

static void update_status_led(void)
{
	if (!ot_connected) {
		k_timer_stop(&status_blink_timer);
		dk_set_led_off(STATUS_LED);
	} else if (mode == MODE_SERVER) {
		k_timer_stop(&status_blink_timer);
		dk_set_led_on(STATUS_LED);
	} else {
		k_timer_start(&status_blink_timer, K_NO_WAIT,
			      K_MSEC(MODE2_BLINK_MS));
	}
}

/* ------------------------------------------------------------------ */
/* Détection simple / double clic                                      */
/* ------------------------------------------------------------------ */


static void on_light_cmd(uint8_t cmd)
{
	static uint8_t val;

	switch (cmd) {
	case THREAD_COAP_UTILS_LIGHT_CMD_ON:     val = 1;    break;
	case THREAD_COAP_UTILS_LIGHT_CMD_OFF:    val = 0;    break;
	case THREAD_COAP_UTILS_LIGHT_CMD_TOGGLE: val = !val; break;
	default: return;
	}

	dk_set_led(LIGHT_LED, val);
}


/* Appelé quand aucun second appui n'est arrivé : c'était un simple clic */
static void on_click_timer_expiry(struct k_timer *timer)
{
	ARG_UNUSED(timer);

	if (mode == MODE_SERVER) {
		LOG_INF("Single click -> toggle server light");
		coap_client_toggle_one_light();
	} else {
		LOG_INF("Single click -> relay toggle to other client");
		coap_client_relay_toggle();
	}
}

K_TIMER_DEFINE(click_timer, on_click_timer_expiry, NULL);

static void switch_mode(void)
{
	mode = (mode == MODE_SERVER) ? MODE_RELAY : MODE_SERVER;

	LOG_INF("Double click -> mode %s",
		mode == MODE_SERVER ? "1 (SERVER)" : "2 (RELAY)");
	update_status_led();
}

#if CONFIG_BT_NUS

#define COMMAND_REQUEST_UNICAST 'u'
#define COMMAND_REQUEST_MULTICAST 'm'
#define COMMAND_REQUEST_PROVISIONING 'p'
#define COMMAND_REQUEST_RELAY 'r'

static void on_nus_received(struct bt_conn *conn, const uint8_t *const data, uint16_t len)
{
	LOG_INF("Received data: %c", data[0]);

	switch (*data) {
	case COMMAND_REQUEST_UNICAST:
		coap_client_toggle_one_light();
		break;

	case COMMAND_REQUEST_MULTICAST:
		coap_client_toggle_mesh_lights();
		break;

	case COMMAND_REQUEST_PROVISIONING:
		coap_client_send_provisioning_request();
		break;

	case COMMAND_REQUEST_RELAY:
		coap_client_relay_toggle();
		break;

	default:
		LOG_WRN("Received invalid data from NUS");
	}
}

/* Plus de LED dédiée au BLE : on se contente d'un log */
static void on_ble_connect(struct k_work *item)
{
	ARG_UNUSED(item);
	LOG_INF("BLE connected");
}

static void on_ble_disconnect(struct k_work *item)
{
	ARG_UNUSED(item);
	LOG_INF("BLE disconnected");
}

#endif /* CONFIG_BT_NUS */

static void on_ot_connect(struct k_work *item)
{
	ARG_UNUSED(item);

	ot_connected = true;
	update_status_led();
}

static void on_ot_disconnect(struct k_work *item)
{
	ARG_UNUSED(item);

	ot_connected = false;
	
	update_status_led();
}

static void on_mtd_mode_toggle(uint32_t med)
{
#if IS_ENABLED(CONFIG_PM_DEVICE)
	const struct device *cons = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

	if (!device_is_ready(cons)) {
		return;
	}

	if (med) {
		pm_device_action_run(cons, PM_DEVICE_ACTION_RESUME);
	} else {
		pm_device_action_run(cons, PM_DEVICE_ACTION_SUSPEND);
	}
#endif
	/* Plus de LED pour le mode SED/MED (seulement 2 LEDs) */
	LOG_INF("MTD mode: %s", med ? "MED" : "SED");
}

static void on_button_changed(uint32_t button_state, uint32_t has_changed)
{
	uint32_t pressed = button_state & has_changed;

	if (!(pressed & USER_BUTTON_MSK)) {
		return;
	}

	/* Pas encore d'adresse serveur : chaque appui lance le provisioning */
	if (!coap_client_is_provisioned()) {
		LOG_INF("Not provisioned -> send provisioning request");
		coap_client_send_provisioning_request();
		return;
	}

	if (k_timer_remaining_get(&click_timer) > 0) {
	
		k_timer_stop(&click_timer);
		switch_mode();
	} else {
	
		k_timer_start(&click_timer, K_MSEC(DOUBLE_CLICK_MS), K_NO_WAIT);
	}
}

int main(void)
{
	int ret;

	LOG_INF("Start CoAP-client sample");

	if (IS_ENABLED(CONFIG_RAM_POWER_DOWN_LIBRARY)) {
		power_down_unused_ram();
	}

	ret = dk_buttons_init(on_button_changed);
	if (ret) {
		LOG_ERR("Cannot init buttons (error: %d)", ret);
		return 0;
	}

	ret = dk_leds_init();
	if (ret) {
		LOG_ERR("Cannot init leds, (error: %d)", ret);
		return 0;
	}

	dk_set_led_off(STATUS_LED);
	dk_set_led_off(LIGHT_LED);

#if CONFIG_BT_NUS
	struct bt_nus_cb nus_clbs = {
		.received = on_nus_received,
		.sent = NULL,
	};

	ret = ble_utils_init(&nus_clbs, on_ble_connect, on_ble_disconnect);
	if (ret) {
		LOG_ERR("Cannot init BLE utilities");
		return 0;
	}

#endif /* CONFIG_BT_NUS */
	coap_client_set_light_cmd_handler(on_light_cmd);
	coap_client_utils_init(on_ot_connect, on_ot_disconnect, on_mtd_mode_toggle);

	return 0;
}