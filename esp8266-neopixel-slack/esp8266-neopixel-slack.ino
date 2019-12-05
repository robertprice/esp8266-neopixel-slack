#include <Arduino.h>

#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>
#include <ESP8266HTTPClient.h>

#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>

#ifdef __AVR__
  #include <avr/power.h>
#endif

#define SLACK_SSL_FINGERPRINT "C1 0D 53 49 D2 3E E5 2B A2 61 D5 9E 6F 99 0D 3D FD 8B B2 B3" // If Slack changes their SSL fingerprint, you would need to update this
#define SLACK_BOT_TOKEN "<INSERT TOKEN>" // Get token by creating new bot integration at https://my.slack.com/services/new/bot 
#define WIFI_SSID       "<INSERT SSID>"
#define WIFI_PASSWORD   "<INSERT PASSWORD>"
#define WORD_SEPERATORS "., \"'()[]<>;:-+&?!\n\t"

// The Neopixel strip is connected to pin D2 on the NodeMCU and has 44 pixels
#define PIN D2   
#define PIXELS_TOTAL 44 

// Parameter 1 = number of pixels in strip
// Parameter 2 = Arduino pin number (most are valid)
// Parameter 3 = pixel type flags, add together as needed:
//   NEO_KHZ800  800 KHz bitstream (most NeoPixel products w/WS2812 LEDs)
//   NEO_KHZ400  400 KHz (classic 'v1' (not v2) FLORA pixels, WS2811 drivers)
//   NEO_GRB     Pixels are wired for GRB bitstream (most NeoPixel products)
//   NEO_RGB     Pixels are wired for RGB bitstream (v1 FLORA pixels, not v2)
//   NEO_RGBW    Pixels are wired for RGBW bitstream (NeoPixel RGBW products)
Adafruit_NeoPixel strip = Adafruit_NeoPixel(PIXELS_TOTAL, PIN, NEO_GRB + NEO_KHZ800);

// IMPORTANT: To reduce NeoPixel burnout risk, add 1000 uF capacitor across
// pixel power leads, add 300 - 500 Ohm resistor on first pixel's data input
// and minimize distance between Arduino and first pixel.  Avoid connecting
// on a live circuit...if you must, connect GND first.

ESP8266WiFiMulti WiFiMulti;
WebSocketsClient webSocket;

long nextCmdId = 1;
bool connected = false;
unsigned long lastPing = 0;

void setup() {
  // This is for Trinket 5V 16MHz, you can remove these three lines if you are not using a Trinket
  #if defined (__AVR_ATtiny85__)
    if (F_CPU == 16000000) clock_prescale_set(clock_div_1);
  #endif
  // End of trinket special code

  Serial.begin(115200);
  Serial.println("Starting...");
  Serial.println(strip.numPixels());


  strip.begin();
  strip.show(); // Initialize all pixels to 'off'

  WiFiMulti.addAP(WIFI_SSID, WIFI_PASSWORD);
  while (WiFiMulti.run() != WL_CONNECTED) {
    delay(100);
  }

  configTime(3 * 3600, 0, "pool.ntp.org", "time.nist.gov");
}



// Fill the dots one after the other with a color
void colorWipe(uint32_t c, uint8_t wait) {
  for(uint16_t i=0; i<strip.numPixels(); i++) {
    strip.setPixelColor(i, c);
    strip.show();
    delay(wait);
  }
}

void rainbow(uint8_t wait) {
  uint16_t i, j;

  for(j=0; j<256; j++) {
    for(i=0; i<strip.numPixels(); i++) {
      strip.setPixelColor(i, Wheel((i+j) & 255));
    }
    strip.show();
    delay(wait);
  }
}

// Slightly different, this makes the rainbow equally distributed throughout
void rainbowCycle(uint8_t wait) {
  uint16_t i, j;

  for(j=0; j<256*5; j++) { // 5 cycles of all colors on wheel
    for(i=0; i< strip.numPixels(); i++) {
      strip.setPixelColor(i, Wheel(((i * 256 / strip.numPixels()) + j) & 255));
    }
    strip.show();
    delay(wait);
  }
}

//Theatre-style crawling lights.
void theatreChase(uint32_t c, uint8_t wait) {
  for (int j=0; j<10; j++) {  //do 10 cycles of chasing
    for (int q=0; q < 3; q++) {
      for (uint16_t i=0; i < strip.numPixels(); i=i+3) {
        strip.setPixelColor(i+q, c);    //turn every third pixel on
      }
      strip.show();

      delay(wait);

      for (uint16_t i=0; i < strip.numPixels(); i=i+3) {
        strip.setPixelColor(i+q, 0);        //turn every third pixel off
      }
    }
  }
}

//Theatre-style crawling lights with rainbow effect
void theatreChaseRainbow(uint8_t wait) {
  for (int j=0; j < 256; j++) {     // cycle all 256 colors in the wheel
    for (int q=0; q < 3; q++) {
      for (uint16_t i=0; i < strip.numPixels(); i=i+3) {
        strip.setPixelColor(i+q, Wheel( (i+j) % 255));    //turn every third pixel on
      }
      strip.show();

      delay(wait);

      for (uint16_t i=0; i < strip.numPixels(); i=i+3) {
        strip.setPixelColor(i+q, 0);        //turn every third pixel off
      }
    }
  }
}

// Input a value 0 to 255 to get a color value.
// The colours are a transition r - g - b - back to r.
uint32_t Wheel(byte WheelPos) {
  WheelPos = 255 - WheelPos;
  if(WheelPos < 85) {
    return strip.Color(255 - WheelPos * 3, 0, WheelPos * 3);
  }
  if(WheelPos < 170) {
    WheelPos -= 85;
    return strip.Color(0, WheelPos * 3, 255 - WheelPos * 3);
  }
  WheelPos -= 170;
  return strip.Color(WheelPos * 3, 255 - WheelPos * 3, 0);
}



/**
  Looks for color names in the incoming slack messages and
  animates the ring accordingly. You can include several
  colors in a single message, e.g. `red blue zebra black yellow rainbow`
*/
void processSlackMessage(char *payload) {
  char *nextWord = NULL;
  bool chase = false;
  bool rainbow = false;
  for (nextWord = strtok(payload, WORD_SEPERATORS); nextWord; nextWord = strtok(NULL, WORD_SEPERATORS)) {
    Serial.println(nextWord);
    if (strcasecmp(nextWord, "chase") == 0) {
      chase = true;
    }
    if (strcasecmp(nextWord, "rainbow") == 0) {
      if (chase) {
        theatreChaseRainbow(50);
      } else {
        rainbowCycle(10);
      }
    }
    if (strcasecmp(nextWord, "red") == 0) {
      if (chase) {
        theatreChase(strip.Color(0, 255, 0), 50);
      } else {
        colorWipe(strip.Color(0, 255, 0), 50);
      }
    }
    if (strcasecmp(nextWord, "green") == 0) {
      if (chase) {
        theatreChase(strip.Color(255, 0, 0), 50);
      } else {
        colorWipe(strip.Color(255, 0, 0), 50);
      }
    }
    if (strcasecmp(nextWord, "blue") == 0) {
      if (chase) {
        theatreChase(strip.Color(0, 0, 255), 50);
      } else {
        colorWipe(strip.Color(0, 0, 255), 50);
      }
    }
    if (strcasecmp(nextWord, "yellow") == 0) {
      if (chase) {
        theatreChase(strip.Color(160, 255, 0), 50);
      } else {
        colorWipe(strip.Color(160, 255, 0), 50);
      }
    }
    if (strcasecmp(nextWord, "white") == 0) {
      if (chase) {
        theatreChase(strip.Color(255, 255, 255), 50);
      } else {
        colorWipe(strip.Color(255, 255, 255), 50);
      }
    }
    if (strcasecmp(nextWord, "purple") == 0) {
      if (chase) {
        theatreChase(strip.Color(0, 128, 128), 50);
      } else {
        colorWipe(strip.Color(0, 128, 128), 50);
      }
    }
    if (strcasecmp(nextWord, "pink") == 0) {
      if (chase) {
        theatreChase(strip.Color(0, 255, 96), 50);
      } else {
        colorWipe(strip.Color(0, 255, 96), 50);
      }
    }
    if (strcasecmp(nextWord, "orange") == 0) {
      if (chase) {
        theatreChase(strip.Color(64, 255, 0), 50);
      } else {
        colorWipe(strip.Color(64, 255, 0), 50);
      }
    }
    if (strcasecmp(nextWord, "black") == 0) {
      if (chase) {
        theatreChase(strip.Color(0, 0, 0), 50);
      } else {
        colorWipe(strip.Color(0, 0, 0), 50);
      }
    }
    if (nextWord[0] == '#') {
      int color = strtol(&nextWord[1], NULL, 16);
      Serial.print("RGB Color: ");
      Serial.println(color, HEX);
      if (color) {
        // swap red and green bytes as the Neopixel strip is GRB not RGB
        int redbyte = color & 0x00FF0000;
        int greenbyte = color & 0x0000FF00;
        int newColor = (color & 0xFF0000FF) | (redbyte >> 8) | (greenbyte << 8);
        Serial.print("GRB Color: ");
        Serial.println(newColor, HEX);
        if (chase) {
          theatreChase(newColor, 50);
        } else {
          colorWipe(newColor, 50);
        }
      }
    }
  }
}


/**
  Sends a ping message to Slack. Call this function immediately after establishing
  the WebSocket connection, and then every 5 seconds to keep the connection alive.
*/
void sendPing() {
  StaticJsonDocument<200> root;
  root["type"] = "ping";
  root["id"] = nextCmdId++;
  String json;
  serializeJson(root, json);
  webSocket.sendTXT(json);
}

/**
  Called on each web socket event. Handles disconnection, and also
  incoming messages from slack.
*/
void webSocketEvent(WStype_t type, uint8_t *payload, size_t len) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.printf("[WebSocket] Disconnected :-( \n");
      connected = false;
      break;

    case WStype_CONNECTED:
      Serial.printf("[WebSocket] Connected to: %s\n", payload);
      sendPing();
      break;

    case WStype_TEXT:
      Serial.printf("[WebSocket] Message: %s\n", payload);
      processSlackMessage((char*)payload);
      break;
  }
}

/**
  Establishes a bot connection to Slack:
  1. Performs a REST call to get the WebSocket URL
  2. Conencts the WebSocket
  Returns true if the connection was established successfully.
*/
bool connectToSlack() {
  // Step 1: Find WebSocket address via RTM API (https://api.slack.com/methods/rtm.connect)
  HTTPClient http;
  http.begin("https://slack.com/api/rtm.connect?token=" SLACK_BOT_TOKEN, SLACK_SSL_FINGERPRINT);
  int httpCode = http.GET();

  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("HTTP GET failed with code %d\n", httpCode);
    return false;
  }

  WiFiClient *client = http.getStreamPtr();
  client->find("wss:\\/\\/");
  String host = client->readStringUntil('\\');
  String path = client->readStringUntil('"');
  path.replace("\\/", "/");

  // Step 2: Open WebSocket connection and register event handler
  Serial.println("WebSocket Host=" + host + " Path=" + path);
  webSocket.beginSSL(host, 443, path, "", "");
  webSocket.onEvent(webSocketEvent);
  return true;
}


void loop() {
  webSocket.loop();

  if (connected) {
    // Send ping every 5 seconds, to keep the connection alive
    if (millis() - lastPing > 5000) {
      sendPing();
      lastPing = millis();
    }
  } else {
    // Try to connect / reconnect to slack
    connected = connectToSlack();
    if (!connected) {
      delay(500);
    }
  }
}
