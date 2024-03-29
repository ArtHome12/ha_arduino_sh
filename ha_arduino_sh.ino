/* ===============================================================================
Мультиплексор с поддержкой 8 датчиков температуры и влажности.
12 March 2019.
----------------------------------------------------------------------------
Licensed under the terms of the GPL version 3.
http://www.gnu.org/licenses/gpl-3.0.html
Copyright (c) 2019 by Artem Khomenko _mag12@yahoo.com.
=============================================================================== */

#include <Wire.h>
#include <EEPROM.h>
#include <HTU21D.h>
#include <INA226_asukiaaa.h>

const uint16_t ina226calib = INA226_asukiaaa::calcCalibByResisterMilliOhm(100); // Max 5120 milli ohm
#define INA226_ASUKIAAA_MAXAVERAGE_CONFIG 0x4F27                                // Default 0x4127 - for once average. Digit F for 1024 averages
INA226_asukiaaa voltCurrMeter(INA226_ASUKIAAA_ADDR_A0_VDD_A1_GND, ina226calib, INA226_ASUKIAAA_MAXAVERAGE_CONFIG);

HTU21D myHTU21D(HTU21D_RES_RH12_TEMP14);// Интерфейс к датчикам температуры и влажности.

#define TCAADDR 0x70
const uint8_t HTUCount = 8;             // Восемь датчиков влажности и температуры.
uint8_t activeHTU = 0;                  // Индекс активного в текущий момент датчика.


unsigned long previousMillis = 0;       // Момент последнего обновления, мс.
const long minDelay = 100;              // Минимально необходимый интервал для работы внутри loop(), мс.
const unsigned long hour = 1000UL * 60UL * 60UL;  // Длина одного часа, мс.
const unsigned long maxWorkTime = hour * 24UL * 3UL;  // Максимально допустимое время непрерывной работы, мс.
unsigned long maxWorkTimeCurLimit = maxWorkTime;  // Текущая цель, после которой нужна очередная перезагрузка RPi.
int delaysCount = 0;                    // Количество прошедших минимальных интервалов.
const int delaysCountLimit = 10;        // Максимальное количество интервалов, при котором срабатывает логика - 100*10=1000мс или один раз в секунду.
int blinkCountdown = 0;                 // Количество оставшихся миганий светодиода.
bool lightIsOn = false;                 // Истина, если в текущем цикле светодиод зажжён.
bool RPiTurnedOff = false;              // Истина, когда с RPi снято питание.


const int fanPin = 3;                   // Пин с вентилятором.
const int buttonPin = 6;                // Кнопка включения/выключения. При отжатии кнопки RPi выключается, а при нажатии включается, но только если питание высокое
const int RPiSendShutdownPin = 8;       // Управление выключением RPi. 
const int RPiPowerOffPin = 9;           // Когда на пине низкий уровень, RPi работает. Когда высокий, она обесточена.

const int resultsCount = HTUCount + 2;  // 1 для температуры, 2 для влажности плюс пара напряжение и мощность плюс время работы.
float results[2][resultsCount];         // Массив для хранения состояния
#ifndef __INTELLISENSE__                // Обходим глюк интеллисенса, не понимающего include внутри ifdef.
const size_t resultsLen = sizeof(float) * 2 * resultsCount;
#else
const unsigned int resultsLen = sizeof(float) * 2 * resultsCount;
#define INT16_MAX 0x7fffL
#endif

const int mVoltageLoBound = 11400;      // При падении напряжения в милливольтах ниже этой границы RPi надо отключить.
// const int mVoltageLoBound = 11100;      // При падении напряжения в милливольтах ниже этой границы RPi надо отключить.
const int mVoltageHiBound = 11800;      // При росте напряжения в милливольтах выше этой границы RPi надо включить, если она была выключена.

// В состоянии покоя 0,23А при 11,99В = 2,76Вт. На датчике 2,85Вт. После выключения 0,18А или 2,23Вт с поправкой по датчику.
// Потребление схемы без RPi4 составляет 0,04А.
const int mWattLoBound = 2500;          // Если энергопотребление упало ниже этой границы, считаем что RPi завершила работу и перешла в idle.

int cyclesPowerLow = 0;                 // Счётчик цикла для проверки падения энергопотребления.
const int cyclesPowerLowLimit = 5;      // Предел для счётчика цикла для проверки падения энергопотребления.
int cyclesVoltageLow = 0;               // Счётчик циклов для продолжительных проверок.
const int cyclesVoltageLowLimit = 30;   // Предел для счётчика циклов.
int cyclesVoltageHigh = 0;              // Счётчик циклов для продолжительных проверок.
const int cyclesVoltageHighLimit = 30;  // Предел для счётчика циклов.
int powerOffTimer = 0;                  // Счётчик циклов отключения питания RPi.
const int powerOffTimerLimit = 5*60;    // Предел для счётчика циклов отключения питания RPi.

const int eepromAddrShutdown = 0;       // Адрес для хранения в EEPROM признака завершения работы.
const byte eepromSendShutdownMode = 1;  // Режим до сброса - отправлен сигнал на выключение.
const byte eepromPowerOffMode = 2;      // Режим до сброса - RPi выключена.


void setup()
{
   // По рекомендации неиспользуемые пины лучше подтянуть к земле, иначе будут потери электроэнергии при спонтанных переключениях от наводок.
   // У нас только у кнопки требуется отдельное состояние.
   for (int i = 0; i < NUM_DIGITAL_PINS; i++) {
      if (i == buttonPin)
         pinMode(i, INPUT_PULLUP);
      else
         pinMode(i, OUTPUT);
   }

   // Индикация старта
   for (int i = 0; i < 2; i++) {
      digitalWrite(LED_BUILTIN, HIGH);
      delay(500);
      digitalWrite(LED_BUILTIN, LOW);
      delay(500);
   }
   delay(500);

   // Иногда arduino сбрасываетсяпри завершении работы RPi, иногда нет.
   // Восстановим из EEPROM информацию о состоянии до сброса.
   // RPi могла быть в режиме завершения работы и уже выключенной.
   switch (EEPROM.read(eepromAddrShutdown)) {
      case eepromSendShutdownMode:
         powerOffTimer = 1;
         blinkCountdown = 3;
         break;
      case eepromPowerOffMode: 
         powerOffTimer = 1;
         RPiTurnedOff = true;
         powerOff();
         cyclesVoltageHigh = cyclesVoltageHighLimit - 1;
         blinkCountdown = 6;
         break;
   }

   Wire.begin();

   voltCurrMeter.setWire(&Wire);
   voltCurrMeter.begin();

   // Заполним состояния недействительными значениями во-избежание их появления у пользователя.
   memset(results, 255, resultsLen);

   // Посылаем команду на инициализацию устройств на всех портах.
   for (activeHTU = 0; activeHTU < HTUCount; activeHTU++) {
      tcaselect(activeHTU);
      myHTU21D.begin();
   }
   activeHTU = 0;

   Serial.begin(115200);

   // Чтобы дать время на опрос датчиков.
   previousMillis = millis();
}


void loop() 
{
   // Текущее время.
   unsigned long currentMillis = millis();

   // Условия вычисляем отдельно, для защиты от перехода через 0.
   unsigned long condition = currentMillis - previousMillis;

   // Если слишком мало времени прошло с предыдущего раза.
   if (condition < minDelay)
      return;

   // Сохраним время срабатывания.
   previousMillis = currentMillis;

   // Обработка логики мигания светодиодом. Если он горел, его надо погасить в этот раз.
   //
   if (lightIsOn) {
      lightIsOn = false;
      digitalWrite(LED_BUILTIN, LOW);
   } else {
      // Если ещё осталось количество миганий
      if (blinkCountdown > 0) {
         // Включаем светодиод и уменьшаем счётчик миганий.
         lightIsOn = true;
         digitalWrite(LED_BUILTIN, HIGH);
         blinkCountdown--;
      }
   }

   // Основная логика.
   //
	 if (++delaysCount >= delaysCountLimit) {
      // Начинаем заново отсчитывать количество маленьких циклов до захода сюда.
      delaysCount = 0;

      // Считываем данные с мультиплексора. Выбираем порт
      tcaselect(activeHTU);

      // Считываем температуру (+-0.3C) и влажность (+-2%).
      float temp = myHTU21D.readTemperature();
      results[0][activeHTU] = temp;
      results[1][activeHTU] = myHTU21D.readCompensatedHumidity(temp);

      // Меняем порт на мультиплексоре.
      if (++activeHTU >= HTUCount)
         activeHTU = 0;

      // Значение напряжения mV и мощности mW
      int16_t mv, mw;
      if (voltCurrMeter.readMV(&mv) == 0) 
         results[0][HTUCount] = mv / 1000.0;
      else {
        results[0][HTUCount] = 255;
        mv = INT16_MAX;
      }

      if (voltCurrMeter.readMW(&mw) == 0)
         results[1][HTUCount] = mw / 1000.0;
      else {
         results[1][HTUCount] = 255;
         mw = INT16_MAX;
      }

      // Проволжительность работы и лимит до перезагрузки в часах
      results[0][HTUCount + 1] = previousMillis / hour;
      results[1][HTUCount + 1] = maxWorkTime / hour;

      // Управляем питанием RPi. 
      powerControl(mv, mw);

      // Если не в режиме выключения, то есть в обычном режиме, мигнём один раз.
      if (powerOffTimer == 0 && blinkCountdown == 0)
         blinkCountdown++;
   }
}


// Входящая информация от RPi.
void serialEvent() {
   while (Serial.available()) {
      // get the new byte:
      char inChar = (char)Serial.read();

      // Если команда верная, отправляем значения.
      switch(inChar) {
         case 'D': Serial.write((uint8_t*)results, resultsLen); break; // Data
         case 'C': setHeater(HTU21D_ON); break;                        // Check heater
         case 'E': setHeater(HTU21D_OFF); break;                       // End check heater
         case 'S': digitalWrite(fanPin, HIGH); break;                  // Start fan
         case 'F': digitalWrite(fanPin, LOW); break;                   // Stop fan
      }
   }
}


// Управление мультиплексором TCA9548A - выбор активного устройства.
void tcaselect(uint8_t i) {
   Wire.beginTransmission(TCAADDR);
   Wire.write(1 << i);
   Wire.endTransmission();  
}

// Включение подогрева на всех присоединённых датчиках HTU21D.
void setHeater(HTU21D_HEATER_SWITCH heaterSwitch) {
   for (uint8_t t = 0; t < HTUCount; t++) {
      tcaselect(t);
      myHTU21D.setHeater(heaterSwitch);
   }
}


// Sends a signal to the RPi OS to shutdown.
void sendShutdown() {

   digitalWrite(RPiSendShutdownPin, HIGH);  

   // If RPi already shutdowned. We can get here when the arduino restarts unexpectedly
   if (RPiTurnedOff)
      return;
   
   // Если это включение таймера (первое увеличение), запишем информацию в EEPROM на случай сброса ардуины при завершении работы RPi.
   if (!powerOffTimer)
      EEPROM.write(eepromAddrShutdown, eepromSendShutdownMode);

   // Включаем либо увеличиваем таймер отключения питания.
   powerOffTimer++;

   // Включаем светодиод для индикации, что RPi завершает работу.
   digitalWrite(LED_BUILTIN, HIGH);
}


// Отключаем питание RPi.
void powerOff() {

   // Если ещё не была выключена, запишем информацию в энергонезависимую память.
   if (!RPiTurnedOff) {
      EEPROM.write(eepromAddrShutdown, eepromPowerOffMode);
      RPiTurnedOff = true;
   }

   // Гасим RPi.
   digitalWrite(RPiPowerOffPin, HIGH);

   // Снимаем сигнал завершения работы для экономии электроэнергии (иногда зажигается сведодиод, если RPi обесточена, а этот сигнал есть).
   digitalWrite(RPiSendShutdownPin, LOW);

   // Погасим светодиод.
   digitalWrite(LED_BUILTIN, LOW);

   // Задержка для коммутации.
   delay(1000);
}

// Управляет питанием RPi.
void powerControl(int voltage, int power) {

   // 1. Проверяем, не упало ли энергопотребление RPi.
   if (power < mWattLoBound) {
      // Увеличиваем счётчик и если достаточно отмотали, выключаем RPi.
      if (cyclesPowerLow++ > cyclesPowerLowLimit)
         powerOff();
   } else {
      // Энергопотребление выше минимального, сбрасываем счётчик.
      cyclesPowerLow = 0;
   }

   // 2. Принудительное выключение, если PRi продолжает работать несмотря на требование отключения.
   if (powerOffTimer > powerOffTimerLimit) {
      powerOff();
   }

   // 3. Проверяем не упало ли напряжение источника питания.
   if (voltage < mVoltageLoBound) {
      // Увеличиваем счётчик и если достаточно отмотали, отправляем сигнал на завершение работы.
      if (cyclesVoltageLow++ > cyclesVoltageLowLimit)
         sendShutdown();
   } else
      // Энергопотребление выше минимального, сбрасываем счётчик.
      cyclesVoltageLow = 0;

   // 4. Проверяем, не отжата ли кнопка.
   if (digitalRead(buttonPin) == HIGH) {
      // Посылаем сигнал завершения работы малины, если ещё не сделано.
      sendShutdown();

      // Уменьшаем задержку на достаточность напряжения, когда кнопка будет включена
      cyclesVoltageHigh = cyclesVoltageHighLimit - 1;

      // Выходим, иначе проверка на достаточное напряжение аннулирует выключение.
      return;
   }

   // 5. Проверяем на предельное время работы без перезагрузки.
   if (previousMillis > maxWorkTimeCurLimit) {
      // Посылаем сигнал завершения работы малины, если ещё не сделано.
      sendShutdown();

      // Увеличиваем лимит. На случай, если перезагрузка не произодёт при выключении/включении RPi.
      maxWorkTimeCurLimit += maxWorkTime;

      // Выходим, иначе проверка на достаточное напряжение аннулирует выключение.
      return;
   }

   // 6. Если напряжение достаточное, а RPi выключена, включаем
   if (voltage > mVoltageHiBound && RPiTurnedOff) {

      // Увеличиваем счётчик и если достаточно отмотали, отправляем сигнал на включение.
      if (cyclesVoltageHigh++ > cyclesVoltageHighLimit) {

         // Отменяем таймер принудительного отключения.
         powerOffTimer = 0;

         // Флаг, что питание подано.
         RPiTurnedOff = false;

         // Снимем в энергонезависимой памяти все флаги.
         EEPROM.write(eepromAddrShutdown, 0);

         // Заново запускаем счётчик низкого энергопотребления с форой на холостой цикл и раскачку.
         cyclesPowerLow = -3;

         // Снимаем сигнал о необходимости завершения работы.
         digitalWrite(RPiSendShutdownPin, LOW);

         // Подаём питание на RPi.
         digitalWrite(RPiPowerOffPin, LOW);

         // Задержка для коммутации.
         delay(1000);

         // Мигнём для индикации режима.
         blinkCountdown = 9;
      }
   } else {
      // Напряжение недостаточно высокое, сбросим счётчик.
      cyclesVoltageHigh = 0;
   }
}
