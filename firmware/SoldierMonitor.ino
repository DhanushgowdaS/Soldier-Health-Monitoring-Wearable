/*
 ESP32 Soldier Health Monitoring Wearable prototype.
 Sensors: MAX30102, DHT22 (ambient), MPU6050, UART GPS.
 UI: I2C LCD, LittleFS dashboard, WebSocket, Telegram and buzzer.
 Educational prototype only; not a medical device.
*/
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <LittleFS.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <DHT.h>
#include <TinyGPSPlus.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <MAX30105.h>
#include "heartRate.h"
#include "secrets.h"

constexpr int DHT_PIN=4, DHT_TYPE=DHT22, GPS_RX=16, GPS_TX=17, BUZZER_PIN=25;
constexpr int SDA_PIN=21, SCL_PIN=22;
constexpr float AMBIENT_HIGH_C=29.0f, HUMIDITY_HIGH=85.0f; // Temporary DHT22 Telegram test threshold: ambient temperature > 29 C
constexpr int HR_HIGH=130, HR_LOW=45;
constexpr uint32_t SENSOR_MS=2000, LCD_MS=1000, WS_MS=1000, TG_COOLDOWN_MS=60000;

WebServer server(80);
WebSocketsServer ws(81);
HardwareSerial GPSSerial(2);
TinyGPSPlus gps;
DHT dht(DHT_PIN,DHT_TYPE);
LiquidCrystal_I2C lcd(0x27,16,2);
Adafruit_MPU6050 mpu;
MAX30105 pulse;

struct State {
 float hr=NAN, ambient=NAN, humidity=NAN;
 double lat=0, lon=0;
 bool gpsFix=false, mpuOK=false, pulseOK=false, fall=false, buzzer=false;
 String activity="Unknown", status="STARTING", reason="";
} s;
float rates[4]={0,0,0,0};
uint8_t rateIndex=0;
uint32_t lastBeat=0, lastSensor=0, lastLcd=0, lastWs=0, lastTelegram=0, impactAt=0;
bool impactPending=false;
String lastAlert="";
bool telegramEnabled=false;

bool valid(float x){return isfinite(x);}
String mapUrl(){return s.gpsFix ? "https://maps.google.com/?q="+String(s.lat,6)+","+String(s.lon,6) : "";}

String makeJson(){
 JsonDocument d;
 if(valid(s.hr)) d["heartRate"]=s.hr; else d["heartRate"]=nullptr;
 if(valid(s.ambient)) d["ambientC"]=s.ambient; else d["ambientC"]=nullptr;
 if(valid(s.humidity)) d["humidity"]=s.humidity; else d["humidity"]=nullptr;
 d["spo2"]=nullptr; // Intentionally unavailable until validated SpO2 algorithm is integrated.
 d["gpsFix"]=s.gpsFix;
 if(s.gpsFix){d["latitude"]=s.lat;d["longitude"]=s.lon;}else{d["latitude"]=nullptr;d["longitude"]=nullptr;}
 d["mapsUrl"]=mapUrl(); d["activity"]=s.activity; d["fallDetected"]=s.fall;
 d["buzzerOn"]=s.buzzer; d["status"]=s.status; d["alertReason"]=s.reason;
 d["uptimeSeconds"]=millis()/1000;
 String out; serializeJson(d,out); return out;
}
void sendJson(){server.send(200,"application/json",makeJson());}
void serveFile(const char* path,const char* mime){
 File f=LittleFS.open(path,"r");
 if(!f){server.send(500,"text/plain",String("Missing LittleFS file: ")+path);return;}
 server.streamFile(f,mime); f.close();
}
void telegramAlert(const String& reason){
 if(!telegramEnabled || WiFi.status()!=WL_CONNECTED || millis()-lastTelegram<TG_COOLDOWN_MS)return;
 WiFiClientSecure client;
 client.setInsecure(); // Prototype only; production should validate Telegram's TLS certificate.
 HTTPClient http;
 String url=String("https://api.telegram.org/bot")+TELEGRAM_BOT_TOKEN+"/sendMessage";
 if(!http.begin(client,url))return;
 http.addHeader("Content-Type","application/json");
 String msg="SOLDIER HEALTH ALERT\nReason: "+reason;
 msg+="\nHeart rate: "+(valid(s.hr)?String(s.hr,0)+" BPM":String("Unavailable"));
 msg+="\nAmbient temperature: "+(valid(s.ambient)?String(s.ambient,1)+" C":String("Unavailable"));
 msg+="\nHumidity: "+(valid(s.humidity)?String(s.humidity,0)+" %":String("Unavailable"));
 msg+="\nActivity: "+s.activity+"\nFall: "+String(s.fall?"POSSIBLE FALL":"No");
 if(s.gpsFix) msg+="\nLatitude: "+String(s.lat,6)+"\nLongitude: "+String(s.lon,6)+"\nLocation: "+mapUrl();
 else msg+="\nGPS: no recent fix";
 JsonDocument d; d["chat_id"]=TELEGRAM_CHAT_ID; d["text"]=msg;
 String body; serializeJson(d,body);
 int code=http.POST(body); Serial.printf("Telegram HTTP status: %d\n",code); http.end();
 if(code>=200 && code<300)lastTelegram=millis();
}
void readPulse(){
 if(!s.pulseOK)return;
 long ir=pulse.getIR();
 if(ir<50000){s.hr=NAN;return;}
 if(checkForBeat(ir)){
  uint32_t now=millis();
  if(lastBeat){float bpm=60000.0f/(now-lastBeat);if(bpm>=35 && bpm<=220)rates[rateIndex++%4]=bpm;
   float sum=0;int n=0;for(float v:rates)if(v>0){sum+=v;n++;}
   if(n)s.hr=sum/n;
  }lastBeat=now;
 }
}
void readGps(){
 while(GPSSerial.available())gps.encode(GPSSerial.read());
 s.gpsFix=gps.location.isValid() && gps.location.age()<5000;
 if(s.gpsFix){s.lat=gps.location.lat();s.lon=gps.location.lng();}
}
void readMotion(){
 if(!s.mpuOK)return;
 sensors_event_t a,g,t;mpu.getEvent(&a,&g,&t);
 float ax=a.acceleration.x/9.80665f, ay=a.acceleration.y/9.80665f, az=a.acceleration.z/9.80665f;
 float amag=sqrtf(ax*ax+ay*ay+az*az);
 float gm=sqrtf(g.gyro.x*g.gyro.x+g.gyro.y*g.gyro.y+g.gyro.z*g.gyro.z);
 if(amag>2.5f){impactPending=true;impactAt=millis();}
 // Demonstration heuristic only: impact followed by near-zero acceleration and gyro.
 if(impactPending && millis()-impactAt>1500 && amag<0.45f && gm<0.35f){s.fall=true;s.activity="Possible fall";impactPending=false;}
 if(impactPending && millis()-impactAt>10000)impactPending=false;
 if(!s.fall)s.activity=(amag>1.2f||gm>0.6f)?"Active":"Stationary";
}
void checkAlerts(){
 String reason="";
 if(valid(s.hr)&&s.hr>HR_HIGH)reason+="High heart rate; ";
 if(valid(s.hr)&&s.hr<HR_LOW)reason+="Low heart rate; ";
 if(valid(s.ambient)&&s.ambient>AMBIENT_HIGH_C)reason+="High ambient temperature; ";
 if(valid(s.humidity)&&s.humidity>HUMIDITY_HIGH)reason+="High humidity; ";
 if(s.fall)reason+="Possible fall detected; ";
 s.reason=reason;s.buzzer=reason.length()>0;
 digitalWrite(BUZZER_PIN,s.buzzer?HIGH:LOW);
 s.status=s.buzzer?"WARNING":(s.gpsFix?"MONITORING":"WAITING FOR GPS");
 if(reason.length() && reason!=lastAlert){telegramAlert(reason);lastAlert=reason;}
 if(!reason.length())lastAlert="";
}
void updateLcd(){
 lcd.clear();lcd.setCursor(0,0);
 lcd.print("HR:");if(valid(s.hr))lcd.print((int)s.hr);else lcd.print("--");
 lcd.print(" T:");if(valid(s.ambient))lcd.print(s.ambient,0);else lcd.print("--");
 lcd.setCursor(0,1);
 if(s.fall)lcd.print("FALL ALERT");
 else if(s.buzzer)lcd.print("HEALTH WARNING");
 else if(s.gpsFix)lcd.print("GPS OK ");
 else lcd.print("GPS SEARCH");
 if(valid(s.humidity)){lcd.print(" ");lcd.print((int)s.humidity);lcd.print("%");}
}
void setup(){
 Serial.begin(115200);pinMode(BUZZER_PIN,OUTPUT);digitalWrite(BUZZER_PIN,LOW);
 Wire.begin(SDA_PIN,SCL_PIN);lcd.init();lcd.backlight();lcd.print("Soldier Monitor");
 dht.begin();
 s.mpuOK=mpu.begin();
 if(s.mpuOK){mpu.setAccelerometerRange(MPU6050_RANGE_8_G);mpu.setGyroRange(MPU6050_RANGE_500_DEG);mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);}
 else Serial.println("MPU6050 not detected");
 s.pulseOK=pulse.begin(Wire,I2C_SPEED_FAST);
 if(s.pulseOK){pulse.setup();pulse.setPulseAmplitudeRed(0x0A);pulse.setPulseAmplitudeGreen(0);}
 else Serial.println("MAX30102 not detected");
 GPSSerial.begin(9600,SERIAL_8N1,GPS_RX,GPS_TX);
 if(!LittleFS.begin(true))Serial.println("LittleFS mount failed");
 WiFi.mode(WIFI_STA);WiFi.begin(WIFI_SSID,WIFI_PASSWORD);
 uint32_t start=millis();while(WiFi.status()!=WL_CONNECTED&&millis()-start<15000){delay(250);Serial.print(".");}
 if(WiFi.status()==WL_CONNECTED){Serial.print("\nDashboard: http://");Serial.println(WiFi.localIP());}
 else Serial.println("\nWi-Fi not connected.");
 telegramEnabled=String(TELEGRAM_BOT_TOKEN).indexOf("REPLACE")<0 && String(TELEGRAM_CHAT_ID).indexOf("REPLACE")<0;
 server.on("/",HTTP_GET,[]{serveFile("/index.html","text/html");});
 server.on("/style.css",HTTP_GET,[]{serveFile("/style.css","text/css");});
 server.on("/script.js",HTTP_GET,[]{serveFile("/script.js","application/javascript");});
 server.on("/api/data",HTTP_GET,sendJson);server.begin();
 ws.begin();ws.onEvent([](uint8_t,WStype_t type,uint8_t*,size_t){if(type==WStype_CONNECTED)ws.broadcastTXT(makeJson());});
 Serial.println("WebSocket port 81");
}
void loop(){
 server.handleClient();ws.loop();readGps();
 uint32_t now=millis();
 if(now-lastSensor>=SENSOR_MS){lastSensor=now;s.ambient=dht.readTemperature();s.humidity=dht.readHumidity();readPulse();readMotion();checkAlerts();}
 if(now-lastLcd>=LCD_MS){lastLcd=now;updateLcd();}
 if(now-lastWs>=WS_MS){lastWs=now;ws.broadcastTXT(makeJson());}
}
