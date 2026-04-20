#include <Arduino.h>
#include "settings.h"

#include "AudioPlayer.h"
#include "HallEffectSensor.h"
#include "Log.h"
#include "MemX.h"
#include "Queues.h"
#include "Rfid.h"
#include "System.h"

#include <esp_task_wdt.h>

#if defined RFID_READER_TYPE_MFRC522_SPI || defined RFID_READER_TYPE_MFRC522_I2C
	#ifdef RFID_READER_TYPE_MFRC522_SPI
		#include <MFRC522.h>
	#endif
	#if defined(RFID_READER_TYPE_MFRC522_I2C) || defined(PORT_EXPANDER_ENABLE)
		#include "Wire.h"
	#endif
	#ifdef RFID_READER_TYPE_MFRC522_I2C
		#include <MFRC522_I2C.h>
	#endif

extern unsigned long Rfid_LastRfidCheckTimestamp;
TaskHandle_t rfidTaskHandle;
static void Rfid_Task(void *parameter);
static bool Rfid_ReadObservedCard(byte *cardId, const RfidPresenceTracker &presenceTracker);
// ESP-IDF expects task stack sizes in bytes, not in FreeRTOS words.
static constexpr uint32_t RfidTaskStackSize = 2048u * sizeof(StackType_t);

	#ifdef RFID_READER_TYPE_MFRC522_I2C
extern TwoWire i2cBusTwo;
static MFRC522_I2C mfrc522(MFRC522_ADDR, MFRC522_RST_PIN, &i2cBusTwo);
	#endif
	#ifdef RFID_READER_TYPE_MFRC522_SPI
static MFRC522 mfrc522(RFID_CS, RST_PIN);
	#endif

void Rfid_Init(void) {
	#ifdef RFID_READER_TYPE_MFRC522_SPI
	SPI.begin(RFID_SCK, RFID_MISO, RFID_MOSI, RFID_CS);
	SPI.setFrequency(1000000);
	#endif

	// Init RC522 Card-Reader
	#if defined(RFID_READER_TYPE_MFRC522_I2C) || defined(RFID_READER_TYPE_MFRC522_SPI)
	mfrc522.PCD_Init();
	delay(10);
	// Get the MFRC522 firmware version, should be 0x91 or 0x92
	byte firmwareVersion = mfrc522.PCD_ReadRegister(MFRC522::VersionReg);
	Log_Printf(LOGLEVEL_DEBUG, "RC522 firmware version=%#lx", firmwareVersion);

	mfrc522.PCD_SetAntennaGain(rfidGain);
	delay(50);
	Log_Println(rfidScannerReady, LOGLEVEL_DEBUG);

	xTaskCreatePinnedToCore(
		Rfid_Task, /* Function to implement the task */
		"rfid", /* Name of the task */
		RfidTaskStackSize, /* Stack size in bytes */
		NULL, /* Task input parameter */
		2 | portPRIVILEGE_BIT, /* Priority of the task */
		&rfidTaskHandle, /* Task handle. */
		ARDUINO_RUNNING_CORE /* Core where the task should run */
	);
	#endif
}

bool Rfid_ReadObservedCard(byte *cardId, const RfidPresenceTracker &presenceTracker) {
	byte bufferATQA[2];
	byte bufferSize = sizeof(bufferATQA);
	const MFRC522::StatusCode wakeupStatus = mfrc522.PICC_WakeupA(bufferATQA, &bufferSize);
	const bool cardPresent = (wakeupStatus == MFRC522::STATUS_OK || wakeupStatus == MFRC522::STATUS_COLLISION);
	if (!cardPresent) {
		mfrc522.PICC_HaltA();
		mfrc522.PCD_StopCrypto1();
		return false;
	}

	bool haveObservedCardId = false;
	if (mfrc522.PICC_ReadCardSerial()) {
		memcpy(cardId, mfrc522.uid.uidByte, cardIdSize);
		haveObservedCardId = true;
	} else if (presenceTracker.hasStableCard) {
		memcpy(cardId, presenceTracker.stableCardId, cardIdSize);
		haveObservedCardId = true;
	}

	mfrc522.PICC_HaltA();
	mfrc522.PCD_StopCrypto1();
	return haveObservedCardId;
}

void Rfid_Task(void *parameter) {
	RfidPresenceTracker presenceTracker;
	RfidPresenceTracker_Init(presenceTracker);
	byte lastAcceptedCardId[cardIdSize] = {0};
	bool hasLastAcceptedCard = false;

	for (;;) {
		uint32_t pollDelayMs = (RFID_SCAN_INTERVAL / 2 >= 20) ? (RFID_SCAN_INTERVAL / 2) : 20;
		vTaskDelay(portTICK_PERIOD_MS * pollDelayMs);

		if ((millis() - Rfid_LastRfidCheckTimestamp) >= RFID_SCAN_INTERVAL) {
			// Log_Printf(LOGLEVEL_DEBUG, "%u", uxTaskGetStackHighWaterMark(NULL));

			Rfid_LastRfidCheckTimestamp = millis();
			byte cardId[cardIdSize];
			const bool observedCard = Rfid_ReadObservedCard(cardId, presenceTracker);

			if (observedCard) {
	#ifdef HALLEFFECT_SENSOR_ENABLE
				cardId[cardIdSize - 1] = cardId[cardIdSize - 1] + gHallEffectSensor.waitForState(HallEffectWaitMS);
	#endif
			}

			const RfidPresenceUpdate presenceUpdate = RfidPresenceTracker_Update(presenceTracker, observedCard, observedCard ? cardId : nullptr, Rfid_LastRfidCheckTimestamp);
			if (presenceUpdate.stableCardRemoved) {
				Log_Printf(LOGLEVEL_DEBUG, "RFID state -> CandidateAbsent confirmed removal");
			}
			if (RfidPresenceTracker_ShouldPause(presenceTracker, Rfid_LastRfidCheckTimestamp)) {
				Log_Println(rfidTagRemoved, LOGLEVEL_NOTICE);
				if (!gPlayProperties.pausePlay && System_GetOperationMode() != OPMODE_BLUETOOTH_SINK && gPlayProperties.playMode != BUSY && gPlayProperties.playMode != NO_PLAYLIST) {
					AudioPlayer_TrackControlToQueueSender(PAUSEPLAY);
				}
			}

			if (!presenceUpdate.stableCardDetected) {
				continue;
			}

			Log_Printf(LOGLEVEL_DEBUG, "RFID state -> PresentStable");
			bool sameCardReapplied = false;
			if (hasLastAcceptedCard && memcmp(lastAcceptedCardId, presenceTracker.stableCardId, cardIdSize) == 0) {
				sameCardReapplied = true;
			}

			char hexString[(cardIdSize * 3u) + 1u] = {0};
			char cardIdString[cardIdStringSize] = {0};
			size_t hexOffset = 0u;
			size_t cardIdOffset = 0u;
			for (uint8_t i = 0u; i < cardIdSize; i++) {
				hexOffset += snprintf(hexString + hexOffset, sizeof(hexString) - hexOffset, "%02x%c", presenceTracker.stableCardId[i], (i < cardIdSize - 1u) ? '-' : ' ');
				cardIdOffset += snprintf(cardIdString + cardIdOffset, sizeof(cardIdString) - cardIdOffset, "%03d", presenceTracker.stableCardId[i]);
			}
			Log_Printf(LOGLEVEL_NOTICE, rfidTagDetected, hexString);

	#ifdef PAUSE_WHEN_RFID_REMOVED
		#ifdef ACCEPT_SAME_RFID_AFTER_TRACK_END
			if (!sameCardReapplied || gPlayProperties.trackFinished || gPlayProperties.playlistFinished) {
		#else
			if (!sameCardReapplied) {
		#endif
				xQueueSend(gRfidCardQueue, cardIdString, 0);
			} else if (gPlayProperties.pausePlay && System_GetOperationMode() != OPMODE_BLUETOOTH_SINK) {
				AudioPlayer_TrackControlToQueueSender(PAUSEPLAY);
			}
	#else
			xQueueSend(gRfidCardQueue, cardIdString, 0);
	#endif

			memcpy(lastAcceptedCardId, presenceTracker.stableCardId, cardIdSize);
			hasLastAcceptedCard = true;
		}
	}
}

void Rfid_Cyclic(void) {
	// Not necessary as cyclic stuff performed by task Rfid_Task()
}

void Rfid_Exit(void) {
	#ifndef RFID_READER_TYPE_MFRC522_I2C
	mfrc522.PCD_SoftPowerDown();
	#endif
}

void Rfid_WakeupCheck(void) {
}

#endif
