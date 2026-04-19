#pragma once

#include <Arduino.h>

constexpr uint8_t cardIdSize = 4u;
constexpr uint8_t cardIdStringSize = (cardIdSize * 3u) + 1u;

extern char gCurrentRfidTagId[cardIdStringSize];

enum class RfidPresenceState : uint8_t {
	NoCard = 0,
	CandidatePresent,
	PresentStable,
	CandidateAbsent,
};

struct RfidPresenceTracker {
	RfidPresenceState state = RfidPresenceState::NoCard;
	bool hasStableCard = false;
	bool hasCandidateCard = false;
	bool pausePending = false;
	uint8_t stableCardId[cardIdSize] = {0};
	uint8_t candidateCardId[cardIdSize] = {0};
	uint8_t presentConfirmCount = 0;
	uint8_t removedConfirmCount = 0;
	uint32_t removedSinceMs = 0;
	uint32_t pausePendingSinceMs = 0;
};

struct RfidPresenceUpdate {
	bool stableCardDetected = false;
	bool stableCardRemoved = false;
};

#ifndef PAUSE_WHEN_RFID_REMOVED
	#ifdef DONT_ACCEPT_SAME_RFID_TWICE // ignore feature silently if PAUSE_WHEN_RFID_REMOVED is active
		#define DONT_ACCEPT_SAME_RFID_TWICE_ENABLE
	#endif
#endif

#ifdef DONT_ACCEPT_SAME_RFID_TWICE_ENABLE
void Rfid_ResetOldRfid(void);
#endif

void Rfid_Init(void);
void Rfid_Cyclic(void);
void Rfid_Exit(void);
void Rfid_TaskPause(void);
void Rfid_TaskResume(void);
void Rfid_WakeupCheck(void);
void Rfid_PreferenceLookupHandler(void);
void RfidPresenceTracker_Init(RfidPresenceTracker &tracker);
RfidPresenceUpdate RfidPresenceTracker_Update(RfidPresenceTracker &tracker, bool cardPresent, const uint8_t *cardId, uint32_t now);
bool RfidPresenceTracker_ShouldPause(RfidPresenceTracker &tracker, uint32_t now);
