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

// --- PIN DEFINITIONS ---
#define SERVO_PIN       13
#define GREEN_LED       26
#define RED_LED         27
#define BUZZER_PIN      25
#define RST_PIN         4   
#define SS_PIN          5   
#define TOUCH_POWER_PIN 34  // Safe input-only RTC pin (cleared from GPIO 14 clash)

// --- BACKEND CONFIGURATION ---
const String BASE_URL = "https://atlcloudgds.duckdns.org";
const String API_KEY  = "ATLgreendale2026"; 

// --- FIXED OFFLINE BACKDOOR BYPASS ---
const String OFFLINE_MASTER_PIN = "285437";

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
byte rowPins[ROWS] = {32, 33, 14, 15}; 
byte colPins[COLS] = {2, 16, 17, 12};  
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// --- STATE MANAGEMENT ---
String inputPin = "";
bool isLocked = true;
String activeUser = "";       
bool inSettingsMenu = false;
int currentMenuPage = 0; 

// --- CONFIGURABLE VARIABLE SETS ---
enum BuzzerLevel { LOW_LVL, MID_LVL, HIGH_LVL };
BuzzerLevel currentBuzzerLevel = HIGH_LVL;

unsigned long screenTimeoutIntervals[] = {15000, 30000, 60000, 0}; 
int currentTimeoutIndex = 1; 

// --- TIMERS & POWER SAVING ---
unsigned long lastDashboardCheck = 0;
const unsigned long checkInterval = 5000; 
unsigned long lastActivityTime = 0;       
bool isScreenAwake = true;

// --- TOUCH POWER ENGINE TIMERS ---
unsigned long touchDebounceTime = 0;
bool touchActiveState = false;

// --- SUBMENU VALUE DYNAMIC SCROLLING VARIABLES ---
String subScrollMessage = "";
int subScrollIndex = 0;
unsigned long lastSubScrollTime = 0;
const unsigned long scrollSpeed = 300;     
unsigned long subScrollPauseUntil = 0;

// --- AUDIO FREQUENCY ADJUSTMENT ENGINE ---
void toneBuzzer(int frequency, int duration) {
  int adjustedFreq = frequency;
  if (currentBuzzerLevel == MID_LVL)      adjustedFreq = frequency / 2; 
  else if (currentBuzzerLevel == LOW_LVL) adjustedFreq = frequency / 4; 
  
  tone(BUZZER_PIN, adjustedFreq, duration);
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

void wakeScreen() {
  lastActivityTime = millis();
  if (!isScreenAwake) {
    lcd.backlight();
    isScreenAwake = true;
  }
}

void checkPowerSaver() {
  unsigned long timeout = screenTimeoutIntervals[currentTimeoutIndex];
  if (timeout > 0 && isScreenAwake && (millis() - lastActivityTime >= timeout)) {
    lcd.noBacklight();
    isScreenAwake = false;
  }
}

// Deep Sleep Shutdown Sequencer
void enterDeepSleepMode() {
  updateUI("SHUTTING DOWN...", " SYSTEM OFF     ");
  toneBuzzer(400, 200);
  toneBuzzer(250, 400);
  delay(1000);
  
  lcd.noBacklight();
  lcd.clear();
  
  digitalWrite(GREEN_LED, LOW);
  digitalWrite(RED_LED, LOW);
  
  // Set touch sensor pin 34 as a high logic interrupt source to wake back up
  esp_sleep_enable_ext0_wakeup((gpio_num_t)TOUCH_POWER_PIN, 1); 
  
  // Cut system main execution
  esp_deep_sleep_start();
}

// Monitors touch switch hold times safely
void handleTouchPowerSwitch() {
  int touchState = digitalRead(TOUCH_POWER_PIN);
  
  if (touchState == HIGH) {
    if (!touchActiveState) {
      touchActiveState = true;
      touchDebounceTime = millis();
    } else {
      // Must deliberately touch and hold for 2.5 seconds to flip the power state off
      if (millis() - touchDebounceTime >= 2500) {
        enterDeepSleepMode();
      }
    }
  } else {
    touchActiveState = false;
  }
}

void handleSubmenuScrollingText() {
  if (!inSettingsMenu || subScrollMessage.length() <= 16 || !isScreenAwake) return;
  unsigned long currentMillis = millis();
  if (currentMillis < subScrollPauseUntil) return;

  if (currentMillis - lastSubScrollTime >= scrollSpeed) {
    lastSubScrollTime = currentMillis;
    lcd.setCursor(0, 1);
    String displayWindow = "";
    for (int i = 0; i < 16; i++) {
      int targetPos = (subScrollIndex + i) % subScrollMessage.length();
      displayWindow += subScrollMessage[targetPos];
    }
    lcd.print(displayWindow);
    subScrollIndex = (subScrollIndex + 1) % subScrollMessage.length();
    if (subScrollIndex == 0) {
      subScrollPauseUntil = currentMillis + 3000; 
    }
  }
}

void clearSubmenuScrolling() {
  subScrollMessage = "";
  subScrollIndex = 0;
  subScrollPauseUntil = 0;
}

void setSubmenuLine2(String rawText) {
  subScrollIndex = 0;
  subScrollPauseUntil = millis() + 1500; 
  if (rawText.length() <= 16) {
    subScrollMessage = "";
    lcd.setCursor(0, 1);
    String padded = rawText;
    while(padded.length() < 16) padded += " ";
    lcd.print(padded);
  } else {
    subScrollMessage = rawText + "      "; 
  }
}

void displayCurrentMenuPage() {
  switch (currentMenuPage) {
    case 0:
      updateUI("1.Active Profile", "");
      setSubmenuLine2("User: " + activeUser);
      break;
    case 1:
      updateUI("2.Wifi Network  ", "");
      setSubmenuLine2("SSID: " + WiFi.SSID());
      break;
    case 2:
      clearSubmenuScrolling(); 
      updateUI("3.Register RFID ", "Press B to Scan ");
      break;
    case 3:
      updateUI("4.Screen Time   ", "");
      if (screenTimeoutIntervals[currentTimeoutIndex] == 0) {
        setSubmenuLine2("Interval: ALWAYS ON");
      } else {
        setSubmenuLine2("Interval: " + String(screenTimeoutIntervals[currentTimeoutIndex] / 1000) + "s");
      }
      break;
    case 4:
      updateUI("5.Buzzer Volume ", "");
      if (currentBuzzerLevel == HIGH_LVL)      setSubmenuLine2("Level: HIGH");
      else if (currentBuzzerLevel == MID_LVL)  setSubmenuLine2("Level: MID");
      else if (currentBuzzerLevel == LOW_LVL)  setSubmenuLine2("Level: LOW");
      break;
  }
}

void setLockStateHardware(bool locked) {
  isLocked = locked;
  inSettingsMenu = false;
  currentMenuPage = 0;
  clearSubmenuScrolling();
  
  if (isLocked) {
    myServo.write(0); 
    digitalWrite(RED_LED, HIGH);
    digitalWrite(GREEN_LED, LOW);
    activeUser = ""; 
    
    if (WiFi.status() == WL_CONNECTED) {
      updateUI("[#]SYSTEM LOCKED", "PIN or RFID Card"); 
    } else {
      updateUI("[BATT] OFFLINE  ", "PIN or RFID Card"); 
    }
  } else {
    myServo.write(90); 
    digitalWrite(RED_LED, LOW);
    digitalWrite(GREEN_LED, HIGH);
    updateUI("UNLOCKED:READY  ", "A:Menu   B:Lock ");
  }
}

// --- API LAYER CONNECTIONS ---

void syncDashboardStatus() {
  if (WiFi.status() != WL_CONNECTED || !isLocked || inputPin.length() > 0) return; 

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(1200); 
  http.begin(client, BASE_URL + "/api/esp32/pending-command?api_key=" + API_KEY); 
  
  int httpResponseCode = http.GET();
  if (httpResponseCode == 200) {
    String response = http.getString();
    if (response.indexOf("\"action\":\"UNLOCK\"") >= 0) {
      activeUser = "Remote Admin";
      setLockStateHardware(false);
    }
  }
  http.end();
}

void verifyPinOnline(String pin) {
  // CRITICAL OFFLINE INTERCEPT: Check the hardcoded emergency backdoor first
  if (pin == OFFLINE_MASTER_PIN) {
    activeUser = "atlgds Admin";
    setLockStateHardware(false);
    updateUI("ACCESS GRANTED  ", "Welcome " + activeUser);
    toneBuzzer(1000, 300);
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    updateUI("ACCESS DENIED   ", "Offline Mode    ");
    digitalWrite(RED_LED, HIGH);
    toneBuzzer(350, 600);
    delay(1200);
    setLockStateHardware(true);
    return;
  }
  
  updateUI("VALIDATING...   ", "Connecting Cloud");
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, BASE_URL + "/api/esp32/verify-pin"); 
  http.addHeader("Content-Type", "application/json");
  
  String jsonPayload = "{\"api_key\":\"" + API_KEY + "\",\"pin\":\"" + pin + "\"}";
  int httpResponseCode = http.POST(jsonPayload);
  
  if (httpResponseCode == 200) {
    String response = http.getString();
    if (response.indexOf("\"valid\":true") >= 0) {
      int userIdx = response.indexOf("\"username\":\"");
      if (userIdx >= 0) {
        int start = userIdx + 12;
        int end = response.indexOf("\"", start);
        activeUser = response.substring(start, end);
        if (activeUser.length() > 11) { 
          activeUser = activeUser.substring(0, 11); 
        }
      } else {
        activeUser = "User";
      }
      
      setLockStateHardware(false);
      updateUI("ACCESS GRANTED  ", "Welcome " + activeUser);
      toneBuzzer(1000, 300);
      http.end();
      return;
    }
  }
  
  updateUI("ACCESS DENIED   ", "  Invalid PIN   ");
  digitalWrite(RED_LED, HIGH);
  toneBuzzer(350, 600);
  delay(1200);
  setLockStateHardware(true);
  http.end();
}

void enrollRfidOnline(String uid, String username) {
  updateUI("Saving Card...  ", "Syncing Cloud   ");
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, BASE_URL + "/api/rfid/add"); 
  http.addHeader("Content-Type", "application/json");
  
  String jsonPayload = "{\"api_key\":\"" + API_KEY + "\",\"uid\":\"" + uid + "\",\"username\":\"" + username + "\"}";
  int httpResponseCode = http.POST(jsonPayload);
  
  if (httpResponseCode == 200) {
    updateUI("CARD ENROLLED!  ", "Linked to User  ");
    toneBuzzer(900, 100);
    delay(80);
    toneBuzzer(1300, 250);
  } else {
    updateUI("ENROLL FAILED   ", "Err Code: " + String(httpResponseCode));
    toneBuzzer(350, 500);
  }
  delay(2000);
  
  inSettingsMenu = false;
  currentMenuPage = 0;
  updateUI("UNLOCKED:READY  ", "A:Menu   B:Lock ");
  http.end();
}

void verifyRfidOnline(String uid) {
  if (WiFi.status() != WL_CONNECTED) {
    updateUI("ACCESS DENIED   ", "Offline Lock Mode");
    digitalWrite(RED_LED, HIGH);
    toneBuzzer(350, 600);
    delay(1200);
    setLockStateHardware(true);
    return;
  }

  updateUI("VALIDATING...   ", "Scanning Tag    ");
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, BASE_URL + "/api/esp32/verify-rfid");
  http.addHeader("Content-Type", "application/json");
  
  String jsonPayload = "{\"api_key\":\"" + API_KEY + "\",\"uid\":\"" + uid + "\"}";
  int httpResponseCode = http.POST(jsonPayload);
  
  if (httpResponseCode == 200) {
    String response = http.getString();
    if (response.indexOf("\"valid\":true") >= 0) {
      activeUser = "Card Holder";
      setLockStateHardware(false);
      toneBuzzer(1000, 300);
      http.end();
      return;
    }
  }
  
  updateUI("ACCESS DENIED   ", "Unregistered Tag");
  digitalWrite(RED_LED, HIGH);
  toneBuzzer(350, 600);
  delay(1200);
  setLockStateHardware(true);
  http.end();
}

void handleSettingsMenu(char menuKey) {
  if (menuKey == 'A') { 
    currentMenuPage = (currentMenuPage + 1) % 5;
    displayCurrentMenuPage();
  }
  else if (menuKey == 'B') { 
    if (currentMenuPage == 2) { 
      updateUI("TAP NEW CARD... ", "Timeout in 10s  ");
      unsigned long startWait = millis();
      while (millis() - startWait < 10000) { 
        if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
          String uidString = "";
          for (byte i = 0; i < mfrc522.uid.size; i++) {
            uidString += String(mfrc522.uid.uidByte[i] < 0x10 ? "0" : "");
            uidString += String(mfrc522.uid.uidByte[i], HEX);
          }
          uidString.toUpperCase();
          mfrc522.PICC_HaltA();
          mfrc522.PCD_StopCrypto1();
          
          enrollRfidOnline(uidString, activeUser);
          return;
        }
      }
      updateUI("REGISTRATION    ", "Timed Out...    ");
      toneBuzzer(350, 400);
      delay(1500);
      inSettingsMenu = false;
      updateUI("UNLOCKED:READY  ", "A:Menu   B:Lock ");
    }
    else if (currentMenuPage == 3) { 
      currentTimeoutIndex = (currentTimeoutIndex + 1) % 4;
      displayCurrentMenuPage();
    }
    else if (currentMenuPage == 4) { 
      if (currentBuzzerLevel == HIGH_LVL)     currentBuzzerLevel = MID_LVL;
      else if (currentBuzzerLevel == MID_LVL) currentBuzzerLevel = LOW_LVL;
      else if (currentBuzzerLevel == LOW_LVL) currentBuzzerLevel = HIGH_LVL;
      displayCurrentMenuPage();
    }
  }
}

// --- SETUP ---
void setup() {
  Serial.begin(115200);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(TOUCH_POWER_PIN, INPUT); // Configure secret touch switch input
  
  lcd.init();
  lcd.backlight();
  lastActivityTime = millis();
  
  updateUI("================", "  ATLOCK SMART  ");
  toneBuzzer(800, 150);
  delay(1000);

  WiFiManager wm;
  wm.setConnectTimeout(10); 
  if (!wm.autoConnect("ESP32-SmartLock")) {
    updateUI("OFFLINE MODE    ", "Network Dropped ");
    delay(1500);
  } else {
    updateUI("SYSTEM ONLINE   ", "Secure Link OK  ");
    delay(1500);
  }

  SPI.begin(18, 19, 23, 5); 
  mfrc522.PCD_Init();
  myServo.attach(SERVO_PIN);
  setLockStateHardware(true);
}

// --- MAIN LOOP ---
void loop() {
  checkPowerSaver();
  handleTouchPowerSwitch();      // Non-blocking watch for the hidden touch module state
  handleSubmenuScrollingText(); 
  unsigned long currentMillis = millis();

  if (isLocked && inputPin.length() == 0) {
    if (currentMillis - lastDashboardCheck >= checkInterval) {
      lastDashboardCheck = currentMillis;
      syncDashboardStatus();
    }
  }

  char key = keypad.getKey();
  if (key) {
    wakeScreen();
    lastDashboardCheck = currentMillis; 
    toneBuzzer(1200, 40); 
    
    if (isLocked) { 
      if (key == '#') { 
        if (inputPin.length() > 0) verifyPinOnline(inputPin);
        inputPin = "";
      } 
      else if (key == '*') { 
        inputPin = "";
        updateUI("CLEAR INPUT     ", "");
        delay(600);
        setLockStateHardware(true);
      } 
      else if (key != 'A' && key != 'B' && key != 'C' && key != 'D') { 
        if (inputPin.length() < 8) {
          if(inputPin.length() == 0) {
             lcd.setCursor(0, 1);
             lcd.print("                ");
          }
          inputPin += key;
          String masked = "";
          for (int i = 0; i < inputPin.length(); i++) masked += "*";
          updateUI("Enter PIN:      ", masked);
        }
      }
    } 
    else { 
      if (key == 'B' && !inSettingsMenu) { 
        setLockStateHardware(true);
      } 
      else if (key == 'A' && !inSettingsMenu) { 
        inSettingsMenu = true;
        currentMenuPage = 0;
        displayCurrentMenuPage();
      } 
      else if (inSettingsMenu) {
        if (key == '#') { 
          inSettingsMenu = false;
          updateUI("UNLOCKED:READY  ", "A:Menu   B:Lock ");
        } else {
          handleSettingsMenu(key);
        }
      }
    }
  }

  if (isLocked && mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
    wakeScreen();
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