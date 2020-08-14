/*  Bluetooth Mesh */

/*
 * Copyright (c) 2017 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "btstack/bluetooth.h"
#include "system/includes.h"
#include "bt_common.h"
#include "api/sig_mesh_api.h"
#include "model_api.h"

#define LOG_TAG         "[Mesh-vendor_srv]"
#define LOG_INFO_ENABLE
#define LOG_DEBUG_ENABLE
#define LOG_DUMP_ENABLE
#define LOG_ERROR_ENABLE
#include "debug.h"

#if (CONFIG_MESH_MODEL == SIG_MESH_VENDOR_SERVER)

extern u16_t primary_addr;
extern void mesh_setup(void (*init_cb)(void));
extern void gpio_pin_write(u8_t led_index, u8_t onoff);
extern void bt_mac_addr_set(u8 *bt_addr);
extern void prov_complete(u16_t net_idx, u16_t addr);
extern void prov_reset(void);
static void vendor_set(struct bt_mesh_model *model,
                       struct bt_mesh_msg_ctx *ctx,
                       struct net_buf_simple *buf);

extern uint32_t btctler_get_rand_from_assign_range(uint32_t rand, uint32_t min, uint32_t max);
extern void pseudo_random_genrate(uint8_t *dest, unsigned size);

/**
 * @brief Config current node features(Relay/Proxy/Friend/Low Power)
 */
/*-----------------------------------------------------------*/
#define BT_MESH_FEAT_SUPPORTED_TEMP         ( \
                                                  \
                                                0 \
                                            )
#include "feature_correct.h"
const int config_bt_mesh_features = BT_MESH_FEAT_SUPPORTED;

/**
 * @brief Config proxy connectable adv hardware param
 */
/*-----------------------------------------------------------*/
#if BT_MESH_FEATURES_GET(BT_MESH_FEAT_LOW_POWER)
const u16 config_bt_mesh_proxy_node_adv_interval = ADV_SCAN_UNIT(3000); // unit: ms
#else
const u16 config_bt_mesh_proxy_node_adv_interval = ADV_SCAN_UNIT(300); // unit: ms
#endif /* BT_MESH_FEATURES_GET(BT_MESH_FEAT_LOW_POWER) */

/**
 * @brief Config adv cache buffer
 */
/*-----------------------------------------------------------*/
const u8 config_bt_mesh_adv_buf_count = 15;

/**
 * @brief Conifg complete local name
 */
/*-----------------------------------------------------------*/
#define BLE_DEV_NAME        'V', 'd', '_','s', 'r', 'v'

const uint8_t mesh_name[] = {
    // Name
    BYTE_LEN(BLE_DEV_NAME) + 1, BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME, BLE_DEV_NAME,
};

void get_mesh_adv_name(u8 *len, u8 **data)
{
    *len = sizeof(mesh_name);

    *data = mesh_name;
}

/**
 * @brief Conifg MAC of current demo
 */
/*-----------------------------------------------------------*/
#define CUR_DEVICE_MAC_ADDR         0x442233445566

struct _switch {
    u8_t sw_num;
    u8_t onoff_state;
};

/* Company Identifiers (see Bluetooth Assigned Numbers) */
#define BT_COMP_ID_LF           0x05D6 // Zhuhai Jieli technology Co.,Ltd

/*
 * Vendor Model ID
 * detail on Mesh_v1.0 <3.7.2 Model identifier>
 */
#define BT_MESH_VENDOR_MODEL_ID_SRV             0x0001

/*
 * Vendor Model Operation Codes
 * detail on Mesh_v1.0 <3.7.3.1 Operation codes>
 */
#define BT_MESH_VENDOR_MODEL_OP_SET			    BT_MESH_MODEL_OP_3(0x01, BT_COMP_ID_LF)
#define BT_MESH_VENDOR_MODEL_OP_STATUS			BT_MESH_MODEL_OP_3(0x02, BT_COMP_ID_LF)

/*
 * Access payload fields
 * detail on Mesh_v1.0 <3.7.3 Access payload>
 */
#define TRANSMIC_SIZE                           4
#define MAX_USEFUL_ACCESS_PAYLOAD_SIZE          11 // 32 bit TransMIC (unsegmented)
#define ACCESS_OP_SIZE      3
#define ACCESS_PARAM_SIZE   (MAX_USEFUL_ACCESS_PAYLOAD_SIZE - ACCESS_OP_SIZE)

/* test data */
#define LED_STATE_LEN       1
#define REMAIN_DATA_LEN     (ACCESS_PARAM_SIZE - LED_STATE_LEN)
#define REMAIN_DATA_VALUE   0x02

/* LED NUMBER */
#define LED0_GPIO_PIN       0

struct onoff_state {
    u8_t onoff;
    u8_t led_gpio_pin;
};

static struct onoff_state onoff_state[] = {
    { .led_gpio_pin = LED0_GPIO_PIN },
};

const u8 led_use_port[1] = {

    IO_PORTA_01,

};

/*
 * Publication Declarations
 *
 * The publication messages are initialized to the
 * the size of the opcode + content
 *
 * For publication, the message must be in static or global as
 * it is re-transmitted several times. This occurs
 * after the function that called bt_mesh_model_publish() has
 * exited and the stack is no longer valid.
 *
 * Note that the additional 4 bytes for the AppMIC is not needed
 * because it is added to a stack variable at the time a
 * transmission occurs.
 *
 */
BT_MESH_MODEL_PUB_DEFINE(vendor_pub_cli, NULL, MAX_USEFUL_ACCESS_PAYLOAD_SIZE);


static void respond_messsage_schedule(u16 *delay, u16 *duration, void *cb_data)
{
    /*  Mesh_v1.0 <3.7.4.1 Transmitting an access message>
     *
     *	  If the message is sent in response to a received message
     *  that was sent to a unicast address, the node should transmit
     *  the response message with a random delay between 20 and 50 milliseconds.
     *
     *    If the message is sent in response to a received message
     *  that was sent to a group address or a virtual address,
     *  the node should transmit the response message with
     *  a random delay between 20 and 500 milliseconds.
     */
    u16 delay_ms;
    struct bt_mesh_msg_ctx *ctx = cb_data;

    pseudo_random_genrate((u8 *)&delay_ms, 2);
    if (BT_MESH_ADDR_IS_UNICAST(ctx->recv_dst)) {
        delay_ms = btctler_get_rand_from_assign_range(delay_ms, 20, 50);
    } else {
        delay_ms = btctler_get_rand_from_assign_range(delay_ms, 20, 150);
    }

    *delay = delay_ms;
    log_info("respond_messsage delay =%u ms", delay_ms);
}

static const struct bt_mesh_send_cb rsp_msg_cb = {
    .user_intercept = respond_messsage_schedule,
};



/*
 * Models in an element must have unique op codes.
 *
 * The mesh stack dispatches a message to the first model in an element
 * that is also bound to an app key and supports the op code in the
 * received message.
 *
 */
/*
 * Vendor Model Server Op Dispatch Table
 */
static const struct bt_mesh_model_op vendor_srv_op[] = {
    { BT_MESH_VENDOR_MODEL_OP_SET, ACCESS_OP_SIZE, vendor_set },
    BT_MESH_MODEL_OP_END,
};

/*
 * Server Configuration Declaration
 */
static struct bt_mesh_cfg_srv cfg_srv = {
    .relay          = BT_MESH_FEATURES_GET(BT_MESH_FEAT_RELAY),
    .frnd           = BT_MESH_FEATURES_GET(BT_MESH_FEAT_FRIEND),
    .gatt_proxy     = BT_MESH_FEATURES_GET(BT_MESH_FEAT_PROXY),
    .beacon         = BT_MESH_BEACON_DISABLED,
    .default_ttl    = 7,
};

/*
 * Client Configuration Declaration
 */
static struct bt_mesh_cfg_cli cfg_cli;

/*
 *
 * Element Model Declarations
 *
 * Element 0 Root Models
 */
static struct bt_mesh_model root_models[] = {
    BT_MESH_MODEL_CFG_SRV(&cfg_srv), // default for root model
    BT_MESH_MODEL_CFG_CLI(&cfg_cli), // default for self-configuration network
};

static struct bt_mesh_model vendor_server_models[] = {
    BT_MESH_MODEL_VND(BT_COMP_ID_LF, BT_MESH_VENDOR_MODEL_ID_SRV,
    vendor_srv_op, NULL, &onoff_state[0]),
};

/*
 * LED to Server Model Assigmnents
 */
static struct bt_mesh_model *mod_srv_sw[] = {
    &vendor_server_models[0],
};

/*
 * Root and Secondary Element Declarations
 */
static struct bt_mesh_elem elements[] = {
    BT_MESH_ELEM(0, root_models, vendor_server_models),
};

static const struct bt_mesh_comp composition = {
    .cid = BT_COMP_ID_LF,
    .elem = elements,
    .elem_count = ARRAY_SIZE(elements),
};

static u8_t dev_uuid[16] = { 0xdd, 0xdd };

static const struct bt_mesh_prov prov = {
    .uuid = dev_uuid,
    .output_size = 0,
    .output_actions = 0,
    .output_number = 0,
    .complete = prov_complete,
    .reset = prov_reset,
};

static void vendor_set(struct bt_mesh_model *model,
                       struct bt_mesh_msg_ctx *ctx,
                       struct net_buf_simple *buf)
{
    log_info("receive vendor client message len except opcode =0x%x", buf->len);
    log_info_hexdump(buf->data, buf->len);

    struct net_buf_simple *msg = model->pub->msg;
    struct onoff_state *onoff_state = model->user_data;

    //< set led onoff
    onoff_state->onoff = buffer_pull_u8_from_head(buf);
    log_info("Local Node 0x%02x shoult set led to 0x%02x\n",
             bt_mesh_model_elem(model)->addr, onoff_state->onoff);
    gpio_pin_write(onoff_state->led_gpio_pin, onoff_state->onoff);

    //< Ack to client with the same receive data
    NET_BUF_SIMPLE_DEFINE(ack_msg, MAX_USEFUL_ACCESS_PAYLOAD_SIZE + TRANSMIC_SIZE);
    bt_mesh_model_msg_init(&ack_msg, BT_MESH_VENDOR_MODEL_OP_STATUS); // Opcode: 3 octets
    buffer_add_u8_at_tail(&ack_msg, onoff_state->onoff); // onoff state: 1 octets
    buffer_memset(&ack_msg, buffer_pull_u8_from_head(buf) + 1, REMAIN_DATA_LEN);

    log_info_hexdump(ack_msg.data, MAX_USEFUL_ACCESS_PAYLOAD_SIZE);
    if (bt_mesh_model_send(model, ctx, &ack_msg, NULL, NULL)) {
        log_info("Unable to send Status response\n");
    }
}

#define NODE_ADDR 0x0008

#define GROUP_ADDR 0xc000

#define OP_VENDOR_BUTTON BT_MESH_MODEL_OP_3(0x00, BT_COMP_ID_LF)

static const u8_t net_key[16] = {
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
};

static const u8_t dev_key[16] = {
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
};

static const u8_t app_key[16] = {
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
};

static const u16_t net_idx;
static const u16_t app_idx;
static const u32_t iv_index;
static u8_t flags;
static u16_t node_addr = NODE_ADDR;

static void configure(void)
{
    log_info("Configuring...");

    u16_t elem_addr = node_addr;
    log_info("node_addr=0x%x, net_idx=0x%x, app_idx=0x%x", node_addr, net_idx, app_idx);

    /* Add Application Key */
    log_info("bt_mesh_cfg_app_key_add");
    bt_mesh_cfg_app_key_add(net_idx, node_addr, net_idx, app_idx, app_key, NULL);

    u16 dst_addr = GROUP_ADDR;

    /* Bind to vendor server model */
    log_info("bt_mesh_cfg_mod_app_bind_vnd server");
    bt_mesh_cfg_mod_app_bind_vnd(net_idx, node_addr, elem_addr, app_idx,
                                 BT_MESH_VENDOR_MODEL_ID_SRV,
                                 BT_COMP_ID_LF,
                                 NULL);

    /* Add model subscription */
    log_info("bt_mesh_cfg_mod_sub_add_vnd server");
    bt_mesh_cfg_mod_sub_add_vnd(net_idx, node_addr, elem_addr, dst_addr,
                                BT_MESH_VENDOR_MODEL_ID_SRV,
                                BT_COMP_ID_LF,
                                NULL);

    log_info("Configuration complete");
}

void input_key_handler(u8 key_status, u8 key_number)
{
    struct _switch press_switch;

    log_info("key_number=0x%x", key_number);

    if ((key_number == 0) && (key_status == KEY_EVENT_LONG)) {
        log_info("\n  <bt_mesh_reset> \n");
        bt_mesh_reset();
        return;
    }

    if ((key_number == 4) && (key_status == KEY_EVENT_CLICK)) {
        log_info("\n  <NODE_ADDR = 0x%x> \n", NODE_ADDR);
        bt_mesh_reset();
        return;
    }

    switch (key_status) {
    case KEY_EVENT_CLICK:
        log_info("  [KEY_EVENT_CLICK]  ");
        break;
    case KEY_EVENT_LONG:
        log_info("  [KEY_EVENT_LONG]  ");
        break;
    case KEY_EVENT_HOLD:
        log_info("  [KEY_EVENT_HOLD]  ");
        break;
    default :
        return;
    }
}

static void mesh_init(void)
{
    int err = bt_mesh_init(&prov, &composition);
    if (err) {
        log_error("Initializing mesh failed (err %d)\n", err);
        return;
    }

    settings_load();

    err = bt_mesh_provision(net_key, net_idx, flags, iv_index, node_addr, dev_key);
    if (err) {
        log_error("Using stored settings\n");
    } else {
        log_info("Provisioning completed\n");

        configure();
    }
}


void bt_ble_init(void)
{
    u8 bt_addr[6] = {MAC_TO_LITTLE_ENDIAN(CUR_DEVICE_MAC_ADDR)};

    bt_mac_addr_set(bt_addr);

    mesh_setup(mesh_init);
}

/********************************************************************************/
/*
 *                  Mesh API Example
 */

/*
 * note:    Each model only support one publish address.
 *          But support more subscribe address list, base on CONFIG_BT_MESH_MODEL_GROUP_COUNT.
 */
void example_node_subscribe_address_overwrite(void)
{
    //< overwrite root element subscribe address to 0xc001 (group address)

    u16_t elem_addr = node_addr;
    u16 dst_addr = 0xc001; // new dst address

    bt_mesh_cfg_mod_sub_overwrite_vnd(net_idx, node_addr, elem_addr, dst_addr,
                                      BT_MESH_VENDOR_MODEL_ID_SRV,
                                      BT_COMP_ID_LF,
                                      NULL);
}

void example_node_subscribe_address_delete(void)
{
    //< delete initial subscribe address

    u16_t elem_addr = node_addr;
    u16 initial_addr = GROUP_ADDR; // initial address

    bt_mesh_cfg_mod_sub_del_vnd(net_idx, node_addr, elem_addr, initial_addr,
                                BT_MESH_VENDOR_MODEL_ID_SRV,
                                BT_COMP_ID_LF,
                                NULL);
}

void example_node_subscribe_address_add(void)
{
    //< add new subscribe address

    u16_t elem_addr = node_addr;
    u16 new_addr = 0xc002; // new add address

    bt_mesh_cfg_mod_sub_add_vnd(net_idx, node_addr, elem_addr, new_addr,
                                BT_MESH_VENDOR_MODEL_ID_SRV,
                                BT_COMP_ID_LF,
                                NULL);
}

void example_node_relay_character_set(void)
{
    //< BT_MESH_RELAY_DISABLED / BT_MESH_RELAY_ENABLED

    u8 relay_attr = BT_MESH_RELAY_DISABLED;
    u8 relay_retransmit = BT_MESH_TRANSMIT(2, 20);

    bt_mesh_cfg_relay_set(net_idx, node_addr, relay_attr,
                          relay_retransmit, NULL, NULL);
}

void example_node_reset(void)
{
    //< reset the node to an unprovisioned device

    bt_mesh_reset();
}

#endif /* (CONFIG_MESH_MODEL == SIG_MESH_VENDOR_SERVER) */
