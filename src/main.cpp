#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <ESPAsyncWebServer.h>
#ifdef ESP8266
#include <ESP8266WiFi.h>
#endif
// #include <Update.h>
#include "FS.h"
#ifdef ESP32
#include "SPIFFS.h"
#endif
#include "AsyncJson.h"
#include "ArduinoJson.h"
#include <SPIFFSEditor.h>
#include "Adafruit_TLC5947.h"
// #include <ArduinoOTA.h>
#include <ESP8266httpUpdate.h>

// To upload to FS using curl:
// curl 10.7.10.23/edit --form 'data=@index.htm;filename=/index.htm' -v
// or
// espupload.py <ip> <filename> [<filename>]

// To run an update over the air
//   cd .pio/build/d1_mini
//   python3 -m http.server
//   curl http://10.1.10.41/ota?url=http%3A%2F%2F10.1.10.2%3A8000%2Ffirmware.bin
// First IP is the IP of the ESP, the second IP in the %2F is the IP of the host

#define VERSION "v0.4.5"

// #define DEBUG

#ifdef DEBUG
#define DEBUG_PRINT(x) Serial.print(x)
#define DEBUG_PRINTDEC(x) Serial.print(x, DEC)
#define DEBUG_PRINTLN(x) Serial.println(x)
#else
#define DEBUG_PRINT(x)
#define DEBUG_PRINTDEC(x)
#define DEBUG_PRINTLN(x)
#endif

#include "wificredentials.h"

#ifdef ESP32
#define DN 18
#define CLK 19
#define LAT 21
uint8_t pin_allocations[] = {15, 2, 0};
#else
#define DN D5
#define CLK D6
#define LAT D7
#define LAT2 D3
uint8_t pin_allocations[] = {D1, D2, D8};
#endif

#define PWM_MAX 3750 // @12v

#define MAX_NUM_STRIPS 3
#define MAX_NUM_LED_PER_STRIP 60
#define MAX_NUM_LEDS MAX_NUM_STRIPS *MAX_NUM_LED_PER_STRIP
#define BRIGHTNESS 255

#define MAX_NUM_PWM_BOARDS 4
#define PWM_PER_BOARD 24
// *2 to allow for 2x boards
#define MAX_NUM_PWM_LEDS MAX_NUM_PWM_BOARDS *PWM_PER_BOARD * 2

#define SCHEME_UPDATES 40 // 25 updates per second
// #define SCHEME_UPDATES 100 // 10 updates per second

#define WEB_PORT 80

// Types of LED Strings
#define RGBC 'c'
#define PWMC 'p'
#define RGBS "rgb"
#define PWMS "pwm"

uint16_t strip_sizes[MAX_NUM_STRIPS];
uint16_t total_pixels = 0;

uint16_t total_pwm_pixels = 0;
uint16_t pwm_pixels_board_1 = 0;

bool dorainbow = false;
byte dotestmode = false;
bool pwmpixelupdates = true;

bool stripupdated[MAX_NUM_STRIPS];
Adafruit_NeoPixel strips[MAX_NUM_STRIPS];
Adafruit_NeoPixel neofuncs = Adafruit_NeoPixel(0, -1, NEO_GRB + NEO_KHZ800);

Adafruit_TLC5947 pwm = Adafruit_TLC5947(0, CLK, DN, LAT);
Adafruit_TLC5947 pwm2 = Adafruit_TLC5947(0, CLK, DN, LAT2);

struct RGBW
{
  byte r;
  byte g;
  byte b;
  byte w;
};

struct Scheme
{
  byte type;
  RGBW start;
  RGBW end;
  unsigned int dur;
  unsigned int altdur;
  unsigned long state;
  unsigned int cooldown;
};

struct PwmScheme
{
  byte type;
  uint16_t start;
  uint16_t end;
  unsigned int dur;
  unsigned int altdur;
  unsigned long state;
  uint8_t state2;
  unsigned int cooldown;
};

Scheme schemes[MAX_NUM_LEDS];
PwmScheme pwmschemes[MAX_NUM_PWM_LEDS];

// Schemes that are known/supported, also needs to be copied to JS code
#define NONE 0
#define FADE 1
#define FADE_ONE_WAY 2
#define FLICKER 3
#define FLASH 4

AsyncWebServer server(WEB_PORT);
AsyncWebSocket ws("/ws");

unsigned long nextschemeupdate = SCHEME_UPDATES;
unsigned long nextupdate = 0;
#define UPDATE_FREQ 1000;

bool configchanged = false;
bool updatenonescheme = true;
unsigned long nextsave = 0;
#define MAX_SAVE_FREQ 10000;

extern void setRGBScheme(int i, String jsondata);
extern void setPWMScheme(int i, String jsondata);
extern uint16_t gamma16(uint16_t x);

unsigned long hextolong(String hex)
{
  return strtoul(hex.c_str(), NULL, 16);
}

RGBW toRGBW(unsigned long src)
{
  RGBW ret;
  ret.r = (src >> 16) & 0xFF;
  ret.g = (src >> 8) & 0xFF;
  ret.b = (src >> 0) & 0xFF;
  ret.w = (src >> 24) & 0xFF;
  return ret;
}
RGBW toRGBW(String srcstr)
{
  return toRGBW(hextolong(srcstr));
}

unsigned long fromRGBW(RGBW src)
{
  unsigned long ret;
  ret = src.r;
  ret += src.g << 8;
  ret += src.b << 16;
  ret += src.w << 24;
  return ret;
}

void loadStripConfig()
{
  File f = SPIFFS.open("/stripdata.txt", "r");
  if (!f)
  {
    Serial.println("file open failed (stripdata)");
  }
  else
  {
    int i = 0;
    total_pixels = 0;
    while (true)
    {
      String s = f.readStringUntil('\n');
      Serial.printf("Got stripsize: [%s]\n", s.c_str());
      strip_sizes[i] = s.toInt();
      if (strip_sizes[i] > MAX_NUM_LED_PER_STRIP)
      {
        strip_sizes[i] = MAX_NUM_LED_PER_STRIP;
        Serial.printf("Strip too big, forcing to: [%u]\n", strip_sizes[i]);
      }
      total_pixels += strip_sizes[i];
      Serial.printf("Setting size of strip %d to %d\n", i, strip_sizes[i]);

      // Actually configure the strip
      strips[i].updateLength(strip_sizes[i]);
      strips[i].updateType(NEO_GRB + NEO_KHZ800);
      strips[i].setPin(pin_allocations[i]);
      strips[i].setBrightness(BRIGHTNESS);
      strips[i].begin();
      stripupdated[i] = true;
      delay(0);
      i++;
      if (i >= MAX_NUM_STRIPS)
      {
        break;
      }
    }
    f.close();
    // Ensure all unused strips are set to zero size
    while (i < MAX_NUM_STRIPS)
    {
      strip_sizes[i] = 0;
      i++;
    }
  }
}

void loadPWMConfig()
{
  File f = SPIFFS.open("/pwmdata.txt", "r");
  if (!f)
  {
    Serial.println("file open failed (pwmdata)");
  }
  else
  {
    total_pwm_pixels = 0;
    for (int i = 1; i <= 2; i++)
    {
      String s = f.readStringUntil('\n');
      Serial.printf("Got PWM Board %d count: [%s]\n", i, s.c_str());
      uint16_t boardcount = s.toInt();
      if (boardcount > MAX_NUM_PWM_BOARDS)
      {
        boardcount = MAX_NUM_PWM_BOARDS;
        Serial.printf("PWM board count too big, forcing to: [%u]\n", boardcount);
      }
      total_pwm_pixels += boardcount * PWM_PER_BOARD;
      if (i == 1)
      {
        DEBUG_PRINT("Setting PWM board 1 board count to: ");
        DEBUG_PRINTLN(boardcount);
        pwm = Adafruit_TLC5947(boardcount, CLK, DN, LAT);
        pwm_pixels_board_1 = boardcount * PWM_PER_BOARD;
      }
      else if (i == 2)
      {
        DEBUG_PRINT("Setting PWM board 2 board count to: ");
        DEBUG_PRINTLN(boardcount);
        pwm2 = Adafruit_TLC5947(boardcount, CLK, DN, LAT2);
      }
    }
    Serial.printf("Total PWM Pixels: %d. Board 1 Pixels: %d\n", total_pwm_pixels, pwm_pixels_board_1);
    pwm.begin();
    pwm2.begin();
    delay(0);
  }
  f.close();
}

uint32_t applyGamma(uint32_t c)
{
  uint8_t r = neofuncs.gamma8((uint8_t)(c >> 16));
  uint8_t g = neofuncs.gamma8((uint8_t)(c >> 8));
  uint8_t b = neofuncs.gamma8((uint8_t)c);
  uint32_t color = (r << 16) + (g << 8) + (b);
  return color;
}

void setPixelColor(uint16_t n, uint32_t c)
{
  if (n >= total_pixels)
  {
    return;
  }
  // First figure out the strip to apply this to
  int i = 0;
  while (i < MAX_NUM_STRIPS)
  {
    if (n >= strips[i].numPixels())
    {
      n -= strips[i].numPixels();
      i++;
    }
    else
    {
      break;
    }
  }
  // Apply gamma correction to each color component
  // XXX need to handle white should we support it down the track
  strips[i].setPixelColor(n, applyGamma(c));
  stripupdated[i] = true;
}

uint32_t getPixelColor(uint16_t n)
{
  // First figure out the strip to apply this to
  int i = 0;
  while (i < MAX_NUM_STRIPS)
  {
    if (n >= strips[i].numPixels())
    {
      n -= strips[i].numPixels();
      i++;
    }
    else
    {
      break;
    }
  }
  return strips[i].getPixelColor(n);
}

uint16_t getPWMPixel(uint16_t n)
{
  if (n < pwm_pixels_board_1)
  {
    return pwm.getPWM(n);
  }
  else
  {
    return pwm2.getPWM(n - pwm_pixels_board_1);
  }
}

void setPWMPixel(uint16_t n, uint16_t b)
{
  DEBUG_PRINT("Updating PWM Pixel: ");
  DEBUG_PRINT(n);
  DEBUG_PRINT(" To: ");
  DEBUG_PRINTLN(b);
  if (n >= total_pwm_pixels)
  {
    return;
  }
  uint16_t gval = gamma16(b);
  if (gval != getPWMPixel(n))
  {
    if (n < pwm_pixels_board_1)
    {
      pwm.setPWM(n, gval);
    }
    else
    {
      pwm2.setPWM(n - pwm_pixels_board_1, gval);
    }
    pwmpixelupdates = true;
  }
}

void showAll()
{
  for (int i = 0; i < MAX_NUM_STRIPS; i++)
  {
    if (strip_sizes[i] > 0 && stripupdated[i])
    {
      strips[i].show();
      stripupdated[i] = false;
    }
  }
}

void showAllPWM()
{
  if (pwmpixelupdates)
  {
    pwm.write();
    pwm2.write();
    pwmpixelupdates = false;
  }
}

// Types of incoming websocket messages - must match index.htm script
#define COLOR_UPDATE String("U\n")
#define SCHEME_UPDATE String("S\n")

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len)
{
  if (type == WS_EVT_CONNECT)
  {
    Serial.printf("ws[%s][%u] connect\n", server->url(), client->id());
    client->printf("Hello Client %u :)", client->id());
    client->ping();
  }
  else if (type == WS_EVT_DISCONNECT)
  {
    Serial.printf("ws[%s][%ul] disconnect: \n", server->url(), client->id());
  }
  else if (type == WS_EVT_ERROR)
  {
    Serial.printf("ws[%s][%u] error(%u): %s\n", server->url(), client->id(), *((uint16_t *)arg), (char *)data);
  }
  else if (type == WS_EVT_PONG)
  {
    Serial.printf("ws[%s][%u] pong[%u]: %s\n", server->url(), client->id(), len, (len) ? (char *)data : "");
  }
  else if (type == WS_EVT_DATA)
  {
    AwsFrameInfo *info = (AwsFrameInfo *)arg;
    String msg = "";
    if (info->final && info->index == 0 && info->len == len)
    {
      //the whole message is in a single frame and we got all of it's data
      Serial.printf("ws[%s][%u] %s-message[%llu]: ", server->url(), client->id(), (info->opcode == WS_TEXT) ? "text" : "binary", info->len);

      if (info->opcode == WS_TEXT)
      {
        for (size_t i = 0; i < info->len; i++)
        {
          msg += (char)data[i];
        }
      }
      else
      {
        char buff[3];
        for (size_t i = 0; i < info->len; i++)
        {
          sprintf(buff, "%02x ", (uint8_t)data[i]);
          msg += buff;
        }
      }
      Serial.printf("%s\n", msg.c_str());

      if (info->opcode == WS_TEXT)
      {
        String tmp;
        if (msg.startsWith(COLOR_UPDATE))
        {
          uint16_t led;
          uint32_t color;
          msg.remove(0, 2);
          // Calculated from https://arduinojson.org/v5/assistant/
          // plus a bunch of margin, global did not work
          StaticJsonBuffer<(500)> globalJsonBuffer;
          JsonObject &js = globalJsonBuffer.parseObject(msg);
          tmp = js["led"].as<String>();
          led = tmp.toInt();
          tmp = js["color"].as<String>();
          color = hextolong(tmp);
          setPixelColor(led, color);
        }
        else if (msg.startsWith(SCHEME_UPDATE))
        {
          uint16_t led;
          msg.remove(0, 2);
          led = msg.toInt();
          msg.remove(0, msg.indexOf('\n') + 1);
          String ledtype = msg.substring(0, msg.indexOf('\n'));
          msg.remove(0, msg.indexOf('\n') + 1);
          if (ledtype.equals(RGBS))
          {
            setRGBScheme(led, msg);
          }
          else if (ledtype.equals(PWMS))
          {
            setPWMScheme(led, msg);
          }
          else
          {
            Serial.print("Unknown scheme! ");
            Serial.println(ledtype);
          }
        }
        client->text("OK");
      }
      else
        client->binary("I got your binary message");
    }
    else
    {
      //message is comprised of multiple frames or the frame is split into multiple packets
      if (info->index == 0)
      {
        if (info->num == 0)
          Serial.printf("ws[%s][%u] %s-message start\n", server->url(), client->id(), (info->message_opcode == WS_TEXT) ? "text" : "binary");
        Serial.printf("ws[%s][%u] frame[%u] start[%llu]\n", server->url(), client->id(), info->num, info->len);
      }

      Serial.printf("ws[%s][%u] frame[%u] %s[%llu - %llu]: ", server->url(), client->id(), info->num, (info->message_opcode == WS_TEXT) ? "text" : "binary", info->index, info->index + len);

      if (info->opcode == WS_TEXT)
      {
        for (size_t i = 0; i < info->len; i++)
        {
          msg += (char)data[i];
        }
      }
      else
      {
        char buff[3];
        for (size_t i = 0; i < info->len; i++)
        {
          sprintf(buff, "%02x ", (uint8_t)data[i]);
          msg += buff;
        }
      }
      Serial.printf("%s\n", msg.c_str());

      if ((info->index + len) == info->len)
      {
        Serial.printf("ws[%s][%u] frame[%u] end[%llu]\n", server->url(), client->id(), info->num, info->len);
        if (info->final)
        {
          Serial.printf("ws[%s][%u] %s-message end\n", server->url(), client->id(), (info->message_opcode == WS_TEXT) ? "text" : "binary");
          if (info->message_opcode == WS_TEXT)
            client->text("I have no idea... some part of the message...");
          else
            client->binary("I got your binary message");
        }
      }
    }
  }
}

uint16_t identifyled = 0;
byte identifytype = '\x0';
unsigned long identifyexpiry = 0;
uint16_t identify_i = 0;
#define IDENTIFY_DURATION 10 * 1000
void handleIdentify(AsyncWebServerRequest *request)
{
  Serial.println("Identifying...");
  String message = "OK";
  String sled;
  String stype;

  sled = request->arg("led");
  stype = request->arg("ledtype");
  identifyled = sled.toInt();
  identify_i = 0;
  if (stype == RGBS)
  {
    identifytype = RGBC;
    identifyexpiry = millis() + IDENTIFY_DURATION;
  }
  else if (stype == PWMS)
  {
    identifytype = PWMC;
    identifyexpiry = millis() + IDENTIFY_DURATION;
  }
  else
  {
    identifyexpiry = 0;
    message = "Invalid data";
  }
  Serial.println(message);
  request->send(200, "text/plain", message);
}

void runIdentify()
{
  uint32_t coloron = neofuncs.Color(255, 0, 255);
  uint32_t coloroff = neofuncs.Color(0, 0, 0);
  uint16_t led = 0;
  if (identify_i % 4 == 0)
  {
    Serial.println("Identify: All off");
    for (led = 0; led < total_pixels; led++)
    {
      setPixelColor(led, coloroff);
    }
    for (led = 0; led < total_pwm_pixels; led++)
    {
      setPWMPixel(led, 0);
    }
    if (identify_i % 8 == 0)
    {
      Serial.print("Identify: on: ");
      Serial.println(identifyled);
      if (identifytype == RGBC)
      {
        setPixelColor(identifyled, coloron);
      }
      else
      {
        setPWMPixel(identifyled, 4000);
      }
    }
    else
    {
      // Ensure everything is off before exiting
      if (millis() > identifyexpiry)
      {
        Serial.println("Identify: Complete");
        identifyexpiry = 0;
        updatenonescheme = true;
      }
    }
    delay(0);
    showAll();
    delay(0);
    showAllPWM();
  }
  identify_i++;
  delay(50);
}

String url = "";
bool doUpdate = false;

void handleOta(AsyncWebServerRequest *request)
{
  Serial.print("Starting OTA: ");
  Serial.println(millis());
  String message = "";

  if (request->hasArg("url"))
  {
    url = request->arg("url");
    message.concat("Scheduling update from URL: ");
    message.concat(url);
    message.concat("\n");
    doUpdate = true;
  }
  else
  {
    message.concat("No URL supplied");
  }
  Serial.println(message);
  request->send(200, "text/plain", message);
}

void runOta()
{
  doUpdate = false;
  WiFiClient client;
  ESPhttpUpdate.setLedPin(LED_BUILTIN, LOW);
  t_httpUpdate_return ret = ESPhttpUpdate.update(client, url, VERSION);
  switch (ret)
  {
  case HTTP_UPDATE_FAILED:
    Serial.printf("HTTP_UPDATE_FAILED Error (%d): %s\n", ESPhttpUpdate.getLastError(), ESPhttpUpdate.getLastErrorString().c_str());
    break;
  case HTTP_UPDATE_NO_UPDATES:
    Serial.println("[update] Update no Update.");
    break;
  case HTTP_UPDATE_OK:
    Serial.println("[update] Update ok."); // may not be called since we reboot the ESP
    break;
  }
}

void handleUpdate(AsyncWebServerRequest *request)
{
  Serial.print("Starting: ");
  Serial.println(millis());

  String message = "Updated\n";

  Serial.print("Made Message: ");
  Serial.println(millis());
  uint16_t led;
  uint32_t color;
  int params = request->params();
  for (int i = 0; i < params; i++)
  {
    AsyncWebParameter *p = request->getParam(i);
    led = p->name().toInt();
    Serial.print("Setting LED :");
    Serial.print(led);
    Serial.print(" to:");
    color = hextolong(p->value().c_str());
    Serial.println(color);
    setPixelColor(led, color);
  }

  request->send(200, "text/plain", message);
}

void handleLedCount(AsyncWebServerRequest *request)
{
  String message = String(total_pixels);
  request->send(200, "text/plain", message);
}

void handleRainbow(AsyncWebServerRequest *request)
{
  String message = "Set";
  request->send(200, "text/plain", message);
  dorainbow = true;
}

void handleTestMode(AsyncWebServerRequest *request)
{
  String sid = request->arg("id");
  byte id = sid.toInt();
  String message;
  if (dotestmode)
  {
    message = "OFF";
    dotestmode = 0;
  }
  else
  {
    message = "ON: ";
    dotestmode = id;
    message.concat(id);
  }
  request->send(200, "text/plain", message);
}

void handleUpdateScheme(AsyncWebServerRequest *request)
{
  String message = "Updating\n";
  request->send(200, "text/plain", message);
  configchanged = true;
  updatenonescheme = true;
}

void handleConfig(AsyncWebServerRequest *request)
{
  char hex[10];
  AsyncJsonResponse *response = new AsyncJsonResponse();
  response->addHeader("Server", "ESP Async Web Server");
  JsonObject &root = response->getRoot();
  root["heap"] = ESP.getFreeHeap();
  root["ssid"] = WiFi.SSID();
  root["version"] = VERSION;
  JsonArray &colors = root.createNestedArray("colors");
  for (int i = 0; i < total_pixels; i++)
  {
    // Serial.printf("LED: %d color %08x\n", i, getPixelColor(i));
    sprintf(hex, "%06X", getPixelColor(i));
    colors.add(hex);
  }
  JsonArray &brightness = root.createNestedArray("brightness");
  for (int i = 0; i < total_pwm_pixels; i++)
  {
    //Serial.printf("LED: %d brightness %d\n", i, getPWMPixel(i));
    brightness.add(getPWMPixel(i));
  }
  response->setLength();
  request->send(response);
}

byte schemeTypeLookup(String stype)
{
  if (stype.equals("fade"))
    return FADE;
  else if (stype.equals("fadeoneway"))
    return FADE_ONE_WAY;
  else if (stype.equals("flicker"))
    return FLICKER;
  else if (stype.equals("flash"))
    return FLASH;
  else
    return NONE;
}

void setRGBScheme(int i, String jsondata)
{
  // Calculated from https://arduinojson.org/v5/assistant/
  // plus a bunch of margin, global did not work
  StaticJsonBuffer<(600)> globalJsonBuffer;
  JsonObject &jscheme = globalJsonBuffer.parseObject(jsondata);
  String tmp;

  if (jscheme.containsKey("start"))
  {
    tmp = jscheme["start"].as<String>();
    schemes[i].start = toRGBW(tmp);
  }
  if (jscheme.containsKey("end"))
  {
    tmp = jscheme["end"].as<String>();
    schemes[i].end = toRGBW(tmp);
  }
  if (jscheme.containsKey("dur"))
  {
    schemes[i].dur = jscheme["dur"];
  }
  if (jscheme.containsKey("altdur"))
  {
    schemes[i].altdur = jscheme["altdur"];
  }
  if (jscheme.containsKey("cdwn"))
  {
    schemes[i].cooldown = jscheme["cdwn"];
  }

  schemes[i].type = schemeTypeLookup(jscheme["sc"]);
  schemes[i].state = 0;

  updatenonescheme = true;
}

void setPWMScheme(int i, String jsondata)
{
  // Calculated from https://arduinojson.org/v5/assistant/
  // plus a bunch of margin, global did not work
  StaticJsonBuffer<(600)> globalJsonBuffer;
  JsonObject &jscheme = globalJsonBuffer.parseObject(jsondata);
  String tmp;

  if (jscheme.containsKey("start"))
  {
    pwmschemes[i].start = jscheme["start"];
  }
  if (jscheme.containsKey("end"))
  {
    pwmschemes[i].end = jscheme["end"];
  }
  if (jscheme.containsKey("dur"))
  {
    pwmschemes[i].dur = jscheme["dur"];
  }
  if (jscheme.containsKey("altdur"))
  {
    pwmschemes[i].altdur = jscheme["altdur"];
  }
  if (jscheme.containsKey("cdwn"))
  {
    pwmschemes[i].cooldown = jscheme["cdwn"];
  }

  pwmschemes[i].type = schemeTypeLookup(jscheme["sc"]);
  pwmschemes[i].state = 0;

  updatenonescheme = true;
}

// Loads the schemes from file files
// Stored as 1 file per LED
void loadSchemes()
{
  Serial.println("Loading Schemes...");
  unsigned long et = millis();

  File f = SPIFFS.open("/schemes.txt", "r");
  if (!f)
  {
    Serial.println("file open failed (schemes)");
    File f = SPIFFS.open("/schemes.txt", "w");
    f.print("");
  }
  else
  {
    char ledtype = RGBC;
    while (1)
    {
      String ledstr = f.readStringUntil(':');
      String jsondata = f.readStringUntil('\n');
      if (ledstr.equals(PWMS))
      {
        ledtype = PWMC;
        continue;
      }
      if (jsondata.length() == 0)
      {
        break;
      }
      int i = ledstr.toInt();
      if (ledtype == RGBC)
      {
        setRGBScheme(i, jsondata);
      }
      else if (ledtype == PWMC)
      {
        setPWMScheme(i, jsondata);
      }

      delay(0);
    }
  }
  f.close();
  Serial.print("Total time:");
  Serial.println(millis() - et);
}

byte colprogress(byte start, byte end, float pcg)
{
  int total = end - start;
  byte ret = start + (total * pcg);
  return ret;
}

RGBW blend(RGBW a, RGBW b, float pcg)
{
  RGBW out;
  out.r = colprogress(a.r, b.r, pcg);
  out.g = colprogress(a.g, b.g, pcg);
  out.b = colprogress(a.b, b.b, pcg);
  out.w = colprogress(a.w, b.w, pcg);
  return out;
}

void updateRGBSchemes(unsigned long now)
{
  for (int i = 0; i < total_pixels; i++)
  {
    Scheme scheme = schemes[i];
    RGBW out;
    bool updated = false;
    if (scheme.type == NONE)
    {
      if (updatenonescheme)
      {
        out = scheme.start;
        updated = true;
      }
    }
    else if (scheme.type == FADE)
    {
      unsigned long step = now % scheme.dur;
      float pcg = (float)step / (float)scheme.dur;
      if (pcg < 0.5)
        pcg = pcg * 2;
      else
        pcg = (1.0 - pcg) * 2;
      out = blend(scheme.start, scheme.end, pcg);
      updated = true;
    }
    else if (scheme.type == FADE_ONE_WAY)
    {
      unsigned long step = now % scheme.dur;
      float pcg = (float)step / (float)scheme.dur;
      out = blend(scheme.start, scheme.end, pcg);
      updated = true;
    }

    else if (scheme.type == FLICKER)
    {
      if (now >= scheme.state)
      {
        // Time to change colour
        unsigned long currentcol = getPixelColor(i);
        if (currentcol == applyGamma(neofuncs.Color(scheme.start.r, scheme.start.g, scheme.start.b)))
        {
          // turn to end state, and wait alt duration
          out = scheme.end;
          schemes[i].state = now + random(scheme.altdur) + 1;
        }
        else
        {
          out = scheme.start;
          schemes[i].state = now + random(scheme.dur) + 1;
        }
        updated = true;
      }
    }

    else if (scheme.type == FLASH)
    {
      unsigned long cycleoffset = now % (scheme.dur + scheme.altdur + scheme.cooldown);
      if (cycleoffset < scheme.dur || cycleoffset > (scheme.dur + scheme.altdur))
      {
        // Not in alt state, set to normal state
        if (scheme.state != 1)
        {
          out = scheme.start;
          schemes[i].state = 1;
          updated = true;
        }
      }

      else
      {
        // Turn to alt state
        if (scheme.state != 2)
        {
          out = scheme.end;
          schemes[i].state = 2;
          updated = true;
        }
      }
    }

    else if (scheme.type == 999) // l XXX
    {
      if (now >= scheme.state)
      {
        // unsigned long cycleoffset = now % (scheme.dur + scheme.altdur);
        // Time to change colour
        unsigned long currentcol = getPixelColor(i);
        if (currentcol == applyGamma(neofuncs.Color(scheme.start.r, scheme.start.g, scheme.start.b)))
        {
          // turn to end state, and wait alt duration
          out = scheme.end;
          schemes[i].state = now + scheme.altdur;
        }
        else
        {
          out = scheme.start;
          schemes[i].state = now + scheme.dur;
        }
        updated = true;
      }
    }
    if (updated)
    {
      setPixelColor(i, neofuncs.Color(out.r, out.g, out.b));
    }
    delay(0);
  }
}

void updatePWMSchemes(unsigned long now, int i)
{
  uint16_t out;
  bool updated = false;
  if (pwmschemes[i].type == NONE)
  {
    if (updatenonescheme)
    {
      out = pwmschemes[i].start;
      updated = true;
    }
  }
  else if (pwmschemes[i].type == FADE)
  {
    unsigned long step = now % pwmschemes[i].dur;
    float pcg = (float)step / (float)pwmschemes[i].dur;
    if (pcg < 0.5)
      pcg = pcg * 2;
    else
      pcg = (1.0 - pcg) * 2;
    out = pwmschemes[i].start + ((pwmschemes[i].end - pwmschemes[i].start) * pcg);
    updated = true;
  }
  else if (pwmschemes[i].type == FADE_ONE_WAY)
  {
    unsigned long step = now % pwmschemes[i].dur;
    float pcg = (float)step / (float)pwmschemes[i].dur;
    out = pwmschemes[i].start + ((pwmschemes[i].end - pwmschemes[i].start) * pcg);
    updated = true;
  }

  else if (pwmschemes[i].type == FLICKER)
  {
    if (now >= pwmschemes[i].state)
    {
      // Time to change state
      if (pwmschemes[i].state2)
      {
        // turn to end state, and wait alt duration
        out = pwmschemes[i].end;
        unsigned long rand = random(pwmschemes[i].altdur);
        pwmschemes[i].state = now + rand + 1;
        pwmschemes[i].state2 = 0;
      }
      else
      {
        out = pwmschemes[i].start;
        pwmschemes[i].state = now + random(pwmschemes[i].dur) + 1;
        pwmschemes[i].state2 = 1;
      }
      updated = true;
    }
  }

  else if (pwmschemes[i].type == FLASH)
  {
    unsigned long cycleoffset = now % (pwmschemes[i].dur + pwmschemes[i].altdur + pwmschemes[i].cooldown);
    if (cycleoffset < pwmschemes[i].dur || cycleoffset > (pwmschemes[i].dur + pwmschemes[i].altdur))
    {
      // Not in alt state, set to normal state
      if (pwmschemes[i].state != 1)
      {
        out = pwmschemes[i].start;
        pwmschemes[i].state = 1;
        updated = true;
      }
    }

    else
    {
      // Turn to alt state
      if (pwmschemes[i].state != 2)
      {
        out = pwmschemes[i].end;
        pwmschemes[i].state = 2;
        updated = true;
      }
    }
  }
  if (updated)
  {
    setPWMPixel(i, out);
  }
}

void updateAllPWMSchemes(unsigned long now)
{
  for (int pwm_i = 0; pwm_i < total_pwm_pixels; pwm_i++)
  {
    updatePWMSchemes(now, pwm_i);
  }
}

// Input a value 0 to 255 to get a color value.
// The colours are a transition r - g - b - back to r.
uint32_t Wheel(byte WheelPos)
{
  WheelPos = 255 - WheelPos;
  if (WheelPos < 85)
  {
    return neofuncs.Color(255 - WheelPos * 3, 0, WheelPos * 3, 0);
  }
  if (WheelPos < 170)
  {
    WheelPos -= 85;
    return neofuncs.Color(0, WheelPos * 3, 255 - WheelPos * 3, 0);
  }
  WheelPos -= 170;
  return neofuncs.Color(WheelPos * 3, 255 - WheelPos * 3, 0, 0);
}

// Slightly different, this makes the rainbow equally distributed throughout
void rainbowCycle(uint8_t wait)
{
  uint16_t i, j;

  for (j = 0; j < 256 * 5; j++)
  { // 5 cycles of all colors on wheel
    for (i = 0; i < total_pixels; i++)
    {
      setPixelColor(i, Wheel(((i * 256 / total_pixels) + j) & 255));
    }
    showAll();
    delay(wait);
  }
  //  configchanged = true;
  updatenonescheme = true;
}

#define TEST_LED_SPACING 7
// XXX debug start
void testMode2()
{
  int loop = 0;
  uint16_t bright = 0;
  while (dotestmode)
  {

    for (int i = 0; i < 24; i++)
    {
      if (loop % 2 == 0)
      {
        if (i % 2 == 0)
          bright = 1000;
        else
          bright = 0;
      }
      else
      {
        if (i % 2 == 1)
          bright = 1000;
        else
          bright = 0;
      }
      pwm2.setPWM(i, bright);
      if (loop % 5 == 0)
      {
        pwm.setPWM(i, bright);
      }
    }
    loop++;
    Serial.println(loop);
    pwm2.write();
    delay(0);
    pwm.write();
    delay(100);
  }
}

void testMode3()
{
  int loop = 0;
  while (dotestmode)
  {
    for (int i = 0; i < total_pwm_pixels; i++)
    {
      if (i <= loop)
      {
        setPWMPixel(i, 2000);
      }
      else
      {
        setPWMPixel(i, 0);
      }
    }
    loop++;
    if (loop > total_pwm_pixels)
    {
      loop = 0;
    }
    Serial.println(loop);
    showAllPWM();
    delay(200);
  }
}

// XXX end
void testMode1()
{
  // uint16_t rgbpixel = 0, pwmpixel = 0, i = 0;
  uint16_t i = 0;
  uint32_t coloron = neofuncs.Color(255, 0, 255);
  uint32_t coloroff = neofuncs.Color(0, 30, 0);
  uint8_t feedback = 1;
  uint16_t led = 0;

  while (dotestmode)
  {
    i++;
    if (i >= TEST_LED_SPACING)
    {
      i = 0;
      feedback = !feedback;
      if (feedback)
        coloroff = neofuncs.Color(0, 30, 0);
      else
        coloroff = neofuncs.Color(30, 0, 0);
    }
    for (led = 0; led < total_pixels; led++)
    {
      if (led % TEST_LED_SPACING == i)
      {
        setPixelColor(led, coloron);
      }
      else
      {
        setPixelColor(led, coloroff);
      }
    }
    for (led = 0; led < total_pwm_pixels; led++)
    {

      if (led % TEST_LED_SPACING == i)
      {
        setPWMPixel(led, 4095);
      }
      else
      {
        setPWMPixel(led, 200);
      }
    }
    digitalWrite(LED_BUILTIN, feedback);

    delay(0);
    showAll();
    delay(0);
    showAllPWM();
    delay(500);
    if (doUpdate)
    {
      runOta();
    }
  }
  digitalWrite(LED_BUILTIN, HIGH);
  updatenonescheme = true;
}

void setup()
{
  Serial.begin(115200);

  Serial.println("\n\n");
  Serial.println("Starting...\n\n");
  Serial.print("Version: ");
  Serial.println(VERSION);
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);
  pwm.begin();
  pwm2.begin();
  showAllPWM();

  Serial.println("Starting SPIFFS...");

#ifdef ESP32
  SPIFFS.begin(true);
#else
  SPIFFS.begin();
#endif
  Serial.println("Started SPIFFS");

  loadStripConfig();
  loadPWMConfig();
  showAll();

  int counter = 0;

  delay(0);
  Serial.print("\nJoining ");
  Serial.print(ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  // Wait for connection
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
    counter++;
    for (uint16_t i = 0; i < counter; i++)
    {
      setPixelColor(i, neofuncs.Color(100, 0, 0));
      setPWMPixel(i, 1500);
    }
    showAll();
    delay(0);
    showAllPWM();
  }
  Serial.println("");
  Serial.print("Connected to ");
  Serial.println(ssid);
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  for (uint16_t i = 0; i < 10; i++)
  {
    setPixelColor(i, neofuncs.Color(0, 20, 0));
  }
  showAll();

  // ArduinoOTA.onStart([]() {
  //   String type;
  //   if (ArduinoOTA.getCommand() == U_FLASH)
  //   {
  //     type = "sketch";
  //   }
  //   else
  //   { // U_FS
  //     type = "filesystem";
  //   }
  //   // NOTE: if updating FS this would be the place to unmount FS using FS.end()
  //   SPIFFS.end();
  //   Serial.println("OTA: Start updating " + type);
  // });
  // ArduinoOTA.onEnd([]() {
  //   Serial.println("\nEnd");
  // });
  // ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
  //   Serial.printf("OTA: %u%%  \r", (progress / (total / 100)));
  // });
  // ArduinoOTA.onError([](ota_error_t error) {
  //   Serial.printf("OTA: Error[%u]: ", error);
  //   if (error == OTA_AUTH_ERROR)
  //   {
  //     Serial.println("OTA: Auth Failed");
  //   }
  //   else if (error == OTA_BEGIN_ERROR)
  //   {
  //     Serial.println("OTA: Begin Failed");
  //   }
  //   else if (error == OTA_CONNECT_ERROR)
  //   {
  //     Serial.println("OTA: Connect Failed");
  //   }
  //   else if (error == OTA_RECEIVE_ERROR)
  //   {
  //     Serial.println("OTA: Receive Failed");
  //   }
  //   else if (error == OTA_END_ERROR)
  //   {
  //     Serial.println("OTA: End Failed");
  //   }
  // });

  // ArduinoOTA.setPort(8266);
  // ArduinoOTA.setHostname("ledsota");
  // ArduinoOTA.begin();

  delay(0);
  Serial.println("OTA: OTA Setup");

  // Web Server setup
#ifdef ESP32
  server.addHandler(new SPIFFSEditor(SPIFFS));
#else
  server.addHandler(new SPIFFSEditor());
#endif

  server.on("/heap", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", String(ESP.getFreeHeap()));
  });

  server.on("/update", HTTP_GET + HTTP_POST, handleUpdate);
  server.on("/updatescheme", HTTP_GET + HTTP_POST, handleUpdateScheme);
  server.on("/ledcount", HTTP_GET + HTTP_POST, handleLedCount);
  server.on("/rainbow", HTTP_GET + HTTP_POST, handleRainbow);
  server.on("/testmode", HTTP_GET + HTTP_POST, handleTestMode);
  server.on("/identify", HTTP_GET + HTTP_POST, handleIdentify);

  server.on("/config", HTTP_GET, handleConfig);
  server.on("/ota", HTTP_GET + HTTP_POST, handleOta);

  server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.htm");

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  delay(0);
  server.begin();
  Serial.println("HTTP server started");
  delay(0);
  loadSchemes();
  delay(0);
  randomSeed(micros());
  delay(0);

  Serial.print("Current MD5: ");
  Serial.println(ESP.getSketchMD5());
  Serial.print("Current Size: ");
  Serial.println(ESP.getSketchSize());
  Serial.print("Current Free Space: ");
  Serial.println(ESP.getFreeSketchSpace());
}

unsigned long nnow = 0;
void loop()
{
  unsigned long now = millis();
  if (now >= nextupdate)
  {
    Serial.printf("Status update at %lu, Free Heap: %d, configchanged: %d, Mins: %lu\n", millis(), ESP.getFreeHeap(), configchanged, millis() / 1000 / 60);
    delay(0);
    nextupdate = now + UPDATE_FREQ;
  }

  if (identifyexpiry)
  {
    runIdentify();
  }
  else
  {
    if (configchanged && now > nextsave)
    {
      Serial.println("Saving config");
      delay(0);
      loadSchemes();
      nextsave = now + MAX_SAVE_FREQ;
      configchanged = false;
    }
    if (now >= nextschemeupdate)
    {
      nnow = now - (now % SCHEME_UPDATES);
      updateRGBSchemes(nnow);
      updateAllPWMSchemes(nnow);
      updatenonescheme = false;
      nextschemeupdate = nnow + SCHEME_UPDATES - (nnow % SCHEME_UPDATES);
      delay(0);
      showAllPWM();
      showAll();
    }
  }

  if (dorainbow)
  {
    dorainbow = false;
    rainbowCycle(8);
  }
  if (dotestmode)
  {
    Serial.println("Entering testmode...");
    if (dotestmode == 1)
    {
      testMode1();
    }
    else if (dotestmode == 2)
    {
      testMode2();
    }
    else if (dotestmode == 3)
    {
      testMode3();
    }
    Serial.println("Test mode exit");
  }
  if (doUpdate)
  {
    runOta();
  }
}

/* A 12-bit gamma-correction table, sliding until maxv.
   Copy & paste this snippet into a Python 2 REPL to regenerate:

import math
maxv = 3750
gamma = 2.6
bits = 2 ** 12
scale = float(maxv) / bits
for x in range(bits):
    print( "{:3},".format( int((math.pow((x) / float(bits), gamma) * float(bits) + 0.5) * scale))),
    if x & 15 == 15:
        print
*/
static const uint16_t PROGMEM _gamma12Table[4096] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3,
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 6,
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
    6, 6, 6, 6, 6, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 8, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 9, 9, 9, 9, 9,
    9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11,
    11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 12, 12, 12, 12, 12,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 13, 13, 13, 13, 13, 13,
    13, 13, 13, 13, 13, 13, 13, 14, 14, 14, 14, 14, 14, 14, 14, 14,
    14, 14, 14, 14, 14, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    15, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 17, 17, 17,
    17, 17, 17, 17, 17, 17, 17, 17, 17, 18, 18, 18, 18, 18, 18, 18,
    18, 18, 18, 18, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 20,
    20, 20, 20, 20, 20, 20, 20, 20, 20, 21, 21, 21, 21, 21, 21, 21,
    21, 21, 21, 21, 22, 22, 22, 22, 22, 22, 22, 22, 22, 23, 23, 23,
    23, 23, 23, 23, 23, 23, 23, 24, 24, 24, 24, 24, 24, 24, 24, 24,
    25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 26, 26, 26, 26, 26, 26,
    26, 26, 26, 27, 27, 27, 27, 27, 27, 27, 27, 28, 28, 28, 28, 28,
    28, 28, 28, 28, 29, 29, 29, 29, 29, 29, 29, 29, 30, 30, 30, 30,
    30, 30, 30, 30, 31, 31, 31, 31, 31, 31, 31, 31, 32, 32, 32, 32,
    32, 32, 32, 32, 33, 33, 33, 33, 33, 33, 33, 33, 34, 34, 34, 34,
    34, 34, 34, 34, 35, 35, 35, 35, 35, 35, 35, 36, 36, 36, 36, 36,
    36, 36, 37, 37, 37, 37, 37, 37, 37, 37, 38, 38, 38, 38, 38, 38,
    38, 39, 39, 39, 39, 39, 39, 39, 40, 40, 40, 40, 40, 40, 40, 41,
    41, 41, 41, 41, 41, 42, 42, 42, 42, 42, 42, 42, 43, 43, 43, 43,
    43, 43, 43, 44, 44, 44, 44, 44, 44, 45, 45, 45, 45, 45, 45, 46,
    46, 46, 46, 46, 46, 46, 47, 47, 47, 47, 47, 47, 48, 48, 48, 48,
    48, 48, 49, 49, 49, 49, 49, 49, 50, 50, 50, 50, 50, 50, 51, 51,
    51, 51, 51, 51, 52, 52, 52, 52, 52, 52, 53, 53, 53, 53, 53, 53,
    54, 54, 54, 54, 54, 55, 55, 55, 55, 55, 55, 56, 56, 56, 56, 56,
    56, 57, 57, 57, 57, 57, 58, 58, 58, 58, 58, 58, 59, 59, 59, 59,
    59, 60, 60, 60, 60, 60, 61, 61, 61, 61, 61, 61, 62, 62, 62, 62,
    62, 63, 63, 63, 63, 63, 64, 64, 64, 64, 64, 65, 65, 65, 65, 65,
    66, 66, 66, 66, 66, 67, 67, 67, 67, 67, 68, 68, 68, 68, 68, 69,
    69, 69, 69, 69, 70, 70, 70, 70, 70, 71, 71, 71, 71, 71, 72, 72,
    72, 72, 72, 73, 73, 73, 73, 74, 74, 74, 74, 74, 75, 75, 75, 75,
    75, 76, 76, 76, 76, 77, 77, 77, 77, 77, 78, 78, 78, 78, 78, 79,
    79, 79, 79, 80, 80, 80, 80, 80, 81, 81, 81, 81, 82, 82, 82, 82,
    83, 83, 83, 83, 83, 84, 84, 84, 84, 85, 85, 85, 85, 86, 86, 86,
    86, 86, 87, 87, 87, 87, 88, 88, 88, 88, 89, 89, 89, 89, 90, 90,
    90, 90, 90, 91, 91, 91, 91, 92, 92, 92, 92, 93, 93, 93, 93, 94,
    94, 94, 94, 95, 95, 95, 95, 96, 96, 96, 96, 97, 97, 97, 97, 98,
    98, 98, 98, 99, 99, 99, 99, 100, 100, 100, 100, 101, 101, 101, 101, 102,
    102, 102, 102, 103, 103, 103, 104, 104, 104, 104, 105, 105, 105, 105, 106, 106,
    106, 106, 107, 107, 107, 108, 108, 108, 108, 109, 109, 109, 109, 110, 110, 110,
    110, 111, 111, 111, 112, 112, 112, 112, 113, 113, 113, 113, 114, 114, 114, 115,
    115, 115, 115, 116, 116, 116, 117, 117, 117, 117, 118, 118, 118, 119, 119, 119,
    119, 120, 120, 120, 121, 121, 121, 121, 122, 122, 122, 123, 123, 123, 123, 124,
    124, 124, 125, 125, 125, 125, 126, 126, 126, 127, 127, 127, 128, 128, 128, 128,
    129, 129, 129, 130, 130, 130, 131, 131, 131, 131, 132, 132, 132, 133, 133, 133,
    134, 134, 134, 134, 135, 135, 135, 136, 136, 136, 137, 137, 137, 138, 138, 138,
    139, 139, 139, 139, 140, 140, 140, 141, 141, 141, 142, 142, 142, 143, 143, 143,
    144, 144, 144, 145, 145, 145, 146, 146, 146, 146, 147, 147, 147, 148, 148, 148,
    149, 149, 149, 150, 150, 150, 151, 151, 151, 152, 152, 152, 153, 153, 153, 154,
    154, 154, 155, 155, 155, 156, 156, 156, 157, 157, 157, 158, 158, 158, 159, 159,
    159, 160, 160, 160, 161, 161, 161, 162, 162, 163, 163, 163, 164, 164, 164, 165,
    165, 165, 166, 166, 166, 167, 167, 167, 168, 168, 168, 169, 169, 170, 170, 170,
    171, 171, 171, 172, 172, 172, 173, 173, 173, 174, 174, 175, 175, 175, 176, 176,
    176, 177, 177, 177, 178, 178, 179, 179, 179, 180, 180, 180, 181, 181, 181, 182,
    182, 183, 183, 183, 184, 184, 184, 185, 185, 186, 186, 186, 187, 187, 187, 188,
    188, 189, 189, 189, 190, 190, 190, 191, 191, 192, 192, 192, 193, 193, 194, 194,
    194, 195, 195, 195, 196, 196, 197, 197, 197, 198, 198, 199, 199, 199, 200, 200,
    201, 201, 201, 202, 202, 202, 203, 203, 204, 204, 204, 205, 205, 206, 206, 206,
    207, 207, 208, 208, 208, 209, 209, 210, 210, 210, 211, 211, 212, 212, 212, 213,
    213, 214, 214, 215, 215, 215, 216, 216, 217, 217, 217, 218, 218, 219, 219, 219,
    220, 220, 221, 221, 222, 222, 222, 223, 223, 224, 224, 224, 225, 225, 226, 226,
    227, 227, 227, 228, 228, 229, 229, 230, 230, 230, 231, 231, 232, 232, 233, 233,
    233, 234, 234, 235, 235, 236, 236, 236, 237, 237, 238, 238, 239, 239, 240, 240,
    240, 241, 241, 242, 242, 243, 243, 243, 244, 244, 245, 245, 246, 246, 247, 247,
    247, 248, 248, 249, 249, 250, 250, 251, 251, 252, 252, 252, 253, 253, 254, 254,
    255, 255, 256, 256, 257, 257, 257, 258, 258, 259, 259, 260, 260, 261, 261, 262,
    262, 263, 263, 263, 264, 264, 265, 265, 266, 266, 267, 267, 268, 268, 269, 269,
    270, 270, 270, 271, 271, 272, 272, 273, 273, 274, 274, 275, 275, 276, 276, 277,
    277, 278, 278, 279, 279, 280, 280, 280, 281, 281, 282, 282, 283, 283, 284, 284,
    285, 285, 286, 286, 287, 287, 288, 288, 289, 289, 290, 290, 291, 291, 292, 292,
    293, 293, 294, 294, 295, 295, 296, 296, 297, 297, 298, 298, 299, 299, 300, 300,
    301, 301, 302, 302, 303, 303, 304, 304, 305, 305, 306, 306, 307, 307, 308, 308,
    309, 309, 310, 310, 311, 311, 312, 312, 313, 313, 314, 315, 315, 316, 316, 317,
    317, 318, 318, 319, 319, 320, 320, 321, 321, 322, 322, 323, 323, 324, 324, 325,
    326, 326, 327, 327, 328, 328, 329, 329, 330, 330, 331, 331, 332, 332, 333, 333,
    334, 335, 335, 336, 336, 337, 337, 338, 338, 339, 339, 340, 341, 341, 342, 342,
    343, 343, 344, 344, 345, 345, 346, 347, 347, 348, 348, 349, 349, 350, 350, 351,
    352, 352, 353, 353, 354, 354, 355, 355, 356, 357, 357, 358, 358, 359, 359, 360,
    360, 361, 362, 362, 363, 363, 364, 364, 365, 366, 366, 367, 367, 368, 368, 369,
    370, 370, 371, 371, 372, 372, 373, 374, 374, 375, 375, 376, 376, 377, 378, 378,
    379, 379, 380, 380, 381, 382, 382, 383, 383, 384, 385, 385, 386, 386, 387, 388,
    388, 389, 389, 390, 390, 391, 392, 392, 393, 393, 394, 395, 395, 396, 396, 397,
    398, 398, 399, 399, 400, 401, 401, 402, 402, 403, 404, 404, 405, 405, 406, 407,
    407, 408, 408, 409, 410, 410, 411, 412, 412, 413, 413, 414, 415, 415, 416, 416,
    417, 418, 418, 419, 420, 420, 421, 421, 422, 423, 423, 424, 424, 425, 426, 426,
    427, 428, 428, 429, 429, 430, 431, 431, 432, 433, 433, 434, 435, 435, 436, 436,
    437, 438, 438, 439, 440, 440, 441, 442, 442, 443, 443, 444, 445, 445, 446, 447,
    447, 448, 449, 449, 450, 450, 451, 452, 452, 453, 454, 454, 455, 456, 456, 457,
    458, 458, 459, 460, 460, 461, 462, 462, 463, 464, 464, 465, 466, 466, 467, 467,
    468, 469, 469, 470, 471, 471, 472, 473, 473, 474, 475, 475, 476, 477, 477, 478,
    479, 479, 480, 481, 481, 482, 483, 484, 484, 485, 486, 486, 487, 488, 488, 489,
    490, 490, 491, 492, 492, 493, 494, 494, 495, 496, 496, 497, 498, 499, 499, 500,
    501, 501, 502, 503, 503, 504, 505, 505, 506, 507, 507, 508, 509, 510, 510, 511,
    512, 512, 513, 514, 514, 515, 516, 517, 517, 518, 519, 519, 520, 521, 522, 522,
    523, 524, 524, 525, 526, 526, 527, 528, 529, 529, 530, 531, 531, 532, 533, 534,
    534, 535, 536, 536, 537, 538, 539, 539, 540, 541, 542, 542, 543, 544, 544, 545,
    546, 547, 547, 548, 549, 550, 550, 551, 552, 552, 553, 554, 555, 555, 556, 557,
    558, 558, 559, 560, 561, 561, 562, 563, 564, 564, 565, 566, 566, 567, 568, 569,
    569, 570, 571, 572, 572, 573, 574, 575, 575, 576, 577, 578, 578, 579, 580, 581,
    581, 582, 583, 584, 585, 585, 586, 587, 588, 588, 589, 590, 591, 591, 592, 593,
    594, 594, 595, 596, 597, 597, 598, 599, 600, 601, 601, 602, 603, 604, 604, 605,
    606, 607, 608, 608, 609, 610, 611, 611, 612, 613, 614, 615, 615, 616, 617, 618,
    618, 619, 620, 621, 622, 622, 623, 624, 625, 626, 626, 627, 628, 629, 630, 630,
    631, 632, 633, 634, 634, 635, 636, 637, 637, 638, 639, 640, 641, 642, 642, 643,
    644, 645, 646, 646, 647, 648, 649, 650, 650, 651, 652, 653, 654, 654, 655, 656,
    657, 658, 659, 659, 660, 661, 662, 663, 663, 664, 665, 666, 667, 668, 668, 669,
    670, 671, 672, 672, 673, 674, 675, 676, 677, 677, 678, 679, 680, 681, 682, 682,
    683, 684, 685, 686, 687, 687, 688, 689, 690, 691, 692, 692, 693, 694, 695, 696,
    697, 698, 698, 699, 700, 701, 702, 703, 703, 704, 705, 706, 707, 708, 709, 709,
    710, 711, 712, 713, 714, 715, 715, 716, 717, 718, 719, 720, 721, 721, 722, 723,
    724, 725, 726, 727, 728, 728, 729, 730, 731, 732, 733, 734, 735, 735, 736, 737,
    738, 739, 740, 741, 742, 742, 743, 744, 745, 746, 747, 748, 749, 749, 750, 751,
    752, 753, 754, 755, 756, 757, 757, 758, 759, 760, 761, 762, 763, 764, 765, 765,
    766, 767, 768, 769, 770, 771, 772, 773, 774, 774, 775, 776, 777, 778, 779, 780,
    781, 782, 783, 783, 784, 785, 786, 787, 788, 789, 790, 791, 792, 793, 794, 794,
    795, 796, 797, 798, 799, 800, 801, 802, 803, 804, 805, 805, 806, 807, 808, 809,
    810, 811, 812, 813, 814, 815, 816, 817, 818, 818, 819, 820, 821, 822, 823, 824,
    825, 826, 827, 828, 829, 830, 831, 832, 833, 833, 834, 835, 836, 837, 838, 839,
    840, 841, 842, 843, 844, 845, 846, 847, 848, 849, 850, 851, 852, 852, 853, 854,
    855, 856, 857, 858, 859, 860, 861, 862, 863, 864, 865, 866, 867, 868, 869, 870,
    871, 872, 873, 874, 875, 876, 877, 878, 879, 880, 880, 881, 882, 883, 884, 885,
    886, 887, 888, 889, 890, 891, 892, 893, 894, 895, 896, 897, 898, 899, 900, 901,
    902, 903, 904, 905, 906, 907, 908, 909, 910, 911, 912, 913, 914, 915, 916, 917,
    918, 919, 920, 921, 922, 923, 924, 925, 926, 927, 928, 929, 930, 931, 932, 933,
    934, 935, 936, 937, 938, 939, 940, 941, 942, 943, 944, 945, 946, 947, 948, 949,
    950, 951, 952, 954, 955, 956, 957, 958, 959, 960, 961, 962, 963, 964, 965, 966,
    967, 968, 969, 970, 971, 972, 973, 974, 975, 976, 977, 978, 979, 980, 981, 982,
    984, 985, 986, 987, 988, 989, 990, 991, 992, 993, 994, 995, 996, 997, 998, 999,
    1000, 1001, 1002, 1003, 1005, 1006, 1007, 1008, 1009, 1010, 1011, 1012, 1013, 1014, 1015, 1016,
    1017, 1018, 1019, 1021, 1022, 1023, 1024, 1025, 1026, 1027, 1028, 1029, 1030, 1031, 1032, 1033,
    1034, 1036, 1037, 1038, 1039, 1040, 1041, 1042, 1043, 1044, 1045, 1046, 1047, 1049, 1050, 1051,
    1052, 1053, 1054, 1055, 1056, 1057, 1058, 1059, 1061, 1062, 1063, 1064, 1065, 1066, 1067, 1068,
    1069, 1070, 1071, 1073, 1074, 1075, 1076, 1077, 1078, 1079, 1080, 1081, 1083, 1084, 1085, 1086,
    1087, 1088, 1089, 1090, 1091, 1093, 1094, 1095, 1096, 1097, 1098, 1099, 1100, 1101, 1103, 1104,
    1105, 1106, 1107, 1108, 1109, 1110, 1112, 1113, 1114, 1115, 1116, 1117, 1118, 1119, 1121, 1122,
    1123, 1124, 1125, 1126, 1127, 1129, 1130, 1131, 1132, 1133, 1134, 1135, 1137, 1138, 1139, 1140,
    1141, 1142, 1143, 1145, 1146, 1147, 1148, 1149, 1150, 1151, 1153, 1154, 1155, 1156, 1157, 1158,
    1160, 1161, 1162, 1163, 1164, 1165, 1166, 1168, 1169, 1170, 1171, 1172, 1173, 1175, 1176, 1177,
    1178, 1179, 1180, 1182, 1183, 1184, 1185, 1186, 1187, 1189, 1190, 1191, 1192, 1193, 1195, 1196,
    1197, 1198, 1199, 1200, 1202, 1203, 1204, 1205, 1206, 1208, 1209, 1210, 1211, 1212, 1213, 1215,
    1216, 1217, 1218, 1219, 1221, 1222, 1223, 1224, 1225, 1227, 1228, 1229, 1230, 1231, 1233, 1234,
    1235, 1236, 1237, 1239, 1240, 1241, 1242, 1243, 1245, 1246, 1247, 1248, 1249, 1251, 1252, 1253,
    1254, 1256, 1257, 1258, 1259, 1260, 1262, 1263, 1264, 1265, 1266, 1268, 1269, 1270, 1271, 1273,
    1274, 1275, 1276, 1277, 1279, 1280, 1281, 1282, 1284, 1285, 1286, 1287, 1289, 1290, 1291, 1292,
    1293, 1295, 1296, 1297, 1298, 1300, 1301, 1302, 1303, 1305, 1306, 1307, 1308, 1310, 1311, 1312,
    1313, 1315, 1316, 1317, 1318, 1320, 1321, 1322, 1323, 1325, 1326, 1327, 1328, 1330, 1331, 1332,
    1333, 1335, 1336, 1337, 1338, 1340, 1341, 1342, 1344, 1345, 1346, 1347, 1349, 1350, 1351, 1352,
    1354, 1355, 1356, 1357, 1359, 1360, 1361, 1363, 1364, 1365, 1366, 1368, 1369, 1370, 1372, 1373,
    1374, 1375, 1377, 1378, 1379, 1381, 1382, 1383, 1384, 1386, 1387, 1388, 1390, 1391, 1392, 1393,
    1395, 1396, 1397, 1399, 1400, 1401, 1403, 1404, 1405, 1406, 1408, 1409, 1410, 1412, 1413, 1414,
    1416, 1417, 1418, 1419, 1421, 1422, 1423, 1425, 1426, 1427, 1429, 1430, 1431, 1433, 1434, 1435,
    1437, 1438, 1439, 1441, 1442, 1443, 1444, 1446, 1447, 1448, 1450, 1451, 1452, 1454, 1455, 1456,
    1458, 1459, 1460, 1462, 1463, 1464, 1466, 1467, 1468, 1470, 1471, 1472, 1474, 1475, 1476, 1478,
    1479, 1480, 1482, 1483, 1485, 1486, 1487, 1489, 1490, 1491, 1493, 1494, 1495, 1497, 1498, 1499,
    1501, 1502, 1503, 1505, 1506, 1508, 1509, 1510, 1512, 1513, 1514, 1516, 1517, 1518, 1520, 1521,
    1523, 1524, 1525, 1527, 1528, 1529, 1531, 1532, 1533, 1535, 1536, 1538, 1539, 1540, 1542, 1543,
    1544, 1546, 1547, 1549, 1550, 1551, 1553, 1554, 1556, 1557, 1558, 1560, 1561, 1562, 1564, 1565,
    1567, 1568, 1569, 1571, 1572, 1574, 1575, 1576, 1578, 1579, 1581, 1582, 1583, 1585, 1586, 1588,
    1589, 1590, 1592, 1593, 1595, 1596, 1597, 1599, 1600, 1602, 1603, 1604, 1606, 1607, 1609, 1610,
    1612, 1613, 1614, 1616, 1617, 1619, 1620, 1621, 1623, 1624, 1626, 1627, 1629, 1630, 1631, 1633,
    1634, 1636, 1637, 1639, 1640, 1641, 1643, 1644, 1646, 1647, 1649, 1650, 1651, 1653, 1654, 1656,
    1657, 1659, 1660, 1662, 1663, 1664, 1666, 1667, 1669, 1670, 1672, 1673, 1675, 1676, 1677, 1679,
    1680, 1682, 1683, 1685, 1686, 1688, 1689, 1691, 1692, 1693, 1695, 1696, 1698, 1699, 1701, 1702,
    1704, 1705, 1707, 1708, 1710, 1711, 1713, 1714, 1715, 1717, 1718, 1720, 1721, 1723, 1724, 1726,
    1727, 1729, 1730, 1732, 1733, 1735, 1736, 1738, 1739, 1741, 1742, 1744, 1745, 1747, 1748, 1750,
    1751, 1752, 1754, 1755, 1757, 1758, 1760, 1761, 1763, 1764, 1766, 1767, 1769, 1770, 1772, 1773,
    1775, 1776, 1778, 1779, 1781, 1782, 1784, 1785, 1787, 1788, 1790, 1791, 1793, 1795, 1796, 1798,
    1799, 1801, 1802, 1804, 1805, 1807, 1808, 1810, 1811, 1813, 1814, 1816, 1817, 1819, 1820, 1822,
    1823, 1825, 1826, 1828, 1830, 1831, 1833, 1834, 1836, 1837, 1839, 1840, 1842, 1843, 1845, 1846,
    1848, 1849, 1851, 1853, 1854, 1856, 1857, 1859, 1860, 1862, 1863, 1865, 1866, 1868, 1870, 1871,
    1873, 1874, 1876, 1877, 1879, 1880, 1882, 1884, 1885, 1887, 1888, 1890, 1891, 1893, 1894, 1896,
    1898, 1899, 1901, 1902, 1904, 1905, 1907, 1909, 1910, 1912, 1913, 1915, 1916, 1918, 1920, 1921,
    1923, 1924, 1926, 1928, 1929, 1931, 1932, 1934, 1935, 1937, 1939, 1940, 1942, 1943, 1945, 1947,
    1948, 1950, 1951, 1953, 1954, 1956, 1958, 1959, 1961, 1962, 1964, 1966, 1967, 1969, 1970, 1972,
    1974, 1975, 1977, 1978, 1980, 1982, 1983, 1985, 1987, 1988, 1990, 1991, 1993, 1995, 1996, 1998,
    1999, 2001, 2003, 2004, 2006, 2008, 2009, 2011, 2012, 2014, 2016, 2017, 2019, 2021, 2022, 2024,
    2025, 2027, 2029, 2030, 2032, 2034, 2035, 2037, 2038, 2040, 2042, 2043, 2045, 2047, 2048, 2050,
    2052, 2053, 2055, 2057, 2058, 2060, 2061, 2063, 2065, 2066, 2068, 2070, 2071, 2073, 2075, 2076,
    2078, 2080, 2081, 2083, 2085, 2086, 2088, 2090, 2091, 2093, 2095, 2096, 2098, 2100, 2101, 2103,
    2105, 2106, 2108, 2110, 2111, 2113, 2115, 2116, 2118, 2120, 2121, 2123, 2125, 2126, 2128, 2130,
    2131, 2133, 2135, 2136, 2138, 2140, 2141, 2143, 2145, 2147, 2148, 2150, 2152, 2153, 2155, 2157,
    2158, 2160, 2162, 2163, 2165, 2167, 2169, 2170, 2172, 2174, 2175, 2177, 2179, 2180, 2182, 2184,
    2186, 2187, 2189, 2191, 2192, 2194, 2196, 2198, 2199, 2201, 2203, 2204, 2206, 2208, 2210, 2211,
    2213, 2215, 2216, 2218, 2220, 2222, 2223, 2225, 2227, 2229, 2230, 2232, 2234, 2235, 2237, 2239,
    2241, 2242, 2244, 2246, 2248, 2249, 2251, 2253, 2255, 2256, 2258, 2260, 2261, 2263, 2265, 2267,
    2268, 2270, 2272, 2274, 2275, 2277, 2279, 2281, 2282, 2284, 2286, 2288, 2289, 2291, 2293, 2295,
    2297, 2298, 2300, 2302, 2304, 2305, 2307, 2309, 2311, 2312, 2314, 2316, 2318, 2319, 2321, 2323,
    2325, 2327, 2328, 2330, 2332, 2334, 2335, 2337, 2339, 2341, 2343, 2344, 2346, 2348, 2350, 2352,
    2353, 2355, 2357, 2359, 2360, 2362, 2364, 2366, 2368, 2369, 2371, 2373, 2375, 2377, 2378, 2380,
    2382, 2384, 2386, 2387, 2389, 2391, 2393, 2395, 2396, 2398, 2400, 2402, 2404, 2405, 2407, 2409,
    2411, 2413, 2415, 2416, 2418, 2420, 2422, 2424, 2425, 2427, 2429, 2431, 2433, 2435, 2436, 2438,
    2440, 2442, 2444, 2446, 2447, 2449, 2451, 2453, 2455, 2457, 2458, 2460, 2462, 2464, 2466, 2468,
    2469, 2471, 2473, 2475, 2477, 2479, 2480, 2482, 2484, 2486, 2488, 2490, 2492, 2493, 2495, 2497,
    2499, 2501, 2503, 2505, 2506, 2508, 2510, 2512, 2514, 2516, 2518, 2519, 2521, 2523, 2525, 2527,
    2529, 2531, 2532, 2534, 2536, 2538, 2540, 2542, 2544, 2546, 2547, 2549, 2551, 2553, 2555, 2557,
    2559, 2561, 2562, 2564, 2566, 2568, 2570, 2572, 2574, 2576, 2578, 2579, 2581, 2583, 2585, 2587,
    2589, 2591, 2593, 2595, 2597, 2598, 2600, 2602, 2604, 2606, 2608, 2610, 2612, 2614, 2616, 2617,
    2619, 2621, 2623, 2625, 2627, 2629, 2631, 2633, 2635, 2637, 2638, 2640, 2642, 2644, 2646, 2648,
    2650, 2652, 2654, 2656, 2658, 2660, 2662, 2663, 2665, 2667, 2669, 2671, 2673, 2675, 2677, 2679,
    2681, 2683, 2685, 2687, 2689, 2691, 2692, 2694, 2696, 2698, 2700, 2702, 2704, 2706, 2708, 2710,
    2712, 2714, 2716, 2718, 2720, 2722, 2724, 2726, 2728, 2730, 2731, 2733, 2735, 2737, 2739, 2741,
    2743, 2745, 2747, 2749, 2751, 2753, 2755, 2757, 2759, 2761, 2763, 2765, 2767, 2769, 2771, 2773,
    2775, 2777, 2779, 2781, 2783, 2785, 2787, 2789, 2791, 2793, 2795, 2797, 2799, 2801, 2803, 2805,
    2807, 2809, 2811, 2813, 2815, 2817, 2819, 2821, 2823, 2825, 2827, 2829, 2831, 2833, 2835, 2837,
    2839, 2841, 2843, 2845, 2847, 2849, 2851, 2853, 2855, 2857, 2859, 2861, 2863, 2865, 2867, 2869,
    2871, 2873, 2875, 2877, 2879, 2881, 2883, 2885, 2887, 2889, 2891, 2893, 2895, 2897, 2899, 2901,
    2903, 2905, 2907, 2909, 2911, 2913, 2915, 2917, 2919, 2921, 2924, 2926, 2928, 2930, 2932, 2934,
    2936, 2938, 2940, 2942, 2944, 2946, 2948, 2950, 2952, 2954, 2956, 2958, 2960, 2962, 2965, 2967,
    2969, 2971, 2973, 2975, 2977, 2979, 2981, 2983, 2985, 2987, 2989, 2991, 2993, 2996, 2998, 3000,
    3002, 3004, 3006, 3008, 3010, 3012, 3014, 3016, 3018, 3020, 3023, 3025, 3027, 3029, 3031, 3033,
    3035, 3037, 3039, 3041, 3043, 3046, 3048, 3050, 3052, 3054, 3056, 3058, 3060, 3062, 3064, 3067,
    3069, 3071, 3073, 3075, 3077, 3079, 3081, 3083, 3086, 3088, 3090, 3092, 3094, 3096, 3098, 3100,
    3102, 3105, 3107, 3109, 3111, 3113, 3115, 3117, 3119, 3122, 3124, 3126, 3128, 3130, 3132, 3134,
    3136, 3139, 3141, 3143, 3145, 3147, 3149, 3151, 3154, 3156, 3158, 3160, 3162, 3164, 3166, 3169,
    3171, 3173, 3175, 3177, 3179, 3181, 3184, 3186, 3188, 3190, 3192, 3194, 3196, 3199, 3201, 3203,
    3205, 3207, 3209, 3212, 3214, 3216, 3218, 3220, 3222, 3225, 3227, 3229, 3231, 3233, 3235, 3238,
    3240, 3242, 3244, 3246, 3249, 3251, 3253, 3255, 3257, 3259, 3262, 3264, 3266, 3268, 3270, 3273,
    3275, 3277, 3279, 3281, 3284, 3286, 3288, 3290, 3292, 3294, 3297, 3299, 3301, 3303, 3305, 3308,
    3310, 3312, 3314, 3317, 3319, 3321, 3323, 3325, 3328, 3330, 3332, 3334, 3336, 3339, 3341, 3343,
    3345, 3348, 3350, 3352, 3354, 3356, 3359, 3361, 3363, 3365, 3368, 3370, 3372, 3374, 3376, 3379,
    3381, 3383, 3385, 3388, 3390, 3392, 3394, 3397, 3399, 3401, 3403, 3406, 3408, 3410, 3412, 3415,
    3417, 3419, 3421, 3424, 3426, 3428, 3430, 3433, 3435, 3437, 3439, 3442, 3444, 3446, 3448, 3451,
    3453, 3455, 3457, 3460, 3462, 3464, 3466, 3469, 3471, 3473, 3476, 3478, 3480, 3482, 3485, 3487,
    3489, 3491, 3494, 3496, 3498, 3501, 3503, 3505, 3507, 3510, 3512, 3514, 3517, 3519, 3521, 3523,
    3526, 3528, 3530, 3533, 3535, 3537, 3539, 3542, 3544, 3546, 3549, 3551, 3553, 3556, 3558, 3560,
    3562, 3565, 3567, 3569, 3572, 3574, 3576, 3579, 3581, 3583, 3586, 3588, 3590, 3593, 3595, 3597,
    3600, 3602, 3604, 3606, 3609, 3611, 3613, 3616, 3618, 3620, 3623, 3625, 3627, 3630, 3632, 3634,
    3637, 3639, 3641, 3644, 3646, 3648, 3651, 3653, 3655, 3658, 3660, 3663, 3665, 3667, 3670, 3672,
    3674, 3677, 3679, 3681, 3684, 3686, 3688, 3691, 3693, 3695, 3698, 3700, 3703, 3705, 3707, 3710,
    3712, 3714, 3717, 3719, 3721, 3724, 3726, 3729, 3731, 3733, 3736, 3738, 3740, 3743, 3745, 3748};

uint16_t gamma16(uint16_t x)
{
  uint16_t ret;
  if (x >= 4096)
  {
    Serial.print("Over 12bit value! ");
    Serial.println(x);
    x = 4095;
  }
  ret = pgm_read_word(_gamma12Table + x);
  if (ret > PWM_MAX)
  {
    ret = PWM_MAX;
  }
  return ret;
}