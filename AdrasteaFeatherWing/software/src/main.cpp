#include "Adrastea.h"
#include "ATCommands/ATDevice.h"
#include "ATCommands/ATEvent.h"
#include "ATCommands/ATMQTT.h"
#include "ATCommands/ATPacketDomain.h"
// #include <RTClib.h>

/* Pin Definitions */
#define PRESSURE_ANALOG_PIN A3
#define BATTERY_ANALOG_PIN A2
#define LED_PIN 8

/* MQTT Settings */
#define MQTT_CLIENT_ID "adrastea_jz_hydra"
#define MQTT_SERVER_ADDRESS "test.mosquitto.org"
#define MQTT_TOPIC "adrtopic/ad506fbe-b5e3-4bba-a75a-73f864309c5c"

/* APN Settings */
#define APN_NAME "iot.1nce.net"
#define PDP_CONTEXT_ID 1

/* State Machine States */
typedef enum {
    STATE_WAIT_FOR_NETWORK,
    STATE_MQTT_CONNECT,
    STATE_MQTT_PUBLISH,
    STATE_IDLE,
    STATE_ERROR
} ApplicationState_t;

/* Globals */
static volatile ApplicationState_t currentState = STATE_IDLE;
static volatile ATPacketDomain_Network_Registration_Status_t networkStatus;
Adrastea_Pins_t adrasteaPins;
char payload[256];

/* Function Prototypes */
void handleNetworkRegistration();
void handleMQTTConnect();
void handleMQTTPublish();
void logError(const char* message);
void publishMQTTData(int pressure, int battery);
void Adrastea_EventCallback(char* eventText);
void generateUUIDv4(char* buffer);
void indicateState();

// RTC_PCF8523 rtc;
// const int chipSelect = 4;
// File dataFile;

/* Setup Function */
void setup() {
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
    delay(3000);
    WE_DEBUG_PRINT("Booting up...\r\n");

    // DateTime now = rtc.now();  // Get current time
    // WE_DEBUG_PRINT("Booting up at: %s\r\n", String(now.timestamp()));

    // if (!SD.begin(chipSelect)) {
    //     WE_DEBUG_PRINT("SD card initialization failed!\r\n");
    //     return;
    // }

    // dataFile = SD.open("log.txt", FILE_WRITE);
    // if (dataFile) {
    //     DateTime now = rtc.now();  // Get current time
    //     WE_DEBUG_PRINT("Booting up at: %s\r\n", String(now.timestamp()));
    //     dataFile.println("Booting up at " + String(now.timestamp()));
    //     dataFile.close();
    // } else {
    //     WE_DEBUG_PRINT("Failed to open log file!\r\n");
    // }

    delay(3000);

#ifdef WE_DEBUG
    WE_Debug_Init();
#endif

    adrasteaPins.Adrastea_Pin_WakeUp.pin = 6;

    if (!Adrastea_Init(&adrasteaPins, 115200, WE_FlowControl_NoFlowControl,
                       WE_Parity_None, &Adrastea_EventCallback)) {
        logError("Failed to initialize Adrastea");
        return;
    }

    // Configure PDP Context with APN
    ATPacketDomain_PDP_Context_t pdpContext;
    pdpContext.cid = PDP_CONTEXT_ID;
    pdpContext.pdpType = ATPacketDomain_PDP_Type_IPv4;
    strcpy((char*)pdpContext.apnName, APN_NAME);

    if (!ATPacketDomain_DefinePDPContext(pdpContext)) {
        logError("Failed to define PDP context with APN");
        return;
    }

    WE_DEBUG_PRINT("Initialization complete\r\n");
    currentState = STATE_WAIT_FOR_NETWORK;
}

/* Main Loop */
void loop() {
    indicateState();
    int battery = analogRead(BATTERY_ANALOG_PIN);
    int pressure = analogRead(PRESSURE_ANALOG_PIN);

    switch (currentState) {
        case STATE_WAIT_FOR_NETWORK:
            handleNetworkRegistration();
            break;
        case STATE_MQTT_CONNECT:
            handleMQTTConnect();
            break;
        case STATE_MQTT_PUBLISH:
            publishMQTTData(pressure, battery);
            break;
        case STATE_IDLE:
            delay(3000);  // Idle delay
            break;
        case STATE_ERROR:
            logError("Error state encountered. Restart required.");
            while (true);  // Halt execution
    }
}

/* State Handlers */
void handleNetworkRegistration() {
    if (!ATPacketDomain_SetNetworkRegistrationResultCode(
            ATPacketDomain_Network_Registration_Result_Code_Enable)) {
        logError("Failed to enable network registration result code");
        currentState = STATE_ERROR;
        return;
    }

    if (!ATPacketDomain_ReadNetworkRegistrationStatus(
            (ATPacketDomain_Network_Registration_Status_t*)&networkStatus)) {
        logError("Failed to read network registration status");
        currentState = STATE_ERROR;
        return;
    }

    // Check for both home network and roaming states
    if (networkStatus.state == ATPacketDomain_Network_Registration_State_Registered_Home_Network ||
        networkStatus.state == ATPacketDomain_Network_Registration_State_Registered_Roaming) {
        WE_DEBUG_PRINT("Network registered\r\n");
        delay(1000);  // Give network time to stabilize before PDP activation

        // Check current PDP context state first
        if (!ATPacketDomain_ReadPDPContextsState()) {
            WE_DEBUG_PRINT("Warning: Could not read PDP context state, attempting activation anyway\r\n");
        }

        delay(500);  // Additional delay to ensure state is read

        // Activate PDP Context
        ATPacketDomain_PDP_Context_CID_State_t cidState;
        cidState.cid = PDP_CONTEXT_ID;
        cidState.state = ATPacketDomain_PDP_Context_State_Activated;

        if (!ATPacketDomain_SetPDPContextState(cidState)) {
            logError("Failed to activate PDP context");
            currentState = STATE_ERROR;
            return;
        }

        WE_DEBUG_PRINT("PDP context activated\r\n");
        delay(2000);  // Wait for PDP context to fully activate
        currentState = STATE_MQTT_CONNECT;
    }
}

void handleMQTTConnect() {
    if (!ATMQTT_SetMQTTUnsolicitedNotificationEvents(
            ATMQTT_Event_All, ATCommon_Event_State_Enable)) {
        logError("Failed to enable MQTT events");
        currentState = STATE_ERROR;
        return;
    }

    if (!ATMQTT_ConfigureNodes(ATMQTT_Conn_ID_Single_Connectivity_Mode,
                               (char*)MQTT_CLIENT_ID,
                               (char*)MQTT_SERVER_ADDRESS, NULL, NULL)) {
        logError("Failed to configure MQTT node");
        currentState = STATE_ERROR;
        return;
    }

    if (!ATMQTT_Connect(ATMQTT_Conn_ID_Single_Connectivity_Mode)) {
        logError("Failed to connect to MQTT");
        currentState = STATE_ERROR;
        return;
    }

    WE_DEBUG_PRINT("MQTT Connected\r\n");
    currentState = STATE_MQTT_PUBLISH;
}

// Generate a UUIDv4 and store it in a fixed-size buffer
void generateUUIDv4(char* buffer) {
    sprintf(
        buffer, "%04x%04x-%04x-%04x-%04x-%04x%04x%04x",
        (unsigned int)random(0, 0xFFFF),
        (unsigned int)random(0, 0xFFFF),             // 8 characters
        (unsigned int)random(0, 0xFFFF),             // 4 characters
        0x4000 | ((unsigned int)random(0, 0x0FFF)),  // 4 characters; version 4
        0x8000 | ((unsigned int)random(0, 0x3FFF)),  // 4 characters; variant
        (unsigned int)random(0, 0xFFFF), (unsigned int)random(0, 0xFFFF),
        (unsigned int)random(0, 0xFFFF)  // 12 characters
    );
}

void indicateState() {
    switch (currentState) {
        case STATE_WAIT_FOR_NETWORK:
            WE_DEBUG_PRINT("slowing blink\r\n");
            digitalWrite(LED_PIN, millis() % 1000 < 500);  // Slow blink
            break;
        case STATE_MQTT_CONNECT:
            WE_DEBUG_PRINT("fast blink\r\n");
            digitalWrite(LED_PIN, millis() % 300 < 150);  // Fast blink
            break;
        case STATE_MQTT_PUBLISH:
            WE_DEBUG_PRINT("blink on\r\n");
            digitalWrite(LED_PIN, HIGH);  // Solid ON
            break;
        case STATE_IDLE:
            WE_DEBUG_PRINT("rapid blink\r\n");
            digitalWrite(LED_PIN, millis() % 200 < 100);  // Rapid blink
            break;
        case STATE_ERROR:
            WE_DEBUG_PRINT("blink off\r\n");
            digitalWrite(LED_PIN, LOW);  // OFF
            break;
    }
}

void publishMQTTData(int pressure, int battery) {
    char uuid[37];  // Buffer for the UUID (36 chars + null terminator)
    generateUUIDv4(uuid);
    snprintf(payload, sizeof(payload),
             "{\"uuid\": \"%s\", \"pressure\": %d, \"battery\": %d}", uuid,
             pressure, battery);

    if (!ATMQTT_Publish(ATMQTT_Conn_ID_Single_Connectivity_Mode,
                        ATMQTT_QoS_At_Least_Once, ATMQTT_Retain_Not_Retained,
                        (char*)MQTT_TOPIC, payload, strlen(payload))) {
        logError("Failed to publish data");
        currentState = STATE_ERROR;
        return;
    }

    WE_DEBUG_PRINT("Data published: %s\r\n", payload);
    delay(3000);  // Delay between publish events
}

/* Helper Functions */
void logError(const char* message) {
    WE_DEBUG_PRINT("ERROR: %s\r\n", message);
    // dataFile = SD.open("log.txt", FILE_WRITE);
    // if (dataFile) {
    //     dataFile.print("ERROR: ");
    //     dataFile.println(message);
    //     dataFile.close();
    // } else {
    //     WE_DEBUG_PRINT("Failed to write to log file!\r\n");
    // }
    currentState = STATE_ERROR;
}

/* Event Callback */
void Adrastea_EventCallback(char* eventText) {
    ATEvent_t event;
    ATEvent_ParseEventType(&eventText, &event);

    switch (event) {
        case ATEvent_PacketDomain_Network_Registration_Status:
            if (!ATPacketDomain_ParseNetworkRegistrationStatusEvent(
                    eventText,
                    (ATPacketDomain_Network_Registration_Status_t*)&networkStatus)) {
                logError("Failed to parse network registration status event");
                return;
            }
            if (networkStatus.state ==
                ATPacketDomain_Network_Registration_State_Registered_Roaming) {
                currentState = STATE_MQTT_CONNECT;
            }
            break;

        case ATEvent_MQTT_Connection_Confirmation: {
            ATMQTT_Connection_Result_t connResult;
            if (!ATMQTT_ParseConnectionConfirmationEvent(eventText,
                                                         &connResult) ||
                connResult.resultCode != ATMQTT_Event_Result_Code_Success) {
                logError("MQTT connection failed");
                return;
            }
            WE_DEBUG_PRINT("MQTT Connection confirmed\r\n");
            currentState = STATE_MQTT_PUBLISH;
            break;
        }

        default:
            break;
    }
}
