
#define BLYNK_TEMPLATE_ID "TMPL3AhhbKaAH"
#define BLYNK_TEMPLATE_NAME "ReVive Monitoring Dashboard"
#define BLYNK_AUTH_TOKEN "yzYKU1LRQyzuR-sd9ZfQuDX4AiwNcTUo"

#define BLYNK_PRINT Serial
#include <WiFi.h>
#include <BlynkSimpleEsp32.h>

char ssid[] = "wifiname";
char pass[] = "wifipswd";

#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BMP085.h>
#include <Adafruit_SSD1306.h>
#include <ESP32Servo.h>


#define VPIN_MAIN_BATTERY    V1
#define VPIN_STATUS          V0
#define VPIN_TEMPERATURE     V2
#define VPIN_PROXIMITY       V3
#define VPIN_CRASH_RISK      V9
#define VPIN_SLOPE_ANGLE     V7
#define VPIN_ENERGY_LEVEL    V10
#define VPIN_AX              V11
#define VPIN_AY              V12
#define VPIN_AZ              V13
#define VPIN_GZ              V14
#define VPIN_ALTITUDE        V4
#define VPIN_BRAKE           V15
#define VPIN_STEERING        V16


Adafruit_MPU6050 mpu;
Adafruit_BMP085 bmp;
Adafruit_SSD1306 display(128, 64, &Wire, -1);


#define SERVO_PIN 25
Servo steeringServo;
int servoPos = 90;
bool servoDir = true;


#define APDS_ADDR  0x39
#define ENABLE_REG 0x80
#define PROX_REG   0x9C

#define MAIN_BATTERY_PIN 18
#define BUZZER_PIN       19
#define ENERGY_ADC       34


bool mpu_ok = false;
bool bmp_ok = false;
bool oled_ok = false;
bool apds_ok = false;


float accX = 0;
float accY = 0;
float accZ = 0;
float gyroZ = 0;


float tiltAngle = 0;
float prevTilt = 0;
int crashConfidence = 0;
bool dangerMode = false;
bool preCrashAlert = false;
float prevAlt = 0;


unsigned long reviveStartTime = 0;
bool reviveTriggered = false;


unsigned long lastBlynkUpdate = 0;
const unsigned long BLYNK_UPDATE_INTERVAL = 300;


unsigned long lastBeepToggle = 0;
const unsigned long BEEP_INTERVAL = 200;
bool buzzerState = false;


uint8_t readProximity() {
  Wire.beginTransmission(APDS_ADDR);
  Wire.write(PROX_REG);
  Wire.endTransmission(false);
  Wire.requestFrom(APDS_ADDR, (uint8_t)1);
  return Wire.available() ? Wire.read() : 0;
}


#define CALIBRATION_FACTOR 1.085

float readEnergyLevel() {
  long sum = 0;
  for (int i = 0; i < 20; i++) {
    sum += analogRead(ENERGY_ADC);
    delay(2);
  }
  float adcAvg = sum / 20.0;
  float v_adc = (adcAvg / 4095.0) * 3.3;
  float batteryVoltage = v_adc * 2.0;
  batteryVoltage *= CALIBRATION_FACTOR;
  return batteryVoltage;
}

float energyPercent = 0;
float smoothEnergyPercent = 0;

float voltageToEnergyPercent(float v) {
  const int N = 13;
  float vTable[N] = {4.20, 4.10, 4.00, 3.92, 3.85, 3.80, 3.75, 3.70, 3.65, 3.60, 3.55, 3.50, 3.40};
  float pTable[N] = {100, 92, 84, 75, 66, 57, 48, 39, 30, 22, 14, 7, 0};
  if (v >= vTable[0]) return 100;
  if (v <= vTable[N - 1]) return 0;
  for (int i = 0; i < N - 1; i++) {
    if (v <= vTable[i] && v > vTable[i + 1]) {
      float ratio = (vTable[i] - v) / (vTable[i] - vTable[i + 1]);
      return pTable[i] - ratio * (pTable[i] - pTable[i + 1]);
    }
  }
  return 0;
}

float smoothEnergy(float prev, float current) {
  return (prev * 0.85) + (current * 0.15);
}


void updateBlynk(bool mainBatteryOn, float temp, float alt, uint8_t prox) {
  Blynk.virtualWrite(VPIN_MAIN_BATTERY, mainBatteryOn ? "ON" : "OFF");
  
  String statusMsg = "";
  if (dangerMode) {
    statusMsg = "WARNING - SLOPE ALERT";
  } else if (preCrashAlert) {
    statusMsg = "ALERT - OBJECT NEAR";
  } else if (!mainBatteryOn) {
    statusMsg = "EMERGENCY - ReVive ACTIVE";
  } else {
    statusMsg = "NORMAL";
  }
  Blynk.virtualWrite(VPIN_STATUS, statusMsg);
  
  if (!mainBatteryOn) {
    Blynk.virtualWrite(VPIN_BRAKE, "FAILSAFE");
    Blynk.virtualWrite(VPIN_STEERING, "LOCKED");
  } else {
    Blynk.virtualWrite(VPIN_BRAKE, "NORMAL");
    Blynk.virtualWrite(VPIN_STEERING, "NORMAL");
  }
  
  Blynk.virtualWrite(VPIN_TEMPERATURE, temp);
  Blynk.virtualWrite(VPIN_PROXIMITY, prox);
  Blynk.virtualWrite(VPIN_CRASH_RISK, crashConfidence);
  Blynk.virtualWrite(VPIN_SLOPE_ANGLE, tiltAngle);
  Blynk.virtualWrite(VPIN_ENERGY_LEVEL, (int)smoothEnergyPercent);
  Blynk.virtualWrite(VPIN_AX, accX);
  Blynk.virtualWrite(VPIN_AY, accY);
  Blynk.virtualWrite(VPIN_AZ, accZ);
  Blynk.virtualWrite(VPIN_GZ, gyroZ);
  Blynk.virtualWrite(VPIN_ALTITUDE, alt);
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Wire.begin(32, 33);
  Wire.setClock(100000);

  pinMode(MAIN_BATTERY_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  steeringServo.attach(SERVO_PIN);

  if (mpu.begin(0x69, &Wire)) {
    mpu_ok = true;
    mpu.setAccelerometerRange(MPU6050_RANGE_2_G);
    mpu.setGyroRange(MPU6050_RANGE_250_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_5_HZ);
  }

  if (bmp.begin(BMP085_ULTRAHIGHRES, &Wire)) {
    bmp_ok = true;
  }

  if (display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    oled_ok = true;
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
  }

  Wire.beginTransmission(APDS_ADDR);
  if (Wire.endTransmission() == 0) {
    apds_ok = true;
    Wire.beginTransmission(APDS_ADDR);
    Wire.write(ENABLE_REG);
    Wire.write(0x05);
    Wire.endTransmission();
  }

  analogReadResolution(12);

  Serial.println("Connecting to WiFi...");
  Blynk.begin(BLYNK_AUTH_TOKEN, ssid, pass);
  Serial.println("Connected to Blynk!");
}

void loop() {
  Blynk.run();


  bool mainBatteryOn = digitalRead(MAIN_BATTERY_PIN) == HIGH;
  
  float energyVolt = readEnergyLevel();

  energyPercent = voltageToEnergyPercent(energyVolt);
  smoothEnergyPercent = smoothEnergy(smoothEnergyPercent, energyPercent);

  float temp = 0;
  float alt = 0;
  uint8_t prox = 0;

  
  sensors_event_t a, g, t;
  if (mpu_ok) {
    mpu.getEvent(&a, &g, &t);
    accX = a.acceleration.x;
    accY = a.acceleration.y;
    accZ = a.acceleration.z;
    gyroZ = abs(g.gyro.z);
  }

  if (bmp_ok) {
    temp = bmp.readTemperature();
    alt = bmp.readAltitude(1013.25);
  }

  prox = apds_ok ? readProximity() : 0;

  
  tiltAngle = atan(sqrt(accX * accX + accY * accY) / abs(accZ)) * 57.3;
  float tiltChange = abs(tiltAngle - prevTilt);
  prevTilt = tiltAngle;

  crashConfidence = 0;
  preCrashAlert = false;

  float accMag = sqrt(accX * accX + accY * accY + accZ * accZ);

  if (accMag > 14) crashConfidence += 30;
  if (accMag > 18) crashConfidence += 20;
  if (accMag > 22) crashConfidence += 15;

  if (gyroZ > 3) crashConfidence += 15;

  if (tiltAngle > 25) crashConfidence += 20;
  if (tiltAngle > 35) crashConfidence += 15;
  if (tiltChange > 12) crashConfidence += 20;
  if (tiltChange > 20) crashConfidence += 15;

  float altDiff = abs(alt - prevAlt);
  if (altDiff > 1.2) crashConfidence += 15;
  prevAlt = alt;

  if (prox >= 200) crashConfidence = 100;
  else if (prox >= 100) {
    crashConfidence += 40;
    preCrashAlert = true;
  } else if (prox >= 10) {
    crashConfidence += 20;
    preCrashAlert = true;
  }

  crashConfidence = constrain(crashConfidence, 0, 100);
  dangerMode = (crashConfidence >= 70 || tiltAngle > 20);

  if (!mainBatteryOn) {
    
    steeringServo.write(90);

    if (!reviveTriggered) {
      reviveTriggered = true;
      reviveStartTime = millis();
      buzzerState = false;
      digitalWrite(BUZZER_PIN, LOW);
    }

    
    if (millis() - reviveStartTime <= 5000) {
      if (millis() - lastBeepToggle >= BEEP_INTERVAL) {
        lastBeepToggle = millis();
        buzzerState = !buzzerState;
        digitalWrite(BUZZER_PIN, buzzerState ? HIGH : LOW);
      }

      
      if (oled_ok) {
        display.clearDisplay();
        display.setTextSize(2);
        display.setCursor(0, 0);
        display.println("EMERGENCY");

        display.setTextSize(1);
        display.setCursor(0, 24);
        display.println("ReVive ACTIVE");
        display.setCursor(0, 44);
        display.println("Steering: Locked");
        display.setCursor(0, 54);
        display.println("Brake: Failsafe");

        display.display();
      }
    } else {
      
      digitalWrite(BUZZER_PIN, LOW);
      buzzerState = false;

      if (oled_ok) {
        display.clearDisplay();

        if (dangerMode) {
          display.setTextSize(2);
          display.setCursor(0, 0);
          display.println("WARNING");

          display.setTextSize(1);
          display.setCursor(0, 24);
          display.print("Slope/Tilt: ");
          display.print(tiltAngle, 1);
          display.print(" deg");
          display.setCursor(0, 36);
          display.print("Crash Risk: ");
          display.print(crashConfidence);
          display.println("%");

          display.setCursor(0, 50);
          display.print("Energy: ");
          display.print(energyVolt, 2);
          display.print(" V | ");
          display.print((int)smoothEnergyPercent);
          display.println("%");
        } else if (preCrashAlert) {
          display.setTextSize(2);
          display.setCursor(0, 0);
          display.println("ALERT");

          display.setTextSize(1);
          display.setCursor(0, 24);
          display.println("Object Near");
          display.setCursor(0, 36);
          display.println("High Crash Risk");

          display.setCursor(0, 50);
          display.print("Energy: ");
          display.print(energyVolt, 2);
          display.print(" V | ");
          display.print((int)smoothEnergyPercent);
          display.println("%");
        } else {
          display.setTextSize(1);
          display.setCursor(0, 0);
          display.print("AX: ");
          display.println(accX, 2);
          display.print("AY: ");
          display.println(accY, 2);
          display.print("AZ: ");
          display.println(accZ, 2);
          display.print("GZ: ");
          display.println(gyroZ, 2);
          display.print("T: ");
          display.print(temp, 1);
          display.println(" C");
          display.print("Alt: ");
          display.print(alt, 1);
          display.println(" m");
          display.print("Prox: ");
          display.println(prox);

          display.setCursor(0, 55);
          display.print("Energy: ");
          display.print(energyVolt, 2);
          display.print(" V | ");
          display.print((int)smoothEnergyPercent);
          display.println("%");
        }

        display.display();
      }
    }

  } else {
    
    
    if (reviveTriggered) {
      reviveTriggered = false;
      digitalWrite(BUZZER_PIN, LOW);
      buzzerState = false;
    }

    
    servoPos += servoDir ? 35 : -35;
    if (servoPos >= 180) servoDir = false;
    if (servoPos <= 0) servoDir = true;
    steeringServo.write(servoPos);

    
    if (oled_ok) {
      display.clearDisplay();

      if (dangerMode) {
        display.setTextSize(2);
        display.setCursor(0, 0);
        display.println("WARNING");

        display.setTextSize(1);
        display.setCursor(0, 24);
        display.print("Slope/Tilt: ");
        display.print(tiltAngle, 1);
        display.print(" deg");
        display.setCursor(0, 36);
        display.print("Crash Risk: ");
        display.print(crashConfidence);
        display.println("%");

        display.setCursor(0, 50);
        display.print("Energy: ");
        display.print(energyVolt, 2);
        display.print(" V | ");
        display.print((int)smoothEnergyPercent);
        display.println("%");
      } else if (preCrashAlert) {
        display.setTextSize(2);
        display.setCursor(0, 0);
        display.println("ALERT");

        display.setTextSize(1);
        display.setCursor(0, 24);
        display.println("Object Near");
        display.setCursor(0, 36);
        display.println("High Crash Risk");

        display.setCursor(0, 50);
        display.print("Energy: ");
        display.print(energyVolt, 2);
        display.print(" V | ");
        display.print((int)smoothEnergyPercent);
        display.println("%");
      } else {
        display.setTextSize(1);
        display.setCursor(0, 0);
        display.print("AX: ");
        display.println(accX, 2);
        display.print("AY: ");
        display.println(accY, 2);
        display.print("AZ: ");
        display.println(accZ, 2);
        display.print("GZ: ");
        display.println(gyroZ, 2);
        display.print("T: ");
        display.print(temp, 1);
        display.println(" C");
        display.print("Alt: ");
        display.print(alt, 1);
        display.println(" m");
        display.print("Prox: ");
        display.println(prox);

        display.setCursor(0, 55);
        display.print("Energy: ");
        display.print(energyVolt, 2);
        display.print(" V | ");
        display.print((int)smoothEnergyPercent);
        display.println("%");
      }

      display.display();
    }
  }

  
  if (millis() - lastBlynkUpdate >= BLYNK_UPDATE_INTERVAL) {
    lastBlynkUpdate = millis();
    updateBlynk(mainBatteryOn, temp, alt, prox);
  }

  delay(150);
}