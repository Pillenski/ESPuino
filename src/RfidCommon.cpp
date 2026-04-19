#include <Arduino.h>
#include "settings.h"

#include "AudioPlayer.h"
#include "Cmd.h"
#include "Common.h"
#include "Log.h"
#include "MemX.h"
#include "Mqtt.h"
#include "Queues.h"
#include "Rfid.h"
#include "System.h"
#include "Web.h"

unsigned long Rfid_LastRfidCheckTimestamp = 0;
char gCurrentRfidTagId[cardIdStringSize] = ""; // No crap here as otherwise it could be shown in GUI
#ifdef DONT_ACCEPT_SAME_RFID_TWICE_ENABLE
char gOldRfidTagId[cardIdStringSize] = "X"; // Init with crap
#endif

static void RfidPresenceTracker_ResetCandidate(RfidPresenceTracker &tracker) {
	tracker.hasCandidateCard = false;
	tracker.presentConfirmCount = 0;
	memset(tracker.candidateCardId, 0, sizeof(tracker.candidateCardId));
}

void RfidPresenceTracker_Init(RfidPresenceTracker &tracker) {
	tracker = {};
}

RfidPresenceUpdate RfidPresenceTracker_Update(RfidPresenceTracker &tracker, bool cardPresent, const uint8_t *cardId, uint32_t now) {
	RfidPresenceUpdate update;

	if (cardPresent && cardId != nullptr) {
		tracker.removedConfirmCount = 0;
		tracker.removedSinceMs = 0;

		if (tracker.hasStableCard && memcmp(tracker.stableCardId, cardId, cardIdSize) == 0) {
			tracker.state = RfidPresenceState::PresentStable;
			tracker.pausePending = false;
			tracker.pausePendingSinceMs = 0;
			RfidPresenceTracker_ResetCandidate(tracker);
			return update;
		}

		if (!tracker.hasCandidateCard || memcmp(tracker.candidateCardId, cardId, cardIdSize) != 0) {
			memcpy(tracker.candidateCardId, cardId, cardIdSize);
			tracker.hasCandidateCard = true;
			tracker.presentConfirmCount = 1;
		} else if (tracker.presentConfirmCount < UINT8_MAX) {
			tracker.presentConfirmCount++;
		}

		tracker.state = RfidPresenceState::CandidatePresent;
		if (tracker.presentConfirmCount >= RFID_PRESENT_CONFIRM_POLLS) {
			memcpy(tracker.stableCardId, tracker.candidateCardId, cardIdSize);
			tracker.hasStableCard = true;
			tracker.state = RfidPresenceState::PresentStable;
			tracker.pausePending = false;
			tracker.pausePendingSinceMs = 0;
			RfidPresenceTracker_ResetCandidate(tracker);
			update.stableCardDetected = true;
		}
		return update;
	}

	RfidPresenceTracker_ResetCandidate(tracker);
	if (!tracker.hasStableCard) {
		tracker.state = RfidPresenceState::NoCard;
		return update;
	}

	if (tracker.removedConfirmCount == 0) {
		tracker.removedSinceMs = now;
	}
	if (tracker.removedConfirmCount < UINT8_MAX) {
		tracker.removedConfirmCount++;
	}
	tracker.state = RfidPresenceState::CandidateAbsent;
	if ((tracker.removedConfirmCount >= RFID_REMOVED_CONFIRM_POLLS) && ((now - tracker.removedSinceMs) >= RFID_REMOVED_MIN_MS)) {
		tracker.hasStableCard = false;
		tracker.removedConfirmCount = 0;
		tracker.removedSinceMs = 0;
		tracker.state = RfidPresenceState::NoCard;
		tracker.pausePending = true;
		tracker.pausePendingSinceMs = now;
		update.stableCardRemoved = true;
	}

	return update;
}

bool RfidPresenceTracker_ShouldPause(RfidPresenceTracker &tracker, uint32_t now) {
	if (!tracker.pausePending || tracker.hasStableCard) {
		return false;
	}
	if ((now - tracker.pausePendingSinceMs) < RFID_REAPPLY_GRACE_MS) {
		return false;
	}

	tracker.pausePending = false;
	tracker.pausePendingSinceMs = 0;
	return true;
}

// check if we have RFID-reader enabled
#if defined(RFID_READER_TYPE_MFRC522_SPI) || defined(RFID_READER_TYPE_MFRC522_I2C) || defined(RFID_READER_TYPE_PN5180)
	#define RFID_READER_ENABLED 1
#endif

// Tries to lookup RFID-tag-string in NVS and extracts parameter from it if found
void Rfid_PreferenceLookupHandler(void) {
#if defined(RFID_READER_ENABLED)
	BaseType_t rfidStatus;
	char rfidTagId[cardIdStringSize];
	char _file[255];
	uint32_t _lastPlayPos = 0;
	uint16_t _trackLastPlayed = 0;
	uint32_t _playMode = 1;

	rfidStatus = xQueueReceive(gRfidCardQueue, &rfidTagId, 0);
	if (rfidStatus == pdPASS) {
		System_UpdateActivityTimer();
		rfidTagId[cardIdStringSize - 1] = '\0';
		copyStringToBuffer(gCurrentRfidTagId, sizeof(gCurrentRfidTagId), rfidTagId);
		Log_Printf(LOGLEVEL_INFO, "%s: %s", rfidTagReceived, gCurrentRfidTagId);
		Web_SendWebsocketData(0, WebsocketCodeType::CurrentRfid); // Push new rfidTagId to all websocket-clients
		String s = "-1";
		if (gPrefsRfid.isKey(gCurrentRfidTagId)) {
			s = gPrefsRfid.getString(gCurrentRfidTagId, "-1"); // Try to lookup rfidId in NVS
		}
		if (!s.compareTo("-1")) {
			Log_Println(rfidTagUnknownInNvs, LOGLEVEL_ERROR);
			System_IndicateError();
			// allow to escape from bluetooth mode with an unknown card, switch back to normal mode
			System_SetOperationMode(OPMODE_NORMAL);
			return;
		}

		if (!parseRfidPreferenceEntry(s, _file, sizeof(_file), _lastPlayPos, _playMode, _trackLastPlayed)) {
			Log_Println(errorOccuredNvs, LOGLEVEL_ERROR);
			System_IndicateError();
		} else {
			// Only pass file to queue if the serialized entry could be parsed successfully
			if (_playMode >= 100) {
				// Modification-cards can change some settings (e.g. introducing track-looping or sleep after track/playlist).
				Cmd_Action(_playMode);
			} else {
		#ifdef DONT_ACCEPT_SAME_RFID_TWICE_ENABLE
				if (strncmp(gCurrentRfidTagId, gOldRfidTagId, 12) == 0) {
					Log_Printf(LOGLEVEL_ERROR, dontAccepctSameRfid, gCurrentRfidTagId);
					// System_IndicateError(); // Enable to have shown error @neopixel every time
					return;
				} else {
					copyStringToBuffer(gOldRfidTagId, sizeof(gOldRfidTagId), gCurrentRfidTagId);
				}
		#endif
	#ifdef MQTT_ENABLE
				publishMqtt(topicRfidState, gCurrentRfidTagId, false);
	#endif

	#ifdef BLUETOOTH_ENABLE
				// if music rfid was read, go back to normal mode
				if (System_GetOperationMode() == OPMODE_BLUETOOTH_SINK) {
					System_SetOperationMode(OPMODE_NORMAL);
				}
	#endif

				AudioPlayer_TrackQueueDispatcher(_file, _lastPlayPos, _playMode, _trackLastPlayed);
			}
		}
	}
#endif
}

#ifdef DONT_ACCEPT_SAME_RFID_TWICE_ENABLE
void Rfid_ResetOldRfid() {
	copyStringToBuffer(gOldRfidTagId, sizeof(gOldRfidTagId), "X");
}
#endif

#if defined(RFID_READER_ENABLED)
extern TaskHandle_t rfidTaskHandle;
#endif

void Rfid_TaskPause(void) {
#if defined(RFID_READER_ENABLED)
	vTaskSuspend(rfidTaskHandle);
#endif
}
void Rfid_TaskResume(void) {
#if defined(RFID_READER_ENABLED)
	vTaskResume(rfidTaskHandle);
#endif
}
