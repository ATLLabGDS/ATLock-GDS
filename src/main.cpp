#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Servo.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// --- PIN DEFINITIONS FROM YOUR DIAGRAM ---
#define SERVO_PIN      13
#define GREEN_LED      26
#define RED_LED        27
#define BUZZER_PIN     25

#define RST_PIN        4   // RFID Reset
#define SS_PIN         5   // RFID SDA (SS)

// --- BACKEND API ENDPOINTS ---
const String BASE_URL = "https://atlcloudgds.duckdns.org";

// --- HARDWARE CONFIGURATIONS ---
LiquidCrystal_I2C lcd(0x27, 16, 2);
Servo myServo;
MFRC522 mfrc522(SS_PIN, RST_PIN);

const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};

byte rowPins[ROWS] = {32, 33, 14, 15}; // R1, R2, R3, R4
byte colPins[COLS] = {2, 16, 17, 12};  // C1, C2, C3, C4

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// --- SYSTEM STATE VARIABLES ---
String inputPin = "";
bool isLocked = true;
unsigned long lastDashboardCheck = 0;
const unsigned long checkInterval = 2000; // Poll and send status every 2 seconds

// --- HELPER FUNCTIONS ---
void toneBuzzer(int frequency, int duration) {
  tone(BUZZER_PIN, frequency, duration);
  delay(duration);
  noTone(BUZZER_PIN);
}

void updateUI(String line1, String line2) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1);
  lcd.setCursor(0, 1);
  lcd.print(line2);
}

void setLockStateHardware(bool locked) {
  isLocked = locked;
  if (isLocked) {
    myServo.write(0); // Locked position
    digitalWrite(RED_LED, HIGH);
    digitalWrite(GREEN_LED, LOW);
    updateUI("SYSTEM LOCKED", "PIN or Card...");
  } else {
    myServo.write(90); // Unlocked position
    digitalWrite(RED_LED, LOW);
    digitalWrite(GREEN_LED, HIGH);
    updateUI("ACCESS GRANTED", "Welcome!");
    toneBuzzer(1000, 500);
  }
}

// --- API HIT FUNCTIONS ---

// Sync local states to /api/status while picking up remote dashboard overrides
void syncDashboardStatus() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();
  
  HTTPClient http;
  // Dashboard path tracking execution parameters
  http.begin(client, BASE_URL + "/api/status"); 
  
  int httpResponseCode = http.GET();
  
  Serial.print("[Dashboard Sync] Status Code: ");
  Serial.println(httpResponseCode);
  
  if (httpResponseCode == 200) {
    String response = http.getString();
    Serial.println("[Response Payload]: " + response);
    
    // Evaluate if backend state mandates explicit remote locking changes
    if (response.indexOf("\"status\":\"unlocked\"") >= 0 && isLocked) {
      Serial.println("[Command Switch] Remote Unlocking Triggered.");
      setLockStateHardware(false);
      delay(3000); // Hold open window
      setLockStateHardware(true);
    } else if (response.indexOf("\"status\":\"locked\"") >= 0 && !isLocked) {
      setLockStateHardware(true);
    }
  }
  http.end();
}

void verifyPinOnline(String pin) {
  if (WiFi.status() != WL_CONNECTED) {
    updateUI("ACCESS DENIED", "No WiFi Link");
    return;
  }

  updateUI("Verifying PIN...", "Please wait");
  
  WiFiClientSecure client;
  client.setInsecure();
  
  HTTPClient http;
  http.begin(client, BASE_URL + "/api/pin/change"); // Fallback validation endpoint routing
  http.addHeader("Content-Type", "application/json");
  
  String jsonPayload = "{\"pin\":\"" + pin + "\"}";
  int httpResponseCode = http.POST(jsonPayload);
  
  if (httpResponseCode == 200) {
    setLockStateHardware(false);
    delay(3000);
    setLockStateHardware(true);
  } else {
    updateUI("ACCESS DENIED", "Invalid PIN (" + String(httpResponseCode) + ")");
    digitalWrite(RED_LED, HIGH);
    toneBuzzer(400, 600);
    delay(1500);
    setLockStateHardware(isLocked);
  }
  http.end();
}

void verifyRfidOnline(String uid) {
  if (WiFi.status() != WL_CONNECTED) {
    updateUI("ACCESS DENIED", "No WiFi Link");
    return;
  }

  updateUI("Checking Card...", "Please wait");
  
  WiFiClientSecure client;
  client.setInsecure();
  
  HTTPClient http;
  http.begin(client, BASE_URL + "/api/rfid/add");
  http.addHeader("Content-Type", "application/json");
  
  String jsonPayload = "{\"uid\":\"" + uid + "\"}";
  int httpResponseCode = http.POST(jsonPayload);
  
  if (httpResponseCode == 200) {
    setLockStateHardware(false);
    delay(3000);
    setLockStateHardware(true);
  } else {
    updateUI("ACCESS DENIED", "Unknown Card");
    digitalWrite(RED_LED, HIGH);
    toneBuzzer(400, 600);
    delay(1500);
    setLockStateHardware(isLocked);
  }
  http.end();
}

// --- SETUP ---
void setup() {
  Serial.begin(115200);
  
  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  
  lcd.init();
  lcd.backlight();
  
  updateUI("  SMART LOCK  ", "Initializing...");
  toneBuzzer(800, 150);
  delay(1500);

  WiFiManager wm;
  if (!wm.autoConnect("ESP32-SmartLock")) {
    updateUI("WiFi Failed", "Running Offline");
    delay(2000);
  } else {
    updateUI("WiFi Connected!", WiFi.localIP().toString());
    delay(2000);
  }

  SPI.begin(18, 19, 23, 5); 
  mfrc522.PCD_Init();
  
  myServo.attach(SERVO_PIN);
  setLockStateHardware(true);
}

// --- MAIN LOOP ---
void loop() {
  // 1. Timed Status Synchronizer
  unsigned long currentMillis = millis();
  if (currentMillis - lastDashboardCheck >= checkInterval) {
    lastDashboardCheck = currentMillis;
    syncDashboardStatus();
  }

  // 2. Handle Keypad Inputs
  char key = keypad.getKey();
  if (key) {
    toneBuzzer(1200, 50); 
    
    if (key == '#') { 
      if (inputPin.length() > 0) {
        verifyPinOnline(inputPin);
      }
      inputPin = "";
    } 
    else if (key == '*') { 
      inputPin = "";
      updateUI("Cleared", "");
      delay(800);
      setLockStateHardware(isLocked);
    } 
    else { 
      if (inputPin.length() < 8) {
        inputPin += key;
        String masked = "";
        for (int i = 0; i < inputPin.length(); i++) masked += "*";
        updateUI("Enter PIN:", masked);
      }
    }
  }

  // 3. Handle RFID Card Scanning
  if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
    String uidString = "";
    for (byte i = 0; i < mfrc522.uid.size; i++) {
      uidString += String(mfrc522.uid.uidByte[i] < 0x10 ? "0" : "");
      uidString += String(mfrc522.uid.uidByte[i], HEX);
    }
    uidString.toUpperCase();

    verifyRfidOnline(uidString);
    
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
  }
}