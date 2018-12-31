#include <M5Stack.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <AzureIotHub.h>
#include <Esp32MQTTClient.h>

// Images to be displayed on LCD.
#define alarm_triggered_img alarm_trig
#define alarm_warning_img alarm_warn
#define PicArray extern unsigned char

// Pins used by the ultrasonic sensor.
#define TRIGGER_PIN 2
#define ECHO_PIN 5

// Choose frequencies (i.e. notes) for the alarm.
#define NOTE_1 300
#define NOTE_2 350

// Interrupt Button Pins.
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
const byte interrupt_pin_button_c = 37;
const byte interrupt_pin_button_b = 38;
const byte interrupt_pin_button_a = 39;

// Global variables.
bool alarm_is_triggered = false;
bool alarm_is_enabled = false;
static bool has_iot_hub = false;
PicArray alarm_triggered_img[]; 
PicArray alarm_warning_img[];
long ultrasonic_sensor_duration;
int ultrasonic_sensor_distance;
static uint64_t send_interval_ms;
StaticJsonBuffer<200> json_buffer;

// Please input the SSID and password of WiFi.
const char* ssid     = "";
const char* password = "";

// Please input connection string of the form "HostName=<host_name>;DeviceId=<device_id>;SharedAccessKey=<device_key>".
static const char* connection_string = "";

void IRAM_ATTR HandleInterruptStopAlarm() {
  /*
  * Interrupt handler for button press. Stops the alarm ring
  * and resets the alarm to its enabled state.
  */
  portENTER_CRITICAL_ISR(&mux);
  alarm_is_triggered = false;
  portEXIT_CRITICAL_ISR(&mux);
}

void ConnectToWiFi(){
  /*
  * Connects the M5Stack to WiFi using the global variable 
  * credentials ssid and password. The M5Stack LCD is
  * initialized to display "Connecting to wiFi...".
  */
  M5.Lcd.setTextColor(WHITE);
  M5.Lcd.setTextSize(4);
  M5.Lcd.printf("Connecting to WiFi...");
  M5.update();
  Serial.println("Starting connecting WiFi.");
  delay(10);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
}

static void SendConfirmationCallback(IOTHUB_CLIENT_CONFIRMATION_RESULT result) {
  if (result == IOTHUB_CLIENT_CONFIRMATION_OK) {
    Serial.println("Send Confirmation Callback finished.");
  }
}

static int  DeviceMethodCallback(const char *methodName, const unsigned char *payload, int size, unsigned char **response, int *response_size) {
  /*
   * Handles commands from IoT Central. The command "stop" 
   * will stop the triggered alarm and return it to its 
   * enabled state.
  */
  LogInfo("Try to invoke method %s", methodName);
  char *responseMessage = "\"Successfully invoke device method\"";
  int result = 200;

  if (strcmp(methodName, "stop") == 0){
    alarm_is_triggered = false;
  } else {
    LogInfo("No method %s found", methodName);
    result = 404;
  }
  
  *response_size = strlen(responseMessage) + 1;
  *response = (unsigned char *)strdup(responseMessage);

  return result;
}

static void DeviceTwinCallback(DEVICE_TWIN_UPDATE_STATE updateState, const unsigned char *payLoad, int size) {
  /*
  * Handles settings updates from IoT Central. The 
  * Enable/Disable toggle will enable or disable the alarm 
  * from operation.
  */

  // Copy the payload containing the message from IoT 
  // central into temp.
  char *temp = (char *)malloc(size + 1);
  if (temp == NULL) return;
  memcpy(temp, payLoad, size);
  temp[size] = '\0';

  // Create an ArduinoJson object to parse the string and set alarm_status as the value of the "Enable/Disable" field.
  JsonObject& json_obj = json_buffer.parseObject(temp);
  bool alarm_status = json_obj["Enable/Disable"]["value"]; 
  const char *desired = json_obj["desired"]["Enable/Disable"]["value"]; 

  if (desired != NULL) alarm_status = json_obj["desired"]["Enable/Disable"]["value"];
  
  if (alarm_status){
    // Enable the alarm.
    alarm_is_enabled = true;
  }
  else{
    // Disable the alarm.
    alarm_is_enabled = false;
    alarm_is_triggered = false;
  }

  free(temp);
  json_buffer.clear();
  send_interval_ms = millis();
}

void PlayAlarmRing(){
   /*
   * Ring played by the speaker when the alarm is triggered.
   */
   
   M5.Speaker.tone(NOTE_1);
   M5.update();
   delay(200);

   M5.Speaker.tone(NOTE_2);
   M5.update();
   delay(200);
}

void TurnAlarmRingOff(){
  /*
  * Mutes the alarm ring.
  */
  M5.Speaker.mute();
  M5.update();
}

int FindSensorDistance(){
  /*
  * Calculatess and returns the distance to the nearest object * within view of the ultrasonic sensor.
  */
  
  digitalWrite(TRIGGER_PIN, LOW);
  delayMicroseconds(2);
  
  // Sets the TRIGGER_PIN on HIGH state for 10 micro seconds.
  digitalWrite(TRIGGER_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIGGER_PIN, LOW);
  
  // Reads the ECHO_PIN and returns the sound wave travel time in microseconds.
  int duration = pulseIn(ECHO_PIN, HIGH);
  return duration*0.034/2;
}

void SendMessageToAzure(char *message, bool has_iot_hub){
  /*
  * The given message string is sent to Azure IoT Central.
  */
  if (has_iot_hub){
    Serial.println(message);
    if (Esp32MQTTClient_SendEvent(message)){
      Serial.println("Sending data succeed");
    } else {
      Serial.println("Failure...");
    }
  }
}

void RunAlarm(){
  int distance = FindSensorDistance();
  char message[128];
  
  if (distance < 70 && !alarm_is_triggered &&                 alarm_is_enabled){
    alarm_is_triggered = true;
    snprintf(message, 128, "{\"alarm_status\":\"triggered\"}{\"status\":\"online\"}");
    SendMessageToAzure(message, has_iot_hub);
  } else if (millis() - send_interval_ms > 10000) {
    // Send regular status updates to Azure IoT Central 
    // regarding the devices operation status as either 
    // online or offline.
    if (alarm_is_enabled){
      snprintf(message, 128, "{\"status\":\"online\"}");
    } else {
      snprintf(message, 128, "{\"status\":\"offline\"}");
    }
    SendMessageToAzure(message, has_iot_hub);
  }

  if (alarm_is_triggered){
    M5.Lcd.drawBitmap(0, 0, 320, 240, (uint16_t *) alarm_triggered_img);
    PlayAlarmRing();
  } else {
    M5.Lcd.drawBitmap(0, 0, 320, 240, (uint16_t *) alarm_warning_img);
    TurnAlarmRingOff();  
  }
  
  delay(100);
}

void setup() {
  /*
   * Alarm setup code, which is run once at the start of 
   * operation. This section is used to connect the device to 
   * WiFi and IoT Central, and initialize I/O pins on the 
   * M5Stack.
  */
  M5.begin();
  Wire.begin();
  Serial.begin(115200);
  json_buffer.clear();

  // Connect to WiFi.
  ConnectToWiFi();
  randomSeed(analogRead(0));

  // Connect to Azure IoT Central and setup device callbacks.
  Esp32MQTTClient_Init(                                   (const uint8_t*)connection_string, true);
  has_iot_hub = true;
  Esp32MQTTClient_SetSendConfirmationCallback(SendConfirmationCallback);
  Esp32MQTTClient_SetDeviceTwinCallback(DeviceTwinCallback);
  Esp32MQTTClient_SetDeviceMethodCallback(                   DeviceMethodCallback);

  // Setup interrupts to be triggered on the button pins.
  pinMode(interrupt_pin_button_a, INPUT_PULLUP);
  attachInterrupt(                                          digitalPinToInterrupt(interrupt_pin_button_a),              HandleInterruptStopAlarm, FALLING);
  pinMode(interrupt_pin_button_b, INPUT_PULLUP);
  attachInterrupt(                                          digitalPinToInterrupt(interrupt_pin_button_b),              HandleInterruptStopAlarm, FALLING);
  pinMode(interrupt_pin_button_c, INPUT_PULLUP);
  attachInterrupt(                                          digitalPinToInterrupt(interrupt_pin_button_c),              HandleInterruptStopAlarm, FALLING);

  // Configure echo and trigger pins used by the ultrasonic sensor.
  pinMode(TRIGGER_PIN, OUTPUT); 
  pinMode(ECHO_PIN, INPUT);
}

void loop() {
  /*
  * Alarm running code is looped continuously during device
  * operation.
  */
  RunAlarm();
  Esp32MQTTClient_Check();
}
