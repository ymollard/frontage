/* Mesh Internal Communication Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "esp_mesh.h"
#include "esp_mesh_internal.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "pthread.h"

/*******************************************************
 *                Macros
 *******************************************************/
//#define MESH_P2P_TOS_OFF

/*******************************************************
 *                Constants
 *******************************************************/
#define RX_SIZE          (1500)
#define TX_SIZE          (1460)
#define TIME_SLEEP 5 //time in seconds
#define SOFT_VERSION 1
#define SEQU_SEUIL 65000

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


/*******************************************************
 *                Variable Definitions
 *******************************************************/
static const char *MESH_TAG = "mesh_main";
static const uint8_t MESH_ID[6] = { 0x77, 0x77, 0x77, 0x77, 0x77, 0x77};
static uint8_t tx_buf[TX_SIZE] = { 0, };
static uint8_t rx_buf[RX_SIZE] = { 0, };
static bool is_running = true;
static bool is_mesh_connected = false;
static mesh_addr_t mesh_parent_addr;
static int mesh_layer = -1;
static uint8_t my_mac[6] = {0};
static unsigned int state = INIT;
static bool is_asleep = false;
static uint16_t current_sequence = 0;

/*Variable du socket */
static struct sockaddr_in tcpServerAddr;
static uint32_t sock_fd;
static bool is_server_connected = false;

/* Table de routage Arbalet Mesh*/
static struct node route_table[CONFIG_MESH_ROUTE_TABLE_SIZE]; 
static int route_table_size = 0;
static int num[CONFIG_MESH_ROUTE_TABLE_SIZE];

/* Buffer de communication */
#define RXB_SIZE 50000
static uint8_t reception_buffer[RXB_SIZE]; // Reception pipe containing all received message
static int rxbuf_free_size = RXB_SIZE;
static int rxbuf_tail = 0;
static int rxbuf_head = 0;
static pthread_mutex_t rxbuf_write = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t rxbuf_read = PTHREAD_MUTEX_INITIALIZER; 

#define TXB_SIZE 50000
static uint8_t transmission_buffer[TXB_SIZE]; // Transmission pipe containing messages to be send
static int txbuf_free_size = TXB_SIZE;
static int txbuf_head = 0;
static pthread_mutex_t txbuf_write = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t txbuf_read = PTHREAD_MUTEX_INITIALIZER;

/*******************************************************
 *                Function Declarations
 *******************************************************/

int get_size(uint8_t type);

/*******************************************************
 *                Function Definitions
 *******************************************************/

/**
 * @brief Computes a CRC on a frame by bits parity
 * @param computed frame
 * @param length of frame
 * @param the offset of the first computed bit
 * @param the frequency of bit extraction. A 1-frequency means every bit is computed, a 2-frequency means one every 2 bits is computed
 */
uint8_t variable_crc_computer(uint8_t * frame, uint8_t len, uint8_t offset, uint8_t frequency) {
    uint8_t crc_bool = 0;
    uint8_t i = offset;
    
    if(frame != NULL) {
        while(i<len) {
            crc_bool = crc_bool + frame[i];
            i = i+frequency;
        }
    }
    return (crc_bool%2);
}

/**
 * @brief Computes a 7-bits long CRC on a frame by bits parity. Security is ensured by first computing a CRC bit-to-bit, then on even and odd bits, and then one every 3 bits, with 3 different offsets. The last bit of the CRC is a bit-to-bit parity on the 6 first bits themselves.
 * @param computed frame
 * @param length of frame
 * @param 7-cell long table where the crc will be written
 * @attention ensure that the crc_table given in parameters is 7-cell long
 */
void frame_crc_computer(uint8_t * frame, uint8_t len, uint8_t * crc_table) {
    uint8_t crc_bool_6 = 0;

    if(frame != NULL) {
        uint8_t crc_bool_0 = variable_crc_computer(frame, len, 0, 1);
        crc_table[0] = crc_bool_0;
        crc_bool_6 = crc_bool_0 + crc_bool_6;

        uint8_t crc_bool_1 = variable_crc_computer(frame, len, 0, 2);
        crc_table[1] = crc_bool_1;
        crc_bool_6 = crc_bool_1 + crc_bool_6;

        uint8_t crc_bool_2 = variable_crc_computer(frame, len, 1, 2);
        crc_table[2] = crc_bool_2;
        crc_bool_6 = crc_bool_2 + crc_bool_6;

        uint8_t crc_bool_3 = variable_crc_computer(frame, len, 0, 3);
        crc_table[3] = crc_bool_3;
        crc_bool_6 = crc_bool_3 + crc_bool_6;

        uint8_t crc_bool_4 = variable_crc_computer(frame, len, 1, 3);
        crc_table[4] = crc_bool_4;
        crc_bool_6 = crc_bool_4 + crc_bool_6;

        uint8_t crc_bool_5 = variable_crc_computer(frame, len, 2, 3);
        crc_table[5] = crc_bool_5;
        crc_bool_6 = crc_bool_5 + crc_bool_6;

        crc_bool_6 = crc_bool_6 % 2;
        crc_table[6] = crc_bool_6;
    }
}

int check_crc(uint8_t * frame) {
    uint8_t crc_table[7];
    int size = get_size(frame[TYPE]);
    uint8_t frame2[(size-1)*8];
    //ESP_LOGI(MESH_TAG, "Frame checked = %d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d", frame[0], frame[1], frame[2], frame[3], frame[4], frame[5], frame[6], frame[7], frame[8], frame[9], frame[10], frame[11], frame[12], frame[13], frame[14], frame[15]);
    for (int i = 0; i < size-1; i++) {
	for (int j = 0; j < 8; j++) {
	    frame2[(i*8)+j] = (frame[i] & (1 << j)) >> j;
	}
    }
    //ESP_LOGI(MESH_TAG, "Frame checked = %d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d", frame2[0], frame2[1], frame2[2], frame2[3], frame2[4], frame2[5], frame2[6], frame2[7], frame2[8], frame2[9], frame2[10], frame2[11], frame2[12], frame2[13], frame2[14], frame2[15], frame2[16], frame2[17], frame2[18], frame2[19], frame2[20], frame2[21], frame2[22], frame2[23], frame2[24], frame2[25], frame2[26], frame2[27], frame2[28], frame2[29], frame2[30], frame2[31], frame2[32], frame2[33], frame2[34], frame2[35], frame2[36], frame2[37], frame2[38], frame2[39]);
    frame_crc_computer(frame2, (size-1)*8, crc_table);
    int crc = crc_table[0] << 6 | crc_table[1] << 5 | crc_table[2] << 4 | crc_table[3] << 3 | crc_table[4] << 2 | crc_table[5] << 1 | crc_table[6];
    ESP_LOGE(MESH_TAG, "Compared %d and %d at CRC check", frame[size-1], crc);
    return frame[size-1] == crc;
}

void get_crc(uint8_t * frame) {
    uint8_t crc_table[7];
    //ESP_LOGI(MESH_TAG, "Frame calculated = %d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d", frame[0], frame[1], frame[2], frame[3], frame[4], frame[5], frame[6], frame[7], frame[8], frame[9], frame[10], frame[11], frame[12], frame[13], frame[14], frame[15]);
    int size = get_size(frame[TYPE]);
    uint8_t frame2[(size-1)*8];
    ESP_LOGI(MESH_TAG, "got size %d", (size-1)*8);
    for (int i = 0; i < size-1; i++) {
	for (int j = 0; j < 8; j++) {
	    frame2[(i*8)+j] = (frame[i] & (1 << j)) >> j;
	}
    }
    //ESP_LOGI(MESH_TAG, "Frame calculated = %d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d", frame2[0], frame2[1], frame2[2], frame2[3], frame2[4], frame2[5], frame2[6], frame2[7], frame2[8], frame2[9], frame2[10], frame2[11], frame2[12], frame2[13], frame2[14], frame2[15], frame2[16], frame2[17], frame2[18], frame2[19], frame2[20], frame2[21], frame2[22], frame2[23], frame2[24], frame2[25], frame2[26], frame2[27], frame2[28], frame2[29], frame2[30], frame2[31], frame2[32], frame2[33], frame2[34], frame2[35], frame2[36], frame2[37], frame2[38], frame2[39]);
    frame_crc_computer(frame2, (size-1)*8, crc_table);
    int crc = crc_table[0] << 6 | crc_table[1] << 5 | crc_table[2] << 4 | crc_table[3] << 3 | crc_table[4] << 2 | crc_table[5] << 1 | crc_table[6];
    frame[size-1] = crc;
}

/**
 * @brief Get the type of a message from the data buffer 
 */
int type_mesg(uint8_t * msg){
    return (int) msg[TYPE];
}

/**
 * @brief Copy the mac adress from a buffer to another 
 */
void copy_mac(uint8_t * from, uint8_t * to){
    for(int k=0; k < 6; k++){
	to[k] = from[k];
    }
}

/**
 * @brief Retrieve the mac adress from the data buffer */
void get_mac(uint8_t * msg, uint8_t * mac){
    int start = -2;
    switch(type_mesg(msg)){
    case BEACON :
    case B_ACK:
    case INSTALL :
	start = DATA;
	break;
    case COLOR_E:
	start = DATA + 5;
	break;
    case COLOR:
    case AMA :
    case SLEEP:
	start = -1;
	break;
    case ERROR:
	start = DATA;
	break;
    default: //all unknown messages are ignored
	start = -2;
     }
    if (start >= 0) {
	copy_mac(msg+start, mac);
	return ;
    }
    if (start == -2) {
	ESP_LOGE(MESH_TAG, "unknown msg type");
    }
}

/**
 * @brief Check if the mac addresses match
 */
int same_mac(uint8_t * mac1, uint8_t * mac2) {
    int i = 0;
    while (i<6 && mac1[i] == mac2[i]) {
	i++;
    }
    return i == 6;
}

/**
 * @brief Return the size of the data buffer depending on the message type
 */
int get_size(uint8_t type) {
    if (type == COLOR) {
	return 3 * route_table_size + 5;
    } else {
	return FRAME_SIZE;
    }
}

/**
 *   @brief Update the route table :
 * - add a mac address to the route table if the position is not used;
 * - swap two element if the mac address is already present and the position is used;
 * - replace the used position by the new mac address otherwise;
 */
void add_route_table(uint8_t * mac, int pos){
    if (pos == route_table_size) {
	copy_mac(mac, route_table[pos].card.addr);
	route_table[pos].state = true;
	route_table_size++;
    } else {
	int i = 0;
	while (! same_mac(mac, route_table[i].card.addr) && i < route_table_size) {
    i++;
	} if (i == route_table_size) { // Remplacement sans substitution
	    ESP_LOGW(MESH_TAG, "MAC not in route_table, replaced old MAC value by new");
	    copy_mac(mac, route_table[pos].card.addr);
	    route_table[pos].state = true;
	    return;
	}
	copy_mac(route_table[pos].card.addr, route_table[i].card.addr);
	copy_mac(mac, route_table[pos].card.addr);
    }
    for (int j = 0; j < route_table_size; j++) {
	ESP_LOGW(MESH_TAG, "Addr %d : "MACSTR"", j, MAC2STR(route_table[j].card.addr));
    }
}

/**
 *@brief Write a number of bytes from the data buffer into the reception pipe, and update the writable size of the pipe
*/
void write_rxbuffer(uint8_t * data, uint16_t size){
 loop:
    while (rxbuf_free_size < size );
    pthread_mutex_lock(&rxbuf_read);
    if (rxbuf_free_size < size ){
	pthread_mutex_unlock(&rxbuf_read);
	goto loop;
    }
    rxbuf_free_size = rxbuf_free_size - size;
    pthread_mutex_lock(&rxbuf_write);
    int head = rxbuf_head;
    rxbuf_head = (rxbuf_head + size) % RXB_SIZE;
    //ESP_LOGI(MESH_TAG, "rxbuf_free_size = %d", rxbuf_free_size);
    pthread_mutex_unlock(&rxbuf_write);
    for(int i = 0; i < size; i++){
	reception_buffer[(head + i) % RXB_SIZE] = data[i];
    }
    pthread_mutex_unlock(&rxbuf_read);
}

/**
 *@brief Write a number of bytes from the data buffer into the transmission pipe, and update the writable size of the pipe
 */
int write_txbuffer(uint8_t * data, uint16_t size){
  looptx:
   while (txbuf_free_size < size );
   pthread_mutex_lock(&txbuf_read);
   if (txbuf_free_size < size ){
     pthread_mutex_unlock(&txbuf_read);
     goto looptx;
   }
   txbuf_free_size = txbuf_free_size - size;
   pthread_mutex_lock(&txbuf_write);
   int head = txbuf_head;
   txbuf_head = (txbuf_head + size) % TXB_SIZE;
   pthread_mutex_unlock(&txbuf_write);
   for(int i = 0; i < size; i++){
       transmission_buffer[(head + i) % TXB_SIZE] = data[i];
   }
   pthread_mutex_unlock(&txbuf_read);
   return head;
}

/**
 *@brief Read the data on the reception pipe, and write it in the data buffer. Update the writable size of the pipe
 */
void read_rxbuffer(uint8_t * data) {
    pthread_mutex_lock(&rxbuf_read);
    if (rxbuf_free_size != RXB_SIZE) {
	//ESP_LOGI(MESH_TAG, "Asking type of trame at %d - %d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d", rxbuf_tail, reception_buffer[(rxbuf_tail+0)%RXB_SIZE], reception_buffer[(rxbuf_tail+1)%RXB_SIZE], reception_buffer[(rxbuf_tail+2)%RXB_SIZE], reception_buffer[(rxbuf_tail+3)%RXB_SIZE], reception_buffer[(rxbuf_tail+4)%RXB_SIZE], reception_buffer[(rxbuf_tail+5)%RXB_SIZE], reception_buffer[(rxbuf_tail+6)%RXB_SIZE], reception_buffer[(rxbuf_tail+7)%RXB_SIZE], reception_buffer[(rxbuf_tail+8)%RXB_SIZE], reception_buffer[(rxbuf_tail+9)%RXB_SIZE], reception_buffer[(rxbuf_tail+10)%RXB_SIZE]);
	int type = get_size(reception_buffer[(rxbuf_tail+TYPE) % RXB_SIZE]);	
	//ESP_LOGI(MESH_TAG, "Read message of type %d and size %d at %d- %d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d", reception_buffer[(rxbuf_tail+TYPE) % RXB_SIZE], type, rxbuf_tail, reception_buffer[(rxbuf_tail+0)%RXB_SIZE], reception_buffer[(rxbuf_tail+1)%RXB_SIZE], reception_buffer[(rxbuf_tail+2)%RXB_SIZE], reception_buffer[(rxbuf_tail+3)%RXB_SIZE], reception_buffer[(rxbuf_tail+4)%RXB_SIZE], reception_buffer[(rxbuf_tail+5)%RXB_SIZE], reception_buffer[(rxbuf_tail+6)%RXB_SIZE], reception_buffer[(rxbuf_tail+7)%RXB_SIZE], reception_buffer[(rxbuf_tail+8)%RXB_SIZE], reception_buffer[(rxbuf_tail+9)%RXB_SIZE], reception_buffer[(rxbuf_tail+10)%RXB_SIZE]);
	for (int i = 0; i < type; i++) {
	    data[i] = reception_buffer[(rxbuf_tail + i) % RXB_SIZE];
	}
	rxbuf_tail = (rxbuf_tail + type) % RXB_SIZE;
	rxbuf_free_size = rxbuf_free_size + type;
    } else {
	//ESP_LOGI(MESH_TAG, "nothing to read");
	pthread_mutex_unlock(&rxbuf_read);
	data[TYPE] = -2;
	return;
    }
    pthread_mutex_unlock(&rxbuf_read);
}

/*Copy size bytes from buffer b to buffer a*/
void  copy_buffer(uint8_t * a, uint8_t * b, int size) {
    for (int i = 0; i < size; i++) {
	a[i] = b[i];
    }
}

/**
 *@brief Debug function : write in monitor mode the colours that should be displayed by the light leds.
 */
void display_color(uint8_t buf[FRAME_SIZE]) {
    uint8_t color[3];
    copy_buffer(color, buf+DATA+2, 3);
    ESP_LOGI(MESH_TAG, "Diplay color triplet : (%d, %d, %d)", color[0], color[1], color[2]);
}

/**
 * @brief Main reception function for the mesh side : always checks if a message is available for this card, and writes it in the reception pipe. 
 *
 * @attention This is a Task that is always running. 
 *
 * @attention The Watchdog is resetted automatically as esp_mesh_recv is an blocking call while no message have been received.
 */
void mesh_reception(void * arg) {
    esp_err_t err;
    mesh_addr_t from;
    mesh_data_t data;
    int flag = 0;
    data.data = rx_buf;
    data.size = RX_SIZE;


    while(is_running) {
	err = esp_mesh_recv(&from, &data, portMAX_DELAY, &flag, NULL, 0);
	if (err != ESP_OK || !data.size) {
	    ESP_LOGE(MESH_TAG, "err:0x%x, size:%d", err, data.size);
	    continue;
	}
	if (data.data[VERSION] != SOFT_VERSION) {
	    ESP_LOGE(MESH_TAG, "Software versions not matching with Mesh");
	    continue;
	} if (!(check_crc(data.data))) {
	    ESP_LOGE(MESH_TAG, "Invalid CRC from Mesh");
	    continue;
	}
	write_rxbuffer(data.data, data.size);
	/*if (is_asleep) {
	    vTaskDelay((TIME_SLEEP * 1000) / portTICK_PERIOD_MS);//Sleep induce delay of 5 s for everything
	    }*/
    }
			
    vTaskDelete(NULL);
}

/**
 * @brief Main reception function for the server side, only used by the root card. It checks if a message is available, and writes it in the repection pipe. 
 *
 * @attention This is a Task that is always running. 
 *
 * @attention The Watchdog is resetted automatically as esp_mesh_recv is an blocking call while no message have been received.
 */
void server_reception(void * arg) {
    uint8_t buf[1500];
    int len;

    while(is_running) {
	len = recv(sock_fd, &buf, 1500, MSG_OOB);
	if (len == -1) {
	    ESP_LOGE(MESH_TAG, "Communication Socket error");
	    continue;
	}
	ESP_LOGI(MESH_TAG, "Message received from server of len %d = %d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d", len, buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7], buf[8], buf[9], buf[10]);
	if (buf[VERSION] != SOFT_VERSION) {
	    ESP_LOGE(MESH_TAG, "Software version not matching with server");
	    continue;
	} if (!check_crc(buf)) {
	    ESP_LOGE(MESH_TAG, "Invalid CRC from server");
	    continue;
	}
	pthread_mutex_lock(&txbuf_write);
	write_rxbuffer(buf, len);
	pthread_mutex_unlock(&txbuf_write);
	/*if (is_asleep) {
	    vTaskDelay((TIME_SLEEP * 1000) / portTICK_PERIOD_MS);//Sleep induce delay of 5 s for everything
	    }*/
    }
			
    vTaskDelete(NULL);
}

/**
 *@brief Function that sends a message to a specific card or broadcast it.
 * - The message is read from the mesh transmission pipe using the adress given in argument.
 * - Then, depending on the type of the message, it will either be sent to a specific card, or to the whole mesh.
 * - This Task is created when the message must be sent, and destroyed afterwards.
 */
void mesh_emission(void * arg) {    
    int err;
    mesh_data_t data;
    uint8_t mesg[FRAME_SIZE];
    
    pthread_mutex_lock(&txbuf_read);
    copy_buffer(mesg, (uint8_t *) arg, FRAME_SIZE);
    txbuf_free_size = txbuf_free_size + FRAME_SIZE;
    pthread_mutex_unlock(&txbuf_read);

    ESP_LOGI(MESH_TAG, "Message to mesh = %d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d", mesg[0], mesg[1], mesg[2], mesg[3], mesg[4], mesg[5], mesg[6], mesg[7], mesg[8], mesg[9], mesg[10], mesg[11], mesg[12], mesg[13], mesg[14], mesg[15]);

    ESP_LOGI(MESH_TAG, "calculating CRC...");
    get_crc(mesg);
    ESP_LOGI(MESH_TAG, "CRC calculated.");
    data.data = mesg;
    data.size = FRAME_SIZE;

    ESP_LOGI(MESH_TAG, "Message to mesh = %d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d", mesg[0], mesg[1], mesg[2], mesg[3], mesg[4], mesg[5], mesg[6], mesg[7], mesg[8], mesg[9], mesg[10], mesg[11], mesg[12], mesg[13], mesg[14], mesg[15]);
    
    switch(type_mesg(mesg)) {
    case BEACON: //Send a beacon to the root.
        err = esp_mesh_send(NULL, &data, MESH_DATA_P2P, NULL, 0);
	if (err != 0) {
	    perror("Beacon failed");
	    ESP_LOGE(MESH_TAG, "Couldn't send BEACON to root");
	    //state = ERROR_S;
	}
	break;
    case COLOR_E: //Send a Color frame (one triplet) to a specific card. The mac is in the frame.
	{
	    mesh_addr_t to;
	    get_mac(mesg, to.addr);
	    err = esp_mesh_send(&to, &data, MESH_DATA_P2P, NULL, 0);
	    if (err != 0) {
		perror("Color fail");
		ESP_LOGE(MESH_TAG, "Couldn't send COLOR to "MACSTR"", MAC2STR(to.addr));
		//state = ERROR_S;
	    }
	}
	break;
    case B_ACK: // Send a beacon acknowledgement to a specific card. The mac is in the frame.
	{
	    mesh_addr_t to;
	    get_mac(mesg, to.addr);
	    err = esp_mesh_send(&to, &data, MESH_DATA_P2P, NULL, 0);
	    if (err != 0) {
		perror("B_ACK fail");
		ESP_LOGE(MESH_TAG, "Couldn't send B_ACK to "MACSTR" - %s", MAC2STR(to.addr), esp_err_to_name(err));
	    }
	}
	break;
	/*case SLEEP_R : // Put all cards in the mesh in sleep mode. To do this, the messages are sent to the cards with the less cards in their subnet, and then to those with greater subnet, to ensure that there is always a card to relay the messages
        data.data[TYPE] = SLEEP;
        for (int i = 0; i < route_table_size; i++) {
	    esp_mesh_get_subnet_nodes_num(&route_table[i].card, &num[i]);
	}
	int count = 0;
	while (count < route_table_size) {
	    for (int i = 0; i < route_table_size; i++) {
		if (num[i] == count) {
		    esp_mesh_send(&route_table[i].card, &data, MESH_DATA_P2P, NULL, 0);
		}
	    }
	    count++;
	}
	break;*/
    default : //Broadcast the message to all the mesh. This include AMA, SLEEP and INSTALL frames.
	for (int i = 0; i < route_table_size; i++) {
	    if (!same_mac(route_table[i].card.addr, my_mac)) {
		err = esp_mesh_send(&route_table[i].card, &data, MESH_DATA_P2P, NULL, 0);
		if (err != 0) {
		    perror("message fail");
		    ESP_LOGE(MESH_TAG, "Couldn't send message %d to "MACSTR"", type_mesg(mesg), MAC2STR(route_table[i].card.addr));
		}
	    }
	}
    }
    vTaskDelete(NULL);
}

/**
 *@brief Function that sends a message to the server.
 * - The message is read from the server transmission pipe, using the address given in arguments.
 * - It is then wrtitten in the socket binding the root card and the server.
 * - This task is created when a message need to be sent, and destroyed afterwards.
 *
 * @attention Only the root card can use this
 */
void server_emission(void * arg) {
    uint8_t mesg[FRAME_SIZE];
    
    pthread_mutex_lock(&txbuf_read);
    copy_buffer(mesg, (uint8_t *) arg, FRAME_SIZE);
    txbuf_free_size = txbuf_free_size + FRAME_SIZE;
    pthread_mutex_unlock(&txbuf_read);

    get_crc(mesg);
    
    int err = write(sock_fd, mesg, FRAME_SIZE);
    if (err == FRAME_SIZE) {
	ESP_LOGI(MESH_TAG, "Message %d send to serveur", type_mesg(mesg));
    }
    else {
	perror("mesg to server fail");
	ESP_LOGE(MESH_TAG, "Error on send to serveur, message %d - sent %d bytes", type_mesg(mesg), err);
    }
    vTaskDelete(NULL);
}

/**
 * @brief Opens the socket between the root card and the server, and initialize the connection. 
 */
void connect_to_server() {
    
    sock_fd = socket(AF_INET, SOCK_STREAM, 0); // Ouverture du socket avec le serveur.
    if (sock_fd == -1) {
	ESP_LOGE(MESH_TAG, "Socket_fail");
	return;
    }
    int ret = connect(sock_fd, (struct sockaddr *)&tcpServerAddr, sizeof(struct sockaddr));
    if (ret < 0 && errno != 119) {
	perror("Erreur socket : ");
	ESP_LOGE(MESH_TAG, "Connection fail");
	close(sock_fd);
    }else {
	ESP_LOGW(MESH_TAG, "Connected to Server");
	xTaskCreate(server_reception, "SERRX", 6000, NULL, 5, NULL);
	is_server_connected = true;
    }
}

/**
 * @brief Main function of the INIT state.
 * In this state, root card will send BEACON to server, and wait for INSTALL to go into CONF state.
 * Node cards will send BEACON to the root, and wait for B_ACK to go into ADDR state
 */
void state_init() {
    uint8_t buf_recv[FRAME_SIZE];
    uint8_t buf_send[FRAME_SIZE];

    int type;
    
    if (esp_mesh_is_root()) {
	if (!is_server_connected) {
	    connect_to_server();
	    return;//Root can't progress if not connected to the server
	}
    }

    /* Check if it has received an acknowledgement */
    read_rxbuffer(buf_recv);
    type = type_mesg(buf_recv);

    if (type == B_ACK) {
	if (!esp_mesh_is_root()) { //dummy test
	    state = ADDR;
	    ESP_LOGE(MESH_TAG, "Went into ADDR state");
	    return;
	}
    } else if (type == INSTALL) {
	if (esp_mesh_is_root()) { //dummy test
	    uint8_t mac[6];
	    get_mac(buf_recv, mac);
	    add_route_table(mac, 0);
	    state = CONF;
	    ESP_LOGE(MESH_TAG, "Went into CONF state");
	    return;
	} 
    }
        
    /*Creation of BEACON frame */
    buf_send[VERSION] = SOFT_VERSION;
    buf_send[TYPE] = BEACON;
    copy_mac(my_mac, buf_send+DATA);
    //Rajout version, checksum, etc...
    int head = write_txbuffer(buf_send, FRAME_SIZE);
    if (esp_mesh_is_root()) {
	xTaskCreate(server_emission, "SERTX", 3072, transmission_buffer+head, 5, NULL);
    }
    else {
	xTaskCreate(mesh_emission, "ESPTX", 3072, transmission_buffer+head, 5, NULL);
    }
}

/**
 * @brief Main function of the CONF state, only used by the root card.
 * In this state, it transfers BEACON frame from the mesh to the server, and wait for INSTALL frame to send a B_ACK to the concerned card.
 * If it receives an AMA_init frame, it goes into the ADDR state
 */
void state_conf() {
    /*var locales*/
    uint8_t buf_recv[FRAME_SIZE];
    uint8_t buf_send[FRAME_SIZE];
    
    int type = 0;

    read_rxbuffer(buf_recv);
    type = type_mesg(buf_recv);

    if (type == BEACON) {
	ESP_LOGI(MESH_TAG, "Received a beacon, transfered");
	copy_buffer(buf_send, buf_recv, FRAME_SIZE);
	int head = write_txbuffer(buf_send, FRAME_SIZE);
	xTaskCreate(server_emission, "SERTX", 3072, transmission_buffer+head, 5, NULL);
    }
    else if (type == INSTALL) {
	uint8_t mac[6];
	get_mac(buf_recv, mac);
	add_route_table(mac, buf_recv[DATA+6]);
	ESP_LOGI(MESH_TAG, "Got install for MAC "MACSTR" at pos %d, acquitted it", MAC2STR(mac), buf_recv[DATA+6]);
	buf_send[VERSION] = SOFT_VERSION;
	buf_send[TYPE] = B_ACK;
	copy_buffer(buf_send+DATA, buf_recv+DATA, 6);
	//Checksum, version, etc...
	int head = write_txbuffer(buf_send, FRAME_SIZE);
	xTaskCreate(mesh_emission, "ESPTX", 3072, transmission_buffer+head, 5, NULL);
    }
    else if (type == AMA) {
	if (buf_recv[DATA] == AMA_INIT) {//HC
	    state = ADDR;
	    ESP_LOGE(MESH_TAG, "Went into ADDR state");
	}
    }
}

/**
 * @brief Main function for the ADDR state.
 * This state is used during the Assisted Manual Addressing.
 * In this state, the root card will wait for INSTALL frame from the server, and broadcast them to the mesh network. On reception, every card will update its route table.
 * If the root receives a COLOR frame, it breaks it into COLOR_E frame, and send them to the proper card using its route table.
 * On reception on AMA_color frame, the Addressing is over, and all cards go into COLOR state
 */
void state_addr() {
    int type = 0;
    uint8_t buf_recv[CONFIG_MESH_ROUTE_TABLE_SIZE * 3 + 5];//Hard-code?
    uint8_t buf_send[FRAME_SIZE];

    //ESP_LOGI(MESH_TAG, "entered addr");
    read_rxbuffer(buf_recv);
    type = type_mesg(buf_recv);
    //ESP_LOGI(MESH_TAG, "read buffer, type = %d", type);

    if (type == INSTALL) { //Mixte
	uint8_t mac[6];
	get_mac(buf_recv, mac);
	add_route_table(mac, buf_recv[DATA+6]);//hardcode
	if (esp_mesh_is_root()) {
	    copy_buffer(buf_send, buf_recv, FRAME_SIZE);
	    int head = write_txbuffer(buf_send, FRAME_SIZE);
	    xTaskCreate(mesh_emission, "ESPTX", 3072, transmission_buffer+head, 5, NULL);
	}
    }
    else if (type == COLOR) { // Root only
	ESP_LOGI(MESH_TAG, "Message = %d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d", buf_recv[0], buf_recv[1], buf_recv[2], buf_recv[3], buf_recv[4], buf_recv[5], buf_recv[6], buf_recv[7], buf_recv[8], buf_recv[9], buf_recv[10]);
	uint16_t sequ = buf_recv[DATA] << 8 | buf_recv[DATA+1];
	ESP_LOGI(MESH_TAG, "comparing %d and %d", sequ, current_sequence);
	if (sequ > current_sequence || current_sequence - sequ > SEQU_SEUIL) {
	    current_sequence = sequ;
	    buf_send[VERSION] = SOFT_VERSION;
	    buf_send[TYPE] = COLOR_E;
	    for (int i = 0; i < route_table_size; i++) {
		copy_buffer(buf_send+DATA, buf_recv+DATA, 2);
		copy_buffer(buf_send+DATA+2, buf_recv+DATA+2+i*3, 3); // copy color triplet
		copy_buffer(buf_send+DATA+5, route_table[i].card.addr, 6); // copy mac adress
		//Checksum
		if (!same_mac(route_table[i].card.addr, my_mac)) {
		    int head = write_txbuffer(buf_send, FRAME_SIZE);
		    xTaskCreate(mesh_emission, "ESPTX", 3072, transmission_buffer+head, 5, NULL);
		} else {
		    display_color(buf_send);
		}
	    }
	}
    }
    else if (type == COLOR_E) {//Mixte
	ESP_LOGI(MESH_TAG, "Message = %d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d", buf_recv[0], buf_recv[1], buf_recv[2], buf_recv[3], buf_recv[4], buf_recv[5], buf_recv[6], buf_recv[7], buf_recv[8], buf_recv[9], buf_recv[10], buf_recv[11], buf_recv[12], buf_recv[13], buf_recv[14], buf_recv[15]);
	uint16_t sequ = buf_recv[DATA]  << 8 | buf_recv[DATA+1];
	ESP_LOGI(MESH_TAG, "comparing %d and %d", sequ, current_sequence);
	if (sequ > current_sequence || current_sequence - sequ > SEQU_SEUIL) {
	    current_sequence = sequ;
	    display_color(buf_recv);
	}
    }
    else if (type == AMA) { //Mixte
	if (buf_recv[DATA] == AMA_COLOR) {//HC
	    if (esp_mesh_is_root()) {
		copy_buffer(buf_send, buf_recv, FRAME_SIZE);
		int head = write_txbuffer(buf_send, FRAME_SIZE);
		xTaskCreate(mesh_emission, "ESPTX", 3072, transmission_buffer + head, 5, NULL);
	    }
	    state = COLOR;
	    ESP_LOGE(MESH_TAG, "Went into COLOR state");
	}
    }
}

/**
 * @brief Main function for the COLOR state.
 * This is the main state of the card.
 * If the root receives a COLOR frame, it breaks it into COLOR_E frame, and send them to the proper card using its route table.
 * On reception of COLOR_E frame, the card will dislay the color indicated.
 * The root card can switch at any time into ERROR state if an error occured within the mesh network or in the server.
 * On reception of SLEEP frame from the server, the root will put the mesh network asleep
 */
void state_color() {
    int type = 0;
    uint8_t buf_recv[1 + CONFIG_MESH_ROUTE_TABLE_SIZE * 3 + 4];
    uint8_t buf_send[FRAME_SIZE];

    read_rxbuffer(buf_recv);
    type = type_mesg(buf_recv);

    if (type == COLOR) { // Root only
	ESP_LOGI(MESH_TAG, "Message = %d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d", buf_recv[0], buf_recv[1], buf_recv[2], buf_recv[3], buf_recv[4], buf_recv[5], buf_recv[6], buf_recv[7], buf_recv[8], buf_recv[9], buf_recv[10]);
	uint16_t sequ = buf_recv[DATA] << 8 | buf_recv[DATA+1];
	if (sequ > current_sequence || current_sequence - sequ > SEQU_SEUIL) {
	    current_sequence = sequ;
	    buf_send[VERSION] = SOFT_VERSION;
	    buf_send[TYPE] = COLOR_E;
	    for (int i = 0; i < route_table_size; i++) {
		copy_buffer(buf_send+DATA, buf_recv+DATA, 2);
		copy_buffer(buf_send+DATA+2, buf_recv+DATA+2+i*3, 3); // copy color triplet
		copy_buffer(buf_send+DATA+5, route_table[i].card.addr, 6); // copy mac adresscopy_buffer(buf_send+DATA, buf_recv+DATA+2+i*3, 3); // copy color triplet
		//Checksum
		if (!same_mac(route_table[i].card.addr, my_mac)) {
		    int head = write_txbuffer(buf_send, FRAME_SIZE);
		    xTaskCreate(mesh_emission, "ESPTX", 3072, transmission_buffer+head, 5, NULL);
		} else {
		    display_color(buf_send);
		}
	    }
	}
    }
    else if (type == COLOR_E) {//Mixte
	ESP_LOGI(MESH_TAG, "Message = %d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d-%d", buf_recv[0], buf_recv[1], buf_recv[2], buf_recv[3], buf_recv[4], buf_recv[5], buf_recv[6], buf_recv[7], buf_recv[8], buf_recv[9], buf_recv[10], buf_recv[11], buf_recv[12], buf_recv[13], buf_recv[14], buf_recv[15]);
	uint16_t sequ = buf_recv[DATA] << 8 | buf_recv[DATA+1];
	if (sequ > current_sequence || current_sequence - sequ > SEQU_SEUIL) {
	    current_sequence = sequ;
	    display_color(buf_recv);
	}
    }
    else if (type == BEACON) {//Root only
	state = ERROR_S;
    }
    else if (type == SLEEP) {
	if (buf_recv[DATA] == SLEEP_SERVER) {
	    ESP_LOGE(MESH_TAG, "Card received Server variant of Sleep");
	
	    if (esp_mesh_is_root()) {
		copy_buffer(buf_send, buf_recv, FRAME_SIZE);
		buf_send[DATA] = SLEEP_MESH;
		int head = write_txbuffer(buf_send, FRAME_SIZE);
		xTaskCreate(mesh_emission, "ESPTX", 3072, transmission_buffer+head, 5, NULL);
	    }
	} else if (buf_recv[DATA] == SLEEP_MESH) {
	    state = SLEEP_S;
	}
    }
}

/**
 * @brief Main function for the SLEEP state
 * In this state, cards don't do much. They simply wait for a WAKEUP frame from the server.
 * (to be implemented/corrected)
 */
void state_sleep() {
    int type = 0;
    uint8_t buf_recv[1 + CONFIG_MESH_ROUTE_TABLE_SIZE * 3 + 4];//Hard-code?
    uint8_t buf_send[FRAME_SIZE];
    
    if (!is_asleep) {
	ESP_LOGE(MESH_TAG, "entered sleep");
	is_asleep = true;
    }
    read_rxbuffer(buf_recv);
    type = type_mesg(buf_recv);

    if (type == SLEEP) {
	if (buf_recv[DATA] == WAKE_UP) {
	    if (esp_mesh_is_root()) {
		copy_buffer(buf_send, buf_recv, FRAME_SIZE);
		int head = write_txbuffer(buf_send, FRAME_SIZE);
		xTaskCreate(mesh_emission, "ESPTX", 3072, transmission_buffer + head, 5, NULL);
	    }
	    ESP_LOGE(MESH_TAG, "Woke up : return to INIT state to check if everyone is here");
	    is_asleep = false;
	    state = INIT;
	}
    }
}

/**
 * @brief To be implemented
 */
void state_error() {
    ESP_LOGE(MESH_TAG, "An error occured during card functionnement");
    state = COLOR;
}

/**
 * @brief Main function
 * This decides which function to call depending on the state of the card, and regulate the watchdogs of the state machine.
 */
void esp_mesh_state_machine(void * arg) {
    esp_err_t err;
    mesh_addr_t from;
    int flag = 0;
    int send_count = 0;
    int recv_count = 0;
    is_running = true;
    vTaskDelay(5000 / portTICK_PERIOD_MS);

    while(is_running) {;
	switch(state) {
	case INIT:
	    state_init();
	    vTaskDelay(5000 / portTICK_PERIOD_MS);
	    break;
	case CONF :
	    state_conf();
	    vTaskDelay(100 / portTICK_PERIOD_MS);
	    break;
	case ADDR :
	    state_addr();
	    vTaskDelay(100 / portTICK_PERIOD_MS);
	    break;
	case COLOR :
	    state_color();
	    vTaskDelay(10 / portTICK_PERIOD_MS);
	    break;
	case ERROR_S :
	    state_error();
	    break;
	case SLEEP_S :
	    state_sleep();
	    if (is_asleep) {
		vTaskDelay((TIME_SLEEP * 1000) / portTICK_PERIOD_MS);//Sleep induce delay of 5 s for everything
	    }
	    break;
	default :
	    ESP_LOGE(MESH_TAG, "ESP entered unknown state %d", state);
	}
    }
    vTaskDelete(NULL);
}

/**
 * @brief Initialise the Task of the card
 */
esp_err_t esp_mesh_comm_p2p_start(void)
{
    static bool is_comm_p2p_started = false;
    if (!is_comm_p2p_started) {
        is_comm_p2p_started = true;
	xTaskCreate(mesh_reception, "ESPRX", 3072, NULL, 5, NULL);
	xTaskCreate(esp_mesh_state_machine, "STMC", 3072, NULL, 5, NULL);
    }
    return ESP_OK;
}

/**
 * @brief Debug logs on event
 */
void mesh_event_handler(mesh_event_t event)
{
    mesh_addr_t id = {0,};
    static uint8_t last_layer = 0;
    ESP_LOGD(MESH_TAG, "esp_event_handler:%d", event.id);

    switch (event.id) {
    case MESH_EVENT_STARTED:
        esp_mesh_get_id(&id);
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_STARTED>ID:"MACSTR"", MAC2STR(id.addr));
        is_mesh_connected = false;
        mesh_layer = esp_mesh_get_layer();
        break;
    case MESH_EVENT_STOPPED:
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_STOPPED>");
        is_mesh_connected = false;
        mesh_layer = esp_mesh_get_layer();
        break;
    case MESH_EVENT_CHILD_CONNECTED:
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_CHILD_CONNECTED>aid:%d, "MACSTR"",
                 event.info.child_connected.aid,
                 MAC2STR(event.info.child_connected.mac));
        break;
    case MESH_EVENT_CHILD_DISCONNECTED:
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_CHILD_DISCONNECTED>aid:%d, "MACSTR"",
                 event.info.child_disconnected.aid,
                 MAC2STR(event.info.child_disconnected.mac));
        break;
    case MESH_EVENT_ROUTING_TABLE_ADD:
        ESP_LOGW(MESH_TAG, "<MESH_EVENT_ROUTING_TABLE_ADD>add %d, new:%d",
                 event.info.routing_table.rt_size_change,
                 event.info.routing_table.rt_size_new);
        break;
    case MESH_EVENT_ROUTING_TABLE_REMOVE:
        ESP_LOGW(MESH_TAG, "<MESH_EVENT_ROUTING_TABLE_REMOVE>remove %d, new:%d",
                 event.info.routing_table.rt_size_change,
                 event.info.routing_table.rt_size_new);
        break;
    case MESH_EVENT_NO_PARENT_FOUND:
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_NO_PARENT_FOUND>scan times:%d",
                 event.info.no_parent.scan_times);
        /* TODO handler for the failure */
        break;
    case MESH_EVENT_PARENT_CONNECTED:
        esp_mesh_get_id(&id);
        mesh_layer = event.info.connected.self_layer;
        memcpy(&mesh_parent_addr.addr, event.info.connected.connected.bssid, 6);
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_PARENT_CONNECTED>layer:%d-->%d, parent:"MACSTR"%s, ID:"MACSTR"",
                 last_layer, mesh_layer, MAC2STR(mesh_parent_addr.addr),
                 esp_mesh_is_root() ? "<ROOT>" :
                 (mesh_layer == 2) ? "<layer2>" : "", MAC2STR(id.addr));
        last_layer = mesh_layer;
        is_mesh_connected = true;
        if (esp_mesh_is_root()) {
            tcpip_adapter_dhcpc_start(TCPIP_ADAPTER_IF_STA);
        }
        esp_mesh_comm_p2p_start();
        break;
    case MESH_EVENT_PARENT_DISCONNECTED:
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_PARENT_DISCONNECTED>reason:%d",
                 event.info.disconnected.reason);
        is_mesh_connected = false;
        mesh_layer = esp_mesh_get_layer();
        break;
    case MESH_EVENT_LAYER_CHANGE:
        mesh_layer = event.info.layer_change.new_layer;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_LAYER_CHANGE>layer:%d-->%d%s",
                 last_layer, mesh_layer,
                 esp_mesh_is_root() ? "<ROOT>" :
                 (mesh_layer == 2) ? "<layer2>" : "");
        last_layer = mesh_layer;
        break;
    case MESH_EVENT_ROOT_ADDRESS:
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROOT_ADDRESS>root address:"MACSTR"",
                 MAC2STR(event.info.root_addr.addr));
        break;
    case MESH_EVENT_ROOT_GOT_IP:
        /* root starts to connect to server */
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_ROOT_GOT_IP>sta ip: " IPSTR ", mask: " IPSTR ", gw: " IPSTR,
                 IP2STR(&event.info.got_ip.ip_info.ip),
                 IP2STR(&event.info.got_ip.ip_info.netmask),
                 IP2STR(&event.info.got_ip.ip_info.gw));
        break;
    case MESH_EVENT_ROOT_LOST_IP:
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROOT_LOST_IP>");
        break;
    case MESH_EVENT_VOTE_STARTED:
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_VOTE_STARTED>attempts:%d, reason:%d, rc_addr:"MACSTR"",
                 event.info.vote_started.attempts,
                 event.info.vote_started.reason,
                 MAC2STR(event.info.vote_started.rc_addr.addr));
        break;
    case MESH_EVENT_VOTE_STOPPED:
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_VOTE_STOPPED>");
        break;
    case MESH_EVENT_ROOT_SWITCH_REQ:
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_ROOT_SWITCH_REQ>reason:%d, rc_addr:"MACSTR"",
                 event.info.switch_req.reason,
                 MAC2STR( event.info.switch_req.rc_addr.addr));
        break;
    case MESH_EVENT_ROOT_SWITCH_ACK:
        /* new root */
        mesh_layer = esp_mesh_get_layer();
        esp_mesh_get_parent_bssid(&mesh_parent_addr);
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROOT_SWITCH_ACK>layer:%d, parent:"MACSTR"", mesh_layer, MAC2STR(mesh_parent_addr.addr));
        break;
    case MESH_EVENT_TODS_STATE:
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_TODS_REACHABLE>state:%d",
                 event.info.toDS_state);
        break;
    case MESH_EVENT_ROOT_FIXED:
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROOT_FIXED>%s",
                 event.info.root_fixed.is_fixed ? "fixed" : "not fixed");
        break;
    case MESH_EVENT_ROOT_ASKED_YIELD:
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_ROOT_ASKED_YIELD>"MACSTR", rssi:%d, capacity:%d",
                 MAC2STR(event.info.root_conflict.addr),
                 event.info.root_conflict.rssi,
                 event.info.root_conflict.capacity);
        break;
    case MESH_EVENT_CHANNEL_SWITCH:
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_CHANNEL_SWITCH>new channel:%d", event.info.channel_switch.channel);
        break;
    case MESH_EVENT_SCAN_DONE:
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_SCAN_DONE>number:%d",
                 event.info.scan_done.number);
        break;
    case MESH_EVENT_NETWORK_STATE:
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_NETWORK_STATE>is_rootless:%d",
                 event.info.network_state.is_rootless);
        break;
    case MESH_EVENT_STOP_RECONNECTION:
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_STOP_RECONNECTION>");
        break;
    case MESH_EVENT_FIND_NETWORK:
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_FIND_NETWORK>new channel:%d, router BSSID:"MACSTR"",
                 event.info.find_network.channel, MAC2STR(event.info.find_network.router_bssid));
        break;
    case MESH_EVENT_ROUTER_SWITCH:
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROUTER_SWITCH>new router:%s, channel:%d, "MACSTR"",
                 event.info.router_switch.ssid, event.info.router_switch.channel, MAC2STR(event.info.router_switch.bssid));
        break;
    default:
        ESP_LOGI(MESH_TAG, "unknown id:%d", event.id);
        break;
    }
}

/**
 * @brief Startup function : initialize the cards
 */
void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    /*  tcpip initialization */
    tcpip_adapter_init();
    /* for mesh
     * stop DHCP server on softAP interface by default
     * stop DHCP client on station interface by default
     * */
    ESP_ERROR_CHECK(tcpip_adapter_dhcps_stop(TCPIP_ADAPTER_IF_AP));
    ESP_ERROR_CHECK(tcpip_adapter_dhcpc_stop(TCPIP_ADAPTER_IF_STA));
#if 0
    /* static ip settings */
    tcpip_adapter_ip_info_t sta_ip;
    sta_ip.ip.addr = ipaddr_addr("192.168.1.102");
    sta_ip.gw.addr = ipaddr_addr("192.168.1.1");
    sta_ip.netmask.addr = ipaddr_addr("255.255.255.0");
    tcpip_adapter_set_ip_info(WIFI_IF_STA, &sta_ip);
#endif
    /*  wifi initialization */
    ESP_ERROR_CHECK(esp_event_loop_init(NULL, NULL));
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&config));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_start());
    /*  mesh initialization */
    ESP_ERROR_CHECK(esp_mesh_init());
    ESP_ERROR_CHECK(esp_mesh_set_max_layer(CONFIG_MESH_MAX_LAYER));
    ESP_ERROR_CHECK(esp_mesh_set_vote_percentage(1));
    ESP_ERROR_CHECK(esp_mesh_set_ap_assoc_expire(10));
#ifdef MESH_FIX_ROOT
    ESP_ERROR_CHECK(esp_mesh_fix_root(1));
#endif
    mesh_cfg_t cfg = MESH_INIT_CONFIG_DEFAULT();
    /* mesh ID */
    memcpy((uint8_t *) &cfg.mesh_id, MESH_ID, 6);
    /* mesh event callback */
    cfg.event_cb = &mesh_event_handler;
    /* router */
    cfg.channel = CONFIG_MESH_CHANNEL;
    cfg.router.ssid_len = strlen(CONFIG_MESH_ROUTER_SSID);
    memcpy((uint8_t *) &cfg.router.ssid, CONFIG_MESH_ROUTER_SSID, cfg.router.ssid_len);
    memcpy((uint8_t *) &cfg.router.password, CONFIG_MESH_ROUTER_PASSWD,
           strlen(CONFIG_MESH_ROUTER_PASSWD));
    /* mesh softAP */
    ESP_ERROR_CHECK(esp_mesh_set_ap_authmode(CONFIG_MESH_AP_AUTHMODE));
    cfg.mesh_ap.max_connection = CONFIG_MESH_AP_CONNECTIONS;
    memcpy((uint8_t *) &cfg.mesh_ap.password, CONFIG_MESH_AP_PASSWD,
           strlen(CONFIG_MESH_AP_PASSWD));
    ESP_ERROR_CHECK(esp_mesh_set_config(&cfg));
    /* Initialisation de l'adresse MAC*/
    esp_efuse_mac_get_default(my_mac);
    /* mesh start */
    ESP_ERROR_CHECK(esp_mesh_start());
    ESP_LOGI(MESH_TAG, "mesh starts successfully, heap:%d, %s\n",  esp_get_free_heap_size(),
             esp_mesh_is_root_fixed() ? "root fixed" : "root not fixed");
    /* Socket creation */
    memset(&tcpServerAddr, 0, sizeof(tcpServerAddr));
    tcpServerAddr.sin_family = AF_INET;
    tcpServerAddr.sin_addr.s_addr = inet_addr("10.42.0.1");
    tcpServerAddr.sin_len = sizeof(tcpServerAddr);
    tcpServerAddr.sin_port = htons(8080);
}
