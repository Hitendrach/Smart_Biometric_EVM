#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>
#include <WiFi.h>
#include <HTTPClient.h>

// Smart Biometric EVM - Wokwi software simulation
// Fingerprint verification is simulated; no biometric sensor is connected.

LiquidCrystal_I2C lcd(0x27, 16, 2);
Preferences db;

const int MAX_VOTES = 100;

// ThingSpeak cloud dashboard configuration.
// Create a ThingSpeak channel with fields 1-4 and paste its WRITE API KEY here.
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASSWORD = "";
const char* THINGSPEAK_WRITE_KEY = "";
const unsigned long CLOUD_UPDATE_INTERVAL_MS = 16000; // ThingSpeak free tier: ~15s minimum
unsigned long lastCloudUpdate = 0;
bool cloudUpdatePending = false;

// Button GPIOs (wired from each button to GND; use INPUT_PULLUP)
const int BTN_VERIFY = 13;
const int BTN_A       = 14;
const int BTN_B       = 26;
const int BTN_C       = 32;
const int BTN_CONFIRM = 27;
const int BTN_SAVE    = 25;
const int BTN_RESET   = 33;

String voterId = "";
String selectedCandidate = "";
bool fingerprintVerified = false;
bool voteConfirmed = false;
String inputLine = "";
int voteCount = 0;

// Button debounce/edge detection
const unsigned long DEBOUNCE_MS = 45;
int lastReading[7] = {HIGH, HIGH, HIGH, HIGH, HIGH, HIGH, HIGH};
int stableState[7] = {HIGH, HIGH, HIGH, HIGH, HIGH, HIGH, HIGH};
unsigned long lastChange[7] = {0, 0, 0, 0, 0, 0, 0};
const int buttonPins[7] = {
  BTN_VERIFY, BTN_A, BTN_B, BTN_C, BTN_CONFIRM, BTN_SAVE, BTN_RESET
};

String voteKey(int index) {
  return "v" + String(index);
}

String getSavedVote(int index) {
  return db.getString(voteKey(index).c_str(), "");
}

String getSavedVoterId(const String &record) {
  int separator = record.indexOf('|');
  if (separator < 0) return "";
  return record.substring(0, separator);
}

String getSavedCandidate(const String &record) {
  int first = record.indexOf('|');
  int second = record.indexOf('|', first + 1);
  if (first < 0 || second < 0) return "";
  return record.substring(first + 1, second);
}

String getSavedReceipt(const String &record) {
  int separator = record.lastIndexOf('|');
  if (separator < 0) return "";
  return record.substring(separator + 1);
}

bool voterHasVoted(const String &id) {
  for (int i = 0; i < voteCount; i++) {
    String record = getSavedVote(i);
    if (getSavedVoterId(record) == id) return true;
  }
  return false;
}

void showLCD(String line1, String line2) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1.substring(0, 16));
  lcd.setCursor(0, 1);
  lcd.print(line2.substring(0, 16));
}

void getVoteTotals(int &countA, int &countB, int &countC) {
  countA = 0;
  countB = 0;
  countC = 0;

  for (int i = 0; i < voteCount; i++) {
    String candidate = getSavedCandidate(getSavedVote(i));
    if (candidate == "A") countA++;
    else if (candidate == "B") countB++;
    else if (candidate == "C") countC++;
  }
}

void connectWiFi() {
  Serial.println();
  Serial.println("Connecting to Wokwi-GUEST WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD, 6);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(250);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.println("WiFi connected.");
    Serial.print("ESP32 IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println();
    Serial.println("WiFi not connected yet; voting will still work locally.");
  }
}

void publishCloudMetrics() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Cloud update skipped: WiFi disconnected.");
    cloudUpdatePending = true;
    return;
  }

  if (String(THINGSPEAK_WRITE_KEY) == "YOUR_WRITE_API_KEY" ||
      String(THINGSPEAK_WRITE_KEY).length() == 0) {
    Serial.println("Cloud dashboard not configured: add your ThingSpeak WRITE API KEY.");
    cloudUpdatePending = false;
    return;
  }

  int countA, countB, countC;
  getVoteTotals(countA, countB, countC);

  lastCloudUpdate = millis(); // throttle retries as well as successful updates
  WiFiClient client;
  HTTPClient http;

  String url = "http://api.thingspeak.com/update?api_key=";
  url += THINGSPEAK_WRITE_KEY;
  url += "&field1=" + String(voteCount);
  url += "&field2=" + String(countA);
  url += "&field3=" + String(countB);
  url += "&field4=" + String(countC);

  http.setTimeout(5000);
  if (http.begin(client, url)) {
    int httpCode = http.GET();
    String response = http.getString();

    if (httpCode == HTTP_CODE_OK && response.toInt() > 0) {
      Serial.println("Cloud dashboard updated successfully.");
      Serial.println("ThingSpeak entry: " + response);
      cloudUpdatePending = false;
      lastCloudUpdate = millis();
    } else {
      Serial.printf("ThingSpeak update failed (HTTP %d), response: %s\n",
                    httpCode, response.c_str());
      // Retry later, especially if ThingSpeak rate-limited the request.
      cloudUpdatePending = true;
    }

    http.end();
  } else {
    Serial.println("Could not start ThingSpeak HTTP request.");
    cloudUpdatePending = true;
  }
}

void serviceCloudUpdate() {
  if (!cloudUpdatePending) return;

  if (lastCloudUpdate == 0 ||
      millis() - lastCloudUpdate >= CLOUD_UPDATE_INTERVAL_MS) {
    publishCloudMetrics();
  }
}

void printHelp() {
  Serial.println();
  Serial.println("===== SMART BIOMETRIC EVM =====");
  Serial.println("Serial commands:");
  Serial.println("ID VOTER001  - Enter voter ID");
  Serial.println("VERIFY       - Verify voter (or press VERIFY)");
  Serial.println("SELECT A/B/C - Select candidate via Serial");
  Serial.println("CONFIRM      - Confirm (or press CONFIRM)");
  Serial.println("SAVE         - Save (or press SAVE)");
  Serial.println("RESET        - Reset session (or press RESET)");
  Serial.println("RESULTS      - Candidate-wise totals");
  Serial.println("HELP         - Show this help");
  Serial.println("Cloud metrics: ThingSpeak fields 1-4 (total, A, B, C)");
  Serial.println("Buttons: VERIFY, A, B, C, CONFIRM, SAVE, RESET");
  Serial.println("================================");
}

void resetSession() {
  voterId = "";
  selectedCandidate = "";
  fingerprintVerified = false;
  voteConfirmed = false;
  showLCD("SMART EVM READY", "Enter ID in Serial");
  Serial.println();
  Serial.println("SESSION RESET. Saved votes are retained.");
  Serial.println("Enter next voter using: ID VOTER002");
}

void verifyVoter() {
  if (voterId.length() == 0) {
    Serial.println("ERROR: Enter ID in Serial Monitor first: ID VOTER001");
    showLCD("ENTER VOTER ID", "Use Serial Monitor");
    return;
  }

  // Software simulation of fingerprint verification.
  if (voterHasVoted(voterId)) {
    fingerprintVerified = false;
    voteConfirmed = false;
    selectedCandidate = "";
    Serial.println("DUPLICATE ID: VOTE REJECTED.");
    Serial.println("This ID already has a saved vote.");
    showLCD("DUPLICATE ID!", "Vote rejected");
    return;
  }

  fingerprintVerified = true;
  voteConfirmed = false;
  selectedCandidate = "";
  Serial.println("SIMULATED FINGERPRINT: VERIFIED");
  Serial.println("UNIQUE ID CHECK: PASSED");
  Serial.println("Select candidate A, B, or C.");
  showLCD("VOTER VERIFIED", "Select A / B / C");
}

void selectCandidate(String candidate) {
  candidate.toUpperCase();
  candidate.trim();

  if (!fingerprintVerified) {
    Serial.println("ERROR: Verify the voter first.");
    return;
  }
  if (voteConfirmed) {
    Serial.println("ERROR: Vote is already confirmed. Save or reset.");
    return;
  }
  if (candidate != "A" && candidate != "B" && candidate != "C") {
    Serial.println("ERROR: Candidate must be A, B, or C.");
    return;
  }

  selectedCandidate = candidate;
  Serial.println("Selected Candidate " + candidate);
  showLCD("SELECTED: " + candidate, "Press CONFIRM");
}

void confirmVote() {
  if (!fingerprintVerified) {
    Serial.println("ERROR: Verify the voter first.");
    return;
  }
  if (selectedCandidate.length() == 0) {
    Serial.println("ERROR: Select a candidate first.");
    return;
  }
  // Recheck the ID before confirmation.
  if (voterHasVoted(voterId)) {
    fingerprintVerified = false;
    voteConfirmed = false;
    selectedCandidate = "";
    Serial.println("DUPLICATE ID: VOTE REJECTED.");
    showLCD("DUPLICATE ID!", "Vote rejected");
    return;
  }

  voteConfirmed = true;
  Serial.println("VOTE CONFIRMED: Candidate " + selectedCandidate);
  Serial.println("Press SAVE to store the vote.");
  showLCD("VOTE CONFIRMED", "Press SAVE");
}

void saveVote() {
  if (!fingerprintVerified || !voteConfirmed ||
      selectedCandidate.length() == 0 || voterId.length() == 0) {
    Serial.println("ERROR: Verify, select, and confirm before saving.");
    return;
  }

  // Final duplicate check immediately before saving.
  if (voterHasVoted(voterId)) {
    Serial.println("DUPLICATE ID: VOTE NOT SAVED.");
    showLCD("DUPLICATE ID!", "Vote rejected");
    return;
  }

  if (voteCount >= MAX_VOTES) {
    Serial.println("ERROR: Maximum vote capacity reached.");
    showLCD("STORAGE FULL", "Contact admin");
    return;
  }

  int nextIndex = voteCount;
  String receipt = "EVM-" + String(nextIndex + 1);
  String record = voterId + "|" + selectedCandidate + "|" + receipt;

  // Write the record first, then advance the count.
  size_t written = db.putString(voteKey(nextIndex).c_str(), record);
  if (written == 0) {
    Serial.println("ERROR: Could not write vote record.");
    showLCD("SAVE FAILED", "Try again");
    return;
  }

  db.putInt("count", nextIndex + 1);
  voteCount = nextIndex + 1;
  cloudUpdatePending = true; // Push latest totals to cloud dashboard

  Serial.println();
  Serial.println("===== VOTE SAVED SUCCESSFULLY =====");
  Serial.println("Voter ID: " + voterId);
  Serial.println("Candidate: " + selectedCandidate);
  Serial.println("Receipt ID: " + receipt);
  Serial.println("===================================");
  showLCD("VOTE SAVED!", receipt);

  // Clear the active session only; keep all saved votes.
  voterId = "";
  selectedCandidate = "";
  fingerprintVerified = false;
  voteConfirmed = false;
  Serial.println("Ready for next voter. Enter: ID VOTER002");
}

void showResults() {
  int countA, countB, countC;
  getVoteTotals(countA, countB, countC);

  Serial.println();
  Serial.println("===== VOTE RESULTS =====");
  Serial.printf("Candidate A: %d\n", countA);
  Serial.printf("Candidate B: %d\n", countB);
  Serial.printf("Candidate C: %d\n", countC);
  Serial.printf("Total votes: %d\n", voteCount);
  Serial.println("========================");
  showLCD("TOTAL VOTES:" + String(voteCount),
          "A:" + String(countA) + " B:" + String(countB) +
          " C:" + String(countC));
}

void processCommand(String command) {
  command.trim();
  if (command.length() == 0) return;

  String upper = command;
  upper.toUpperCase();

  if (upper == "HELP") { printHelp(); return; }
  if (upper == "VERIFY") { verifyVoter(); return; }
  if (upper == "CONFIRM") { confirmVote(); return; }
  if (upper == "SAVE") { saveVote(); return; }
  if (upper == "RESET") { resetSession(); return; }
  if (upper == "RESULTS") { showResults(); return; }

  if (upper.startsWith("ID ")) {
    String id = command.substring(3);
    id.trim();
    if (id.length() == 0 || id.length() > 40 || id.indexOf('|') >= 0) {
      Serial.println("ERROR: Invalid ID. Use 1-40 characters, no | character.");
      return;
    }
    voterId = id;
    selectedCandidate = "";
    fingerprintVerified = false;
    voteConfirmed = false;
    Serial.println("Voter ID entered: " + voterId);
    Serial.println("Press VERIFY or type VERIFY.");
    showLCD("ID ENTERED", "Press VERIFY");
    return;
  }

  if (upper.startsWith("SELECT ")) {
    selectCandidate(upper.substring(7));
    return;
  }

  Serial.println("Unknown command. Type HELP.");
}

void handleButtonPress(int buttonIndex) {
  switch (buttonIndex) {
    case 0: verifyVoter(); break;
    case 1: selectCandidate("A"); break;
    case 2: selectCandidate("B"); break;
    case 3: selectCandidate("C"); break;
    case 4: confirmVote(); break;
    case 5: saveVote(); break;
    case 6: resetSession(); break;
  }
}

void scanButtons() {
  unsigned long now = millis();

  for (int i = 0; i < 7; i++) {
    int reading = digitalRead(buttonPins[i]);

    if (reading != lastReading[i]) {
      lastChange[i] = now;
      lastReading[i] = reading;
    }

    if ((now - lastChange[i]) > DEBOUNCE_MS &&
        reading != stableState[i]) {
      stableState[i] = reading;

      // Button is active-low; act once when pressed.
      if (stableState[i] == LOW) {
        handleButtonPress(i);
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  for (int i = 0; i < 7; i++) {
    pinMode(buttonPins[i], INPUT_PULLUP);
  }

  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();

  db.begin("smart-evm", false);
  voteCount = db.getInt("count", 0);

  if (voteCount < 0 || voteCount > MAX_VOTES) {
    voteCount = 0;
    db.putInt("count", 0);
  }

  connectWiFi();
  cloudUpdatePending = true; // Publish initial totals when dashboard key is configured

  showLCD("SMART BIOMETRIC", "EVM SIMULATOR");
  Serial.println();
  Serial.println("SMART BIOMETRIC EVM STARTED");
  Serial.println("Fingerprint: simulated (no hardware sensor)");
  Serial.println("Storage: ESP32 Preferences / NVS");
  Serial.printf("Previously saved votes: %d\n", voteCount);
  printHelp();
  delay(1200);
  showLCD("SMART EVM READY", "Enter ID in Serial");
}

void loop() {
  scanButtons();
  serviceCloudUpdate();

  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (inputLine.length() > 0) {
        processCommand(inputLine);
        inputLine = "";
      }
    } else if (inputLine.length() < 100) {
      inputLine += c;
    }
  }
}