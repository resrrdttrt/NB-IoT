#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "string.h"
#include "driver/gpio.h"
#include "freertos/timers.h"

#define RX_BUF_SIZE  1024     // Khai báo độ dài BUFFER
#define COMMAND_BUFFER_SIZE 2048

#define TXD_PIN (GPIO_NUM_17)   // Khai báo chân GPIO
#define RXD_PIN (GPIO_NUM_16)
#define PWRKEY 18

static const char *TX_TASK_TAG = "TX_TASK"; // Tạo TAG
static const char *RX_TASK_TAG = "RX_TASK";
static const char *SEMAPHORE_TAG = "SEMAPHORE_TASK";


#define QUEUE_LENGTH 30     // Khai báo Queue để chứa các câu lệnh
#define ITEM_SIZE sizeof(const char *)

#define TOKEN_ID "qBCrWi8RzcQpWYjnalQqLlxaEHW3vrql" //Khai báo MQTT ID
#define DEVICE_ID "9084a7ab-9597-4004-ad9b-11e4f0670f89"


const char *CHECK = "AT\r\n";   //Khởi tạo các câu lệnh cơ bản
const char *CENG = "AT+CENG?\r\n";
const char *ACT = "AT+CNACT=0,1\r\n";
const char *URL = "AT+SMCONF=\"URL\",mqtt.innoway.vn,1883\r\n";
const char *CLIENT_ID = "AT+SMCONF=\"CLIENTID\",BaiTapNBIoT\r\n";
const char *USERNAME = "AT+SMCONF=\"USERNAME\",BaiTapNBIoT\r\n";
const char *PASSWORD = "AT+SMCONF=\"PASSWORD\","TOKEN_ID"\r\n";
const char *GNSS_TAKE = "AT+SGNSCMD=1,0\r\n";
// const char *GNSS_START = "AT+CGNSPWR=1\r\n";
// const char *GNSS_GET = "AT+CGNSINF\r\n";
const char *CONNECT = "AT+SMCONN\r\n";
const char *PUBLISH = "AT+SMPUB=\"messages/"DEVICE_ID"/attributets\"";
const char *TURNOFF = "AT+CPOWD=1\r\n";


char pub_command[COMMAND_BUFFER_SIZE]; //Khởi tạo bộ nhớ để chứa các lệnh publish
char *pub_command_pointer;
char pub_payload[COMMAND_BUFFER_SIZE];
char *pub_payload_pointer;



SemaphoreHandle_t xSemaphoreOK = NULL, xSemaphoreError = NULL,xSemaphorePub = NULL;; //Khởi tạo Timer, hàng chờ, cờ hiệu
QueueHandle_t queue = NULL;
TimerHandle_t xTimer;

static uint8_t state_of_command = 1; // trạng thái phản hồi 0 là chưa trả lời, 1 là yêu cầu thành công, 2 là yêu cầu thất bại


#define MAX_TIMES_SEND 100 //Khai báo số lần gửi lại tối đa của mỗi lệnh
static uint8_t times_send = 0; //số lần gửi
#define MAX_WAITING_TIME 10000 //Khai báo thời gian chờ đợi phản hồi 
static uint32_t request_time = 0; // thời gian gửi lệnh AT đến module


static struct Ceng //cấu trúc của một CENG
{
    int cell;
    int earfcn;
    int pci;
    int rsrp;
    int rssi;
    int rsrq;
    int sinr;
    int tac;
    int cellid;
    int mcc;
    int mnc;
    int tx_power;
} ceng;



static struct GNSS_Data
{
    int status;
    char *date;
    float lat;
    float lon;
} gnss;

void init_uart(void) //khởi tạo uart
{
    const uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(UART_NUM_2, RX_BUF_SIZE * 2, 0, 0, NULL, 0);
    uart_param_config(UART_NUM_2, &uart_config);
    uart_set_pin(UART_NUM_2, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

void init_gpio(int GPIO_NUM) //khởi tạo GPIO
{
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = 1ULL << GPIO_NUM;
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;
    gpio_config(&io_conf);
}

void create_queue() { // tạo hàng chờ các câu lệnh để gửi lần lượt
    queue = xQueueCreate(QUEUE_LENGTH, ITEM_SIZE);
    xQueueSend(queue, &CHECK, portMAX_DELAY);
    xQueueSend(queue, &CENG, portMAX_DELAY);
    xQueueSend(queue, &GNSS_TAKE, portMAX_DELAY);
    xQueueSend(queue, &ACT, portMAX_DELAY);
    xQueueSend(queue, &URL, portMAX_DELAY);
    xQueueSend(queue, &CLIENT_ID, portMAX_DELAY);
    xQueueSend(queue, &USERNAME, portMAX_DELAY);
    xQueueSend(queue, &PASSWORD, portMAX_DELAY);
    // xQueueSend(queue, &GNSS_GET, portMAX_DELAY);
    xQueueSend(queue, &CONNECT, portMAX_DELAY);

}


void add_pub_command_to_queue(){ // Add các lệnh publish vào hàng chờ 
    if( queue != 0 )
    {
        size_t len1 = strlen(pub_payload_pointer);
        snprintf(pub_command,sizeof(pub_command),"%s,%u,1,1\r\n",PUBLISH,(unsigned int)len1);
        pub_command_pointer=pub_command;
        xQueueSendToBack(queue, &pub_command_pointer, portMAX_DELAY);
        xQueueSendToBack(queue, &pub_payload_pointer, portMAX_DELAY);
        xQueueSendToBack(queue, &TURNOFF, portMAX_DELAY);
    }
}

int sendData(const char *logName, const char *data) // hàm gửi dữ liệu đến module NB-IoT
{
    const int len = strlen(data);
    state_of_command = 0;
    request_time = esp_timer_get_time()/1000;
    times_send++;
    const int txBytes = uart_write_bytes(UART_NUM_2, data, len);
    ESP_LOGI(logName, "Sent: %s", data);
    return txBytes;
}


void turn_on_module(){ // hàm bật module NB-IoT
    gpio_set_level(PWRKEY, 0);
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    gpio_set_level(PWRKEY, 1);
    vTaskDelay(10000 / portTICK_PERIOD_MS);
}


static void send_task(void *arg){ // tác vụ gửi dữ liệu đến module NB-IoT
    for(;;){
        if( xSemaphoreTake( xSemaphoreOK, ( TickType_t ) 10 ) == pdTRUE ) //gửi tiếp câu lệnh tiếp theo nếu gửi thành công
        {   
            ESP_LOGI(SEMAPHORE_TAG,"SMPOK taken");
            if( queue != 0 )
            {
                const char *message;
                if( xQueueReceive( queue, &(message), ( TickType_t ) 10 ) ) 
                {
                    if( xQueuePeek( queue, &(message), ( TickType_t ) 10 ) )
                    {
                        if(times_send<MAX_TIMES_SEND){
                            sendData(TX_TASK_TAG, message);
                            times_send=1;
                            ESP_LOGI(SEMAPHORE_TAG,"Time send: %u",times_send);
                        }
                    }
                }
            }
        }
        else if( xSemaphoreTake( xSemaphoreError, ( TickType_t ) 10 ) == pdTRUE )//gửi lại câu lệnh vừa gửi nếu gửi thất bại
        {   
            ESP_LOGI(SEMAPHORE_TAG,"SMPERROR taken");
            if( queue != 0 )
            {
                const char *message;
                if( xQueuePeek( queue, &(message), ( TickType_t ) 10 ) )
                {
                    if(times_send<MAX_TIMES_SEND){
                        sendData(TX_TASK_TAG, message);
                        ESP_LOGI(SEMAPHORE_TAG,"Time send: %u",times_send);
                    }
                }
            }
            
        }
    }
}

static void start_module(){ //hàm khởi chạy module và nạp các câu lệnh vào queue
    create_queue();
    turn_on_module();
    sendData(TX_TASK_TAG,CHECK);
}


static void CallbackFunction( TimerHandle_t xTimer ){ //Hàm chạy theo chu kỳ timer
    ESP_LOGI(SEMAPHORE_TAG,"START TIMER");
    start_module();
}



static void data_handle(uint8_t *data) // Hàm xử lý dữ liệu nhận được từ RX
{
    char *token;
    token = strtok((char *)data, "\n");
    if (strncmp(token, "AT+CENG?", 8) == 0) //Xử lý lấy dữ liệu từ câu lệnh AT+CENG
    {
        bool noService = true;
        while (token != NULL)
        {   
            if (strstr(token, "NO SERVICE") != NULL){
                noService = true;
                state_of_command=2;
            }
            else if(strstr(token, "LTE") != NULL){
                noService = false;
                state_of_command = 1;
            }
            if (!noService){
                if (strncmp(token, "+CENG: 0,\"", 9) == 0)
                {
                sscanf(token, "+CENG: %d,\"%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\"", &ceng.cell, &ceng.earfcn, &ceng.pci, &ceng.rsrp, &ceng.rssi, &ceng.rsrq, &ceng.sinr, &ceng.tac, &ceng.cellid, &ceng.mcc, &ceng.mnc, &ceng.tx_power);
                snprintf(pub_payload, sizeof(pub_payload), "{\"rsrp\": %d, \"rsrq\": %d, \"sinr\": %d, \"pci\": %d, \"cell_id\": %d}\r\n", ceng.rsrp, ceng.rsrq, ceng.sinr, ceng.pci, ceng.cellid);
                pub_payload_pointer = pub_payload;
                ESP_LOGI(TX_TASK_TAG,"This is payload:%s",pub_payload_pointer);
                add_pub_command_to_queue();
                }
            }
            token = strtok(NULL, "\n"); 
        }
    }
    // else if (strstr(token, "AT+CGNSINF")!=NULL){
    //     while (token != NULL)
    //     {   
    //         sscanf(token, "+CGNSINF: %f,%f,%f,%f,%f", &GNSS_Data[0],&GNSS_Data[1],&GNSS_Data[2],&GNSS_Data[3],&GNSS_Data[4]);
    //         if(GNSS_Data[3]>0||GNSS_Data[4]>0){
    //             // snprintf(pub_payload, sizeof(pub_payload), "{\"lon\": %ld, \"lat\": %ld}\r\n", GNSS_Data[3],GNSS_Data[4]);
    //             snprintf(pub_payload, sizeof(pub_payload), "{\"rsrp\": %d, \"rsrq\": %d, \"sinr\": %d, \"pci\": %d, \"cell_id\": %d, \"lon\": %f, \"lat\": %f}\r\n", ceng.rsrp, ceng.rsrq, ceng.sinr, ceng.pci, ceng.cellid, GNSS_Data[3],GNSS_Data[4]);
    //             pub_payload_pointer = pub_payload;
    //             ESP_LOGI(TX_TASK_TAG,"This is payload:%s",pub_payload_pointer);
    //             add_pub_command_to_queue();  
    //             ESP_LOGI(TX_TASK_TAG,"Lon %e, Lat %e",GNSS_Data[3],GNSS_Data[4]);
    //             state_of_command = 1; 
    //             break;
    //         }
    //         else{
    //             state_of_command=2;
    //         }
    //         token = strtok(NULL, "\n"); 
    //     }
    // }
    else{
         while (token != NULL)
            {  
            if(strstr(token, "ERROR") != NULL) { // Nếu nhận được phản hồi error
                state_of_command = 2;
                break;
            }
            else if(strstr(token, "OK") != NULL || strstr(token, "POWER DOWN")!=NULL|| strstr(token, "DEACTIVE")!=NULL|| strstr(token, "READY")!=NULL||strstr(token, "SMPUB")!=NULL) // các trường hợp phản hồi hợp lệ
                {
                    state_of_command = 1;
                }
            else if (strstr(token, "+SGNSCMD:")!=NULL){
            ESP_LOGI(TX_TASK_TAG,"SUCCEED");   
            sscanf(token, "+SGNSCMD: %d,%s,%f,%f", &gnss.status,gnss.date,&gnss.lat,&gnss.lon);
            if(gnss.lat>0||gnss.lon>0){
                // snprintf(pub_payload, sizeof(pub_payload), "{\"lon\": %ld, \"lat\": %ld}\r\n", GNSS_Data[3],GNSS_Data[4]);
                snprintf(pub_payload, sizeof(pub_payload), "{\"rsrp\": %d, \"rsrq\": %d, \"sinr\": %d, \"pci\": %d, \"cell_id\": %d, \"lon\": %f, \"lat\": %f}\r\n", ceng.rsrp, ceng.rsrq, ceng.sinr, ceng.pci, ceng.cellid,gnss.lon, gnss.lat);
                pub_payload_pointer = pub_payload;
                ESP_LOGI(TX_TASK_TAG,"This is payload:%s",pub_payload_pointer);
                if( xSemaphoreGive( xSemaphorePub ) != pdTRUE )
                {
                    ESP_LOGI(SEMAPHORE_TAG,"SMPPub was not given");
                }
                else{
                    ESP_LOGI(SEMAPHORE_TAG,"SMPPub was given");
                }                
            // ESP_LOGI(TX_TASK_TAG,"Lon %e, Lat %e",GNSS_Data[3],GNSS_Data[4]);

            }
       
            }
            token = strtok(NULL, "\n");
            }

    }


    if (state_of_command == 1){ //Nếu nhận được phản hồi OK
        if( xSemaphoreGive( xSemaphoreOK ) != pdTRUE )
        {
            ESP_LOGI(SEMAPHORE_TAG,"SMPOK was not given");
        }
        else{
            ESP_LOGI(SEMAPHORE_TAG,"SMPOK was given");
        }
    }
    else if(state_of_command == 2){ //Nếu không nhận được phản hồi OK
        if( xSemaphoreGive( xSemaphoreError ) != pdTRUE )
        {
            ESP_LOGI(SEMAPHORE_TAG,"SMPERROR was not given");
        }
        else{
            ESP_LOGI(SEMAPHORE_TAG,"SMPERROR was given");
        }
    }
}

static void rx_task(void *arg)  // Tác vụ xử lý dữ liệu nhận được từ RX
{
    esp_log_level_set(RX_TASK_TAG, ESP_LOG_INFO);
    uint8_t *data = (uint8_t *)malloc(RX_BUF_SIZE + 1);
    while (1)
    {
        const int rxBytes = uart_read_bytes(UART_NUM_2, data, RX_BUF_SIZE, 1000 / portTICK_PERIOD_MS);
        if (rxBytes > 0)
        {
            data[rxBytes] = 0;
            ESP_LOGI(RX_TASK_TAG, "%s", data);
            data_handle(data);
        }
    }
    free(data);
}

static void time_out_check(void *arg){ //Tác vụ xử lý trường hợp gửi lại quá số lần cho phép hoặc module không trả lời
    for(;;){
        if (state_of_command==0){
            if(esp_timer_get_time()/1000-request_time>MAX_WAITING_TIME||times_send==MAX_TIMES_SEND){
                vTaskSuspendAll();
                vQueueDelete(queue);
                sendData(TX_TASK_TAG, TURNOFF);
                vTaskDelay(10000);
                xTimerReset(xTimer,100/portTICK_PERIOD_MS);
                start_module();
                xTaskResumeAll();
                state_of_command=1;
                }
            }
    }
}


void app_main(void)
{
    xSemaphoreOK = xSemaphoreCreateBinary();
    xSemaphoreError = xSemaphoreCreateBinary();
    xSemaphorePub = xSemaphoreCreateBinary();
    init_uart();
    init_gpio(PWRKEY);
    xTaskCreate(rx_task, "uart_rx_task", 1024 * 2, NULL, configMAX_PRIORITIES, NULL);
    xTaskCreate(send_task, "send_task", 1024 * 2, NULL, configMAX_PRIORITIES - 1, NULL);
    xTaskCreate(time_out_check, "time_out_check", 1024, NULL, 0, NULL);
    xTimer = xTimerCreate("Timer1", 5*60000/portTICK_PERIOD_MS, pdTRUE, "1", CallbackFunction);
    xTimerStart(xTimer,0);
    start_module();
}