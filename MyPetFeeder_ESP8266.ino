#if ESP8266
#include <ESP8266WiFi.h>
#include <LittleFS.h>  // ESP8266: LittleFS
#elif ESP32
#include <WiFi.h>
#include <SPIFFS.h>  // ESP32: SPIFFS
#endif

#include <ESPAsyncWebServer.h>
#include <PubSubClient.h>
#include <WiFiUdp.h>  // 用來取得 NTP 時間

AsyncWebServer server(80);

// index.html 內容，large char array, tested with 14k
const char index_html[] PROGMEM = R"rawliteral(
<html>
<head>
    <meta http-equiv="Content-Type" content="text/html; charset=UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Wi-Fi設定</title>
    <style>
        body {
            font-size: 16px;
        }

        table {
            width: 100%;
            max-width: 400px;
            margin: auto;
        }

        input {
            width: 100%;
            box-sizing: border-box;
        }
    </style>
</head>
<body>
    <form method="post" action="/save">
        <table>
            <tr>
                <td colspan="2" style="text-align: center; background-color: silver;">
                    Wi-Fi 連線設定
                </td>
            </tr>
            <tr>
                <td style="text-align: right;">SSID </td>
                <td><input name="ssid"></td>
            </tr>
            <tr>
                <td style="text-align: right;">PASSWORD </td>
                <td><input name="password" type="password"></td>
            </tr>
            <tr>
                <td colspan="2" style="text-align: center;">
                    <button name="connect" type="submit">連線</button>
                    <button name="reset" type="reset">重置</button>
                </td>
            </tr>
        </table>
    </form>
</body>
</html>
)rawliteral";

#define AP_SSID "MyPetFeeder"       // AP開啟預設連線名稱
#define AP_PASS "00000000"          // AP開啟預設連線密，至少8位
#define SSID_PATH "/WIFI/SSID.txt"  // WIFI連線名稱儲存位置
#define PASS_PATH "/WIFI/PASS.txt"  // WIFI連線密碼儲存位置

String ssid, pass;  // WIFI連線名稱、密碼
String fixFeedTime1, fixFeedTime2, fixFeedTime3, fixFeedTime4, fixFeedTime5, fixFeedTime6; // 固定時間餵食時間
char fixFeedTimeView[50]; //顯示固定餵食時間
char matchTime[8]; //系統時間，用來比對固定餵食時間

/* ==============================
   ESP file system 存取操作
   ============================== */
// File system 初始化
bool initLittleFS() {
  Serial.print("掛載 LittleFS...");
  if (!LittleFS.begin()) {
    Serial.println("失敗");
    return false;
  }
  Serial.println("成功");
  return true;
}

// File system 讀取
String readLittleFS(const char* path) {
  File file = LittleFS.open(path, "r");
  if (!file) {
    Serial.println("文件開啟失敗!");
    return "";
  }

  String payload = file.readStringUntil('\n');
  Serial.printf("從%s中，讀取資料%s \n", path, payload);
  file.close();
  return payload;
}

// File system 寫入
void writeLittleFS(const char* path, const char* message) {
  File file = LittleFS.open(path, "w");
  if (!file) {
    Serial.println("文件開啟失敗!");
    return;
  }

  if (!file.print(message)) {
    Serial.printf("寫入%s至%s中 \n", message, path);
    delay(2000);
  } else {
    Serial.println("寫入訊息失敗");
  }
  file.close();
}

/* ============================== */

/* ==============================
   AP 設定及開啟
   ============================== */
void initAPMode() {
  WiFi.disconnect();
  WiFi.mode(WIFI_AP);             // 切換 WiFi 到 AP mode
  WiFi.softAP(AP_SSID, AP_PASS);  // 打開 ESP WiFi 熱點
  Serial.print("AP 模式已開啟\nIP: ");
  Serial.println(WiFi.softAPIP());
  Serial.println("請連接 ESP 設定 SSID 及 PASSWORD");
}

/* ============================== */

/* ==============================
   Web server 設定及開啟
   ============================== */
void initServerHandle() {
  // 在 "/" 目錄下送出 GET 至伺服器時的處理
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    // 送出 PROGMEM 內大型的網頁 (網頁存在 flash 內，避免使用 String 變數方式占用記憶體)
    request->send_P(200, "text/html", index_html);
    // 回應送出 file system 內的 wifimanager.html
    // request->send(LittleFS,"/wifimanager.html");
  });

  // 在 "/save" 目錄下送出 POST (通常是表單) 至伺服器時的處理
  server.on("/save", HTTP_POST, [](AsyncWebServerRequest* request) {
    // 處理網址上的各參數
    int post_count = request->params();
    for (int i = 0; i < post_count; i++) {
      AsyncWebParameter* p = request->getParam(i);

      // 如果網址有收到POST，則處理收到表單的各項值
      if (p->isPost()) {
        if (p->name() == "ssid") {
          String ssid_val = p->value();
          Serial.printf("從表單 ssid 獲取 %s \n", ssid_val);
          writeLittleFS(SSID_PATH, ssid_val.c_str());  // 將儲存格為 ssid 的值存入 SSID_PATH
        }

        if (p->name() == "password") {
          String pass_val = p->value();
          Serial.printf("從表單 password 獲取 %s \n", pass_val);
          writeLittleFS(PASS_PATH, pass_val.c_str());  // 將儲存格為 password 的值存入 PASS_PATH
        }
      }
    }

    request->send(404, "text/plain", "WIFI setting is done. System restarts in 5 seconds.");
    Serial.print("WIFI 連線資料已更新，ESP 將在 5 秒後重新啟動");
    delay(5000);
    ESP.restart();  // SSID 及 PASS 儲存完畢重開機
  });

  server.begin();  // 開始運行 web server
}

/* ============================== */

/* ==============================
   網路校時 NTP 設定
   ============================== */
unsigned int localPort = 2390;  // local port to listen for UDP packets

IPAddress timeServerIP;  // time.nist.gov NTP server address
const char* ntpServerName = "time.nist.gov";

const int NTP_PACKET_SIZE = 48;  // NTP time stamp is in the first 48 bytes of the message

byte packetBuffer[NTP_PACKET_SIZE];  // buffer to hold incoming and outgoing packets

// A UDP instance to let us send and receive packets over UDP
WiFiUDP udp;

// send an NTP request to the time server at the given address
void sendNTPpacket(IPAddress& address) {
  // Serial.println("sending NTP packet...");
  // set all bytes in the buffer to 0
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  // Initialize values needed to form NTP request
  // (see URL above for details on the packets)
  packetBuffer[0] = 0b11100011;  // LI, Version, Mode
  packetBuffer[1] = 0;           // Stratum, or type of clock
  packetBuffer[2] = 6;           // Polling Interval
  packetBuffer[3] = 0xEC;        // Peer Clock Precision
  // 8 bytes of zero for Root Delay & Root Dispersion
  packetBuffer[12] = 49;
  packetBuffer[13] = 0x4E;
  packetBuffer[14] = 49;
  packetBuffer[15] = 52;

  // all NTP fields have been given values, now
  // you can send a packet requesting a timestamp:
  udp.beginPacket(address, 123);  // NTP requests are to port 123
  udp.write(packetBuffer, NTP_PACKET_SIZE);
  udp.endPacket();
}

/* ============================== */

/* ==============================
   MQTT 設定及連線
   ============================== */
const char* mqtt_server = "broker.mqttgo.io";  // MQTT broker 連線位置
WiFiClient espClient;
PubSubClient client(espClient);
const char* feeder_topic1 = "MyPetFeeder/Button";  // 餵食按鈕狀態
const char* feeder_topic2 = "MyPetFeeder/Time";    // 餵食時間
const char* feeder_topic3 = "MyPetFeeder/Connected";
const char* feeder_topic4 = "MyPetFeeder/workState";
const char* feeder_topic5 = "MyPetFeeder/fixFeedTime1"; // 固定餵食時間1
const char* feeder_topic6 = "MyPetFeeder/fixFeedTime2"; // 固定餵食時間2
const char* feeder_topic7 = "MyPetFeeder/fixFeedTime3"; // 固定餵食時間3
const char* feeder_topic8 = "MyPetFeeder/fixFeedTime4"; // 固定餵食時間4
const char* feeder_topic9 = "MyPetFeeder/fixFeedTime5"; // 固定餵食時間5
const char* feeder_topic10 = "MyPetFeeder/fixFeedTime6"; // 固定餵食時間6
const char* feeder_topic11 = "MyPetFeeder/fixFeedTimeView"; // 顯示固定餵食時間
const byte feedPin = 14;  // 連接到 relay 的腳位() 控制放出飼料
bool feedState = false;   // 餵食按鈕狀態 (預設OFF)
byte feedTime = 0;        // 餵食時間 (預設 0 秒)
unsigned long lastRecodeTime = 0;
unsigned long lastRecodeTime2 = 0;
char lastRecodeTimeMessage[50];  // 紀錄時間發佈到 MQTT broker，做為系統運作指示

//MQTT 訂閱監聽
void callback(char* topic, byte* payload, unsigned int length) {
  Serial.printf("Message arrived [%s] ", topic);
  for (int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
  }
  Serial.println();

  // 判斷餵食按鈕是否按下
  if (!strcmp(topic, feeder_topic1) && (char)payload[0] == '1') {
    feedState = true;
  } else {
    feedState = false;
  }

  // 判斷餵食時間為多久(秒)
  if (!strcmp(topic, feeder_topic2) && (char)payload[0] != '0') {
    //payload[length] = '\0'; // 可以將 byte 加入 \0 在結尾轉換為 String
    char* receivePayload = (char*)payload;
    if (isDigit(receivePayload[0])) feedTime = atoi(receivePayload);  // atoi() 將字串轉成整，無法轉換則傳回 0
  }

  // 判斷是否有設定自動餵食時間1
  if (!strcmp(topic, feeder_topic5) && (char)payload[0] != '0') {
    fixFeedTime1 = "";
    for (int i = 0; i < length; i++) {
      fixFeedTime1 += (char)payload[i];
    }
  }

  // 判斷是否有設定自動餵食時間2
  if (!strcmp(topic, feeder_topic6) && (char)payload[0] != '0') {
    fixFeedTime2 = "";
    for (int i = 0; i < length; i++) {
      fixFeedTime2 += (char)payload[i];
    }
  }

  // 判斷是否有設定自動餵食時間3
  if (!strcmp(topic, feeder_topic7) && (char)payload[0] != '0') {
    fixFeedTime3 = "";
    for (int i = 0; i < length; i++) {
      fixFeedTime3 += (char)payload[i];
    }
  }

  // 判斷是否有設定自動餵食時間4
  if (!strcmp(topic, feeder_topic8) && (char)payload[0] != '0') {
    fixFeedTime4 = "";
    for (int i = 0; i < length; i++) {
      fixFeedTime4 += (char)payload[i];
    }
  }

  // 判斷是否有設定自動餵食時間5
  if (!strcmp(topic, feeder_topic9) && (char)payload[0] != '0') {
    fixFeedTime5 = "";
    for (int i = 0; i < length; i++) {
      fixFeedTime5 += (char)payload[i];
    }
  }

  // 判斷是否有設定自動餵食時間6
  if (!strcmp(topic, feeder_topic10) && (char)payload[0] != '0') {
    fixFeedTime6 = "";
    for (int i = 0; i < length; i++) {
      fixFeedTime6 += (char)payload[i];
    }
  }

  Serial.printf("餵食狀態：%s | 餵食時間：%d 秒\n", feedState ? "ON" : "OFF", feedTime);

  if (feedState && feedTime != 0) {
    char workState[30];
    snprintf(workState, 30, "開始餵食 %d 秒\n", feedTime);
    client.publish(feeder_topic4, workState);
    Serial.print(workState);

    digitalWrite(feedPin, LOW);       // 輸出為 HIGH 馬達運轉供應飼料
    digitalWrite(BUILTIN_LED, LOW);   // 開啟內建指示燈
    delay(feedTime * 1000);           // 餵食時間(秒)
    digitalWrite(feedPin, HIGH);      // 時間到停止馬達運轉
    digitalWrite(BUILTIN_LED, HIGH);  // 關閉內建指示燈
    delay(10);
    feedState = false;
    client.publish(feeder_topic1, "0");
  }
}

void reconnect() {
  // Loop until we're reconnected
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Create a random client ID
    String clientId = "ESP8266Client-";
    clientId += String(random(0xffff), HEX);
    // Attempt to connect
    if (client.connect(clientId.c_str())) {
      Serial.println("connected");
      // Once connected, publish an announcement...
      Serial.print("All Topic initialized...");
      client.publish(feeder_topic1, "0");
      client.publish(feeder_topic2, "0");
      Serial.println("Done");
      // ... and resubscribe
      client.subscribe(feeder_topic1);
      client.subscribe(feeder_topic2);
      client.subscribe(feeder_topic5);
      client.subscribe(feeder_topic6);
      client.subscribe(feeder_topic7);
      client.subscribe(feeder_topic8);
      client.subscribe(feeder_topic9);
      client.subscribe(feeder_topic10);
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

/* ============================== */

void setup() {

  Serial.begin(115200);

  // 讀取 file system 內的 SSID 及 password 連線 WIFI
  if (initLittleFS()) {
    ssid = readLittleFS(SSID_PATH);
    pass = readLittleFS(PASS_PATH);
    delay(10);
  }

  Serial.println("WIFI 開始連線");
  Serial.println();
  Serial.print("連線到：");
  Serial.println(ssid);
  WiFi.setSleep(false);                    // 關閉 WIFI 休眠
  WiFi.mode(WIFI_STA);                     // 切換 WiFi 到 STA mode
  WiFi.begin(ssid.c_str(), pass.c_str());  // ssid, pass 為 String，要用 c_str() 轉換為字串常數

  int n = 0;  // WIFI 嘗試連線次數,也可以判斷連線時間 millis()
  while (WiFi.status() != WL_CONNECTED && n < 20) {
    delay(500);
    Serial.print(".");
    // vTaskDelay(200 / portTICK_PERIOD_MS);
    n++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WIFI 連線已逾時，開啟 WIFI AP 模式");
    initAPMode();
  }
  if (WiFi.status() == WL_CONNECTED) {
    randomSeed(micros());  // 每次連線重置亂數種子(MQTT client ID 使用)
    Serial.println("");
    Serial.println("WIFI 成功連線");
    Serial.print("IP 位置：");
    Serial.println(WiFi.localIP());
  }

  // Async web server 處理
  initServerHandle();

  // MQTT broker 連線
  pinMode(feedPin, OUTPUT);
  pinMode(BUILTIN_LED, OUTPUT);
  feedState = false;
  feedTime = 0;
  digitalWrite(feedPin, HIGH);
  digitalWrite(BUILTIN_LED, HIGH);
  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);

  // 網路校時 NTP 連線
  udp.begin(2390);  // Local port to listen for UDP packets
}

void loop() {
  // MQTT broker 持續連線
  if (!client.connected() && WiFi.status() == WL_CONNECTED) {
    reconnect();
  }
  if (client.connected()) {
    /*
    // 若 MQTT 有持續連，發布持續開機時間做為運作指示
    unsigned long now = millis(); // 紀錄持續開機時間
    // 每 5 秒發布一次時間紀錄作為開機指示
    if (now - lastRecodeTime > 5000) {
      lastRecodeTime = now;
      snprintf(lastRecodeTimeMessage, 50, "開機已經過 %ld 秒", now/1000); //將 now 值印出在字串內，儲存到 lastRecodeTimeMessage，長度最多為 50，超過最大值-1的自動截斷
      client.publish(feeder_topic3, lastRecodeTimeMessage); // 將開機時間發佈到 MQTT broker 作為機器運作中指示
    }
    */
  }
  client.loop();

  // 網路校時 NTP 連線讀取
  unsigned long now = millis();  // 紀錄持續開機時間
  // 每 5 秒讀取一次 NTP
  if (now - lastRecodeTime >= 5000) {
    lastRecodeTime = now;

    // get a random server from the pool
    WiFi.hostByName(ntpServerName, timeServerIP);

    sendNTPpacket(timeServerIP);  // send an NTP packet to a time server
    // wait to see if a reply is available
    delay(200);

    int cb = udp.parsePacket();
    if (!cb) {
      Serial.println("no packet yet");
    } else {
      // Serial.print("packet received, length=");
      // Serial.println(cb);
      // We've received a packet, read the data from it
      udp.read(packetBuffer, NTP_PACKET_SIZE);  // read the packet into the buffer

      // the timestamp starts at byte 40 of the received packet and is four bytes,
      //  or two words, long. First, esxtract the two words:

      unsigned long highWord = word(packetBuffer[40], packetBuffer[41]);
      unsigned long lowWord = word(packetBuffer[42], packetBuffer[43]);
      // combine the four bytes (two words) into a long integer
      // this is NTP time (seconds since Jan 1 1900):
      unsigned long secsSince1900 = highWord << 16 | lowWord;
      // Serial.print("Seconds since Jan 1 1900 = ");
      // Serial.println(secsSince1900);

      // now convert NTP time into everyday time:
      // Serial.print("Unix time = ");
      // Unix time starts on Jan 1 1970. In seconds, that's 2208988800:
      const unsigned long seventyYears = 2208988800UL;
      // subtract seventy years:
      unsigned long epoch = secsSince1900 - seventyYears;
      // print Unix time:
      // Serial.println(epoch);


      // print the hour, minute and second:
      Serial.print("The UTC time is ");                 // UTC is the time at Greenwich Meridian (GMT)
      Serial.print(((epoch % 86400L) + 28800) / 3600);  // print the hour (86400 equals secs per day), +28800 (8 * 3600) means UTC+8
      Serial.print(':');
      if (((epoch % 3600) / 60) < 10) {
        // In the first 10 minutes of each hour, we'll want a leading '0'
        Serial.print('0');
      }
      Serial.print((epoch % 3600) / 60);  // print the minute (3600 equals secs per minute)
      Serial.print(':');
      if ((epoch % 60) < 10) {
        // In the first 10 seconds of each minute, we'll want a leading '0'
        Serial.print('0');
      }
      Serial.println(epoch % 60);  // print the second

      //將值印出在字串內，儲存到 lastRecodeTimeMessage，長度最多為 50，超過最大值-1的自動截斷
      snprintf(lastRecodeTimeMessage, 50, "自動餵食機運作中 (%d:%d:%d)", ((epoch % 86400L) + 28800) / 3600, (epoch % 3600) / 60, epoch % 60);
      client.publish(feeder_topic3, lastRecodeTimeMessage);  // 將機器時間發佈到 MQTT broker 作為機器運作中指示

      snprintf(matchTime, 8, "%d:%d", ((epoch % 86400L) + 28800) / 3600, (epoch % 3600) / 60); // 自動餵食比對時間
      snprintf(fixFeedTimeView, 50, "固定 (T1:%s T2:%s T3:%s T4:%s T5:%s T6:%s)", fixFeedTime1, fixFeedTime2, fixFeedTime3, fixFeedTime4, fixFeedTime5, fixFeedTime6);
      client.publish(feeder_topic11, fixFeedTimeView);  // 將固定餵食時間發佈到 MQTT broker 觀看
      
    }
  }

  // 自動餵食比對
  if (now - lastRecodeTime2 >= 60000) {
    lastRecodeTime2 = now;
    if (fixFeedTime1.equals(matchTime)) {
      char workState[30];
      snprintf(workState, 30, "開始定時餵食 %d 秒\n", feedTime);
      client.publish(feeder_topic4, workState);
      Serial.print(workState);

      digitalWrite(feedPin, LOW);       // 輸出為 HIGH 馬達運轉供應飼料
      digitalWrite(BUILTIN_LED, LOW);   // 開啟內建指示燈
      delay(feedTime * 1000);           // 餵食時間(秒)
      digitalWrite(feedPin, HIGH);      // 時間到停止馬達運轉
      digitalWrite(BUILTIN_LED, HIGH);  // 關閉內建指示燈
    }
    if (fixFeedTime2.equals(matchTime)) {
      char workState[30];
      snprintf(workState, 30, "開始定時餵食 %d 秒\n", feedTime);
      client.publish(feeder_topic4, workState);
      Serial.print(workState);

      digitalWrite(feedPin, LOW);       // 輸出為 HIGH 馬達運轉供應飼料
      digitalWrite(BUILTIN_LED, LOW);   // 開啟內建指示燈
      delay(feedTime * 1000);           // 餵食時間(秒)
      digitalWrite(feedPin, HIGH);      // 時間到停止馬達運轉
      digitalWrite(BUILTIN_LED, HIGH);  // 關閉內建指示燈
    }
    if (fixFeedTime3.equals(matchTime)) {
      char workState[30];
      snprintf(workState, 30, "開始定時餵食 %d 秒\n", feedTime);
      client.publish(feeder_topic4, workState);
      Serial.print(workState);

      digitalWrite(feedPin, LOW);       // 輸出為 HIGH 馬達運轉供應飼料
      digitalWrite(BUILTIN_LED, LOW);   // 開啟內建指示燈
      delay(feedTime * 1000);           // 餵食時間(秒)
      digitalWrite(feedPin, HIGH);      // 時間到停止馬達運轉
      digitalWrite(BUILTIN_LED, HIGH);  // 關閉內建指示燈
    }
    if (fixFeedTime4.equals(matchTime)) {
      char workState[30];
      snprintf(workState, 30, "開始定時餵食 %d 秒\n", feedTime);
      client.publish(feeder_topic4, workState);
      Serial.print(workState);

      digitalWrite(feedPin, LOW);       // 輸出為 HIGH 馬達運轉供應飼料
      digitalWrite(BUILTIN_LED, LOW);   // 開啟內建指示燈
      delay(feedTime * 1000);           // 餵食時間(秒)
      digitalWrite(feedPin, HIGH);      // 時間到停止馬達運轉
      digitalWrite(BUILTIN_LED, HIGH);  // 關閉內建指示燈
    }
    if (fixFeedTime5.equals(matchTime)) {
      char workState[30];
      snprintf(workState, 30, "開始定時餵食 %d 秒\n", feedTime);
      client.publish(feeder_topic4, workState);
      Serial.print(workState);

      digitalWrite(feedPin, LOW);       // 輸出為 HIGH 馬達運轉供應飼料
      digitalWrite(BUILTIN_LED, LOW);   // 開啟內建指示燈
      delay(feedTime * 1000);           // 餵食時間(秒)
      digitalWrite(feedPin, HIGH);      // 時間到停止馬達運轉
      digitalWrite(BUILTIN_LED, HIGH);  // 關閉內建指示燈
    }
    if (fixFeedTime6.equals(matchTime)) {
      char workState[30];
      snprintf(workState, 30, "開始定時餵食 %d 秒\n", feedTime);
      client.publish(feeder_topic4, workState);
      Serial.print(workState);

      digitalWrite(feedPin, LOW);       // 輸出為 HIGH 馬達運轉供應飼料
      digitalWrite(BUILTIN_LED, LOW);   // 開啟內建指示燈
      delay(feedTime * 1000);           // 餵食時間(秒)
      digitalWrite(feedPin, HIGH);      // 時間到停止馬達運轉
      digitalWrite(BUILTIN_LED, HIGH);  // 關閉內建指示燈
    }
  }
}
