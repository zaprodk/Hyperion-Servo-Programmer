#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "secrets.h"

// ---------------------------------------------------------
// ESP32 D1 Mini Pin Definitions & Serial Setup
// ---------------------------------------------------------
#define RX_PIN 16  // Labeled "16" or "RX2" -> Connect trough 100 Ohm resistor to Servo Signal Wire
#define TX_PIN 17  // Labeled "17" or "TX2" -> Connect to 1N4004 diode cathode (banded side) -> Anode to Servo Signal Wire
                   // Add a 22 Kohm pull-up resistor from Servo Signal Wire to 3.3/5V to ensure proper idle state
#define LED_PIN 2  // Onboard Blue LED on ESP32 D1 Mini (GPIO 2)

WebServer server(80);

// Servo State Variables
String currentFwRev = "ATLAS1.2"; // Dynamically parsed from servo
uint16_t currentCenter = 1500;
uint8_t currentThrow = 50;
uint8_t currentSpeed = 100;
uint8_t currentTxMode = 0; // Microsecond offset (0 = Standard/JR/Hitec, 22 = Futaba, 100 = Multiplex)
bool currentReverse = false;
uint8_t currentDeadband = 2;

// ---------------------------------------------------------
// Helper: Universal ASCII Checksum Calculation
// ---------------------------------------------------------
uint8_t calculateChecksum(const char *payload24) {
  uint16_t asciiSum = 0;
  for (int i = 0; i < 24; i++) {
    asciiSum += (uint8_t)payload24[i];
  }
  return (uint8_t)((asciiSum + 6) & 0xFF);
}

// ---------------------------------------------------------
// Helper: Build Exact 36-Byte Command Frame
// ---------------------------------------------------------
String buildPacket(uint16_t pulseUs, uint8_t totalThrow, uint8_t speedPct, uint8_t txMode, bool rev, uint8_t deadbandUs) {
  char payload[25];
  sprintf(payload, "%04X%02X%02X%02X%02X%02X001D4C0514",
          pulseUs, totalThrow, speedPct, txMode, rev ? 1 : 0, deadbandUs);

  uint8_t chk = calculateChecksum(payload);

  char fullStr[64];
  // Standard 36-byte frame: \x0C + ATLAS1.2 + 24 payload chars + 2 checksum chars + \r
  sprintf(fullStr, "\x0C%s%s%02X\r", currentFwRev.c_str(), payload, chk);
  return String(fullStr);
}

// ---------------------------------------------------------
// Web Server Frontend (HTML/CSS/JS in PROGMEM)
// ---------------------------------------------------------
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>Hyperion Servo Commander</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: 'Segoe UI', Tahoma, sans-serif; background-color: #d4d0c8; padding: 20px; user-select: none; }
    .container { max-width: 600px; margin: auto; border: 2px solid #808080; border-top-color: #fff; border-left-color: #fff; background: #ece9d8; padding: 10px; }
    .header-panel { display: flex; align-items: center; justify-content: space-between; border: 1px solid #808080; background: #e0dcd3; padding: 10px; margin-bottom: 10px; }
    .status-indicator { width: 20px; height: 20px; border-radius: 50%; background: red; border: 2px inset #999; margin-right: 15px; }
    .status-indicator.connected { background: #00ff00; }
    .panel-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; }
    .group-box { border: 2px groove #fff; padding: 10px; }
    .group-box legend { color: #000; font-weight: bold; font-size: 14px; }
    fieldset { margin: 0; }
    
    .slider-container { text-align: center; margin-top: 10px; }
    input[type=range] { width: 100%; cursor: pointer; }
    .val-box { border: 1px inset #fff; background: #fff; color: #000080; padding: 2px 5px; font-family: monospace; display: inline-block; min-width: 40px; text-align: right; }
    
    .btn { background: #ece9d8; border: 2px outset #fff; padding: 5px 15px; cursor: pointer; font-size: 12px; font-weight: bold; }
    .btn:active { border-style: inset; padding: 6px 14px 4px 16px; }
    .btn-connect { width: 80px; height: 35px; }
    
    .side-controls { display: flex; flex-direction: column; gap: 10px; justify-content: space-between; }
    .logo { color: #003366; font-size: 24px; font-style: italic; font-weight: 900; letter-spacing: 1px; margin: 0; }
  </style>
</head>
<body>
<div class="container">
  <div class="header-panel">
    <div>
      <button class="btn btn-connect" onclick="connectServo()">Connect</button>
    </div>
    <div style="display: flex; align-items: center; border: 1px inset #fff; padding: 5px 15px; background: #ece9d8;">
      <div id="statusLed" class="status-indicator"></div>
      <div style="text-align: center; font-size: 12px;">
        Hyperion Digital Servo<br>Firmware Revision<br><b id="fwRevText" style="color: #0066cc; font-size: 16px;">---</b>
      </div>
    </div>
    <div>
      <h1 class="logo">HYPERION<br><span style="font-size:12px; color:#555;">Quality R/C</span></h1>
    </div>
  </div>

  <div class="panel-grid">
    <fieldset class="group-box">
      <legend>Right/Left Deflection (degrees)</legend>
      <div class="slider-container">
        <span class="val-box" id="throwVal">50</span>
        <input type="range" id="throwSlider" min="17" max="78" value="50" oninput="updateThrow(this.value)">
      </div>
    </fieldset>

    <fieldset class="group-box">
      <legend>Servo Center (1500+/-127 uS)</legend>
      <div class="slider-container">
        <span class="val-box" id="centerVal">1500</span>
        <input type="range" id="centerSlider" min="1373" max="1627" value="1500" oninput="updateVal('centerVal', this.value)">
      </div>
    </fieldset>

    <fieldset class="group-box">
      <legend>Speed (1 - 100 % of maximum)</legend>
      <div class="slider-container">
        <span class="val-box" id="speedVal">100</span>
        <input type="range" id="speedSlider" min="1" max="100" value="100" oninput="updateVal('speedVal', this.value)">
      </div>
    </fieldset>

    <fieldset class="group-box">
      <legend>Deadband Width (0 - 16 uS)</legend>
      <div class="slider-container">
        <span class="val-box" id="dbVal">2</span>
        <input type="range" id="dbSlider" min="0" max="16" value="2" oninput="updateVal('dbVal', this.value)">
      </div>
    </fieldset>

    <fieldset class="group-box">
      <legend>Test Servo Deflection</legend>
      <div class="slider-container">
        <span class="val-box" id="testVal">0</span>
        <input type="range" id="testSlider" min="-50" max="50" value="0" oninput="updateVal('testVal', this.value)">
      </div>
    </fieldset>

    <div class="side-controls">
      <fieldset class="group-box" style="text-align: center;">
        <label><input type="checkbox" id="revCheck"> <b>Reverse</b></label>
      </fieldset>
      
      <fieldset class="group-box" style="text-align: center;">
        <label style="font-size: 12px; font-weight: bold;">Tx Compatibility<br>
          <select id="txSelect" onchange="onTxChange()">
            <option value="0" data-offset="0" data-throw="50">Standard</option>
            <option value="22" data-offset="22" data-throw="45">Futaba</option>
            <option value="0_hitec" data-offset="0" data-throw="55">Hitec</option>
            <option value="0_jr" data-offset="0" data-throw="45">JR</option>
            <option value="100" data-offset="100" data-throw="55">Multiplex</option>
          </select>
        </label>
      </fieldset>

      <fieldset class="group-box" style="text-align: center; background: #e0dcd3;">
        <label style="font-size: 11px;"><input type="checkbox" id="resetCheck"> Reset to Default</label>
        <button class="btn" style="width: 90%; margin-top: 5px;" onclick="programServo()">PROGRAM</button>
      </fieldset>
    </div>
  </div>

  <div style="margin-top: 15px; text-align: center; border-top: 1px solid #808080; padding-top: 10px;">
    <b style="font-size: 12px; color: #333;">Multiplex USB Adapter Wiring Diagram:</b><br>
    <img src="https://wiki.rc-network.de/w/images/0/04/Mpxprog.png" style="width: 100%; max-width: 550px; height: auto; margin-top: 8px; border: 1px solid #808080; background: #fff;" alt="Multiplex Adapter Schematic"><br>
    <a href="https://wiki.rc-network.de/wiki/Multiplex_USB-Adapter" target="_blank" style="font-size: 11px; color: #003399; text-decoration: underline; display: inline-block; margin-top: 5px;">Multiplex USB-Adapter Wiki Page</a>
  </div>
</div>

<script>
  function updateVal(id, val) { document.getElementById(id).innerText = val; }

  function updateThrow(val) {
    let throwDeg = parseInt(val) || 50;
    document.getElementById('throwVal').innerText = throwDeg;
    
    let tSlider = document.getElementById('testSlider');
    tSlider.min = -throwDeg;
    tSlider.max = throwDeg;
    
    if (Math.abs(parseInt(tSlider.value)) > throwDeg) {
      tSlider.value = 0;
      document.getElementById('testVal').innerText = 0;
    }
  }

  function onTxChange() {
    let sel = document.getElementById('txSelect');
    let opt = sel.options[sel.selectedIndex];
    let offset = parseInt(opt.getAttribute('data-offset')) || 0;
    let defThrow = parseInt(opt.getAttribute('data-throw')) || 50;

    let newCenter = 1500 + offset;
    document.getElementById('centerSlider').value = newCenter;
    document.getElementById('centerVal').innerText = newCenter;

    document.getElementById('throwSlider').value = defThrow;
    updateThrow(defThrow);
  }

  function connectServo() {
    fetch('/connect')
      .then(r => r.json())
      .then(data => {
        if(data.success) {
          document.getElementById('statusLed').classList.add('connected');
          if(data.fw) document.getElementById('fwRevText').innerText = data.fw;
          document.getElementById('centerSlider').value = data.center;
          document.getElementById('centerVal').innerText = data.center;
          document.getElementById('speedSlider').value = data.speed;
          document.getElementById('speedVal').innerText = data.speed;
          document.getElementById('dbSlider').value = data.db;
          document.getElementById('dbVal').innerText = data.db;
          document.getElementById('revCheck').checked = data.rev;
          
          document.getElementById('throwSlider').value = data.throw;
          updateThrow(data.throw);

          let selectVal = "0";
          if (data.tx === 22) {
            selectVal = "22";
          } else if (data.tx === 100) {
            selectVal = "100";
          } else if (data.tx === 0) {
            if (data.throw === 55) selectVal = "0_hitec";
            else if (data.throw === 45) selectVal = "0_jr";
            else selectVal = "0";
          }
          document.getElementById('txSelect').value = selectVal;
        }
      });
  }

  function programServo() {
    let sel = document.getElementById('txSelect');
    let opt = sel.options[sel.selectedIndex];
    let txOffset = parseInt(opt.getAttribute('data-offset')) || 0;

    let params = new URLSearchParams({
      center: document.getElementById('centerSlider').value,
      throw: document.getElementById('throwSlider').value,
      speed: document.getElementById('speedSlider').value,
      db: document.getElementById('dbSlider').value,
      rev: document.getElementById('revCheck').checked ? 1 : 0,
      tx: txOffset,
      reset: document.getElementById('resetCheck').checked ? 1 : 0,
      testDeg: document.getElementById('testSlider').value
    });
    fetch('/program?' + params.toString())
      .then(r => r.text())
      .then(t => {
        alert("Program sent!");
        document.getElementById('resetCheck').checked = false;
        connectServo();
      });
  }
</script>
</body>
</html>
)rawliteral";

// ---------------------------------------------------------
// Web Server Route Handlers
// ---------------------------------------------------------
void handleRoot() {
  server.send_P(200, "text/html", index_html);
}

void handleConnect() {
  digitalWrite(LED_PIN, HIGH);
  
  while(Serial1.available()) Serial1.read();
  
  Serial1.write('U');
  Serial1.flush();
  Serial.println("Sent query byte: U (0x55)");

  String response = "";
  unsigned long start = millis();
  
  while(millis() - start < 500) {
    if (Serial1.available()) {
      char c = Serial1.read();
      response += c;
    }
  }
  
  digitalWrite(LED_PIN, LOW);

  int atlasIdx = response.indexOf("ATLAS");
  if (atlasIdx != -1 && response.length() >= (unsigned int)(atlasIdx + 8 + 24)) {
    currentFwRev = response.substring(atlasIdx, atlasIdx + 8);
    String payload = response.substring(atlasIdx + 8, atlasIdx + 8 + 24);
    Serial.println("Parsed FW Rev: " + currentFwRev);
    Serial.println("Parsed Payload: " + payload);
    
    long cHex = strtol(payload.substring(0,4).c_str(), NULL, 16);
    long tHex = strtol(payload.substring(4,6).c_str(), NULL, 16);
    long sHex = strtol(payload.substring(6,8).c_str(), NULL, 16);
    long mHex = strtol(payload.substring(8,10).c_str(), NULL, 16);
    long rHex = strtol(payload.substring(10,12).c_str(), NULL, 16);
    long dHex = strtol(payload.substring(12,14).c_str(), NULL, 16);
    
    currentCenter = cHex;
    currentThrow = constrain((int)tHex, 17, 78);
    currentSpeed = sHex;
    currentTxMode = mHex;
    currentReverse = (rHex == 1);
    currentDeadband = dHex;

    String json = "{\"success\":true, \"fw\":\"" + currentFwRev + 
                  "\", \"center\":" + String(currentCenter) + 
                  ", \"throw\":" + String(currentThrow) + 
                  ", \"speed\":" + String(currentSpeed) + 
                  ", \"tx\":" + String(currentTxMode) + 
                  ", \"rev\":" + (currentReverse ? "true" : "false") + 
                  ", \"db\":" + String(currentDeadband) + "}";
    server.send(200, "application/json", json);
  } else {
    server.send(200, "application/json", "{\"success\":false}");
  }
}

void handleProgram() {
  digitalWrite(LED_PIN, HIGH);
  bool isReset = server.arg("reset") == "1";
  int testDeg = server.arg("testDeg").toInt();
  
  if (isReset) {
    currentCenter = 1500;
    currentThrow = 50;
    currentSpeed = 100;
    currentTxMode = 0;
    currentReverse = false;
    currentDeadband = 2;
    testDeg = 0;
  } else {
    currentCenter = server.arg("center").toInt();
    currentThrow = constrain((int)server.arg("throw").toInt(), 17, 78);
    currentSpeed = server.arg("speed").toInt();
    currentTxMode = server.arg("tx").toInt();
    currentReverse = server.arg("rev") == "1";
    currentDeadband = server.arg("db").toInt();
  }

  testDeg = constrain(testDeg, -((int)currentThrow), (int)currentThrow);

  // Write base parameters to EEPROM
  String packet = buildPacket(currentCenter, currentThrow, currentSpeed, currentTxMode, currentReverse, currentDeadband);
  
  while(Serial1.available()) Serial1.read();
  
  Serial.print("Writing EEPROM: ");
  Serial.println(packet);
  
  Serial1.print(packet);
  Serial1.flush();
  
  delay(300); // EEPROM commit delay
  
  while(Serial1.available()) Serial1.read();
  
  // Apply test deflection offset position if non-zero
  if (testDeg != 0) {
    uint16_t testPulse = currentCenter + (testDeg * 10);
    String testPacket = buildPacket(testPulse, currentThrow, currentSpeed, currentTxMode, currentReverse, currentDeadband);
    Serial.print("Applying Test Position: ");
    Serial.println(testPacket);
    Serial1.print(testPacket);
    Serial1.flush();
    delay(50);
    while(Serial1.available()) Serial1.read();
  }
  
  digitalWrite(LED_PIN, LOW);
  server.send(200, "text/plain", "OK");
}

// ---------------------------------------------------------
// Main Setup and Loop
// ---------------------------------------------------------
void setup() {
  Serial.begin(115200);
  
  Serial1.begin(19200, SERIAL_8N1, RX_PIN, TX_PIN);
  
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Serial.print("\nConnecting to WiFi: ");
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("\nWiFi Connected!");
  Serial.print("Hyperion Web UI Address: http://");
  Serial.println(WiFi.localIP());

  server.on("/", handleRoot);
  server.on("/connect", handleConnect);
  server.on("/program", handleProgram);
  server.begin();
}

void loop() {
  server.handleClient();
  
  if (Serial1.available()) {
    Serial.write(Serial1.read());
  }
}