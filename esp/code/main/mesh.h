#ifndef __MESH_H__
#define __MESH_H__

#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "esp_mesh.h"
#include "esp_mesh_internal.h"

#define TIME_SLEEP 5 //time in seconds
#define SOFT_VERSION 1
#define SEQU_SEUIL 65000

#define RX_SIZE          (1500)
#define TX_SIZE          (1460)

/* Frames composition*/

#define VERSION 0
#define TYPE 1
#define DATA 2
#define CHECKSUM 15
#define FRAME_SIZE 16

/* Frames types */

#define BEACON 1
#define B_ACK 2
#define INSTALL 3
#define COLOR 4
#define COLOR_E 5
#define AMA 6
#define ERROR 7
#define SLEEP 8

/* AMA sub types */

#define AMA_INIT 61
#define AMA_COLOR 62
#define AMA_REPRISE 69

/* SLEEP sub types */

#define SLEEP_SERVER 81
#define SLEEP_MESH 82
#define WAKE_UP 89

/* States */

#define INIT 1
#define CONF 2
#define ADDR 3
#define COLOR 4
#define ERROR_S 5
#define SLEEP_S 6

/*******************************************************
 *                Structures
 *******************************************************/
/**
 * @brief Route table element
 */
struct node {
    mesh_addr_t card; /**< Mac address of the card, mesh_addr_t format */
    bool state; /**< Indicate if card is currently connected to the root */
};

struct node route_table[CONFIG_MESH_ROUTE_TABLE_SIZE];
extern int route_table_size;
extern char *MESH_TAG;
extern uint8_t MESH_ID[6];

extern bool is_running;
extern bool is_mesh_connected;
extern mesh_addr_t mesh_parent_addr;
extern int mesh_layer;
extern uint8_t my_mac[6];
extern unsigned int state;
extern bool is_asleep;
extern uint16_t current_sequence;

/*Variable du socket */
extern struct sockaddr_in tcpServerAddr;
extern struct sockaddr_in tcpServerReset;
extern uint32_t sock_fd;
extern bool is_server_connected;

/* Table de routage Arbalet Mesh*/
extern int route_table_size;

void connect_to_server();
void reset_and_connect_server();
void add_route_table(uint8_t * mac, int pos);

#endif
