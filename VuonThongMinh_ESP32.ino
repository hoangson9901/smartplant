#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include <DHT.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ===================== WIFI =====================
const char* WIFI_SSID = "Son";
const char* WIFI_PASSWORD = "son123456";

// ===================== FIREBASE RTDB =====================
const char* FIREBASE_HOST = "https://vuonthongminh-bfabc-default-rtdb.firebaseio.com";
const char* FIREBASE_PATH_DATA = "/vuonthongminh/data.json";
const char* FIREBASE_PATH_STATE = "/vuonthongminh/state.json";
const char* FIREBASE_PATH_CONFIG = "/vuonthongminh/config.json";
const char* FIREBASE_PATH_HISTORY = "/vuonthongminh/history.json";
const char* FIREBASE_PATH_NHATKY = "/vuonthongminh/nhatky.json";

// ===================== NTP =====================
const long GMT_OFFSET_SEC = 7L * 3600L;
const int DAYLIGHT_OFFSET_SEC = 0;
const char* NTP_SERVER_1 = "pool.ntp.org";
const char* NTP_SERVER_2 = "time.google.com";

// ===================== GPIO =====================
const int PIN_DHT = 4;
const int PIN_SOIL_AOUT = 34;  // ADC1, phu hop khi dung Wi-Fi
const int PIN_RELAY_PUMP = 26;
const int PIN_LCD_SDA = 21;
const int PIN_LCD_SCL = 22;

// Doi thanh false neu relay cua ban kich muc LOW.
const bool RELAY_ACTIVE_HIGH = true;

// ===================== CALIBRATION =====================
const int SOIL_KHO = 2900;
const int SOIL_UOT = 1200;

// ===================== CYCLES =====================
const unsigned long CHU_KY_KIEM_TRA_MS = 3000UL;
const unsigned long CHU_KY_GUI_FIREBASE_MS = 15000UL;
const unsigned long CHU_KY_GUI_HISTORY_MS = 60000UL; // Luu lich su moi 60 giay
const int SO_MAU_LICH_SU_TOI_DA = 100; // Vong dem co dinh, khong tang vo han
const unsigned long CHU_KY_DOC_CONFIG_MS = 5000UL;
const unsigned long THOI_GIAN_THU_LAI_WIFI_MS = 10000UL;
const unsigned long THOI_GIAN_NGHI_GIUA_CAC_LAN_TUOI_MS = 30000UL;
const unsigned long HTTP_TIMEOUT_MS = 3000UL;

// ===================== DEVICES =====================
DHT cbNhietDoAm(PIN_DHT, DHT22);
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ===================== STATE =====================
bool dangTuoi = false;
bool manualTimedOut = false;
unsigned long thoiDiemBatBom = 0;
unsigned long thoiDiemKetThucTuoi = 0;

unsigned long lanKiemTraTruoc = 0;
unsigned long lanGuiFirebaseTruoc = 0;
unsigned long lanGuiHistoryTruoc = 0;
int historySlot = 0;
unsigned long lanDocConfigTruoc = 0;
unsigned long lanThuKetNoiWiFiTruoc = 0;

float nhietDo = NAN;
float doAmKhongKhi = NAN;
int doAmDatPhanTram = 0;
int soilRawCuoi = 0;
bool camBienDatHopLe = false;
bool dhtHopLe = false;
bool daDongBoNTP = false;

// ===================== WEB CONFIG =====================
String cheDo = "auto";          // "auto" hoac "manual"
bool lenhBom = false;
int nguongDoAmDatTuoi = 35;       // Bat bom khi do am dat < nguong nay
int nguongDungTuoiDat = 60;       // Tat bom khi do am dat >= nguong nay
unsigned long thoiGianTuoiMs = 10000UL;

// --------------------- HELPERS ---------------------
String firebaseUrl(const String& path) {
  return String(FIREBASE_HOST) + path;
}

void setRelay(bool on) {
  bool outputHigh = RELAY_ACTIVE_HIGH ? on : !on;
  digitalWrite(PIN_RELAY_PUMP, outputHigh ? HIGH : LOW);
}

bool thoiGianHopLe() {
  time_t now = time(nullptr);
  return now >= 1700000000; 
}

long long timestampHienTai() {
  if (!thoiGianHopLe()) return 0;
  return (long long)time(nullptr);
}

void hienThiLCD() {
  char dong1[17];
  char dong2[17];

  if (isnan(nhietDo) || isnan(doAmKhongKhi)) {
    snprintf(dong1, sizeof(dong1), "T:--C H:--%%");
  } else {
    snprintf(dong1, sizeof(dong1), "T:%2.0fC H:%2.0f%%", nhietDo, doAmKhongKhi);
  }

  if (dangTuoi) {
    snprintf(dong2, sizeof(dong2), "Dat:%3d%% BOM:ON", doAmDatPhanTram);
  } else {
    snprintf(dong2, sizeof(dong2), "Dat:%3d%% %s",
             doAmDatPhanTram, cheDo == "auto" ? "AUTO" : "MANUAL");
  }

  lcd.setCursor(0, 0);
  lcd.print("                ");
  lcd.setCursor(0, 0);
  lcd.print(dong1);

  lcd.setCursor(0, 1);
  lcd.print("                ");
  lcd.setCursor(0, 1);
  lcd.print(dong2);
}

void kiemTraVaKetNoiWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  unsigned long now = millis();
  if (now - lanThuKetNoiWiFiTruoc >= THOI_GIAN_THU_LAI_WIFI_MS) {
    lanThuKetNoiWiFiTruoc = now;
    Serial.println("Wi-Fi mat ket noi, dang thu ket noi lai...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
}

void dongBoThoiGian() {
  // configTime khoi dong dong bo NTP bat dong bo; khong cho loop dung lau.
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER_1, NTP_SERVER_2);
  daDongBoNTP = thoiGianHopLe();
  Serial.println(daDongBoNTP ? "Thoi gian da hop le." : "Dang cho dong bo NTP...");
}

void docCamBien() {
  soilRawCuoi = analogRead(PIN_SOIL_AOUT);

  // Bao ve co ban: ADC sat rail thuong la cam bien/duong day co van de.
  // Can hieu chuan lai SOIL_KHO/SOIL_UOT theo cam bien thuc te.
  camBienDatHopLe = (SOIL_KHO != SOIL_UOT && soilRawCuoi >= 50 && soilRawCuoi <= 4000);
  if (camBienDatHopLe) {
    doAmDatPhanTram = map(soilRawCuoi, SOIL_KHO, SOIL_UOT, 0, 100);
    doAmDatPhanTram = constrain(doAmDatPhanTram, 0, 100);
  }

  float t = cbNhietDoAm.readTemperature();
  float h = cbNhietDoAm.readHumidity();
  dhtHopLe = !isnan(t) && !isnan(h) && t >= -40 && t <= 80 && h >= 0 && h <= 100;
  if (dhtHopLe) {
    nhietDo = t;
    doAmKhongKhi = h;
  } else {
    nhietDo = NAN;
    doAmKhongKhi = NAN;
  }

  hienThiLCD();
}

bool httpPutJson(const String& path, JsonDocument& doc) {
  if (WiFi.status() != WL_CONNECTED) return false;

  String payload;
  serializeJson(doc, payload);

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(firebaseUrl(path))) return false;
  http.addHeader("Content-Type", "application/json");
  int code = http.PUT(payload);
  http.end();
  return code >= 200 && code < 300;
}

bool httpPatchJson(const String& path, JsonDocument& doc) {
  if (WiFi.status() != WL_CONNECTED) return false;
  String payload;
  serializeJson(doc, payload);
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(firebaseUrl(path))) return false;
  http.addHeader("Content-Type", "application/json");
  int code = http.sendRequest("PATCH", payload);
  http.end();
  return code >= 200 && code < 300;
}

void dongLenhBomTrenFirebase() {
  lenhBom = false;
  JsonDocument patch;
  patch["lenhBom"] = false;
  if (!httpPatchJson(String(FIREBASE_PATH_CONFIG), patch)) {
    Serial.println("Khong cap nhat duoc lenhBom=false len Firebase; khoa an toan van giu bom tat.");
  }
}

bool httpPostJson(const String& path, JsonDocument& doc) {
  if (WiFi.status() != WL_CONNECTED) return false;

  String payload;
  serializeJson(doc, payload);

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(firebaseUrl(path))) return false;
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(payload);
  http.end();
  return code >= 200 && code < 300;
}

void ghiNhatKy(const char* thietBi, bool bat, const char* lyDo) {
  if (WiFi.status() != WL_CONNECTED) return;

  JsonDocument doc;
  doc["thietBi"] = thietBi;
  doc["hanhDong"] = bat ? "bat" : "tat";
  doc["lyDo"] = lyDo;
  doc["thoiGian"] = timestampHienTai();
  httpPostJson(FIREBASE_PATH_NHATKY, doc);
}

void batBom(bool bat, const char* lyDo) {
  if (bat == dangTuoi) return;

  dangTuoi = bat;
  setRelay(dangTuoi);

  if (dangTuoi) {
    thoiDiemBatBom = millis();
    Serial.printf("BOM BAT: %s\n", lyDo);
  } else {
    thoiDiemKetThucTuoi = millis();
    Serial.printf("BOM TAT: %s\n", lyDo);
  }

  ghiNhatKy("bom", dangTuoi, lyDo);
  hienThiLCD();
}

void xuLyBom() {
  unsigned long now = millis();

  if (!camBienDatHopLe) {
    if (dangTuoi) batBom(false, "loi cam bien dat");
    if (cheDo == "manual" && lenhBom) {
      manualTimedOut = true;
      dongLenhBomTrenFirebase();
    }
    return;
  }

  if (cheDo == "manual") {
    if (!lenhBom) {
      manualTimedOut = false;
      if (dangTuoi) batBom(false, "lenh tay tat bom");
      return;
    }

    if (manualTimedOut) {
      if (dangTuoi) batBom(false, "lenh tay het thoi gian an toan");
      return;
    }

    if (!dangTuoi) {
      batBom(true, "lenh tay tu web");
    }

    if (dangTuoi && now - thoiDiemBatBom >= thoiGianTuoiMs) {
      batBom(false, "lenh tay het thoi gian an toan");
      manualTimedOut = true;
      dongLenhBomTrenFirebase();
    }
    return;
  }

  bool datKho = doAmDatPhanTram < nguongDoAmDatTuoi;
  bool datDaDuAm = doAmDatPhanTram >= nguongDungTuoiDat;

  if (dangTuoi) {
    if (datDaDuAm) {
      batBom(false, "dat da du do am");
    } else if (now - thoiDiemBatBom >= thoiGianTuoiMs) {
      batBom(false, "het thoi gian tuoi toi da");
    }
    return;
  }

  bool daNghiDu = (thoiDiemKetThucTuoi == 0) ||
                  (now - thoiDiemKetThucTuoi >= THOI_GIAN_NGHI_GIUA_CAC_LAN_TUOI_MS);

  if (datKho && daNghiDu) {
    batBom(true, "do am dat thap hon nguong");
  }
}

void guiDuLieuFirebase() {
  if (WiFi.status() != WL_CONNECTED) return;

  JsonDocument docData;
  if (!isnan(nhietDo)) docData["nhietDo"] = nhietDo;
  if (!isnan(doAmKhongKhi)) docData["doAmKhongKhi"] = doAmKhongKhi;
  docData["doAmDat"] = doAmDatPhanTram;
  docData["soilRaw"] = soilRawCuoi;
  docData["capNhatLuc"] = timestampHienTai();
  docData["dhtHopLe"] = dhtHopLe;
  docData["camBienDatHopLe"] = camBienDatHopLe;
  httpPutJson(FIREBASE_PATH_DATA, docData);

  JsonDocument docState;
  docState["bom"] = dangTuoi;
  docState["cheDo"] = cheDo;
  docState["capNhatLuc"] = timestampHienTai();
  docState["camBienDatHopLe"] = camBienDatHopLe;
  docState["dhtHopLe"] = dhtHopLe;
  docState["manualTimedOut"] = manualTimedOut;
  httpPutJson(FIREBASE_PATH_STATE, docState);
}

void guiLichSuFirebase() {
  if (WiFi.status() != WL_CONNECTED) return;
  JsonDocument doc;
  if (dhtHopLe) {
    doc["nhietDo"] = nhietDo;
    doc["doAmKhongKhi"] = doAmKhongKhi;
  }
  doc["doAmDat"] = doAmDatPhanTram;
  doc["soilRaw"] = soilRawCuoi;
  doc["capNhatLuc"] = timestampHienTai();
  doc["dhtHopLe"] = dhtHopLe;
  doc["camBienDatHopLe"] = camBienDatHopLe;
  String path = String("/vuonthongminh/history/slot") + String(historySlot) + ".json";
  if (httpPutJson(path, doc)) {
    historySlot = (historySlot + 1) % SO_MAU_LICH_SU_TOI_DA;
  }
}

void docConfigTuFirebase() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(firebaseUrl(FIREBASE_PATH_CONFIG))) return;

  int code = http.GET();
  if (code == HTTP_CODE_OK) {
    String payload = http.getString();

    if (payload != "null" && payload.length() > 0) {
      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, payload);

      if (!err) {
        String webCheDo = doc["cheDo"] | "auto";
        if (webCheDo == "manual" || webCheDo == "auto") {
          cheDo = webCheDo;
        }

        lenhBom = doc["lenhBom"] | false;
        if (!lenhBom || cheDo != "manual") manualTimedOut = false;

        int webNguongBat = doc["nguongDoAmDat"] | 35;
        int webNguongTat = doc["nguongDungTuoiDat"] | 60;
        webNguongBat = constrain(webNguongBat, 0, 99);
        webNguongTat = constrain(webNguongTat, 1, 100);

        if (webNguongTat > webNguongBat) {
          nguongDoAmDatTuoi = webNguongBat;
          nguongDungTuoiDat = webNguongTat;
        }

        unsigned long webThoiGianGiay = doc["thoiGianTuoiGiay"] | 10UL;
        webThoiGianGiay = constrain(webThoiGianGiay, 1UL, 300UL);
        thoiGianTuoiMs = webThoiGianGiay * 1000UL;
      }
    }
  }
  http.end();
  hienThiLCD();
}

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(PIN_SOIL_AOUT, INPUT);
  pinMode(PIN_RELAY_PUMP, OUTPUT);
  setRelay(false); 

  analogReadResolution(12);

  Wire.begin(PIN_LCD_SDA, PIN_LCD_SCL);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("VUON THONG MINH");
  lcd.setCursor(0, 1);
  lcd.print("Dang khoi dong");

  cbNhietDoAm.begin();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start < 15000UL)) {
    delay(300);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Wi-Fi connected, IP: ");
    Serial.println(WiFi.localIP());
    dongBoThoiGian();
    docConfigTuFirebase();
  }

  docCamBien();
  lanKiemTraTruoc = millis();
  lanGuiFirebaseTruoc = millis();
  lanDocConfigTruoc = millis();
  hienThiLCD();
}

void loop() {
  unsigned long now = millis();

  kiemTraVaKetNoiWiFi();

  if (WiFi.status() == WL_CONNECTED && !thoiGianHopLe()) {
    static unsigned long lanThuNTP = 0;
    if (now - lanThuNTP >= 60000UL) {
      lanThuNTP = now;
      dongBoThoiGian();
    }
  }

  if (now - lanKiemTraTruoc >= CHU_KY_KIEM_TRA_MS) {
    lanKiemTraTruoc = now;
    docCamBien();
    xuLyBom();
  }

  if (dangTuoi) {
    xuLyBom();
  }

  if (now - lanGuiFirebaseTruoc >= CHU_KY_GUI_FIREBASE_MS) {
    lanGuiFirebaseTruoc = now;
    guiDuLieuFirebase();
  }

  if (now - lanGuiHistoryTruoc >= CHU_KY_GUI_HISTORY_MS) {
    lanGuiHistoryTruoc = now;
    guiLichSuFirebase();
  }

  if (now - lanDocConfigTruoc >= CHU_KY_DOC_CONFIG_MS) {
    lanDocConfigTruoc = now;
    docConfigTuFirebase();
  }
}