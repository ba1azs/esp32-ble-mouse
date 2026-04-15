#include <BleCompositeHID.h>
#include <KeyboardDevice.h>
#include <MouseDevice.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include "esp_sleep.h"
#include <Wire.h>
#include <RTClib.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Preferences.h>
#include <math.h>
#include "esp_bt.h"

enum KeyboardLayout {
  LAYOUT_US,
  LAYOUT_DE,
  LAYOUT_HU
};

KeyboardLayout activeLayout = LAYOUT_DE;


WebServer server(80);
RTC_DS3231 rtc;
Adafruit_MPU6050 mpu;
Preferences prefs;
KeyboardDevice* keyboard = nullptr;
MouseDevice* mouse = nullptr;
BleCompositeHID compositeHID("CompositeHID Keyboard and Mouse", "Mystfit", 100);
// ---------------- SETTINGS ----------------

const unsigned long hotspotStopDelay = 30000;
unsigned long jiggleInterval = 60000; // 60 seconds, configurable

const char* apSSID = "Magic Mouse";
const char* apPassword = "";

// I2C pins
const int SDA_PIN = 8;
const int SCL_PIN = 9;

// I2C addresses
const uint8_t MPU_ADDR = 0x69;

// Vibration motor pin
const int VIBRATION_PIN = 4;

// GPIO list for webpage status
const int gpioList[] = {0, 1, 2, 3, 5, 6, 7, 10, 20, 21};
const int gpioCount = sizeof(gpioList) / sizeof(gpioList[0]);

// MPU6050 calibration
const float STABLE_ACCEL_DELTA = 0.08f;
const float STABLE_GYRO_DELTA  = 0.03f;
const int   STABLE_SAMPLES     = 40;
const int   CAL_SAMPLES        = 300;

// Gyro mouse control
const float GYRO_MOUSE_DEADZONE = 0.06f;
const float GYRO_MOUSE_SCALE    = 18.0f;
const unsigned long gyroMoveInterval = 12;

// Head-down detection
const float HEAD_DOWN_Z_THRESHOLD = -7.0f;

// ---------------- STATE ----------------

unsigned long lastJiggle = 0;
unsigned long lastGyroMove = 0;
unsigned long normalOrientationSince = 0;

bool apStarted = false;
bool rtcAvailable = false;
bool mpuAvailable = false;
bool gyroMouseEnabled = false;
bool jiggleEnabled = true; // default ON

// Default / current MPU offsets
float accelOffsetX = 0.8085f;
float accelOffsetY = 0.1241f;
float accelOffsetZ = 0.1855f;

float gyroOffsetX  = -0.0656f;
float gyroOffsetY  = 0.0128f;
float gyroOffsetZ  = 0.0097f;


struct ScheduledTaskConfig {
  bool enabled = false;
  bool repeat = false;

  // once
  int year = 0;
  int month = 0;
  int day = 0;

  // once + repeat
  int hour = 0;
  int minute = 0;

  // repeat bitmask: bit0=Sun ... bit6=Sat
  uint8_t daysMask = 0;

  // prevents multiple runs in same minute/day
  uint32_t lastRunKey = 0;
};

bool bluetoothEnabled = true; // default ON
int jigglePixels = 2;         // default jump distance

ScheduledTaskConfig scheduledTask;
String scheduledTaskText = "Hello world!";

// ---------------- OTA PAGE ----------------

const char* otaPage = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<title>Magic mouse Control Page</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
body{
  font-family:Arial,sans-serif;
  max-width:420px;
  margin:auto;
  padding:20px;
}
progress{
  width:100%;
  height:20px;
}
#status,#timeStatus,#gyroStatus,#recalStatus{
  margin-top:10px;
  font-weight:bold;
}
.section{
  margin-top:28px;
  padding-top:18px;
  border-top:1px solid #ccc;
}
input,button,select{
  font-size:16px;
}
input[type="datetime-local"], input[type="time"], input[type="text"], select{
  width:100%;
  padding:8px;
  box-sizing:border-box;
}
button{
  padding:10px 14px;
  cursor:pointer;
  width:100%;
  margin-top:8px;
}
.small{
  font-size:14px;
  color:#555;
  margin-top:6px;
}
.state{
  display:inline-block;
  margin-top:10px;
  padding:6px 10px;
  background:#f2f2f2;
  border-radius:8px;
}
.gpio-row{
  display:flex;
  justify-content:space-between;
  align-items:center;
  padding:8px 10px;
  border:1px solid #ddd;
  border-radius:8px;
  margin-top:8px;
}
.gpio-value{
  font-weight:bold;
  min-width:24px;
  text-align:center;
  padding:4px 8px;
  border-radius:6px;
  background:#f2f2f2;
}
</style>
</head>
<body>
<h1 style="text-align:center;">Magic Mouse Settings</h1>
<h2>Magic mouse Firmware Update</h2>

<input type="file" id="file">
<br><br>
<button onclick="upload()">Upload Firmware</button>
<br><br>

<progress id="bar" value="0" max="100"></progress>
<div id="status"></div>


<div class="section">
  <h2>Bluetooth</h2>
  <div class="small">Turn the BLE keyboard/mouse feature ON or OFF.</div>

  <div style="margin-top:12px;">
    <label for="bluetoothEnabledSelect"><b>Bluetooth</b></label>
    <select id="bluetoothEnabledSelect" style="width:100%;padding:8px;margin-top:6px;">
      <option value="on">ON</option>
      <option value="off">OFF</option>
    </select>
  </div>

  <button onclick="saveBluetooth()">Save Bluetooth</button>
  <button onclick="loadBluetooth()">Refresh Bluetooth</button>

  <div id="bluetoothStatus"></div>
  <div class="state" id="bluetoothNow">Loading Bluetooth state...</div>
</div>


<div class="section">
  <h2>Set RTC Time</h2>
  <div class="state" id="rtcNow">Loading RTC time...</div>
  <input type="datetime-local" id="rtcTime">
  <div class="small">Current device RTC time is shown above.</div>
  <button onclick="setTime()">Set Time</button>
  <button onclick="loadRtcTimeIntoInput()">Use device RTC time</button>
  <div id="timeStatus"></div>
</div>

<div class="section">
  <h2>Scheduled Task</h2>

  <label for="scheduleMode"><b>Mode</b></label>
  <select id="scheduleMode" onchange="toggleScheduleMode()" style="width:100%;padding:8px;margin-top:6px;">
    <option value="once">Once</option>
    <option value="repeat">Repeat</option>
  </select>

  <div id="onceBox" style="margin-top:12px;">
    <label for="scheduleDateTime"><b>Date and time</b></label>
    <input type="datetime-local" id="scheduleDateTime">
  </div>

  <div id="repeatBox" style="display:none;margin-top:12px;">
    <label for="repeatTime"><b>Time</b></label>
    <input type="time" id="repeatTime" style="width:100%;padding:8px;box-sizing:border-box;">

    <div style="margin-top:12px;">
      <b>Repeat on</b>
      <div style="display:grid;grid-template-columns:repeat(2,1fr);gap:8px;margin-top:8px;">
        <label><input type="checkbox" class="weekday" value="0"> Sunday</label>
        <label><input type="checkbox" class="weekday" value="1"> Monday</label>
        <label><input type="checkbox" class="weekday" value="2"> Tuesday</label>
        <label><input type="checkbox" class="weekday" value="3"> Wednesday</label>
        <label><input type="checkbox" class="weekday" value="4"> Thursday</label>
        <label><input type="checkbox" class="weekday" value="5"> Friday</label>
        <label><input type="checkbox" class="weekday" value="6"> Saturday</label>
      </div>
    </div>
  </div>

  <div style="margin-top:12px;">
    <label for="taskText"><b>Text to type</b></label>
    <input type="text" id="taskText" value="Hello world!" style="width:100%;padding:8px;box-sizing:border-box;">
    <div class="small">This text will be saved in memory and used when the scheduled task runs.</div>
  </div>

  <button onclick="saveSchedule()">Save Schedule</button>
  <button onclick="loadSchedule()">Refresh Schedule</button>

  <div id="scheduleStatus"></div>
  <div class="state" id="scheduleNow">Loading schedule...</div>
</div>

<div class="section">
  <h2>Mouse Jiggle</h2>
  <div class="small">Keeps the mouse active by moving it slightly. Default is ON.</div>

  <div style="margin-top:12px;">
    <label for="jiggleEnabledSelect"><b>Jiggle</b></label>
    <select id="jiggleEnabledSelect" style="width:100%;padding:8px;margin-top:6px;">
      <option value="on">ON</option>
      <option value="off">OFF</option>
    </select>
  </div>
    <div style="margin-top:12px;">
  <label for="jigglePixels"><b>Jump pixels</b></label>
  <input type="number" id="jigglePixels" min="1" max="50" value="2">
  <div class="small">How many pixels the mouse jumps each jiggle.</div>
  </div>
<div style="margin-top:12px;">
  <label for="jiggleInterval"><b>Jiggle interval (ms)</b></label>
  <input type="number" id="jiggleInterval" min="1000" max="3600000" value="60000">
  <div class="small">How often the mouse jiggles. 60000 = 60 seconds.</div>
</div>

  <button onclick="saveJiggle()">Save Mouse Jiggle</button>
  <button onclick="loadJiggle()">Refresh Mouse Jiggle</button>

  <div id="jiggleStatus"></div>
  <div class="state" id="jiggleNow">Loading jiggle state...</div>


</div>

<div class="section">
  <h2>GPIO Status</h2>
  <div class="small">0 = pulled low, 1 = released/high.</div>
  <button onclick="refreshGPIO()">Refresh GPIO states</button>
  <div id="gpioList">Loading GPIO states...</div>
</div>

<script>
function upload(){
  let file=document.getElementById("file").files[0];
  if(!file){
    alert("Select a .bin file first");
    return;
  }

  let xhr=new XMLHttpRequest();

  xhr.upload.addEventListener("progress",function(e){
    if(e.lengthComputable){
      let percent=(e.loaded/e.total)*100;
      document.getElementById("bar").value=percent;
    }
  });

  xhr.onreadystatechange=function(){
    if(xhr.readyState==4){
      document.getElementById("status").innerHTML=xhr.responseText;
    }
  };

  xhr.open("POST","/update",true);
  let formData=new FormData();
  formData.append("firmware",file);
  xhr.send(formData);

  document.getElementById("status").innerHTML="Uploading...";
}

function saveBluetooth(){
  let enabled = document.getElementById("bluetoothEnabledSelect").value === "on";

  let xhr = new XMLHttpRequest();
  xhr.onreadystatechange = function(){
    if(xhr.readyState === 4){
      document.getElementById("bluetoothStatus").innerHTML = xhr.responseText;
      loadBluetooth();
    }
  };

  xhr.open("POST", "/bluetooth", true);
  xhr.setRequestHeader("Content-Type", "application/x-www-form-urlencoded");
  xhr.send("enabled=" + (enabled ? "1" : "0"));

  document.getElementById("bluetoothStatus").innerHTML = "Saving Bluetooth setting...";
}

function loadBluetooth(){
  let xhr = new XMLHttpRequest();
  xhr.onreadystatechange = function(){
    if(xhr.readyState === 4){
      if(xhr.status === 200){
        let data = JSON.parse(xhr.responseText);
        document.getElementById("bluetoothEnabledSelect").value = data.enabled ? "on" : "off";
        document.getElementById("bluetoothNow").innerHTML = "Bluetooth: " + (data.enabled ? "ON" : "OFF");
      } else {
        document.getElementById("bluetoothNow").innerHTML = "Failed to load Bluetooth state";
      }
    }
  };

  xhr.open("GET", "/bluetooth", true);
  xhr.send();
}

function setTime(){
  let dt = document.getElementById("rtcTime").value;
  if(!dt){
    alert("Select date and time first");
    return;
  }

  let xhr = new XMLHttpRequest();
  xhr.onreadystatechange = function(){
    if(xhr.readyState == 4){
      document.getElementById("timeStatus").innerHTML = xhr.responseText;
      refreshRtcTime();
    }
  };

  xhr.open("POST", "/settime", true);
  xhr.setRequestHeader("Content-Type", "application/x-www-form-urlencoded");
  xhr.send("datetime=" + encodeURIComponent(dt));

  document.getElementById("timeStatus").innerHTML = "Setting time...";
}

function refreshRtcTime(){
  let xhr = new XMLHttpRequest();
  xhr.onreadystatechange = function(){
    if(xhr.readyState == 4){
      if(xhr.status == 200){
        document.getElementById("rtcNow").innerHTML = "Device RTC: " + xhr.responseText;
      } else {
        document.getElementById("rtcNow").innerHTML = "Device RTC: unavailable";
      }
    }
  };
  xhr.open("GET", "/rtctime", true);
  xhr.send();
}

function loadRtcTimeIntoInput(){
  let xhr = new XMLHttpRequest();
  xhr.onreadystatechange = function(){
    if(xhr.readyState == 4 && xhr.status == 200){
      document.getElementById("rtcTime").value = xhr.responseText;
    }
  };
  xhr.open("GET", "/rtctime-local", true);
  xhr.send();
}

function toggleScheduleMode(){
  let mode = document.getElementById("scheduleMode").value;
  document.getElementById("onceBox").style.display = (mode === "once") ? "block" : "none";
  document.getElementById("repeatBox").style.display = (mode === "repeat") ? "block" : "none";
}


function loadSchedule(){
  let xhr = new XMLHttpRequest();
  xhr.onreadystatechange = function(){
    if(xhr.readyState === 4){
      if(xhr.status === 200){
        let data = JSON.parse(xhr.responseText);

        document.getElementById("scheduleMode").value = data.mode || "once";
        document.getElementById("taskText").value = data.text || "Hello world!";
        toggleScheduleMode();

        if(data.mode === "once"){
          document.getElementById("scheduleDateTime").value = data.datetime || "";
        } else {
          document.getElementById("repeatTime").value = data.time || "";

          let boxes = document.querySelectorAll(".weekday");
          for(let i = 0; i < boxes.length; i++){
            boxes[i].checked = false;
          }

          if(data.days){
            for(let i = 0; i < data.days.length; i++){
              let day = data.days[i];
              let el = document.querySelector('.weekday[value="' + day + '"]');
              if(el) el.checked = true;
            }
          }
        }

        document.getElementById("scheduleNow").innerHTML = data.summary || "No schedule set";
      } else {
        document.getElementById("scheduleNow").innerHTML = "Failed to load schedule";
      }
    }
  };

  xhr.open("GET", "/schedule", true);
  xhr.send();
}

function refreshGPIO(){
  let xhr = new XMLHttpRequest();
  xhr.onreadystatechange = function(){
    if(xhr.readyState == 4){
      if(xhr.status == 200){
        let data = JSON.parse(xhr.responseText);
        let html = "";

        for (let i = 0; i < data.length; i++) {
          html += '<div class="gpio-row">';
          html += '<span>GPIO ' + data[i].pin + '</span>';
          html += '<span class="gpio-value">' + data[i].value + '</span>';
          html += '</div>';
        }

        document.getElementById("gpioList").innerHTML = html;
      } else {
        document.getElementById("gpioList").innerHTML = "Failed to load GPIO states";
      }
    }
  };
  xhr.open("GET", "/gpiostate", true);
  xhr.send();
}

function saveSchedule(){
  let mode = document.getElementById("scheduleMode").value;
  let taskText = document.getElementById("taskText").value;

  if(!taskText){
    taskText = "Hello world!";
  }

  let payload = {
    mode: mode,
    text: taskText
  };

  if(mode === "once"){
    let dt = document.getElementById("scheduleDateTime").value;
    if(!dt){
      alert("Select date and time first");
      return;
    }
    payload.datetime = dt;
  } else {
    let t = document.getElementById("repeatTime").value;
    if(!t){
      alert("Select repeat time first");
      return;
    }
    payload.time = t;

    let days = [];
    let boxes = document.querySelectorAll(".weekday");
    for(let i = 0; i < boxes.length; i++){
      if(boxes[i].checked){
        days.push(parseInt(boxes[i].value));
      }
    }

    if(days.length === 0){
      alert("Select at least one weekday");
      return;
    }

    payload.days = days;
  }

  let xhr = new XMLHttpRequest();
  xhr.onreadystatechange = function(){
    if(xhr.readyState === 4){
      document.getElementById("scheduleStatus").innerHTML = xhr.responseText;
      loadSchedule();
    }
  };

  xhr.open("POST", "/schedule", true);
  xhr.setRequestHeader("Content-Type", "application/json");
  xhr.send(JSON.stringify(payload));

  document.getElementById("scheduleStatus").innerHTML = "Saving schedule...";
}

function saveJiggle(){
  let enabled = document.getElementById("jiggleEnabledSelect").value === "on";
  let pixels = parseInt(document.getElementById("jigglePixels").value, 10);
  let interval = parseInt(document.getElementById("jiggleInterval").value, 10);

  if(isNaN(pixels) || pixels < 1) pixels = 1;
  if(pixels > 50) pixels = 50;

  if(isNaN(interval) || interval < 1000) interval = 1000;
  if(interval > 3600000) interval = 3600000;

  let xhr = new XMLHttpRequest();
  xhr.onreadystatechange = function(){
    if(xhr.readyState === 4){
      document.getElementById("jiggleStatus").innerHTML = xhr.responseText;
      loadJiggle();
    }
  };

  xhr.open("POST", "/jiggle", true);
  xhr.setRequestHeader("Content-Type", "application/x-www-form-urlencoded");
  xhr.send(
    "enabled=" + (enabled ? "1" : "0") +
    "&pixels=" + encodeURIComponent(pixels) +
    "&interval=" + encodeURIComponent(interval)
  );

  document.getElementById("jiggleStatus").innerHTML = "Saving jiggle setting...";
}

function loadJiggle(){
  let xhr = new XMLHttpRequest();
  xhr.onreadystatechange = function(){
    if(xhr.readyState === 4){
      if(xhr.status === 200){
        let data = JSON.parse(xhr.responseText);

        document.getElementById("jiggleEnabledSelect").value = data.enabled ? "on" : "off";
        document.getElementById("jigglePixels").value = data.pixels || 2;
        document.getElementById("jiggleInterval").value = data.interval || 60000;

        document.getElementById("jiggleNow").innerHTML =
          "Mouse jiggle: " + (data.enabled ? "ON" : "OFF") +
          " | pixels: " + (data.pixels || 2) +
          " | interval: " + (data.interval || 60000) + " ms";
      } else {
        document.getElementById("jiggleNow").innerHTML = "Failed to load jiggle state";
      }
    }
  };

  xhr.open("GET", "/jiggle", true);
  xhr.send();
}

window.onload = function(){
  refreshRtcTime();
  refreshGPIO();
  loadSchedule();
  loadJiggle();
  loadBluetooth();
  toggleScheduleMode();
};
</script>

</body>
</html>
)rawliteral";

// ---------------- VIBRATION ----------------



void vibrateOnce(int onMs = 120, int offMs = 0) {
  digitalWrite(VIBRATION_PIN, HIGH);
  delay(onMs);
  digitalWrite(VIBRATION_PIN, LOW);
  if (offMs > 0) delay(offMs);
}

void vibratePattern(int pulses, int onMs = 120, int offMs = 100) {
  for (int i = 0; i < pulses; i++) {
    digitalWrite(VIBRATION_PIN, HIGH);
    delay(onMs);
    digitalWrite(VIBRATION_PIN, LOW);
    if (i < pulses - 1) delay(offMs);
  }
}

void vibrateCalibrationFinished() {
  vibratePattern(1, 300, 0);
}

void vibrateHotspotOn() {
  vibratePattern(2, 120, 120);
}

void vibrateHotspotOff() {
  vibratePattern(3, 300, 200);
}



void saveBluetoothToFlash() {
  prefs.begin("ble", false);
  prefs.putBool("enabled", bluetoothEnabled);
  prefs.end();

  Serial.print("Bluetooth saved: ");
  Serial.println(bluetoothEnabled ? "ON" : "OFF");
}

void loadBluetoothFromFlash() {
  prefs.begin("ble", true);
  bluetoothEnabled = prefs.getBool("enabled", true); // default ON
  prefs.end();

  Serial.print("Bluetooth loaded: ");
  Serial.println(bluetoothEnabled ? "ON" : "OFF");
}

void saveJiggleSettingsToFlash() {
  prefs.begin("jiggle", false);
  prefs.putBool("enabled", jiggleEnabled);
  prefs.putInt("pixels", jigglePixels);
  prefs.putULong("interval", jiggleInterval);
  prefs.end();

  Serial.print("Jiggle saved: ");
  Serial.print(jiggleEnabled ? "ON" : "OFF");
  Serial.print(" | pixels: ");
  Serial.print(jigglePixels);
  Serial.print(" | interval ms: ");
  Serial.println(jiggleInterval);
}

void loadJiggleSettingsFromFlash() {
  prefs.begin("jiggle", true);
  jiggleEnabled = prefs.getBool("enabled", true);
  jigglePixels = prefs.getInt("pixels", 2);
  jiggleInterval = prefs.getULong("interval", 60000);
  prefs.end();

  if (jigglePixels < 1) jigglePixels = 1;
  if (jigglePixels > 50) jigglePixels = 50;

  if (jiggleInterval < 1000) jiggleInterval = 1000;
  if (jiggleInterval > 3600000UL) jiggleInterval = 3600000UL;

  Serial.print("Jiggle loaded: ");
  Serial.print(jiggleEnabled ? "ON" : "OFF");
  Serial.print(" | pixels: ");
  Serial.print(jigglePixels);
  Serial.print(" | interval ms: ");
  Serial.println(jiggleInterval);
}
// ---------------- PERSISTENCE ----------------


// ---------------- RTC ----------------

void printRTCNow() {
  if (!rtcAvailable) return;

  DateTime now = rtc.now();
  Serial.printf(
    "RTC Time: %04d-%02d-%02d %02d:%02d:%02d\n",
    now.year(),
    now.month(),
    now.day(),
    now.hour(),
    now.minute(),
    now.second()
  );
}

String getRTCStringHuman() {
  if (!rtcAvailable) return "RTC not available";

  DateTime now = rtc.now();
  char buf[20];
  snprintf(
    buf, sizeof(buf),
    "%04d-%02d-%02d %02d:%02d:%02d",
    now.year(), now.month(), now.day(),
    now.hour(), now.minute(), now.second()
  );
  return String(buf);
}

String getRTCStringForInput() {
  if (!rtcAvailable) return "";

  DateTime now = rtc.now();
  char buf[17];
  snprintf(
    buf, sizeof(buf),
    "%04d-%02d-%02dT%02d:%02d",
    now.year(), now.month(), now.day(),
    now.hour(), now.minute()
  );
  return String(buf);
}

void setupRTC() {
  if (!rtc.begin()) {
    Serial.println("DS3231 not found");
    rtcAvailable = false;
    return;
  }

  rtcAvailable = true;
  Serial.println("DS3231 initialized");

  if (rtc.lostPower()) {
    Serial.println("DS3231 lost power, setting time from compile time");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  printRTCNow();
}

bool parseDateTimeLocal(String dt, int &year, int &month, int &day, int &hour, int &minute) {
  if (dt.length() < 16) return false;
  if (dt.charAt(4) != '-' || dt.charAt(7) != '-' || dt.charAt(10) != 'T' || dt.charAt(13) != ':')
    return false;

  year   = dt.substring(0, 4).toInt();
  month  = dt.substring(5, 7).toInt();
  day    = dt.substring(8, 10).toInt();
  hour   = dt.substring(11, 13).toInt();
  minute = dt.substring(14, 16).toInt();

  if (year < 2000 || year > 2099) return false;
  if (month < 1 || month > 12) return false;
  if (day < 1 || day > 31) return false;
  if (hour > 23) return false;
  if (minute > 59) return false;

  return true;
}

// ---------------- MPU6050 ----------------

bool setupMPU() {
  if (!mpu.begin(MPU_ADDR, &Wire)) {
    Serial.println("MPU6050 not found at 0x69");
    return false;
  }

  mpu.setAccelerometerRange(MPU6050_RANGE_2_G);
  mpu.setGyroRange(MPU6050_RANGE_250_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);

  Serial.println("MPU6050 initialized at 0x69");
  return true;
}




void readCorrectedMPU(float &ax, float &ay, float &az, float &gx, float &gy, float &gz) {
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);

  ax = a.acceleration.x - accelOffsetX;
  ay = a.acceleration.y - accelOffsetY;
  az = a.acceleration.z - accelOffsetZ;

  gx = g.gyro.x - gyroOffsetX;
  gy = g.gyro.y - gyroOffsetY;
  gz = g.gyro.z - gyroOffsetZ;
}

void printMPUCorrected() {
  if (!mpuAvailable) return;

  float ax, ay, az, gx, gy, gz;
  readCorrectedMPU(ax, ay, az, gx, gy, gz);

  Serial.printf("MPU A[%.3f %.3f %.3f] G[%.3f %.3f %.3f]\n", ax, ay, az, gx, gy, gz);
}

bool isHeadDownNow(float &ax, float &ay, float &az, float &gx, float &gy, float &gz) {
  readCorrectedMPU(ax, ay, az, gx, gy, gz);
  return (az < HEAD_DOWN_Z_THRESHOLD);
}


// ---------------- OTA / TIME / GYRO / GPIO HANDLERS ----------------

void handleRoot() {
  server.send(200, "text/html", otaPage);
}

void handleSetTime() {
  if (!rtcAvailable) {
    server.send(500, "text/plain", "RTC not available");
    return;
  }

  if (!server.hasArg("datetime")) {
    server.send(400, "text/plain", "Missing datetime field");
    return;
  }

  String dt = server.arg("datetime");

  int year, month, day, hour, minute;
  if (!parseDateTimeLocal(dt, year, month, day, hour, minute)) {
    server.send(400, "text/plain", "Invalid datetime format");
    return;
  }

  rtc.adjust(DateTime(year, month, day, hour, minute, 0));

  Serial.println("RTC updated from webpage");
  printRTCNow();

  server.send(200, "text/plain", "RTC time set successfully");
}

void handleRTCNow() {
  if (!rtcAvailable) {
    server.send(500, "text/plain", "RTC not available");
    return;
  }

  server.send(200, "text/plain", getRTCStringHuman());
}

void handleRTCNowLocal() {
  if (!rtcAvailable) {
    server.send(500, "text/plain", "");
    return;
  }

  server.send(200, "text/plain", getRTCStringForInput());
}

void handleGyroState() {
  String msg = "Gyroscope mouse: ";
  msg += gyroMouseEnabled ? "ON" : "OFF";

  if (!mpuAvailable) {
    msg += " (MPU not available)";
  }

  server.send(200, "text/plain", msg);
}

void handleToggleGyro() {
  if (!mpuAvailable) {
    server.send(500, "text/plain", "MPU6050 not available");
    return;
  }

  gyroMouseEnabled = !gyroMouseEnabled;

  if (gyroMouseEnabled) {
    Serial.println("Gyroscope mouse enabled");
    server.send(200, "text/plain", "Gyroscope mouse enabled");
  } else {
    Serial.println("Gyroscope mouse disabled");
    server.send(200, "text/plain", "Gyroscope mouse disabled");
  }
}


void handleGPIOState() {
  String json = "[";

  for (int i = 0; i < gpioCount; i++) {
    int pin = gpioList[i];
    int value = digitalRead(pin);

    json += "{\"pin\":";
    json += pin;
    json += ",\"value\":";
    json += value;
    json += "}";

    if (i < gpioCount - 1) {
      json += ",";
    }
  }

  json += "]";
  server.send(200, "application/json", json);
}

void handleUpdateResult() {
  if (Update.hasError()) {
    server.send(200, "text/plain", "Update Failed");
  } else {
    server.send(200, "text/plain", "Update successful. Rebooting...");
    server.client().stop();
    delay(500);
    ESP.restart();
  }
}

void handleUpload() {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("Update start: %s\n", upload.filename.c_str());
    printRTCNow();

    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
    }

  } else if (upload.status == UPLOAD_FILE_WRITE) {

    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }

  } else if (upload.status == UPLOAD_FILE_END) {

    if (Update.end(true)) {
      Serial.printf("Update Success: %u bytes\n", upload.totalSize);
      printRTCNow();
    } else {
      Update.printError(Serial);
    }
  }
}

// ---------------- HOTSPOT ----------------

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);

  server.on(
    "/update",
    HTTP_POST,
    handleUpdateResult,
    handleUpload
  );

  server.on("/settime", HTTP_POST, handleSetTime);
  server.on("/rtctime", HTTP_GET, handleRTCNow);
  server.on("/rtctime-local", HTTP_GET, handleRTCNowLocal);
  server.on("/gyrostate", HTTP_GET, handleGyroState);
  server.on("/togglegyro", HTTP_POST, handleToggleGyro);
  server.on("/gpiostate", HTTP_GET, handleGPIOState);
  server.on("/schedule", HTTP_GET, handleGetSchedule);
  server.on("/schedule", HTTP_POST, handleSetSchedule);
  server.on("/jiggle", HTTP_GET, handleGetJiggle);
  server.on("/jiggle", HTTP_POST, handleSetJiggle);
  server.on("/bluetooth", HTTP_GET, handleGetBluetooth);
  server.on("/bluetooth", HTTP_POST, handleSetBluetooth);

  server.begin();
}

void startHotspot() {
  if (apStarted) return;

  WiFi.mode(WIFI_AP);
  WiFi.softAP(apSSID, apPassword);

  apStarted = true;

  Serial.println("Hotspot started");
  Serial.print("SSID: ");
  Serial.println(apSSID);
  Serial.print("IP: ");
  Serial.println(WiFi.softAPIP());
  printRTCNow();

  setupWebServer();
  vibrateHotspotOn();
}

void stopHotspot() {
  if (!apStarted) return;

  server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);

  apStarted = false;

  Serial.println("Hotspot stopped");
  vibrateHotspotOff();
}

void tapKey(uint8_t key) {
  if (!bluetoothEnabled || keyboard == nullptr) return;

  keyboard->keyPress(key);
  keyboard->sendKeyReport();
  delay(30);

  keyboard->keyRelease(key);
  keyboard->sendKeyReport();
  delay(40);
}

void tapModifiedKey(uint8_t mod, uint8_t key) {
  if (!bluetoothEnabled || keyboard == nullptr) return;

  keyboard->modifierKeyPress(mod);
  keyboard->sendKeyReport();
  delay(15);

  keyboard->keyPress(key);
  keyboard->sendKeyReport();
  delay(30);

  keyboard->keyRelease(key);
  keyboard->sendKeyReport();
  delay(15);

  keyboard->modifierKeyRelease(mod);
  keyboard->sendKeyReport();
  delay(40);
}

void leftClick() {
  if (!bluetoothEnabled || mouse == nullptr) return;

  mouse->mousePress(1);
  mouse->sendMouseReport();
  delay(60);

  mouse->mouseRelease(1);
  mouse->sendMouseReport();
  delay(150);
}


void manageHotspotByOrientation() {
  if (!mpuAvailable) return;

  static bool lastHeadDown = false;

  float ax, ay, az, gx, gy, gz;
  bool headDown = isHeadDownNow(ax, ay, az, gx, gy, gz);
  unsigned long now = millis();

  if (headDown && !lastHeadDown) {
    Serial.println("Orientation -> HEAD DOWN");
    normalOrientationSince = 0;

    if (!apStarted) {
      Serial.println("Starting hotspot");
      startHotspot();
    }
  }

  if (!headDown && lastHeadDown) {
    Serial.println("Orientation -> NORMAL");
    normalOrientationSince = now;
  }

  if (!headDown && apStarted) {
    if (normalOrientationSince != 0 &&
        (now - normalOrientationSince >= hotspotStopDelay)) {
      Serial.println("Normal for 30s -> stopping hotspot");
      stopHotspot();
      normalOrientationSince = 0;
    }
  }

  if (headDown) {
    normalOrientationSince = 0;
  }

  lastHeadDown = headDown;

  if (apStarted) {
    server.handleClient();
  }
}

bool parseTimeOnly(String t, int &hour, int &minute) {
  if (t.length() < 5) return false;
  if (t.charAt(2) != ':') return false;

  hour = t.substring(0, 2).toInt();
  minute = t.substring(3, 5).toInt();

  if (hour < 0 || hour > 23) return false;
  if (minute < 0 || minute > 59) return false;

  return true;
}

String getScheduledTaskSummary() {
  if (!scheduledTask.enabled) {
    return "No schedule set | Text: " + scheduledTaskText;
  }

  if (!scheduledTask.repeat) {
    char buf[120];
    snprintf(buf, sizeof(buf),
      "Mode: once | %04d-%02d-%02d %02d:%02d | Text: ",
      scheduledTask.year, scheduledTask.month, scheduledTask.day,
      scheduledTask.hour, scheduledTask.minute
    );
    return String(buf) + scheduledTaskText;
  }

  String days = "";
  const char* dayNames[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  for (int i = 0; i < 7; i++) {
    if (scheduledTask.daysMask & (1 << i)) {
      if (days.length() > 0) days += ", ";
      days += dayNames[i];
    }
  }

  char buf[120];
  snprintf(buf, sizeof(buf),
    "Mode: repeat | %02d:%02d | Days: ",
    scheduledTask.hour, scheduledTask.minute
  );

  return String(buf) + days + " | Text: " + scheduledTaskText;
}


void loadScheduleFromFlash() {
  prefs.begin("schedule", true);

  scheduledTask.enabled    = prefs.getBool("enabled", false);
  scheduledTask.repeat     = prefs.getBool("repeat", false);
  scheduledTask.year       = prefs.getInt("year", 0);
  scheduledTask.month      = prefs.getInt("month", 0);
  scheduledTask.day        = prefs.getInt("day", 0);
  scheduledTask.hour       = prefs.getInt("hour", 0);
  scheduledTask.minute     = prefs.getInt("minute", 0);
  scheduledTask.daysMask   = prefs.getUChar("daysMask", 0);
  scheduledTask.lastRunKey = prefs.getUInt("lastRunKey", 0);
  scheduledTaskText        = prefs.getString("text", "Hello world!");

  prefs.end();

  Serial.println("Schedule loaded from flash");
  Serial.println(getScheduledTaskSummary());
  Serial.print("Scheduled text: ");
  Serial.println(scheduledTaskText);
}





bool sendMapped(uint8_t key, uint8_t mod) {
  if (mod) tapModifiedKey(mod, key);
  else tapKey(key);
  return true;
}

bool mapCharDE(char c, uint8_t &key, uint8_t &mod) {
  mod = 0;

  if (c >= 'a' && c <= 'z') {
    key = KEY_A + (c - 'a');
    if (c == 'y') key = KEY_Z;
    else if (c == 'z') key = KEY_Y;
    return true;
  }

  if (c >= 'A' && c <= 'Z') {
    mod = KEY_MOD_LSHIFT;
    char lower = c - 'A' + 'a';
    key = KEY_A + (lower - 'a');
    if (c == 'Y') key = KEY_Z;
    else if (c == 'Z') key = KEY_Y;
    return true;
  }

  if (c >= '1' && c <= '9') { key = KEY_1 + (c - '1'); return true; }
  if (c == '0') { key = KEY_0; return true; }

  switch (c) {
    case ' ': key = KEY_SPACE; return true;
    case ',': key = KEY_COMMA; return true;
    case '.': key = KEY_DOT; return true;
    case '-': key = KEY_SLASH; return true;     // verify on your host
    case '+': key = KEY_RIGHTBRACE; return true;
    case '?': key = KEY_MINUS; mod = KEY_MOD_LSHIFT; return true;
    case '!': key = KEY_1; mod = KEY_MOD_LSHIFT; return true;

    // 8-bit chars only if your source encoding matches
    case '\xE4': key = KEY_APOSTROPHE; return true;               // ä
    case '\xF6': key = KEY_SEMICOLON; return true;                // ö
    case '\xFC': key = KEY_LEFTBRACE; return true;                // ü
    case '\xC4': key = KEY_APOSTROPHE; mod = KEY_MOD_LSHIFT; return true; // Ä
    case '\xD6': key = KEY_SEMICOLON; mod = KEY_MOD_LSHIFT; return true;  // Ö
    case '\xDC': key = KEY_LEFTBRACE; mod = KEY_MOD_LSHIFT; return true;  // Ü
    case '\xDF': key = KEY_MINUS; return true;                    // ß
  }

  return false;
}

bool mapCharHU(char c, uint8_t &key, uint8_t &mod) {
  mod = 0;

  if (c >= 'a' && c <= 'z') {
    key = KEY_A + (c - 'a');
    if (c == 'y') key = KEY_Z;
    else if (c == 'z') key = KEY_Y;
    return true;
  }

  if (c >= 'A' && c <= 'Z') {
    mod = KEY_MOD_LSHIFT;
    char lower = c - 'A' + 'a';
    key = KEY_A + (lower - 'a');
    if (c == 'Y') key = KEY_Z;
    else if (c == 'Z') key = KEY_Y;
    return true;
  }

  if (c >= '1' && c <= '9') { key = KEY_1 + (c - '1'); return true; }
  if (c == '0') { key = KEY_0; return true; }

  switch (c) {
    case ' ': key = KEY_SPACE; return true;
    case ',': key = KEY_COMMA; return true;
    case '.': key = KEY_DOT; return true;
    case '!': key = KEY_1; mod = KEY_MOD_LSHIFT; return true;

    // these are placeholders you must verify on your OS/layout
    // Hungarian special letters are not portable across hosts
    default: return false;
  }
}

bool mapChar(char c, KeyboardLayout layout, uint8_t &key, uint8_t &mod) {
  switch (layout) {
    case LAYOUT_DE: return mapCharDE(c, key, mod);
    case LAYOUT_HU: return mapCharHU(c, key, mod);
    case LAYOUT_US:
    default:
      return false;
  }
}

void typeText(const char* text) {
  for (int i = 0; text[i] != '\0'; i++) {
    uint8_t key = 0, mod = 0;
    if (mapChar(text[i], activeLayout, key, mod)) {
      sendMapped(key, mod);
    }
  }
}


void startBLEHID() {
  if (keyboard != nullptr || mouse != nullptr) return;

  KeyboardConfiguration keyboardConfig;
  keyboardConfig.setAutoReport(false);
  keyboard = new KeyboardDevice(keyboardConfig);

  MouseConfiguration mouseConfig;
  mouseConfig.setAutoReport(false);
  mouse = new MouseDevice(mouseConfig);

  compositeHID.addDevice(keyboard);
  compositeHID.addDevice(mouse);
  compositeHID.begin();

  Serial.println("BLE HID started");
}

void stopBLEHID() {
  if (keyboard != nullptr) {
    delete keyboard;
    keyboard = nullptr;
  }

  if (mouse != nullptr) {
    delete mouse;
    mouse = nullptr;
  }

  compositeHID.end();

  delay(100);

  Serial.println("BLE HID stopped");
}

void runScheduledTask() {
  if (!bluetoothEnabled) {
    Serial.println("Scheduled task skipped: Bluetooth disabled");
    return;
  }

  if (!compositeHID.isConnected()) {
    Serial.println("Scheduled task skipped: BLE HID not connected");
    return;
  }

  delay(2000);

  tapKey(KEY_ENTER);

  if (mouse != nullptr) {
    mouse->mouseMove(10, 0);
    mouse->sendMouseReport();
  }

  delay(1000);

  leftClick();
  typeText(scheduledTaskText.c_str());
  delay(200);
  tapKey(KEY_ENTER);
}

void handleGetSchedule() {
  String json = "{";
  json += "\"enabled\":" + String(scheduledTask.enabled ? "true" : "false") + ",";
  json += "\"mode\":\"";
  json += scheduledTask.repeat ? "repeat" : "once";
  json += "\",";

  char dt[20];
  snprintf(dt, sizeof(dt), "%04d-%02d-%02dT%02d:%02d",
           scheduledTask.year, scheduledTask.month, scheduledTask.day,
           scheduledTask.hour, scheduledTask.minute);

  char tm[6];
  snprintf(tm, sizeof(tm), "%02d:%02d", scheduledTask.hour, scheduledTask.minute);

  String safeText = scheduledTaskText;
  safeText.replace("\\", "\\\\");
  safeText.replace("\"", "\\\"");
  safeText.replace("\n", "\\n");
  safeText.replace("\r", "");

  json += "\"datetime\":\"" + String(dt) + "\",";
  json += "\"time\":\"" + String(tm) + "\",";
  json += "\"text\":\"" + safeText + "\",";
  json += "\"days\":[";

  bool first = true;
  for (int i = 0; i < 7; i++) {
    if (scheduledTask.daysMask & (1 << i)) {
      if (!first) json += ",";
      json += String(i);
      first = false;
    }
  }

  json += "],";
  json += "\"summary\":\"" + getScheduledTaskSummary() + "\"";
  json += "}";

  server.send(200, "application/json", json);
}

void handleGetBluetooth() {
  String json = "{";
  json += "\"enabled\":";
  json += bluetoothEnabled ? "true" : "false";
  json += "}";

  server.send(200, "application/json", json);
}

void handleSetBluetooth() {
  if (!server.hasArg("enabled")) {
    server.send(400, "text/plain", "Missing enabled field");
    return;
  }

  bool newState = server.arg("enabled") == "1";

  if (newState == bluetoothEnabled) {
    server.send(200, "text/plain",
      String("Bluetooth already ") + (bluetoothEnabled ? "enabled" : "disabled"));
    return;
  }

  bluetoothEnabled = newState;
  saveBluetoothToFlash();

  if (bluetoothEnabled) {
    Serial.println("Starting BLE (enable)");
    startBLEHID();
    server.send(200, "text/plain", "Bluetooth enabled");
  } else {
    Serial.println("Stopping BLE (disable)");
    stopBLEHID();
    server.send(200, "text/plain", "Bluetooth disabled");
  }
}

String extractJsonString(String body, const String& key) {
  String pattern = "\"" + key + "\":\"";
  int start = body.indexOf(pattern);
  if (start < 0) return "";

  start += pattern.length();
  int end = start;
  bool escape = false;

  while (end < body.length()) {
    char c = body.charAt(end);

    if (c == '\\' && !escape) {
      escape = true;
      end++;
      continue;
    }

    if (c == '"' && !escape) {
      break;
    }

    escape = false;
    end++;
  }

  String value = body.substring(start, end);
  value.replace("\\n", "\n");
  value.replace("\\\"", "\"");
  value.replace("\\\\", "\\");
  return value;
}

void handleSetSchedule() {
  if (!rtcAvailable) {
    server.send(500, "text/plain", "RTC not available");
    return;
  }

  String body = server.arg("plain");
  if (body.length() == 0) {
    server.send(400, "text/plain", "Missing JSON body");
    return;
  }

  String textValue = extractJsonString(body, "text");
  if (textValue.length() == 0) {
    textValue = "Hello world!";
  }


  if (body.indexOf("\"mode\":\"once\"") >= 0) {
    int idx = body.indexOf("\"datetime\":\"");
    if (idx < 0) {
      server.send(400, "text/plain", "Missing datetime");
      return;
    }

    idx += 12;
    int end = body.indexOf("\"", idx);
    String dt = body.substring(idx, end);

    int year, month, day, hour, minute;
    if (!parseDateTimeLocal(dt, year, month, day, hour, minute)) {
      server.send(400, "text/plain", "Invalid datetime format");
      return;
    }

    scheduledTask.enabled = true;
    scheduledTask.repeat = false;
    scheduledTask.year = year;
    scheduledTask.month = month;
    scheduledTask.day = day;
    scheduledTask.hour = hour;
    scheduledTask.minute = minute;
    scheduledTask.daysMask = 0;
    scheduledTask.lastRunKey = 0;
    scheduledTaskText = textValue;

    saveScheduleToFlash();
    server.send(200, "text/plain", "One-time schedule saved");
    Serial.println(getScheduledTaskSummary());
    return;
  }

  if (body.indexOf("\"mode\":\"repeat\"") >= 0) {
    int idxTime = body.indexOf("\"time\":\"");
    if (idxTime < 0) {
      server.send(400, "text/plain", "Missing time");
      return;
    }

    idxTime += 8;
    int endTime = body.indexOf("\"", idxTime);
    String t = body.substring(idxTime, endTime);

    int hour, minute;
    if (!parseTimeOnly(t, hour, minute)) {
      server.send(400, "text/plain", "Invalid time format");
      return;
    }

    int idxDays = body.indexOf("\"days\":[");
    if (idxDays < 0) {
      server.send(400, "text/plain", "Missing days");
      return;
    }

    idxDays += 8;
    int endDays = body.indexOf("]", idxDays);
    String dayPart = body.substring(idxDays, endDays);

    uint8_t mask = 0;
    int start = 0;
    while (start < dayPart.length()) {
      int comma = dayPart.indexOf(",", start);
      String token;
      if (comma == -1) {
        token = dayPart.substring(start);
        start = dayPart.length();
      } else {
        token = dayPart.substring(start, comma);
        start = comma + 1;
      }

      if (token.length() > 0) {
        int d = token.toInt();
        if (d >= 0 && d <= 6) {
          mask |= (1 << d);
        }
      }
    }

    if (mask == 0) {
      server.send(400, "text/plain", "No weekdays selected");
      return;
    }

    scheduledTask.enabled = true;
    scheduledTask.repeat = true;
    scheduledTask.year = 0;
    scheduledTask.month = 0;
    scheduledTask.day = 0;
    scheduledTask.hour = hour;
    scheduledTask.minute = minute;
    scheduledTask.daysMask = mask;
    scheduledTask.lastRunKey = 0;
    scheduledTaskText = textValue;

    saveScheduleToFlash();
    server.send(200, "text/plain", "Repeat schedule saved");
    Serial.println(getScheduledTaskSummary());
    return;
  }

  server.send(400, "text/plain", "Invalid mode");
}

uint32_t makeDateKey(const DateTime& now) {
  return (uint32_t)now.year() * 10000UL + (uint32_t)now.month() * 100UL + (uint32_t)now.day();
}

void handleScheduledTaskRun() {
  if (!rtcAvailable || !scheduledTask.enabled) return;

  DateTime now = rtc.now();
  uint32_t todayKey = makeDateKey(now);

  if (!scheduledTask.repeat) {
    if (now.year() == scheduledTask.year &&
        now.month() == scheduledTask.month &&
        now.day() == scheduledTask.day &&
        now.hour() == scheduledTask.hour &&
        now.minute() == scheduledTask.minute) {

      if (scheduledTask.lastRunKey != todayKey) {
        scheduledTask.lastRunKey = todayKey;
        saveScheduleToFlash();

        runScheduledTask();

        scheduledTask.enabled = false;
        saveScheduleToFlash();
        Serial.println("One-time schedule executed and disabled");
      }
    }
  } else {
    int dow = now.dayOfTheWeek(); // 0=Sunday ... 6=Saturday

    bool dayMatch = (scheduledTask.daysMask & (1 << dow)) != 0;
    bool timeMatch = (now.hour() == scheduledTask.hour && now.minute() == scheduledTask.minute);

    if (dayMatch && timeMatch) {
      if (scheduledTask.lastRunKey != todayKey) {
        scheduledTask.lastRunKey = todayKey;
        saveScheduleToFlash();

        runScheduledTask();
      }
    }
  }
}

void saveScheduleToFlash() {
  prefs.begin("schedule", false);

  prefs.putBool("enabled", scheduledTask.enabled);
  prefs.putBool("repeat", scheduledTask.repeat);
  prefs.putInt("year", scheduledTask.year);
  prefs.putInt("month", scheduledTask.month);
  prefs.putInt("day", scheduledTask.day);
  prefs.putInt("hour", scheduledTask.hour);
  prefs.putInt("minute", scheduledTask.minute);
  prefs.putUChar("daysMask", scheduledTask.daysMask);
  prefs.putUInt("lastRunKey", scheduledTask.lastRunKey);
  prefs.putString("text", scheduledTaskText);

  prefs.end();

  Serial.println("Schedule saved to flash");
}


void jiggle() {
  if (!bluetoothEnabled) return;
  if (!compositeHID.isConnected()) return;
  if (!jiggleEnabled) return;
  if (mouse == nullptr) return;

  int x = random(2) ? jigglePixels : -jigglePixels;
  int y = random(2) ? jigglePixels : -jigglePixels;

  mouse->mouseMove(x, y);
  mouse->sendMouseReport();
  delay(5);

  mouse->mouseMove(-x, -y);
  mouse->sendMouseReport();

  Serial.print("Mouse jiggled | pixels: ");
  Serial.println(jigglePixels);
  printRTCNow();
}

void handleMouseJiggle() {
  if (!jiggleEnabled) return;
  if (!compositeHID.isConnected()) return;
  if (gyroMouseEnabled) return;

  unsigned long now = millis();
  if (now - lastJiggle >= jiggleInterval) {
    lastJiggle = now;
    jiggle();
  }
}

void handleGetJiggle() {
  String json = "{";
  json += "\"enabled\":";
  json += jiggleEnabled ? "true" : "false";
  json += ",";
  json += "\"pixels\":";
  json += String(jigglePixels);
  json += ",";
  json += "\"interval\":";
  json += String(jiggleInterval);
  json += "}";

  server.send(200, "application/json", json);
}

void handleSetJiggle() {
  if (!server.hasArg("enabled")) {
    server.send(400, "text/plain", "Missing enabled field");
    return;
  }

  if (!server.hasArg("pixels")) {
    server.send(400, "text/plain", "Missing pixels field");
    return;
  }

  if (!server.hasArg("interval")) {
    server.send(400, "text/plain", "Missing interval field");
    return;
  }

  String value = server.arg("enabled");
  jiggleEnabled = (value == "1" || value == "true" || value == "on");

  int px = server.arg("pixels").toInt();
  if (px < 1) px = 1;
  if (px > 50) px = 50;
  jigglePixels = px;

  unsigned long interval = strtoul(server.arg("interval").c_str(), nullptr, 10);
  if (interval < 1000) interval = 1000;
  if (interval > 3600000UL) interval = 3600000UL;
  jiggleInterval = interval;

  saveJiggleSettingsToFlash();

  Serial.print("Mouse jiggle set to: ");
  Serial.print(jiggleEnabled ? "ON" : "OFF");
  Serial.print(" | pixels: ");
  Serial.print(jigglePixels);
  Serial.print(" | interval ms: ");
  Serial.println(jiggleInterval);

  server.send(200, "text/plain",
    String("Mouse jiggle ") + (jiggleEnabled ? "enabled" : "disabled") +
    " | pixels: " + String(jigglePixels) +
    " | interval ms: " + String(jiggleInterval));
}
// ---------------- SETUP ----------------

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(0, INPUT_PULLUP);
  pinMode(1, INPUT_PULLUP);
  pinMode(2, INPUT_PULLUP);
  pinMode(3, INPUT_PULLUP);
  pinMode(4, OUTPUT);
  digitalWrite(4, LOW);
  pinMode(5, INPUT_PULLUP);
  pinMode(6, INPUT_PULLUP);
  pinMode(7, INPUT_PULLUP);
  pinMode(10, INPUT_PULLUP);
  pinMode(20, INPUT_PULLUP);
  pinMode(21, INPUT_PULLUP);

  randomSeed(micros());

  Wire.begin(SDA_PIN, SCL_PIN);

setupRTC();
loadBluetoothFromFlash();
loadScheduleFromFlash();
loadJiggleSettingsFromFlash();

if (bluetoothEnabled) {
  startBLEHID();
} else {
  keyboard = nullptr;
  mouse = nullptr;
  Serial.println("Bluetooth is disabled in settings, BLE HID not started");
}

  mpuAvailable = setupMPU();

}

// ---------------- LOOP ----------------

void loop() {
  manageHotspotByOrientation();
  handleScheduledTaskRun();
  handleMouseJiggle();
  delay(5);
}
