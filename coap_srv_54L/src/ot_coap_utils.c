/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/logging/log.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/net/net_l2.h>
#include <zephyr/net/openthread.h>
#include <openthread/coap.h>
#include <openthread/ip6.h>
#include <openthread/message.h>
#include <openthread/thread.h>

#include "ot_coap_utils.h"

LOG_MODULE_REGISTER(ot_coap_utils, CONFIG_OT_COAP_UTILS_LOG_LEVEL);

#define MAX_CLIENTS 4
static otIp6Address clients[MAX_CLIENTS];
static uint8_t client_count;

struct server_context {
	struct otInstance *ot;
	bool provisioning_enabled;
	light_request_callback_t on_light_request;
	provisioning_request_callback_t on_provisioning_request;
};

static struct server_context srv_context = {
	.ot = NULL,
	.provisioning_enabled = false,
	.on_light_request = NULL,
	.on_provisioning_request = NULL,
};

/**@brief Definition of CoAP resources for provisioning. */
static otCoapResource provisioning_resource = {
	.mUriPath = PROVISIONING_URI_PATH,
	.mHandler = NULL,
	.mContext = NULL,
	.mNext = NULL,
};

/**@brief Definition of CoAP resources for light. */
static otCoapResource light_resource = {
	.mUriPath = LIGHT_URI_PATH,
	.mHandler = NULL,
	.mContext = NULL,
	.mNext = NULL,
};

static otCoapResource relay_resource = {
	.mUriPath = RELAY_URI_PATH,
	.mHandler = NULL,
	.mContext = NULL,
	.mNext = NULL,
};

static int client_register(const otIp6Address *addr)
{
	char addr_str[OT_IP6_ADDRESS_STRING_SIZE];
 
	for (int i = 0; i < client_count; i++) {
		if (otIp6IsAddressEqual(&clients[i], addr)) {
			return i;
		}
	}
 
	if (client_count >= MAX_CLIENTS) {
		LOG_WRN("Client table full");
		return -1;
	}
 
	clients[client_count] = *addr;
	otIp6AddressToString(addr, addr_str, sizeof(addr_str));
	LOG_INF("New client [%d]: %s", client_count, addr_str);
 
	return client_count++;
}

static otError provisioning_response_send(otMessage *request_message,
					  const otMessageInfo *message_info)
{
	otError error = OT_ERROR_NO_BUFS;
	otMessage *response;
	const void *payload;
	uint16_t payload_size;

	response = otCoapNewMessage(srv_context.ot, NULL);
	if (response == NULL) {
		goto end;
	}

	otCoapMessageInit(response, OT_COAP_TYPE_NON_CONFIRMABLE,
			  OT_COAP_CODE_CONTENT);

	error = otCoapMessageSetToken(
		response, otCoapMessageGetToken(request_message),
		otCoapMessageGetTokenLength(request_message));
	if (error != OT_ERROR_NONE) {
		goto end;
	}

	error = otCoapMessageSetPayloadMarker(response);
	if (error != OT_ERROR_NONE) {
		goto end;
	}

	payload = otThreadGetMeshLocalEid(srv_context.ot);
	payload_size = sizeof(otIp6Address);

	error = otMessageAppend(response, payload, payload_size);
	if (error != OT_ERROR_NONE) {
		goto end;
	}

	error = otCoapSendResponse(srv_context.ot, response, message_info);

	LOG_HEXDUMP_INF(payload, payload_size, "Sent provisioning response:");

end:
	if (error != OT_ERROR_NONE && response != NULL) {
		otMessageFree(response);
	}

	return error;
}

static void provisioning_request_handler(void *context, otMessage *message,
					 const otMessageInfo *message_info)
{
	otError error;
	otMessageInfo msg_info;

	ARG_UNUSED(context);

	if (!srv_context.provisioning_enabled) {
		LOG_WRN("Received provisioning request but provisioning "
			"is disabled");
		return;
	}

	LOG_INF("Received provisioning request");

	if ((otCoapMessageGetType(message) == OT_COAP_TYPE_NON_CONFIRMABLE) &&
	    (otCoapMessageGetCode(message) == OT_COAP_CODE_GET)) {
		client_register(&message_info->mPeerAddr);
		msg_info = *message_info;
		memset(&msg_info.mSockAddr, 0, sizeof(msg_info.mSockAddr));

		error = provisioning_response_send(message, &msg_info);
		if (error == OT_ERROR_NONE) {
			srv_context.on_provisioning_request();
			srv_context.provisioning_enabled = false;
		}
	}
}

static void light_request_handler(void *context, otMessage *message,
				  const otMessageInfo *message_info)
{
	uint8_t command;

	ARG_UNUSED(context);

	if (otCoapMessageGetType(message) != OT_COAP_TYPE_NON_CONFIRMABLE) {
		LOG_ERR("Light handler - Unexpected type of message");
		goto end;
	}

	if (otCoapMessageGetCode(message) != OT_COAP_CODE_PUT) {
		LOG_ERR("Light handler - Unexpected CoAP code");
		goto end;
	}

	if (otMessageRead(message, otMessageGetOffset(message), &command, 1) !=
	    1) {
		LOG_ERR("Light handler - Missing light command");
		goto end;
	}

	LOG_INF("Received light request: %c", command);

	srv_context.on_light_request(command);

end:
	return;
}

static otError send_light_cmd(const otIp6Address *addr, uint8_t command)
{
	otError error = OT_ERROR_NO_BUFS;
	otMessage *msg;
	otMessageInfo info;
 
	msg = otCoapNewMessage(srv_context.ot, NULL);
	if (msg == NULL) {
		goto end;
	}
 
	otCoapMessageInit(msg, OT_COAP_TYPE_NON_CONFIRMABLE, OT_COAP_CODE_PUT);
	otCoapMessageGenerateToken(msg, OT_COAP_DEFAULT_TOKEN_LENGTH);
 
	error = otCoapMessageAppendUriPathOptions(msg, LIGHT_URI_PATH);
	if (error != OT_ERROR_NONE) {
		goto end;
	}
 
	error = otCoapMessageSetPayloadMarker(msg);
	if (error != OT_ERROR_NONE) {
		goto end;
	}
 
	error = otMessageAppend(msg, &command, sizeof(command));
	if (error != OT_ERROR_NONE) {
		goto end;
	}
 
	memset(&info, 0, sizeof(info));
	info.mPeerAddr = *addr;
	info.mPeerPort = COAP_PORT;
 
	error = otCoapSendRequest(srv_context.ot, msg, &info, NULL, NULL);
 
end:
	if (error != OT_ERROR_NONE && msg != NULL) {
		otMessageFree(msg);
	}
 
	return error;
}
 


static void relay_request_handler(void *context, otMessage *message,
				  const otMessageInfo *message_info)
{
	uint8_t command;
	int sender;
	int forwarded = 0;
 
	ARG_UNUSED(context);
 
	if (otCoapMessageGetType(message) != OT_COAP_TYPE_NON_CONFIRMABLE) {
		LOG_ERR("Relay handler - Unexpected type of message");
		return;
	}
 
	if (otCoapMessageGetCode(message) != OT_COAP_CODE_PUT) {
		LOG_ERR("Relay handler - Unexpected CoAP code");
		return;
	}
 
	if (otMessageRead(message, otMessageGetOffset(message), &command, 1) != 1) {
		LOG_ERR("Relay handler - Missing command");
		return;
	}
 
	sender = client_register(&message_info->mPeerAddr);
 
	LOG_INF("Relay request '%c' from client [%d]", command, sender);
 
	/* Retransmission à tous les clients sauf l'émetteur */
	for (int i = 0; i < client_count; i++) {
		if (i == sender) {
			continue;
		}
 
		if (send_light_cmd(&clients[i], command) == OT_ERROR_NONE) {
			LOG_INF("  -> forwarded to client [%d]", i);
			forwarded++;
		} else {
			LOG_ERR("  -> failed to forward to client [%d]", i);
		}
	}
 
	if (forwarded == 0) {
		LOG_WRN("No other client registered: nothing to forward");
	}
}

static void coap_default_handler(void *context, otMessage *message,
				 const otMessageInfo *message_info)
{
	ARG_UNUSED(context);
	ARG_UNUSED(message);
	ARG_UNUSED(message_info);

	LOG_INF("Received CoAP message that does not match any request "
		"or resource");
}

void ot_coap_activate_provisioning(void)
{
	srv_context.provisioning_enabled = true;
}

void ot_coap_deactivate_provisioning(void)
{
	srv_context.provisioning_enabled = false;
}

bool ot_coap_is_provisioning_active(void)
{
	return srv_context.provisioning_enabled;
}

int ot_coap_init(provisioning_request_callback_t on_provisioning_request,
		 light_request_callback_t on_light_request)
{
	otError error;

	srv_context.provisioning_enabled = false;
	srv_context.on_provisioning_request = on_provisioning_request;
	srv_context.on_light_request = on_light_request;

	srv_context.ot = openthread_get_default_instance();
	if (!srv_context.ot) {
		LOG_ERR("There is no valid OpenThread instance");
		error = OT_ERROR_FAILED;
		goto end;
	}

	provisioning_resource.mContext = srv_context.ot;
	provisioning_resource.mHandler = provisioning_request_handler;

	light_resource.mContext = srv_context.ot;
	light_resource.mHandler = light_request_handler;
	relay_resource.mContext = srv_context.ot;
	relay_resource.mHandler = relay_request_handler;

	otCoapSetDefaultHandler(srv_context.ot, coap_default_handler, NULL);
	otCoapAddResource(srv_context.ot, &light_resource);
	otCoapAddResource(srv_context.ot, &provisioning_resource);
	otCoapAddResource(srv_context.ot, &relay_resource);

	error = otCoapStart(srv_context.ot, COAP_PORT);
	if (error != OT_ERROR_NONE) {
		LOG_ERR("Failed to start OT CoAP. Error: %d", error);
		goto end;
	}

end:
	return error == OT_ERROR_NONE ? 0 : 1;
}
