#include <SPI.h>
#include "DW1000Ranging.h"
#include "DW1000.h"
#include "HTTPClient.h"
#include <WiFi.h>
#include <WebServer.h>

// ESP32_UWB pin definitions
#define SPI_SCK 18
#define SPI_MISO 19
#define SPI_MOSI 23
#define DW_CS 5

// connection pins
const uint8_t PIN_RST = 27; // reset pin
const uint8_t PIN_IRQ = 34; // irq pin
const uint8_t PIN_SS = 5;   // spi select pin

char this_anchor_addr[] = "85:00:22:EA:82:60:3B:9C";
float this_anchor_target_distance = 1; // measured distance to anchor in meters

const char* ssid = "ESP32_AP";
const char* password = "123456789";
String tag_ip = "192.168.4.1";

WebServer server(80);

bool not_sent = true;

uint16_t this_anchor_Adelay = 16600; // starting value
uint16_t Adelay_delta = 50; // initial binary search step size
const int sampleSize = 100; // number of samples to collect for each iteration
uint16_t adelaySamples[sampleSize]; // array to store Adelay samples
int sampleCount = 0;

// Kalman filter variables
float Q = 0.1; // process noise covariance
float R = 0.1; // measurement noise covariance
float P = 1;   // estimation error covariance
float K;       // Kalman gain
float X = 0;   // estimated value

float kalmanFilter(float measurement) {
  // Prediction update
  P = P + Q;
  // Measurement update
  K = P / (P + R);
  X = X + K * (measurement - X);
  P = (1 - K) * P;
  return X;
}

void sendCalibrationDataToTag(uint16_t finalAdelay) {
    if (tag_ip != "") {
        HTTPClient http;
        String url = "http://" + tag_ip + "/receive_calibration?final_adelay=" + String(finalAdelay);
        http.begin(url);
        int httpCode = http.GET();
        if (httpCode > 0) {
            String payload = http.getString();
            Serial.println("Calibration data sent to tag. Response: " + payload);
        } else {
            Serial.println("Failed to send calibration data to tag.");
        }
        http.end();
    }
}

void newRange(){
    //    Serial.print("from: ");
  Serial.print(DW1000Ranging.getDistantDevice()->getShortAddress(), HEX);
  Serial.print(", ");

#define NUMBER_OF_DISTANCES 1
  float dist = 0.0;
  for (int i = 0; i < NUMBER_OF_DISTANCES; i++) {
    dist += DW1000Ranging.getDistantDevice()->getRange();
  }
  dist = dist/NUMBER_OF_DISTANCES;
  Serial.println(dist);
}

void handleCalibration()
{
  // while(true){
  static float last_delta = 0.0;
  Serial.print(DW1000Ranging.getDistantDevice()->getShortAddress(), DEC);

  float dist = DW1000Ranging.getDistantDevice()->getRange();
  float filteredDist = kalmanFilter(dist);

  Serial.print(",");
  Serial.print(filteredDist, 4); // Print filtered distance with more resolution

  float this_delta = filteredDist - this_anchor_target_distance;  // error in measured distance

  // Print current status
  Serial.print(", Adelay = ");
  Serial.print(this_anchor_Adelay);
  Serial.print(", delta = ");
  Serial.println(this_delta, 4); // Print delta with more resolution

  // Check for stopping condition
  if (Adelay_delta < 1 && abs(this_delta) < 0.01) {
    adelaySamples[sampleCount++] = this_anchor_Adelay; // store final sample
    if (sampleCount >= sampleSize) {
      uint32_t sum = 0;
      for (int i = 0; i < sampleSize; i++) {
        sum += adelaySamples[i];
      }
      uint16_t finalAdelay = sum / sampleSize; // calculate the average Adelay
      Serial.print("Final Adelay: ");
      Serial.println(finalAdelay);
      while(1);  // done calibrating
    }
  } else {
    if (this_delta * last_delta < 0.0) {
      Adelay_delta = Adelay_delta / 2; // sign changed, reduce step size
    } else if (abs(this_delta) < abs(last_delta)) {
      Adelay_delta = Adelay_delta * 0.9; // Reduce step size if getting closer
    } else {
      Adelay_delta = Adelay_delta * 1.1; // Increase step size if moving away
    }
    last_delta = this_delta;
    
    if (this_delta > 0.0) {
      this_anchor_Adelay += Adelay_delta; // new trial Adelay
    } else {
      this_anchor_Adelay -= Adelay_delta;
    }

    // Apply the new antenna delay
    DW1000.setAntennaDelay(this_anchor_Adelay);
    delay(10); // Adding a small delay to ensure the change is applied

    // Store the sample
    adelaySamples[sampleCount++] = this_anchor_Adelay;
    if (sampleCount >= sampleSize) {
      uint32_t sum = 0;
      for (int i = 0; i < sampleSize; i++) {
        sum += adelaySamples[i];
      }
      uint16_t finalAdelay = sum / sampleSize; // calculate the average Adelay
      Serial.print("Final Adelay: ");
      Serial.println(finalAdelay);
      // while(1);  // done calibrating
      if (not_sent){
        sendCalibrationDataToTag(finalAdelay);
        not_sent = false;
        // break;
      }
    }
  // }
  }

  // Reinitialize the communication if needed
  DW1000Ranging.startAsAnchor(this_anchor_addr, DW1000.MODE_LONGDATA_RANGE_LOWPOWER, false);
}

void newDevice(DW1000Device *device)
{
  Serial.print("Device added: ");
  Serial.println(device->getShortAddress(), HEX);
}

void inactiveDevice(DW1000Device *device)
{
  Serial.print("delete inactive device: ");
  Serial.println(device->getShortAddress(), HEX);
}

void calib(){
      if (server.hasArg("target_distance") && server.hasArg("tag_ip")) {
        String targetDistance = server.arg("target_distance");
        tag_ip = server.arg("tag_ip");
        this_anchor_target_distance = targetDistance.toFloat();
        Serial.println("Starting calibration process with target distance: " + targetDistance);
        // startCalibration();
        DW1000Ranging.attachNewRange(handleCalibration);
        DW1000Ranging.attachNewDevice(newDevice);
        DW1000Ranging.attachInactiveDevice(inactiveDevice);
  
        server.send(200, "text/plain", "Calibration started with target distance: " + targetDistance);
    } else {
        server.send(400, "text/plain", "Bad Request");
    }

}

void setup()
{
  Serial.begin(115200);
  while (!Serial);
    WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.println("Connecting to WiFi...");
  }

  Serial.println("Connected to WiFi");

  server.on("/start_calibration", calib);
  server.begin();
  // init the configuration
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
  DW1000Ranging.initCommunication(PIN_RST, PIN_SS, PIN_IRQ); // Reset, CS, IRQ pin

  Serial.print("Starting Adelay "); Serial.println(this_anchor_Adelay);
  Serial.print("Measured distance "); Serial.println(this_anchor_target_distance);
  
  DW1000.setAntennaDelay(this_anchor_Adelay);

  DW1000Ranging.attachNewRange(newRange);
  DW1000Ranging.attachNewDevice(newDevice);
  DW1000Ranging.attachInactiveDevice(inactiveDevice);
  
  // start the module as anchor, don't assign random short address
  DW1000Ranging.startAsAnchor(this_anchor_addr, DW1000.MODE_LONGDATA_RANGE_LOWPOWER, false);
}

void loop()
{
  DW1000Ranging.loop();
}
